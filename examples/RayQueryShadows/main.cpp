// RayQueryShadows -- inline ray tracing from one Slang source (ray_query_shadows.slang) on Vulkan and Direct3D 12.
//
//   RayQueryShadows.exe --vk      RayQueryShadows.exe --dx12
//
// A rasterized scene (a ground, four pillars, 16 moving boxes) whose fragment shader casts a shadow ray towards the sun and a few
// ambient-occlusion rays with RayQuery. The boxes move, so the top-level acceleration structure is rebuilt every frame (one per frame
// in flight, so a rebuild never touches what an earlier frame still reads). Inspired by the Nox engine's ray-traced shadows / AO.
//   S: shadows on / off     A: ambient-occlusion rays 0 / 2 / 4 / 8     Space: pause

#include "../Common/ExampleApp.h"

#include <NRI/Extensions/NRIRayTracing.h>

namespace
{
    constexpr uint32_t kMaxObjects = 32;

    struct Object
    {
        nox::Mat4 model;
        nox::Mat4 normalMatrix;
        nox::Float4 color;
    };

    struct Objects
    {
        Object items[kMaxObjects];
    };

    struct Frame
    {
        nox::Mat4 viewProj;
        nox::Float4 cameraPosition;
        nox::Float4 sunDirection;
        uint32_t objectCount, aoRays, frameIndex, shadowsOn;
    };

    struct Vertex
    {
        float position[4];
        float normal[4];
    };

    struct Push
    {
        uint64_t frame, objects, vertices, tlas;
    };

    std::vector<Vertex> MakeCube()
    {
        std::vector<Vertex> vertices;
        const float faces[6][3][3] = {
            { { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 } }, { { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },
            { { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } }, { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },
            { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } },  { { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 } } };
        const float corners[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
        for (const auto& f : faces)
            for (const auto& c : corners)
            {
                Vertex v{};
                for (int i = 0; i < 3; ++i)
                {
                    v.position[i] = f[0][i] + f[1][i] * c[0] + f[2][i] * c[1];
                    v.normal[i] = f[0][i];
                }
                vertices.push_back(v);
            }
        return vertices;
    }

    nri::AccelerationStructure CreateAccelerationStructure(nox::Host& host, NriAccelerationStructureType type, uint64_t size, nri::Buffer& storage)
    {
        storage = host.device.createBuffer({ .size = size, .usage = NRI_BUFFER_USAGE_ACCELERATION_STRUCTURE });
        const NriAccelerationStructureDesc desc{ type, storage.handle(), 0, size };
        NriAccelerationStructure handle = nullptr;
        if (nriCreateAccelerationStructure(host.device.handle(), &desc, &handle) != NRI_SUCCESS || !handle)
            throw std::runtime_error("nriCreateAccelerationStructure failed");
        return nri::AccelerationStructure(handle);
    }
}

class RayQueryShadows final : public nox::Example
{
public:
    const char* Name() const override { return "RayQueryShadows"; }

