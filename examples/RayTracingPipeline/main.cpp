// RayTracingPipeline -- a ray tracing pipeline (ray generation, miss, closest hit, any hit, a shader binding table, DispatchRays /
// vkCmdTraceRaysKHR) from one Slang source (ray_tracing_pipeline.slang) on Vulkan and Direct3D 12.
//
//   RayTracingPipeline.exe --vk      RayTracingPipeline.exe --dx12
//
// RayQueryShadows' scene (a ground, four pillars, 16 moving boxes), every pixel ray traced: the camera ray's closest hit shades the
// surface and traces a shadow ray towards the sun; a third of the boxes are cut-outs (a lattice of holes) whose hit group adds an any
// hit shader that ignores hits in the holes -- camera and shadow rays pass through them. The TLAS is rebuilt every frame (one per frame
// in flight). The other examples trace with RayQuery (inline); this one is the only user of NoxRHI's ray tracing pipeline path.
//   S: shadows on / off     C: cut-outs on / off     Space: pause
//   Start-up options: --no-shadows, --no-cutouts

#include "../Common/ExampleApp.h"

#include <NRI/Extensions/NRIRayTracing.h>

namespace
{
    constexpr uint32_t kMaxObjects = 32;
    // Instance flags (the same bits in Vulkan and D3D12): opaque instances skip any hit shaders.
    constexpr uint32_t kInstanceForceOpaque = 0x4;

    struct Object
    {
        nox::Mat4 model;
        nox::Mat4 normalMatrix;
        nox::Float4 color; // a: 1 = checker, 2 = cut-out
    };

    struct Objects
    {
        Object items[kMaxObjects];
    };

    struct Frame
    {
        nox::Float4 cameraPosition, cameraForward, cameraRight, cameraUp;
        nox::Float4 sunDirection;
        uint32_t objectCount, frameIndex, shadowsOn, padding;
    };

    struct Vertex
    {
        float position[4];
        float normal[4];
    };

    struct Push
    {
        uint64_t frame, objects, vertices, tlas;
        uint32_t outputSlot, width, height, padding;
    };

    struct PresentPush
    {
        uint32_t imageSlot;
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

class RayTracingPipeline final : public nox::Example
{
public:
    const char* Name() const override { return "RayTracingPipeline"; }

    void Init(nox::Host& host) override
    {
        const std::vector<Vertex> cube = MakeCube();
        std::vector<uint32_t> indices(cube.size());
        for (uint32_t i = 0; i < indices.size(); ++i)
            indices[i] = i;
        m_Vertices = host.CreateBuffer(cube.size() * sizeof(Vertex), NRI_BUFFER_USAGE_STORAGE_STATIC, cube.data());
        m_VerticesRef = host.Refs(m_Vertices, cube.size() * sizeof(Vertex), false).read;
        m_Indices = host.CreateBuffer(indices.size() * 4, NRI_BUFFER_USAGE_STORAGE_STATIC, indices.data());

        // The BLAS: the unit cube, NOT opaque -- any hit shaders may run on it (the opaque instances say FORCE_OPAQUE instead).
        NriAccelerationStructureTrianglesDesc triangles{};
        triangles.vertexBufferAddress = m_Vertices.getDeviceAddress();
        triangles.vertexStride = sizeof(Vertex);
        triangles.maxVertex = uint32_t(cube.size() - 1);
        triangles.indexBufferAddress = m_Indices.getDeviceAddress();
        triangles.primitiveCount = uint32_t(indices.size() / 3);
        triangles.isOpaque = false;
        NriAccelerationStructureBuildDesc blasBuild{};
        blasBuild.type = NRI_ACCELERATION_STRUCTURE_BOTTOM_LEVEL;
        blasBuild.flags = NRI_AS_BUILD_PREFER_FAST_TRACE;
        blasBuild.triangles = &triangles;
        blasBuild.triangleCount = 1;
        const NriAccelerationStructureBuildSizes blasSizes = nriGetAccelerationStructureBuildSizes(host.device.handle(), &blasBuild);
        if (blasSizes.accelerationStructureSize == 0)
            throw std::runtime_error("this device has no ray tracing");
        m_Blas = CreateAccelerationStructure(host, NRI_ACCELERATION_STRUCTURE_BOTTOM_LEVEL, blasSizes.accelerationStructureSize, m_BlasStorage);

        NriAccelerationStructureBuildDesc tlasBuild{};
        tlasBuild.type = NRI_ACCELERATION_STRUCTURE_TOP_LEVEL;
        tlasBuild.flags = NRI_AS_BUILD_PREFER_FAST_BUILD;
        tlasBuild.instances = { 0, kMaxObjects };
        const NriAccelerationStructureBuildSizes tlasSizes = nriGetAccelerationStructureBuildSizes(host.device.handle(), &tlasBuild);
        m_BlasScratch = host.device.createBuffer({ .size = blasSizes.buildScratchSize, .usage = NRI_BUFFER_USAGE_ACCELERATION_STRUCTURE_SCRATCH });
        for (uint32_t slot = 0; slot < nox::kFramesInFlight; ++slot)
        {
            m_Tlas[slot] = CreateAccelerationStructure(host, NRI_ACCELERATION_STRUCTURE_TOP_LEVEL, tlasSizes.accelerationStructureSize, m_TlasStorage[slot]);
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
            // (the vertices are read by the closest hit shader: the ray tracing stage)
            const nri::ResourceState rayTracingRead{ NRI_ACCESS_SHADER_READ, NRI_STAGE_RAY_TRACING };
            const nri::BufferBarrierDesc after[2] = { { m_Vertices.handle(), buildInput, rayTracingRead }, { m_Indices.handle(), buildInput, shaderRead } };
            cmd.resourceBarriers({}, after);
        });

