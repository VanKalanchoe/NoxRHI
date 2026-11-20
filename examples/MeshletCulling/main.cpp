// MeshletCulling -- task + mesh shaders from one Slang source (meshlet_culling.slang) on Vulkan and Direct3D 12.
//
//   MeshletCulling.exe --vk      MeshletCulling.exe --dx12
//
// 576 spheres split into meshlets (<= 64 vertices, <= 124 triangles, with a bounding sphere and a normal cone). A task shader culls each
// meshlet against the frustum and its cone and launches mesh shaders for the visible ones; the window title shows how many were drawn
// (read back from the GPU). The meshlet triangle stream is packed bytes, as in the Nox engine -- read with LoadByte, since DXIL has no
// 8-bit types.
//   F: freeze the culling camera (fly away and look at what was culled)     C: meshlet / instance colours     Space: pause

#include "../Common/ExampleApp.h"

#include <unordered_map>

namespace
{
    struct Meshlet
    {
        uint32_t vertexOffset, triangleOffset, vertexCount, triangleCount;
        float sphere[4];
        float cone[4];
    };

    struct Vertex
    {
        float position[4];
        float normal[4];
    };

    struct Instance
    {
        float positionScale[4];
        float color[4];
    };

    struct View
    {
        nox::Mat4 viewProj;
        nox::Float4 cullPlanes[5];
        nox::Float4 cullPosition;
        uint32_t meshletCount, instanceCount, colorMode, pad;
    };

    struct Push
    {
        uint64_t view, meshlets, meshletVertices, meshletTriangles, vertices, instances, stats;
    };

    constexpr uint32_t kMaxVertices = 64, kMaxTriangles = 124, kTaskGroupSize = 32;
    constexpr uint32_t kGrid = 24;

    struct Mesh
    {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
    };

    Mesh MakeSphere(uint32_t segments, uint32_t rings)
    {
        Mesh mesh;
        for (uint32_t r = 0; r <= rings; ++r)
            for (uint32_t s = 0; s <= segments; ++s)
            {
                const float theta = float(r) / rings * 3.14159265f, phi = float(s) / segments * 6.2831853f;
                const float x = std::sin(theta) * std::cos(phi), y = std::cos(theta), z = std::sin(theta) * std::sin(phi);
                mesh.vertices.push_back({ { x, y, z, 1 }, { x, y, z, 0 } });
            }
        for (uint32_t r = 0; r < rings; ++r)
            for (uint32_t s = 0; s < segments; ++s)
            {
                const uint32_t a = r * (segments + 1) + s, b = a + segments + 1;
                // counter-clockwise seen from outside
                if (r != 0)
                    mesh.indices.insert(mesh.indices.end(), { a, a + 1, b });
                if (r != rings - 1)
                    mesh.indices.insert(mesh.indices.end(), { a + 1, b + 1, b });
            }
        return mesh;
    }