    void Init(nox::Host& host) override
    {
        const std::vector<Vertex> cube = MakeCube();
        std::vector<uint32_t> indices(cube.size());
        for (uint32_t i = 0; i < indices.size(); ++i)
            indices[i] = i;
        m_Vertices = host.CreateBuffer(cube.size() * sizeof(Vertex), NRI_BUFFER_USAGE_STORAGE_STATIC, cube.data());
        m_VerticesRef = host.Refs(m_Vertices, cube.size() * sizeof(Vertex), false).read;
        m_Indices = host.CreateBuffer(indices.size() * 4, NRI_BUFFER_USAGE_STORAGE_STATIC, indices.data());

        // The BLAS: the unit cube (positions are the first 12 bytes of each 32-byte vertex).
        NriAccelerationStructureTrianglesDesc triangles{};
        triangles.vertexBufferAddress = m_Vertices.getDeviceAddress();
        triangles.vertexStride = sizeof(Vertex);
        triangles.maxVertex = uint32_t(cube.size() - 1);
        triangles.indexBufferAddress = m_Indices.getDeviceAddress();
        triangles.primitiveCount = uint32_t(indices.size() / 3);
        triangles.isOpaque = true;
        NriAccelerationStructureBuildDesc blasBuild{};
        blasBuild.type = NRI_ACCELERATION_STRUCTURE_BOTTOM_LEVEL;
        blasBuild.flags = NRI_AS_BUILD_PREFER_FAST_TRACE;
        blasBuild.triangles = &triangles;
        blasBuild.triangleCount = 1;
        const NriAccelerationStructureBuildSizes blasSizes = nriGetAccelerationStructureBuildSizes(host.device.handle(), &blasBuild);
        if (blasSizes.accelerationStructureSize == 0)
            throw std::runtime_error("this device has no ray tracing");
        m_Blas = CreateAccelerationStructure(host, NRI_ACCELERATION_STRUCTURE_BOTTOM_LEVEL, blasSizes.accelerationStructureSize, m_BlasStorage);

        // One TLAS (with its instance buffer and scratch) per frame in flight; rebuilt each frame (PREFER_FAST_BUILD).
        NriAccelerationStructureBuildDesc tlasBuild{};
        tlasBuild.type = NRI_ACCELERATION_STRUCTURE_TOP_LEVEL;
        tlasBuild.flags = NRI_AS_BUILD_PREFER_FAST_BUILD;
        tlasBuild.instances = { 0, kMaxObjects };
        const NriAccelerationStructureBuildSizes tlasSizes = nriGetAccelerationStructureBuildSizes(host.device.handle(), &tlasBuild);
        m_BlasScratch = host.device.createBuffer({ .size = blasSizes.buildScratchSize, .usage = NRI_BUFFER_USAGE_ACCELERATION_STRUCTURE_SCRATCH });
        for (uint32_t slot = 0; slot < nox::kFramesInFlight; ++slot)
        {
            m_Tlas[slot] = CreateAccelerationStructure(host, NRI_ACCELERATION_STRUCTURE_TOP_LEVEL, tlasSizes.accelerationStructureSize, m_TlasStorage[slot]);
            m_TlasRef[slot] = host.TlasRef(m_Tlas[slot]);
            m_TlasScratch[slot] = host.device.createBuffer({ .size = tlasSizes.buildScratchSize, .usage = NRI_BUFFER_USAGE_ACCELERATION_STRUCTURE_SCRATCH });
            m_InstanceBuffers[slot] = host.device.createBuffer({ .size = sizeof(NriAccelerationStructureInstance) * kMaxObjects,
                                                                 .usage = NRI_BUFFER_USAGE_ACCELERATION_STRUCTURE_INSTANCE });
        }

        host.Submit([&](nri::CommandBuffer& cmd) {
            const nri::ResourceState shaderRead{ NRI_ACCESS_SHADER_READ, NRI_STAGE_VERTEX | NRI_STAGE_FRAGMENT | NRI_STAGE_COMPUTE };
            const nri::ResourceState buildInput{ NRI_ACCESS_ACCELERATION_STRUCTURE_READ, NRI_STAGE_ACCELERATION_STRUCTURE_BUILD };
            const nri::ResourceState scratch{ NRI_ACCESS_ACCELERATION_STRUCTURE_READ | NRI_ACCESS_ACCELERATION_STRUCTURE_WRITE, NRI_STAGE_ACCELERATION_STRUCTURE_BUILD };
            std::vector<nri::BufferBarrierDesc> before = { { m_Vertices.handle(), shaderRead, buildInput }, { m_Indices.handle(), shaderRead, buildInput },
                                                           { m_BlasScratch.handle(), {}, scratch } };
            for (nri::Buffer& s : m_TlasScratch)
                before.push_back({ s.handle(), {}, scratch });
            cmd.resourceBarriers({}, before);
            nriCmdBuildAccelerationStructure(cmd.handle(), &blasBuild, m_BlasScratch.getDeviceAddress(), m_Blas.handle());
            nriCmdAccelerationStructureBarrier(cmd.handle(), NRI_AS_BARRIER_BUILD_TO_BUILD);
            const nri::BufferBarrierDesc after[2] = { { m_Vertices.handle(), buildInput, shaderRead }, { m_Indices.handle(), buildInput, shaderRead } };
            cmd.resourceBarriers({}, after);
        });

        m_Frame.Create(host);
        m_Objects.Create(host);
        m_Pipeline = host.Graphics("ray_query_shadows.slang", "vsScene", "psScene");
    }