        // Each slot's TLAS is built once before a descriptor refers to it (D3D12: the SRV is created after the first build).
        for (uint32_t slot = 0; slot < nox::kFramesInFlight; ++slot)
        {
            Objects objectData{};
            const uint32_t count = WriteScene(0.0f, objectData, m_InstanceBuffers[slot]);
            host.Submit([&](nri::CommandBuffer& cmd) {
                NriAccelerationStructureBuildDesc build{};
                build.type = NRI_ACCELERATION_STRUCTURE_TOP_LEVEL;
                build.flags = NRI_AS_BUILD_PREFER_FAST_BUILD;
                build.instances = { m_InstanceBuffers[slot].getDeviceAddress(), count };
                nriCmdBuildAccelerationStructure(cmd.handle(), &build, m_TlasScratch[slot].getDeviceAddress(), m_Tlas[slot].handle());
                nriCmdAccelerationStructureBarrier(cmd.handle(), NRI_AS_BARRIER_BUILD_TO_SHADER_READ);
            });
            m_TlasRef[slot] = host.TlasRef(m_Tlas[slot]);
        }

        m_Frame.Create(host);
        m_Objects.Create(host);

        // The ray tracing pipeline. Shader groups (both APIs): 0 ray generation, 1 / 2 the misses in this order (TraceRay's miss index
        // 0 sky, 1 shadow), then the hit groups: 0 opaque, 1 cut-out (an instance's record offset picks one).
        const std::string source = host.ShaderPath("ray_tracing_pipeline.slang");
        nri::PipelineDesc desc{
            .type = NRI_PIPELINE_RAY_TRACING,
            .shaders = { { NRI_SHADER_STAGE_RAY_GEN, "rayGenMain", source },            // 0
                         { NRI_SHADER_STAGE_MISS, "missSkyMain", source },              // 1
                         { NRI_SHADER_STAGE_MISS, "missShadowMain", source },           // 2
                         { NRI_SHADER_STAGE_CLOSEST_HIT, "closestHitMain", source },    // 3
                         { NRI_SHADER_STAGE_ANY_HIT, "anyHitCutoutMain", source } },    // 4
            .hitGroups = { { 3, ~0u, ~0u }, { 3, 4, ~0u } },
            .maxRecursionDepth = 2,  // the closest hit traces the shadow ray
            .buildNow = true,
            .maxPayloadSize = 16,    // RayPayload: float3 colour + uint visible
            .maxAttributeSize = 8 }; // the triangle barycentrics
        m_RayTracing = host.device.createPipeline(desc, host.compiler.handle());
        host.device.finishPipelineBuilds();
        BakeShaderTable(host);

        m_Present = host.Graphics("present.slang", "vsFullscreen", "psPresent");
    }

    // The shader binding table: one record per group (its handle, no record data), each on its own base-aligned slot (ShaderBindingTable.h).
    void BakeShaderTable(nox::Host& host)
    {
        const NriPipeline pipeline = m_RayTracing.handle();
        const uint32_t handleSize = nriGetPipelineShaderGroupHandleSize(pipeline);
        const uint32_t baseAlignment = nriGetPipelineShaderGroupBaseAlignment(pipeline);
        uint32_t missCount = 0, hitCount = 0, handlesBytes = 0;
        const uint32_t* missIndices = nriGetPipelineMissGroupIndices(pipeline, &missCount);
        const uint32_t* hitIndices = nriGetPipelineHitGroupIndices(pipeline, &hitCount);
        const uint8_t* handles = nriGetPipelineShaderGroupHandles(pipeline, &handlesBytes);
        if (handleSize == 0 || baseAlignment == 0 || !handles || missCount != 2 || hitCount != 2)
            throw std::runtime_error("RayTracingPipeline: the pipeline reports no shader groups");
        const uint64_t size = nriCalculateShaderTableSize(1, missCount, hitCount, 0, baseAlignment);
        m_ShaderTable = host.device.createBuffer({ .size = size, .usage = NRI_BUFFER_USAGE_SHADER_BINDING_TABLE });
        auto* mapped = static_cast<uint8_t*>(m_ShaderTable.map(0, size));
        nriBakeShaderTable(mapped, m_ShaderTable.getDeviceAddress(), handles, handleSize, baseAlignment, int32_t(nriGetPipelineRayGenGroupIndex(pipeline)),
                           missIndices, missCount, hitIndices, hitCount, nullptr, 0, &m_Table);
        m_ShaderTable.unmap();
    }