    // Greedy meshlets in index order, with meshoptimizer-style bounds: a sphere around the vertices and a cone of the triangle normals.
    void BuildMeshlets(const Mesh& mesh, std::vector<Meshlet>& meshlets, std::vector<uint32_t>& meshletVertices, std::vector<uint8_t>& meshletTriangles)
    {
        std::unordered_map<uint32_t, uint8_t> local;
        std::vector<uint32_t> triangles; // indices of the current meshlet's triangles
        const auto flush = [&]() {
            if (triangles.empty())
                return;
            Meshlet m{};
            m.vertexOffset = uint32_t(meshletVertices.size());
            m.triangleOffset = uint32_t(meshletTriangles.size());
            m.triangleCount = uint32_t(triangles.size());
            std::vector<uint32_t> order(local.size());
            for (const auto& [global, l] : local)
                order[l] = global;
            m.vertexCount = uint32_t(order.size());
            meshletVertices.insert(meshletVertices.end(), order.begin(), order.end());
            nox::Float3 lo{ 1e9f, 1e9f, 1e9f }, hi{ -1e9f, -1e9f, -1e9f };
            for (uint32_t v : order)
            {
                const float* p = mesh.vertices[v].position;
                lo = { std::min(lo.x, p[0]), std::min(lo.y, p[1]), std::min(lo.z, p[2]) };
                hi = { std::max(hi.x, p[0]), std::max(hi.y, p[1]), std::max(hi.z, p[2]) };
            }
            const nox::Float3 center = (lo + hi) * 0.5f;
            float radius = 0.0f;
            for (uint32_t v : order)
            {
                const float* p = mesh.vertices[v].position;
                const nox::Float3 d = nox::Float3{ p[0], p[1], p[2] } - center;
                radius = std::max(radius, std::sqrt(nox::Dot(d, d)));
            }
            nox::Float3 axis{};
            std::vector<nox::Float3> normals;
            for (uint32_t t : triangles)
            {
                const float* a = mesh.vertices[mesh.indices[t * 3 + 0]].position;
                const float* b = mesh.vertices[mesh.indices[t * 3 + 1]].position;
                const float* c = mesh.vertices[mesh.indices[t * 3 + 2]].position;
                const nox::Float3 n = nox::Normalize(nox::Cross(nox::Float3{ b[0] - a[0], b[1] - a[1], b[2] - a[2] }, nox::Float3{ c[0] - a[0], c[1] - a[1], c[2] - a[2] }));
                normals.push_back(n);
                axis = axis + n;
                for (int k = 0; k < 3; ++k)
                    meshletTriangles.push_back(local[mesh.indices[t * 3 + k]]);
            }
            axis = nox::Normalize(axis);
            float minDot = 1.0f;
            for (const nox::Float3& n : normals)
                minDot = std::min(minDot, nox::Dot(axis, n));
            m.sphere[0] = center.x; m.sphere[1] = center.y; m.sphere[2] = center.z; m.sphere[3] = radius;
            m.cone[0] = axis.x; m.cone[1] = axis.y; m.cone[2] = axis.z;
            m.cone[3] = minDot <= 0.1f ? 2.0f : std::sqrt(1.0f - minDot * minDot); // (> 1: the cone is too wide to ever cull)
            meshlets.push_back(m);
            local.clear();
            triangles.clear();
        };

        for (uint32_t t = 0; t < mesh.indices.size() / 3; ++t)
        {
            uint32_t added = 0;
            for (int k = 0; k < 3; ++k)
                added += local.count(mesh.indices[t * 3 + k]) ? 0 : 1;
            if (local.size() + added > kMaxVertices || triangles.size() + 1 > kMaxTriangles)
                flush();
            for (int k = 0; k < 3; ++k)
                local.try_emplace(mesh.indices[t * 3 + k], uint8_t(local.size()));
            triangles.push_back(t);
        }
        flush();
        while (meshletTriangles.size() % 4)
            meshletTriangles.push_back(0); // (whole 32-bit words: the shader reads words)
    }
}

class MeshletCulling final : public nox::Example
{
public:
    const char* Name() const override { return "MeshletCulling"; }