    void Render(nox::Host& host, nri::RenderGraph& graph, const nox::FrameInfo& frame) override
    {
        if (!m_Paused)
            m_AnimTime += frame.deltaTime;

        // --- the scene: the same objects drawn by the rasterizer and placed in the TLAS ---
        std::vector<Object> objects;
        const auto add = [&](nox::Float3 position, nox::Float3 scale, float yaw, float pitch, nox::Float4 color) {
            const nox::Mat4 rotation = nox::RotateY(yaw) * nox::RotateX(pitch);
            objects.push_back({ nox::Translate(position) * rotation * nox::Scale(scale), rotation * nox::Scale({ 1 / scale.x, 1 / scale.y, 1 / scale.z }), color });
        };
        add({ 0, -0.5f, 0 }, { 40, 0.5f, 40 }, 0, 0, { 0.8f, 0.8f, 0.78f, 1 }); // ground (checker), top at y = 0
        for (int i = 0; i < 4; ++i)
            add({ (i & 1 ? 5.0f : -5.0f), 2.0f, (i & 2 ? 5.0f : -5.0f) }, { 0.45f, 2.0f, 0.45f }, 0.3f * i, 0, { 0.85f, 0.75f, 0.6f, 0 });
        for (int i = 0; i < 16; ++i)
        {
            const float t = m_AnimTime * (0.25f + 0.04f * i) + i * 0.39f, radius = 1.8f + 0.28f * i;
            add({ std::cos(t) * radius, 0.9f + 0.7f * std::sin(t * 1.7f + i), std::sin(t) * radius }, { 0.35f, 0.35f, 0.35f }, t * 1.3f, t * 0.7f,
                { 0.5f + 0.5f * std::cos(i * 0.8f), 0.5f + 0.5f * std::cos(i * 0.8f + 2.1f), 0.5f + 0.5f * std::cos(i * 0.8f + 4.2f), 0 });
        }

        Objects objectData{};
        std::vector<NriAccelerationStructureInstance> instances(objects.size());
        for (size_t i = 0; i < objects.size(); ++i)
        {
            objectData.items[i] = objects[i];
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c)
                    instances[i].transform.matrix[r][c] = objects[i].model.At(r, c); // (3 x 4, row-major: both APIs)
            instances[i].instanceCustomIndex = uint32_t(i);
            instances[i].mask = 0xFF;
            instances[i].accelerationStructureReference = m_Blas.getDeviceAddress();
        }
        nri::Buffer& instanceBuffer = m_InstanceBuffers[frame.frameSlot];
        std::memcpy(instanceBuffer.map(0, instances.size() * sizeof(instances[0])), instances.data(), instances.size() * sizeof(instances[0]));
        instanceBuffer.unmap();

        const float orbit = m_AnimTime * 0.07f;
        const nox::Float3 eye{ std::sin(orbit) * 13.0f, 6.5f, std::cos(orbit) * 13.0f };
        Frame frameData{};
        frameData.viewProj = nox::PerspectiveReverseZ(0.85f, frame.Aspect(), 0.1f) * nox::LookAt(eye, { 0, 0.8f, 0 }, { 0, 1, 0 });
        frameData.cameraPosition = { eye.x, eye.y, eye.z, 1 };
        const nox::Float3 sun = nox::Normalize({ std::cos(m_AnimTime * 0.2f) * 0.8f, 1.0f, std::sin(m_AnimTime * 0.2f) * 0.8f });
        frameData.sunDirection = { sun.x, sun.y, sun.z, 0 };
        frameData.objectCount = uint32_t(objects.size());
        frameData.aoRays = m_AoRays;
        frameData.frameIndex = uint32_t(frame.frameNumber);
        frameData.shadowsOn = m_Shadows ? 1 : 0;