    // The scene at `time`: the objects (into objectData) and the TLAS instances (written to instanceBuffer).
    uint32_t WriteScene(float time, Objects& objectData, nri::Buffer& instanceBuffer)
    {
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
            const float t = time * (0.25f + 0.04f * i) + i * 0.39f, radius = 1.8f + 0.28f * i;
            const bool cutout = m_Cutouts && i % 3 == 0;
            const float s = cutout ? 0.6f : 0.35f; // (the lattice boxes larger: their holes visible)
            add({ std::cos(t) * radius, 0.9f + 0.7f * std::sin(t * 1.7f + i), std::sin(t) * radius }, { s, s, s }, t * 1.3f, t * 0.7f,
                { 0.5f + 0.5f * std::cos(i * 0.8f), 0.5f + 0.5f * std::cos(i * 0.8f + 2.1f), 0.5f + 0.5f * std::cos(i * 0.8f + 4.2f), cutout ? 2.0f : 0.0f });
        }
        std::vector<NriAccelerationStructureInstance> instances(objects.size());
        for (size_t i = 0; i < objects.size(); ++i)
        {
            objectData.items[i] = objects[i];
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c)
                    instances[i].transform.matrix[r][c] = objects[i].model.At(r, c); // (3 x 4, row-major: both APIs)
            const bool cutout = objects[i].color.w > 1.5f;
            instances[i].instanceCustomIndex = uint32_t(i);
            instances[i].mask = 0xFF;
            instances[i].instanceShaderBindingTableRecordOffset = cutout ? 1u : 0u; // hit group 1: closest hit + the any hit
            instances[i].flags = cutout ? 0u : kInstanceForceOpaque;
            instances[i].accelerationStructureReference = m_Blas.getDeviceAddress();
        }
        std::memcpy(instanceBuffer.map(0, instances.size() * sizeof(instances[0])), instances.data(), instances.size() * sizeof(instances[0]));
        instanceBuffer.unmap();
        return uint32_t(objects.size());
    }

    void Render(nox::Host& host, nri::RenderGraph& graph, const nox::FrameInfo& frame) override
    {
        if (!m_Paused)
            m_AnimTime += frame.deltaTime;

        Objects objectData{};
        const uint32_t sceneObjects = WriteScene(m_AnimTime, objectData, m_InstanceBuffers[frame.frameSlot]);

        const float orbit = m_AnimTime * 0.07f;
        const nox::Float3 eye{ std::sin(orbit) * 13.0f, 6.5f, std::cos(orbit) * 13.0f };
        const nox::Float3 forward = nox::Normalize(nox::Float3{ 0 - eye.x, 0.8f - eye.y, 0 - eye.z });
        const nox::Float3 right = nox::Normalize(nox::Cross(forward, { 0, 1, 0 }));
        const nox::Float3 up = nox::Cross(right, forward);
        const float tanHalf = std::tan(0.85f * 0.5f), aspect = frame.Aspect();
        Frame frameData{};
        frameData.cameraPosition = { eye.x, eye.y, eye.z, 1 };
        frameData.cameraForward = { forward.x, forward.y, forward.z, 0 };
        frameData.cameraRight = { right.x * tanHalf * aspect, right.y * tanHalf * aspect, right.z * tanHalf * aspect, 0 };
        frameData.cameraUp = { up.x * tanHalf, up.y * tanHalf, up.z * tanHalf, 0 };
        const nox::Float3 sun = nox::Normalize({ std::cos(m_AnimTime * 0.2f) * 0.8f, 1.0f, std::sin(m_AnimTime * 0.2f) * 0.8f });
        frameData.sunDirection = { sun.x, sun.y, sun.z, 0 };
        frameData.objectCount = sceneObjects;
        frameData.frameIndex = uint32_t(frame.frameNumber);
        frameData.shadowsOn = m_Shadows ? 1 : 0;

        const uint64_t frameRef = m_Frame.Write(frame.frameSlot, frameData), objectsRef = m_Objects.Write(frame.frameSlot, objectData);
        const uint32_t objectCount = sceneObjects, slot = frame.frameSlot;
        const nri::Extent2D extent = frame.extent;

        char status[96];
        std::snprintf(status, sizeof(status), "shadows %s, cut-outs %s (S / C)", m_Shadows ? "on" : "off", m_Cutouts ? "on" : "off");
        host.SetStatus(status);

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

        // Every pixel ray traced into a storage texture (RGPassFlags::RayTracing: its barriers name the ray tracing stage).
        const nri::RGTexture color = graph.CreateTexture("Color", { .Format = NRI_FORMAT_R16G16B16A16_SFLOAT, .Usage = NRI_TEXTURE_USAGE_STORAGE });
        graph.AddPass("RayTrace", nri::RGPassFlags::RayTracing,
            [&](nri::RGBuilder& b) {
                b.Write(color);
            },
            [this, color, frameRef, objectsRef, slot, extent](nri::RGPassContext& ctx) {
                nri::CommandBuffer& cmd = ctx.Cmd();
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_RAY_TRACING, m_RayTracing);
                const Push push{ frameRef, objectsRef, m_VerticesRef, m_TlasRef[slot], ctx.StorageSlot(color), extent.width, extent.height, 0 };
                cmd.pushData(&push, sizeof(push));
                nriCmdTraceRays(cmd.handle(), &m_Table.rayGen, &m_Table.miss, &m_Table.hitGroups, &m_Table.callable, extent.width, extent.height, 1);
            });

        // The image checks itself (every frame; printed with --probe): the ray generation writes alpha 1 to every pixel, so anything else
        // in the centre means it did not run and the texture holds what its memory held -- how a broken ray tracing pipeline shows (the
        // Nox engine's path tracer read its push data from the shader record and wrote nothing). The scene is never black there either.
        host.Probe(graph, color, "Color", 8, frame, [this](const nox::ProbeResult& result) {
            const bool written = result.min[3] > 0.999f && result.max[3] < 1.001f;
            const bool lit = result.mean[0] + result.mean[1] + result.mean[2] > 0.05f;
            if ((written && lit) || m_ImageFailed)
                return;
            m_ImageFailed = true;
            std::fprintf(stderr, "[RayTracingPipeline][ERROR] the ray traced image is %s: centre mean (%.3f %.3f %.3f), alpha %.3f .. %.3f\n",
                         written ? "black" : "not written by the ray generation shader", result.mean[0], result.mean[1], result.mean[2],
                         result.min[3], result.max[3]);
            ++nox::g_Errors;
        });

        graph.AddPass("Present", nri::RGPassFlags::Raster,
            [&](nri::RGBuilder& b) {
                b.Read(color);
                b.SwapchainTarget(host.swapchain, frame.imageIndex, NRI_LOAD_OP_DONT_CARE, NRI_STORE_OP_STORE);
                b.SetRenderArea(frame.extent);
            },
            [this, &host, color](nri::RGPassContext& ctx) {
                nri::CommandBuffer& cmd = ctx.Cmd();
                nox::Host::SetRasterState(cmd, {});
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, m_Present);
                const PresentPush push{ host.ImageSlot(ctx.Slot(color)) };
                cmd.pushData(&push, sizeof(push));
                cmd.draw(3);
            });
    }

    int OnArgument(int, char** argv, int index) override
    {
        if (!std::strcmp(argv[index], "--no-shadows"))
        {
            m_Shadows = false;
            return 1;
        }
        if (!std::strcmp(argv[index], "--no-cutouts"))
        {
            m_Cutouts = false;
            return 1;
        }
        return 0;
    }

    void OnKey(SDL_Keycode key) override
    {
        if (key == SDLK_S)
            m_Shadows = !m_Shadows;
        else if (key == SDLK_C)
            m_Cutouts = !m_Cutouts;
        else if (key == SDLK_SPACE)
            m_Paused = !m_Paused;
    }

    void Shutdown(nox::Host&) override
    {
        m_RayTracing.reset();
        m_Present.reset();
        m_ShaderTable.reset();
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
    nri::Buffer m_Vertices, m_Indices, m_BlasStorage, m_BlasScratch, m_ShaderTable;
    nri::AccelerationStructure m_Blas;
    nri::AccelerationStructure m_Tlas[nox::kFramesInFlight];
    nri::Buffer m_TlasStorage[nox::kFramesInFlight], m_TlasScratch[nox::kFramesInFlight], m_InstanceBuffers[nox::kFramesInFlight];
    uint64_t m_TlasRef[nox::kFramesInFlight]{};
    uint64_t m_VerticesRef = 0;
    nox::FrameConstants<Frame> m_Frame;
    nox::FrameConstants<Objects> m_Objects;
    nri::Pipeline m_RayTracing, m_Present;
    NriShaderTableState m_Table{};
    bool m_Shadows = true, m_Cutouts = true, m_Paused = false, m_ImageFailed = false;
    float m_AnimTime = 0.0f;
};

NOX_EXAMPLE_MAIN(RayTracingPipeline)