    void Init(nox::Host& host) override
    {
        if (!host.device.supportsMeshShaders())
            throw std::runtime_error("this device has no mesh shaders");

        const Mesh sphere = MakeSphere(48, 24);
        std::vector<Meshlet> meshlets;
        std::vector<uint32_t> meshletVertices;
        std::vector<uint8_t> meshletTriangles;
        BuildMeshlets(sphere, meshlets, meshletVertices, meshletTriangles);
        m_MeshletCount = uint32_t(meshlets.size());

        std::vector<Instance> instances;
        for (uint32_t z = 0; z < kGrid; ++z)
            for (uint32_t x = 0; x < kGrid; ++x)
            {
                const float hue = float(x + z * kGrid) * 0.37f;
                instances.push_back({ { (float(x) - (kGrid - 1) * 0.5f) * 2.6f, std::sin(x * 0.5f + z * 0.3f) * 0.6f, (float(z) - (kGrid - 1) * 0.5f) * 2.6f,
                                        0.7f + 0.3f * float((x * 3 + z * 5) % 7) / 6.0f },
                                      { 0.6f + 0.4f * std::cos(hue), 0.6f + 0.4f * std::cos(hue + 2.1f), 0.6f + 0.4f * std::cos(hue + 4.2f), 1 } });
            }
        m_InstanceCount = uint32_t(instances.size());

        const auto upload = [&](const void* data, uint64_t bytes, nri::Buffer& buffer) {
            buffer = host.CreateBuffer(bytes, NRI_BUFFER_USAGE_STORAGE_STATIC, data);
            return host.Refs(buffer, bytes, false).read;
        };
        m_Push.meshlets = upload(meshlets.data(), meshlets.size() * sizeof(Meshlet), m_Meshlets);
        m_Push.meshletVertices = upload(meshletVertices.data(), meshletVertices.size() * 4, m_MeshletVertices);
        m_Push.meshletTriangles = upload(meshletTriangles.data(), meshletTriangles.size(), m_MeshletTriangles);
        m_Push.vertices = upload(sphere.vertices.data(), sphere.vertices.size() * sizeof(Vertex), m_Vertices);
        m_Push.instances = upload(instances.data(), instances.size() * sizeof(Instance), m_Instances);

        m_Stats = host.CreateBuffer(4, NRI_BUFFER_USAGE_STORAGE);
        m_Push.stats = host.Refs(m_Stats, 4, true).write;
        const uint32_t zero = 0;
        m_Zero = host.CreateBuffer(4, NRI_BUFFER_USAGE_STAGING, &zero);
        for (nri::Buffer& readback : m_Readback)
            readback = host.device.createBuffer({ .size = 4, .usage = NRI_BUFFER_USAGE_READBACK });

        m_View.Create(host);
        m_Pipeline = host.Mesh("meshlet_culling.slang", "tsCull", "msMeshlet", "psMeshlet");
        std::printf("[MeshletCulling] %u meshlets per sphere (%zu triangles), %u spheres\n", m_MeshletCount, sphere.indices.size() / 3, m_InstanceCount);
    }