        const Push push{ m_Frame.Write(frame.frameSlot, frameData), m_Objects.Write(frame.frameSlot, objectData), m_VerticesRef, m_TlasRef[frame.frameSlot] };
        const uint32_t objectCount = uint32_t(objects.size()), slot = frame.frameSlot;

        char status[96];
        std::snprintf(status, sizeof(status), "shadows %s, AO rays %u (S / A)", m_Shadows ? "on" : "off", m_AoRays);
        host.SetStatus(status);

        // --- rebuild this slot's TLAS, then draw ---
        graph.AddPass("BuildTLAS", nri::RGPassFlags::NeverCull,
            [](nri::RGBuilder&) {},
            [this, slot, objectCount](nri::RGPassContext& ctx) {
                NriAccelerationStructureBuildDesc build{};
                build.type = NRI_ACCELERATION_STRUCTURE_TOP_LEVEL;
                build.flags = NRI_AS_BUILD_PREFER_FAST_BUILD;
                build.instances = { m_InstanceBuffers[slot].getDeviceAddress(), objectCount };
                nriCmdBuildAccelerationStructure(ctx.Cmd().handle(), &build, m_TlasScratch[slot].getDeviceAddress(), m_Tlas[slot].handle());
                nriCmdAccelerationStructureBarrier(ctx.Cmd().handle(), NRI_AS_BARRIER_BUILD_TO_SHADER_READ);
            });

        const nri::RGTexture depth = graph.CreateTexture("Depth", { .Format = NRI_FORMAT_NONE, .Usage = NRI_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT });
        graph.AddPass("Scene", nri::RGPassFlags::Raster,
            [&](nri::RGBuilder& b) {
                b.SwapchainTarget(host.swapchain, frame.imageIndex, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE, { 0.35f, 0.45f, 0.62f, 1.0f });
                b.DepthTarget(depth, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_DONT_CARE, { 0.0f, 0 });
                b.SetRenderArea(frame.extent);
            },
            [this, push, objectCount](nri::RGPassContext& ctx) {
                nri::CommandBuffer& cmd = ctx.Cmd();
                nox::Host::SetRasterState(cmd, { .depthTest = true, .depthWrite = true, .cull = NRI_CULL_MODE_BACK });
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline);
                cmd.pushData(&push, sizeof(push));
                cmd.draw(36, objectCount);
            });
    }

    void OnKey(SDL_Keycode key) override
    {
        if (key == SDLK_S)
            m_Shadows = !m_Shadows;
        else if (key == SDLK_A)
            m_AoRays = m_AoRays == 0 ? 2 : m_AoRays == 2 ? 4 : m_AoRays == 4 ? 8 : 0;
        else if (key == SDLK_SPACE)
            m_Paused = !m_Paused;
    }

    void Shutdown(nox::Host&) override
    {
        m_Pipeline.reset();
        m_Frame.Reset();
        m_Objects.Reset();
        for (uint32_t slot = 0; slot < nox::kFramesInFlight; ++slot)
        {
            m_Tlas[slot].reset();
            m_TlasStorage[slot].reset();
            m_TlasScratch[slot].reset();
            m_InstanceBuffers[slot].reset();
        }
        m_Blas.reset();
        m_BlasStorage.reset();
        m_BlasScratch.reset();
        m_Vertices.reset();
        m_Indices.reset();
    }

private:
    nri::Buffer m_Vertices, m_Indices, m_BlasStorage, m_BlasScratch;
    nri::AccelerationStructure m_Blas;
    nri::AccelerationStructure m_Tlas[nox::kFramesInFlight];
    nri::Buffer m_TlasStorage[nox::kFramesInFlight], m_TlasScratch[nox::kFramesInFlight], m_InstanceBuffers[nox::kFramesInFlight];
    uint64_t m_TlasRef[nox::kFramesInFlight]{};
    uint64_t m_VerticesRef = 0;
    nox::FrameConstants<Frame> m_Frame;
    nox::FrameConstants<Objects> m_Objects;
    nri::Pipeline m_Pipeline;
    uint32_t m_AoRays = 4;
    bool m_Shadows = true, m_Paused = false;
    float m_AnimTime = 0.0f;
};

NOX_EXAMPLE_MAIN(RayQueryShadows)