    void Render(nox::Host& host, nri::RenderGraph& graph, const nox::FrameInfo& frame) override
    {
        // The frame that last used this slot has finished (the swapchain waited for it): its count is ready.
        if (frame.frameNumber >= nox::kFramesInFlight)
        {
            uint32_t drawn = 0;
            std::memcpy(&drawn, m_Readback[frame.frameSlot].map(0, 4), 4);
            m_Readback[frame.frameSlot].unmap();
            const uint32_t total = m_MeshletCount * m_InstanceCount;
            char status[128];
            std::snprintf(status, sizeof(status), "meshlets drawn: %u / %u (%.0f%% culled)%s", drawn, total, 100.0 * (total - drawn) / total,
                          m_Frozen ? " [culling frozen]" : "");
            host.SetStatus(status);
        }

        if (!m_Paused)
            m_AnimTime += frame.deltaTime;
        const float orbit = m_AnimTime * 0.1f;
        const nox::Float3 eye{ std::sin(orbit) * 22.0f, 9.0f + 5.0f * std::sin(m_AnimTime * 0.23f), std::cos(orbit) * 22.0f };
        const nox::Float3 target{ std::sin(orbit + 0.9f) * 8.0f, 0.0f, std::cos(orbit + 0.9f) * 8.0f };
        const nox::Mat4 viewProj = nox::PerspectiveReverseZ(0.9f, frame.Aspect(), 0.1f) * nox::LookAt(eye, target, { 0, 1, 0 });
        if (!m_Frozen)
        {
            m_CullViewProj = viewProj;
            m_CullPosition = eye;
        }
        View view{};
        view.viewProj = viewProj;
        nox::FrustumPlanes(m_CullViewProj, view.cullPlanes);
        view.cullPosition = { m_CullPosition.x, m_CullPosition.y, m_CullPosition.z, 1 };
        view.meshletCount = m_MeshletCount;
        view.instanceCount = m_InstanceCount;
        view.colorMode = m_ColorMode;
        Push push = m_Push;
        push.view = m_View.Write(frame.frameSlot, view);

        const nri::RGBuffer stats = graph.ImportBuffer("Stats", m_Stats);
        const nri::RGTexture depth = graph.CreateTexture("Depth", { .Format = NRI_FORMAT_NONE, .Usage = NRI_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT });

        graph.AddPass("ResetStats", nri::RGPassFlags::None,
            [&](nri::RGBuilder& b) { b.Write(stats, nri::RGBufferAccess::CopyDestination); },
            [this](nri::RGPassContext& ctx) { ctx.Cmd().copyBuffer(m_Zero, m_Stats, nri::BufferCopyRegion{ 0, 0, 4 }); });

        graph.AddPass("Meshlets", nri::RGPassFlags::Raster,
            [&](nri::RGBuilder& b) {
                b.Write(stats);
                b.SwapchainTarget(host.swapchain, frame.imageIndex, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE, { 0.08f, 0.09f, 0.12f, 1.0f });
                b.DepthTarget(depth, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_DONT_CARE, { 0.0f, 0 });
                b.SetRenderArea(frame.extent);
            },
            [this, push](nri::RGPassContext& ctx) {
                nri::CommandBuffer& cmd = ctx.Cmd();
                nox::Host::SetRasterState(cmd, { .depthTest = true, .depthWrite = true, .cull = NRI_CULL_MODE_BACK });
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline);
                cmd.pushData(&push, sizeof(push));
                nri::drawMeshTasks(cmd, (m_MeshletCount * m_InstanceCount + kTaskGroupSize - 1) / kTaskGroupSize);
            });

        graph.AddPass("ReadStats", nri::RGPassFlags::None,
            [&](nri::RGBuilder& b) { b.Read(stats, nri::RGBufferAccess::CopySource); },
            [this, slot = frame.frameSlot](nri::RGPassContext& ctx) { ctx.Cmd().copyBuffer(m_Stats, m_Readback[slot], nri::BufferCopyRegion{ 0, 0, 4 }); });
    }

    void OnKey(SDL_Keycode key) override
    {
        if (key == SDLK_F)
            m_Frozen = !m_Frozen;
        else if (key == SDLK_C)
            m_ColorMode ^= 1;
        else if (key == SDLK_SPACE)
            m_Paused = !m_Paused;
    }

    void Shutdown(nox::Host&) override
    {
        m_Pipeline.reset();
        m_View.Reset();
        for (nri::Buffer* b : { &m_Meshlets, &m_MeshletVertices, &m_MeshletTriangles, &m_Vertices, &m_Instances, &m_Stats, &m_Zero, &m_Readback[0], &m_Readback[1] })
            b->reset();
    }

private:
    nri::Buffer m_Meshlets, m_MeshletVertices, m_MeshletTriangles, m_Vertices, m_Instances, m_Stats, m_Zero;
    nri::Buffer m_Readback[nox::kFramesInFlight];
    nox::FrameConstants<View> m_View;
    nri::Pipeline m_Pipeline;
    Push m_Push{};
    uint32_t m_MeshletCount = 0, m_InstanceCount = 0, m_ColorMode = 0;
    nox::Mat4 m_CullViewProj;
    nox::Float3 m_CullPosition;
    float m_AnimTime = 0.0f;
    bool m_Frozen = false, m_Paused = false;
};

NOX_EXAMPLE_MAIN(MeshletCulling)
