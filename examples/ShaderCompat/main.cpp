// ShaderCompat: one Slang source (shader_compat.slang) run on Vulkan and on D3D12, each test checked against what the CPU computes, and
// -- with --all -- the two APIs checked against each other. Headless: no window, no swapchain.
//
//   ShaderCompat --vk            Vulkan only
//   ShaderCompat --dx12          D3D12 only
//   ShaderCompat --all           every API this build has, then a cross-API comparison
//   ShaderCompat ... --only csWave --verbose
//
// Exit code 0 when nothing failed (a test the device cannot run is SKIP, not a failure).

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <NRIHPP/NRI.hpp>
#include <NRIHPP/NRIExtensions.hpp>
#include <NRI/Extensions/NRIMeshShader.h>

#include <csignal>
#include <exception>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include <NRI/Extensions/NRIRayTracing.h>

namespace
{
    int g_ValidationErrors = 0;
    bool g_Verbose = false;
    bool g_Finished = false;
    const char* g_Stage = "starting"; // what the program was doing, printed if it dies (a crash, terminate, or a library calling exit)
    std::string g_StageText;

    void SetStage(std::string text)
    {
        g_StageText = std::move(text);
        g_Stage = g_StageText.c_str();
        if (g_Verbose)
            std::fprintf(stderr, "[ShaderCompat] %s\n", g_Stage);
    }

    void InstallDeathReports()
    {
        std::atexit([] {
            if (!g_Finished)
                std::fprintf(stderr, "[ShaderCompat] the process exited early (a library called exit) while: %s\n", g_Stage);
        });
        std::set_terminate([] {
            std::fprintf(stderr, "[ShaderCompat] std::terminate (an exception nobody caught) while: %s\n", g_Stage);
            std::abort();
        });
        std::signal(SIGABRT, [](int) { std::fprintf(stderr, "[ShaderCompat] abort() while: %s\n", g_Stage); });
#ifdef _WIN32
        SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* info) -> LONG {
            std::fprintf(stderr, "[ShaderCompat] crashed (exception 0x%08lX) while: %s\n", info->ExceptionRecord->ExceptionCode, g_Stage);
            return EXCEPTION_CONTINUE_SEARCH;
        });
#endif
    }

    void NRI_CALL OnNriLog(NriLogLevel level, const char* message, void*)
    {
        if (level == NRI_LOG_ERROR)
        {
            ++g_ValidationErrors;
            std::fprintf(stderr, "[ShaderCompat][ERROR] %s\n", message);
        }
        else if (g_Verbose)
            std::fprintf(stderr, "[ShaderCompat][%s] %s\n", level == NRI_LOG_WARN ? "WARN" : "INFO", message);
    }

    // Must match `Push` in shader_compat.slang byte for byte (Vulkan push data, D3D12 root constants: at most 128 bytes).
    struct Push
    {
        uint64_t outAddress = 0;
        uint64_t inAddress = 0;
        uint32_t outSlot = 0;
        uint32_t inSlot = 0;
        uint32_t texSlot = 0;
        uint32_t storageTexSlot = 0;
        uint32_t pointSampler = 0;
        uint32_t linearSampler = 1;
        uint32_t count = 0;
        uint32_t tlasSlot = 0;
        float params[4]{};
        uint32_t uparams[4]{};
        uint64_t tlasAddress = 0;
    };
    static_assert(offsetof(Push, outSlot) == 16 && offsetof(Push, count) == 40 && offsetof(Push, params) == 48 && offsetof(Push, uparams) == 64 &&
                  offsetof(Push, tlasAddress) == 80 && sizeof(Push) == 88, "Push must match shader_compat.slang");

    constexpr uint64_t kBufferBytes = 64 * 1024;
    constexpr uint32_t kStorageSize = 64;  // 64 x 64 RGBA8: 256-byte rows, the copy pitch both APIs use
    constexpr uint32_t kTexSize = 4;

    enum class Status { Pass, Fail, Skip };
    const char* StatusText(Status s) { return s == Status::Pass ? "PASS" : s == Status::Fail ? "FAIL" : "SKIP"; }

    struct Result
    {
        Status status = Status::Skip;
        std::string detail;
        std::vector<uint32_t> raw; // what the GPU wrote (for the cross-API comparison)
    };

    // --- CPU references --------------------------------------------------------------------------------------------------------------

    float AsFloat(uint32_t u) { return std::bit_cast<float>(u); }
    uint32_t AsUint(float f) { return std::bit_cast<uint32_t>(f); }

    uint16_t FloatToHalf(float value)
    {
        // Round to nearest even, with subnormals, infinities and NaN.
        const uint32_t f = AsUint(value);
        const uint32_t sign = (f >> 16) & 0x8000u;
        int32_t exponent = static_cast<int32_t>((f >> 23) & 0xFF) - 127 + 15;
        uint32_t mantissa = f & 0x7FFFFFu;
        if (((f >> 23) & 0xFF) == 0xFF)
            return static_cast<uint16_t>(sign | 0x7C00u | (mantissa ? 0x200u : 0u));
        if (exponent >= 31)
            return static_cast<uint16_t>(sign | 0x7C00u);
        if (exponent <= 0)
        {
            if (exponent < -10)
                return static_cast<uint16_t>(sign);
            mantissa |= 0x800000u;
            const uint32_t shift = static_cast<uint32_t>(14 - exponent);
            uint32_t half = mantissa >> shift;
            const uint32_t rest = mantissa & ((1u << shift) - 1u), halfway = 1u << (shift - 1);
            if (rest > halfway || (rest == halfway && (half & 1u)))
                ++half;
            return static_cast<uint16_t>(sign | half);
        }
        uint32_t half = (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13);
        const uint32_t rest = mantissa & 0x1FFFu;
        if (rest > 0x1000u || (rest == 0x1000u && (half & 1u)))
            ++half;
        return static_cast<uint16_t>(sign | half);
    }

    float HalfToFloat(uint16_t h)
    {
        const uint32_t sign = (h & 0x8000u) << 16;
        const uint32_t exponent = (h >> 10) & 0x1Fu, mantissa = h & 0x3FFu;
        if (exponent == 0)
            return std::ldexp(static_cast<float>(mantissa), -24) * (sign ? -1.0f : 1.0f);
        if (exponent == 31)
            return AsFloat(sign | 0x7F800000u | (mantissa << 13));
        return AsFloat(sign | ((exponent + 112) << 23) | (mantissa << 13));
    }

    float RoundHalf(float f) { return HalfToFloat(FloatToHalf(f)); }

    // Round toward zero (for normal and subnormal halves; overflow gives the largest finite half).
    float RoundHalfTowardZero(float value)
    {
        const uint32_t f = AsUint(value);
        const uint32_t sign = (f >> 16) & 0x8000u;
        const int32_t exponent = static_cast<int32_t>((f >> 23) & 0xFF) - 127 + 15;
        const uint32_t mantissa = f & 0x7FFFFFu;
        if (((f >> 23) & 0xFF) == 0xFF)
            return value;
        if (exponent >= 31)
            return HalfToFloat(static_cast<uint16_t>(sign | 0x7BFFu));
        if (exponent <= 0)
            return exponent < -10 ? HalfToFloat(static_cast<uint16_t>(sign)) : HalfToFloat(static_cast<uint16_t>(sign | ((mantissa | 0x800000u) >> (14 - exponent))));
        return HalfToFloat(static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13)));
    }

    uint32_t FirstBitHigh(uint32_t u) { return u == 0 ? 0xFFFFFFFFu : 31u - static_cast<uint32_t>(std::countl_zero(u)); }
    uint32_t FirstBitLow(uint32_t u) { return u == 0 ? 0xFFFFFFFFu : static_cast<uint32_t>(std::countr_zero(u)); }
    uint32_t ReverseBits(uint32_t u)
    {
        uint32_t r = 0;
        for (int i = 0; i < 32; ++i)
            r |= ((u >> i) & 1u) << (31 - i);
        return r;
    }

    bool Near(float got, float want, float absTol, float relTol)
    {
        if (std::isnan(want))
            return std::isnan(got);
        return std::fabs(got - want) <= absTol + relTol * std::fabs(want);
    }

    std::string Hex(uint32_t v)
    {
        char t[16];
        std::snprintf(t, sizeof(t), "0x%08X", v);
        return t;
    }

    // --- One API's device and the resources every test shares --------------------------------------------------------------------

    class Suite
    {
    public:
        Suite(NriGraphicsAPI api, std::string shaderPath) : m_Api(api), m_ShaderPath(std::move(shaderPath))
        {
            nri::DeviceDesc desc{};
            desc.api = api;
            desc.window.type = NRI_WINDOW_SYSTEM_HEADLESS;
            desc.shaderCacheDir = api == NRI_GRAPHICS_API_D3D12 ? "shader_cache_compat_dx12" : "shader_cache_compat_vk";
            desc.logCallback = OnNriLog;
            m_Device = nri::createDevice(desc);

            m_Resources = m_Device.createDescriptorHeap({ .type = NRI_DESCRIPTOR_HEAP_RESOURCE, .maxBufferDescriptors = 64, .maxImageDescriptors = 64 });
            nri::SamplerDesc point{};
            point.magFilter = point.minFilter = NRI_FILTER_NEAREST;
            point.mipmapMode = NRI_SAMPLER_MIPMAP_NEAREST;
            point.addressModeU = point.addressModeV = point.addressModeW = NRI_SAMPLER_ADDRESS_CLAMP_TO_EDGE;
            point.maxLod = 1000.0f;
            nri::SamplerDesc linear = point;
            linear.magFilter = linear.minFilter = NRI_FILTER_LINEAR;
            linear.mipmapMode = NRI_SAMPLER_MIPMAP_LINEAR;
            m_Samplers = m_Device.createDescriptorHeap({ .type = NRI_DESCRIPTOR_HEAP_SAMPLER, .maxSamplerDescriptors = 8, .samplers = { point, linear } });

            if (api != NRI_GRAPHICS_API_NONE)
            {
                m_Compiler = nri::createSlangCompiler();
                m_Compiler.setTargetAPI(api);
            }

            // Storage-static: a storage buffer that is also a copy source on Vulkan (the readback copies `out`).
            m_In = m_Device.createBuffer({ .size = kBufferBytes, .usage = NRI_BUFFER_USAGE_STORAGE_STATIC });
            m_Out = m_Device.createBuffer({ .size = kBufferBytes, .usage = NRI_BUFFER_USAGE_STORAGE_STATIC });
            m_Upload = m_Device.createBuffer({ .size = kBufferBytes, .usage = NRI_BUFFER_USAGE_STAGING });
            m_Zeros = m_Device.createBuffer({ .size = kBufferBytes, .usage = NRI_BUFFER_USAGE_STAGING });
            m_Readback = m_Device.createBuffer({ .size = kBufferBytes, .usage = NRI_BUFFER_USAGE_READBACK });
            std::memset(m_Zeros.map(0, kBufferBytes), 0, kBufferBytes);
            m_Zeros.unmap();

            m_Allocator = m_Device.createCommandAllocator(NRI_COMMAND_BUFFER_RESET_PER_COMMAND_BUFFER, NRI_QUEUE_GRAPHICS);
            m_Cmd = m_Allocator.allocateCommandBuffer(1);

            // Vulkan reaches buffers by address, D3D12 by descriptor: both are set, each API's shader uses its own.
            m_BasePush.inAddress = m_In.getDeviceAddress();
            m_BasePush.outAddress = m_Out.getDeviceAddress();
            m_BasePush.inSlot = m_Resources.registerBuffer(m_In, kBufferBytes);
            m_BasePush.outSlot = m_Resources.registerStorageBuffer(m_Out, kBufferBytes);
            m_BasePush.pointSampler = 0;
            m_BasePush.linearSampler = 1;

            CreateTextures();
        }

        ~Suite()
        {
            if (m_Device)
                m_Device.waitIdle();
        }

        NriGraphicsAPI Api() const { return m_Api; }
        bool SupportsMeshShaders() const { return m_Device.supportsMeshShaders(); }
        const Push& BasePush() const { return m_BasePush; }

        // The pipeline of one entry point; empty with `error` set when it cannot be made (not compiled, or the device lacks a feature).
        nri::Pipeline* GetPipeline(const std::string& entry, std::string& error)
        {
            if (auto found = m_Pipelines.find(entry); found != m_Pipelines.end())
                return &found->second;
            try
            {
                nri::Pipeline pipeline = m_Device.createPipeline({
                    .type = NRI_PIPELINE_COMPUTE,
                    .shaders = { { NRI_SHADER_STAGE_COMPUTE, entry, m_ShaderPath } },
                    .buildNow = true
                }, m_Compiler.handle());
                m_Device.finishPipelineBuilds();
                return &m_Pipelines.emplace(entry, std::move(pipeline)).first->second;
            }
            catch (const std::exception& e)
            {
                error = e.what();
                return nullptr;
            }
        }

        // Uploads `input` to `in`, zeroes the first `outWords` of `out`, runs the entry point and returns what it wrote. When
        // `storageImage` is given, the storage texture is written by the shader and copied back into it (64 x 64 RGBA8).
        std::vector<uint32_t> Run(nri::Pipeline& pipeline, const std::vector<uint32_t>& input, uint32_t outWords, uint32_t groupsX, uint32_t groupsY,
                                  const Push& push, std::vector<uint32_t>* storageImage = nullptr)
        {
            const uint64_t inBytes = std::max<uint64_t>(4, input.size() * 4), outBytes = std::max<uint64_t>(4, outWords * 4ull);
            if (!input.empty())
            {
                std::memcpy(m_Upload.map(0, inBytes), input.data(), input.size() * 4);
                m_Upload.unmap();
            }

            nri::CommandBuffer& cmd = m_Cmd;
            cmd.begin(0, true);
            cmd.bindDescriptorHeaps(&m_Resources, &m_Samplers);
            if (m_PreDispatch)
                m_PreDispatch(cmd);
            if (!input.empty())
                cmd.copyBuffer(m_Upload, m_In, nri::BufferCopyRegion{ 0, 0, inBytes });
            cmd.copyBuffer(m_Zeros, m_Out, nri::BufferCopyRegion{ 0, 0, outBytes });

            const nri::ResourceState transferWrite{ NRI_ACCESS_TRANSFER_WRITE, NRI_STAGE_TRANSFER };
            const nri::ResourceState computeRead{ NRI_ACCESS_SHADER_READ, NRI_STAGE_COMPUTE };
            const nri::ResourceState computeWrite{ NRI_ACCESS_SHADER_READ | NRI_ACCESS_SHADER_WRITE, NRI_STAGE_COMPUTE };
            const nri::ResourceState transferRead{ NRI_ACCESS_TRANSFER_READ, NRI_STAGE_TRANSFER };
            std::vector<nri::TextureBarrierDesc> textureBarriers;
            if (storageImage)
                textureBarriers.push_back({ m_StorageTexture.handle(), transferRead, computeWrite });
            const nri::BufferBarrierDesc before[2] = { { m_In.handle(), transferWrite, computeRead }, { m_Out.handle(), transferWrite, computeWrite } };
            cmd.resourceBarriers(textureBarriers, before);

            cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            cmd.pushData(&push, sizeof(push));
            cmd.dispatch(groupsX, groupsY, 1);

            textureBarriers.clear();
            if (storageImage)
                textureBarriers.push_back({ m_StorageTexture.handle(), computeWrite, transferRead });
            const nri::BufferBarrierDesc after[1] = { { m_Out.handle(), computeWrite, transferRead } };
            cmd.resourceBarriers(textureBarriers, after);
            cmd.copyBuffer(m_Out, m_Readback, nri::BufferCopyRegion{ 0, 0, outBytes });
            if (storageImage)
                m_StorageTexture.copyToBuffer(cmd.handle(), m_StorageReadback.handle(), 0, 0, kStorageSize, kStorageSize);
            cmd.end(0);
            m_Device.submitAndWait(cmd, 0);

            std::vector<uint32_t> out(outWords);
            std::memcpy(out.data(), m_Readback.map(0, outBytes), outWords * 4ull);
            m_Readback.unmap();
            if (storageImage)
            {
                storageImage->resize(kStorageSize * kStorageSize);
                std::memcpy(storageImage->data(), m_StorageReadback.map(0, storageImage->size() * 4), storageImage->size() * 4);
                m_StorageReadback.unmap();
            }
            return out;
        }

        // The scene of RayQueryShadows in miniature: a BLAS of a cube (-1..1) from 32-byte vertices (position + normal), and a TLAS
        // (PREFER_FAST_BUILD) of three translated and scaled instances -- `instances` holds each one's translation and scale.
        bool BuildInstancedScene(Push& push, const float (*instances)[6], uint32_t instanceCount, std::string& error)
        {
            try
            {
                std::vector<float> vertices; // 8 floats per vertex
                const float faces[6][3][3] = {
                    { { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 } }, { { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },
                    { { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } }, { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },
                    { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } },  { { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 } } };
                const float corners[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
                for (const auto& f : faces)
                    for (const auto& c : corners)
                    {
                        for (int i = 0; i < 3; ++i)
                            vertices.push_back(f[0][i] + f[1][i] * c[0] + f[2][i] * c[1]);
                        vertices.push_back(1.0f);
                        for (int i = 0; i < 3; ++i)
                            vertices.push_back(f[0][i]);
                        vertices.push_back(0.0f);
                    }
                std::vector<uint32_t> indices(36);
                for (uint32_t i = 0; i < 36; ++i)
                    indices[i] = i;
                const uint64_t vertexBytes = vertices.size() * 4, indexBytes = indices.size() * 4;
                m_CubeVertices = m_Device.createBuffer({ .size = vertexBytes, .usage = NRI_BUFFER_USAGE_STORAGE_STATIC });
                m_CubeIndices = m_Device.createBuffer({ .size = indexBytes, .usage = NRI_BUFFER_USAGE_STORAGE_STATIC });
                auto* staging = static_cast<uint8_t*>(m_Upload.map(0, 4096));
                std::memcpy(staging, vertices.data(), vertexBytes);
                std::memcpy(staging + 2048, indices.data(), indexBytes);
                m_Upload.unmap();

                NriAccelerationStructureTrianglesDesc triangles{};
                triangles.vertexBufferAddress = m_CubeVertices.getDeviceAddress();
                triangles.vertexStride = 32;
                triangles.maxVertex = 35;
                triangles.indexBufferAddress = m_CubeIndices.getDeviceAddress();
                triangles.primitiveCount = 12;
                triangles.isOpaque = true;
                NriAccelerationStructureBuildDesc blasBuild{};
                blasBuild.type = NRI_ACCELERATION_STRUCTURE_BOTTOM_LEVEL;
                blasBuild.flags = NRI_AS_BUILD_PREFER_FAST_TRACE;
                blasBuild.triangles = &triangles;
                blasBuild.triangleCount = 1;
                const NriAccelerationStructureBuildSizes blasSizes = nriGetAccelerationStructureBuildSizes(m_Device.handle(), &blasBuild);
                if (blasSizes.accelerationStructureSize == 0)
                    throw std::runtime_error("no acceleration structure sizes (ray tracing unsupported)");
                const auto create = [&](NriAccelerationStructureType type, uint64_t size, nri::Buffer& storage) {
                    storage = m_Device.createBuffer({ .size = size, .usage = NRI_BUFFER_USAGE_ACCELERATION_STRUCTURE });
                    const NriAccelerationStructureDesc desc{ type, storage.handle(), 0, size };
                    NriAccelerationStructure handle = nullptr;
                    if (nriCreateAccelerationStructure(m_Device.handle(), &desc, &handle) != NRI_SUCCESS || !handle)
                        throw std::runtime_error("nriCreateAccelerationStructure failed");
                    return nri::AccelerationStructure(handle);
                };
                m_CubeBlas = create(NRI_ACCELERATION_STRUCTURE_BOTTOM_LEVEL, blasSizes.accelerationStructureSize, m_CubeBlasStorage);

                std::vector<NriAccelerationStructureInstance> records(instanceCount);
                for (uint32_t i = 0; i < instanceCount; ++i)
                {
                    NriAccelerationStructureInstance& r = records[i];
                    r = {};
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        r.transform.matrix[axis][axis] = instances[i][3 + axis]; // scale
                        r.transform.matrix[axis][3] = instances[i][axis];        // translation
                    }
                    r.instanceCustomIndex = i;
                    r.mask = 0xFF;
                    r.accelerationStructureReference = m_CubeBlas.getDeviceAddress();
                }
                m_CubeInstances = m_Device.createBuffer({ .size = records.size() * sizeof(records[0]), .usage = NRI_BUFFER_USAGE_ACCELERATION_STRUCTURE_INSTANCE });
                std::memcpy(m_CubeInstances.map(0, records.size() * sizeof(records[0])), records.data(), records.size() * sizeof(records[0]));
                m_CubeInstances.unmap();

                NriAccelerationStructureBuildDesc tlasBuild{};
                tlasBuild.type = NRI_ACCELERATION_STRUCTURE_TOP_LEVEL;
                tlasBuild.flags = NRI_AS_BUILD_PREFER_FAST_BUILD;
                tlasBuild.instances = { m_CubeInstances.getDeviceAddress(), instanceCount };
                const NriAccelerationStructureBuildSizes tlasSizes = nriGetAccelerationStructureBuildSizes(m_Device.handle(), &tlasBuild);
                m_CubeTlas = create(NRI_ACCELERATION_STRUCTURE_TOP_LEVEL, tlasSizes.accelerationStructureSize, m_CubeTlasStorage);
                m_CubeBlasScratch = m_Device.createBuffer({ .size = blasSizes.buildScratchSize, .usage = NRI_BUFFER_USAGE_ACCELERATION_STRUCTURE_SCRATCH });
                m_CubeTlasScratch = m_Device.createBuffer({ .size = tlasSizes.buildScratchSize, .usage = NRI_BUFFER_USAGE_ACCELERATION_STRUCTURE_SCRATCH });

                nri::CommandBuffer& cmd = m_Cmd;
                cmd.begin(0, true);
                cmd.copyBuffer(m_Upload, m_CubeVertices, nri::BufferCopyRegion{ 0, 0, vertexBytes });
                cmd.copyBuffer(m_Upload, m_CubeIndices, nri::BufferCopyRegion{ 2048, 0, indexBytes });
                const nri::ResourceState copied{ NRI_ACCESS_TRANSFER_WRITE, NRI_STAGE_TRANSFER };
                const nri::ResourceState buildInput{ NRI_ACCESS_ACCELERATION_STRUCTURE_READ, NRI_STAGE_ACCELERATION_STRUCTURE_BUILD };
                const nri::ResourceState scratch{ NRI_ACCESS_ACCELERATION_STRUCTURE_READ | NRI_ACCESS_ACCELERATION_STRUCTURE_WRITE, NRI_STAGE_ACCELERATION_STRUCTURE_BUILD };
                const nri::BufferBarrierDesc barriers[4] = { { m_CubeVertices.handle(), copied, buildInput }, { m_CubeIndices.handle(), copied, buildInput },
                                                             { m_CubeBlasScratch.handle(), {}, scratch }, { m_CubeTlasScratch.handle(), {}, scratch } };
                cmd.resourceBarriers({}, barriers);
                nriCmdBuildAccelerationStructure(cmd.handle(), &blasBuild, m_CubeBlasScratch.getDeviceAddress(), m_CubeBlas.handle());
                cmd.end(0);
                m_Device.submitAndWait(cmd, 0);

                // (the TLAS in a later submission, as RayQueryShadows builds it every frame)
                cmd.begin(0, true);
                nriCmdBuildAccelerationStructure(cmd.handle(), &tlasBuild, m_CubeTlasScratch.getDeviceAddress(), m_CubeTlas.handle());
                nriCmdAccelerationStructureBarrier(cmd.handle(), NRI_AS_BARRIER_BUILD_TO_SHADER_READ);
                cmd.end(0);
                m_Device.submitAndWait(cmd, 0);

                push.tlasAddress = m_CubeTlas.getDeviceAddress();
                push.tlasSlot = nriRegisterAccelerationStructure(m_Resources.handle(), m_CubeTlas.handle(), ~0u);
                m_CubeInstanceCount = instanceCount;
                return true;
            }
            catch (const std::exception& e)
            {
                error = e.what();
                return false;
            }
        }

        // Records a rebuild of the instanced scene's TLAS (BuildInstancedScene first) and the barrier before shaders read it.
        void RecordCubeTlasRebuild(nri::CommandBuffer& cmd)
        {
            NriAccelerationStructureBuildDesc tlasBuild{};
            tlasBuild.type = NRI_ACCELERATION_STRUCTURE_TOP_LEVEL;
            tlasBuild.flags = NRI_AS_BUILD_PREFER_FAST_BUILD;
            tlasBuild.instances = { m_CubeInstances.getDeviceAddress(), m_CubeInstanceCount };
            nriCmdBuildAccelerationStructure(cmd.handle(), &tlasBuild, m_CubeTlasScratch.getDeviceAddress(), m_CubeTlas.handle());
            nriCmdAccelerationStructureBarrier(cmd.handle(), NRI_AS_BARRIER_BUILD_TO_SHADER_READ);
        }

        // Called right after the descriptor heaps are bound in Run (a test's extra commands before the dispatch); null: none.
        std::function<void(nri::CommandBuffer&)> m_PreDispatch;

        // Builds a BLAS of one triangle and a TLAS of one instance; false (with `error`) when the device cannot.
        bool BuildRayTracingScene(Push& push, std::string& error)
        {
            try
            {
                const float vertices[9] = { 0, 0, 5, 1, 0, 5, 0, 1, 5 };
                const uint32_t indices[3] = { 0, 1, 2 };
                m_Vertices = m_Device.createBuffer({ .size = sizeof(vertices), .usage = NRI_BUFFER_USAGE_STORAGE_STATIC });
                m_Indices = m_Device.createBuffer({ .size = 256, .usage = NRI_BUFFER_USAGE_STORAGE_STATIC });
                auto* staging = static_cast<uint8_t*>(m_Upload.map(0, 512));
                std::memcpy(staging, vertices, sizeof(vertices));
                std::memcpy(staging + 256, indices, sizeof(indices));
                m_Upload.unmap();

                NriAccelerationStructureTrianglesDesc triangle{};
                triangle.vertexBufferAddress = m_Vertices.getDeviceAddress();
                triangle.vertexStride = 12;
                triangle.maxVertex = 2; // (the highest index, as in Vulkan)
                triangle.indexBufferAddress = m_Indices.getDeviceAddress();
                triangle.primitiveCount = 1;
                triangle.isOpaque = true;
                NriAccelerationStructureBuildDesc blasBuild{};
                blasBuild.type = NRI_ACCELERATION_STRUCTURE_BOTTOM_LEVEL;
                blasBuild.flags = NRI_AS_BUILD_PREFER_FAST_TRACE;
                blasBuild.triangles = &triangle;
                blasBuild.triangleCount = 1;
                const NriAccelerationStructureBuildSizes blasSizes = nriGetAccelerationStructureBuildSizes(m_Device.handle(), &blasBuild);
                if (blasSizes.accelerationStructureSize == 0)
                    throw std::runtime_error("no acceleration structure sizes (ray tracing unsupported)");
                m_BlasStorage = m_Device.createBuffer({ .size = blasSizes.accelerationStructureSize, .usage = NRI_BUFFER_USAGE_ACCELERATION_STRUCTURE });
                NriAccelerationStructureDesc blasDesc{ NRI_ACCELERATION_STRUCTURE_BOTTOM_LEVEL, m_BlasStorage.handle(), 0, blasSizes.accelerationStructureSize };
                NriAccelerationStructure blas = nullptr;
                if (nriCreateAccelerationStructure(m_Device.handle(), &blasDesc, &blas) != NRI_SUCCESS || !blas)
                    throw std::runtime_error("nriCreateAccelerationStructure (BLAS) failed");
                m_Blas = nri::AccelerationStructure(blas);

                m_Instances = m_Device.createBuffer({ .size = sizeof(NriAccelerationStructureInstance), .usage = NRI_BUFFER_USAGE_ACCELERATION_STRUCTURE_INSTANCE });
                NriAccelerationStructureInstance instance{};
                instance.transform.matrix[0][0] = instance.transform.matrix[1][1] = instance.transform.matrix[2][2] = 1.0f;
                instance.mask = 0xFF;
                instance.accelerationStructureReference = m_Blas.getDeviceAddress();
                std::memcpy(m_Instances.map(0, sizeof(instance)), &instance, sizeof(instance));
                m_Instances.unmap();

                NriAccelerationStructureBuildDesc tlasBuild{};
                tlasBuild.type = NRI_ACCELERATION_STRUCTURE_TOP_LEVEL;
                tlasBuild.flags = NRI_AS_BUILD_PREFER_FAST_TRACE;
                tlasBuild.instances = { m_Instances.getDeviceAddress(), 1 };
                const NriAccelerationStructureBuildSizes tlasSizes = nriGetAccelerationStructureBuildSizes(m_Device.handle(), &tlasBuild);
                m_TlasStorage = m_Device.createBuffer({ .size = tlasSizes.accelerationStructureSize, .usage = NRI_BUFFER_USAGE_ACCELERATION_STRUCTURE });
                NriAccelerationStructureDesc tlasDesc{ NRI_ACCELERATION_STRUCTURE_TOP_LEVEL, m_TlasStorage.handle(), 0, tlasSizes.accelerationStructureSize };
                NriAccelerationStructure tlas = nullptr;
                if (nriCreateAccelerationStructure(m_Device.handle(), &tlasDesc, &tlas) != NRI_SUCCESS || !tlas)
                    throw std::runtime_error("nriCreateAccelerationStructure (TLAS) failed");
                m_Tlas = nri::AccelerationStructure(tlas);

                const uint64_t scratchBytes = (std::max(blasSizes.buildScratchSize, tlasSizes.buildScratchSize) + 255) & ~255ull;
                m_Scratch = m_Device.createBuffer({ .size = scratchBytes, .usage = NRI_BUFFER_USAGE_ACCELERATION_STRUCTURE_SCRATCH });

                nri::CommandBuffer& cmd = m_Cmd;
                cmd.begin(0, true);
                cmd.copyBuffer(m_Upload, m_Vertices, nri::BufferCopyRegion{ 0, 0, sizeof(vertices) });
                cmd.copyBuffer(m_Upload, m_Indices, nri::BufferCopyRegion{ 256, 0, sizeof(indices) });
                const nri::ResourceState copied{ NRI_ACCESS_TRANSFER_WRITE, NRI_STAGE_TRANSFER };
                const nri::ResourceState buildInput{ NRI_ACCESS_SHADER_READ | NRI_ACCESS_ACCELERATION_STRUCTURE_READ, NRI_STAGE_ACCELERATION_STRUCTURE_BUILD };
                // (a build reads and writes its scratch through acceleration-structure access, not shader access: Vulkan's validation
                // rejects SHADER_WRITE at the build stage)
                const nri::ResourceState scratch{ NRI_ACCESS_ACCELERATION_STRUCTURE_READ | NRI_ACCESS_ACCELERATION_STRUCTURE_WRITE, NRI_STAGE_ACCELERATION_STRUCTURE_BUILD };
                const nri::BufferBarrierDesc barriers[3] = { { m_Vertices.handle(), copied, buildInput }, { m_Indices.handle(), copied, buildInput },
                                                             { m_Scratch.handle(), {}, scratch } };
                cmd.resourceBarriers({}, barriers);
                nriCmdBuildAccelerationStructure(cmd.handle(), &blasBuild, m_Scratch.getDeviceAddress(), m_Blas.handle());
                nriCmdAccelerationStructureBarrier(cmd.handle(), NRI_AS_BARRIER_BUILD_TO_BUILD);
                nriCmdBuildAccelerationStructure(cmd.handle(), &tlasBuild, m_Scratch.getDeviceAddress(), m_Tlas.handle());
                nriCmdAccelerationStructureBarrier(cmd.handle(), NRI_AS_BARRIER_BUILD_TO_SHADER_READ);
                cmd.end(0);
                m_Device.submitAndWait(cmd, 0);

                push.tlasAddress = m_Tlas.getDeviceAddress();
                push.tlasSlot = nriRegisterAccelerationStructure(m_Resources.handle(), m_Tlas.handle(), ~0u);
                return true;
            }
            catch (const std::exception& e)
            {
                error = e.what();
                return false;
            }
        }

        // --- graphics ------------------------------------------------------------------------------------------------------------------

        struct GfxOptions
        {
            uint32_t shape = 0;
            uint32_t vertexCount = 3;
            bool depth = false;
            bool blend = false;
            bool cullBack = false;
            uint32_t samples = 1;
            bool mesh = false;
                    const Push* push = nullptr; // (null: the base push data)
        };

        nri::Pipeline* GetGfxPipeline(bool mesh, std::string& error, const char* fragment = "psGfx", const char* task = "tsGfx", const char* meshEntry = "msGfx")
        {
            const std::string key = std::string(mesh ? std::string("#mesh") + task + meshEntry : "#raster") + fragment;
            if (auto found = m_Pipelines.find(key); found != m_Pipelines.end())
                return &found->second;
            try
            {
                nri::PipelineDesc desc{ .type = NRI_PIPELINE_GRAPHICS, .colorFormats = { NRI_FORMAT_RGBA8 }, .buildNow = true };
                if (mesh)
                    desc.shaders = { { NRI_SHADER_STAGE_TASK, task, m_ShaderPath }, { NRI_SHADER_STAGE_MESH, meshEntry, m_ShaderPath },
                                     { NRI_SHADER_STAGE_FRAGMENT, fragment, m_ShaderPath } };
                else
                    desc.shaders = { { NRI_SHADER_STAGE_VERTEX, "vsGfx", m_ShaderPath }, { NRI_SHADER_STAGE_FRAGMENT, fragment, m_ShaderPath } };
                nri::Pipeline pipeline = m_Device.createPipeline(desc, m_Compiler.handle());
                m_Device.finishPipelineBuilds();
                return &m_Pipelines.emplace(key, std::move(pipeline)).first->second;
            }
            catch (const std::exception& e)
            {
                error = e.what();
                return nullptr;
            }
        }

        // Renders one shape into the 64 x 64 target (multisampled and resolved when samples > 1) and returns its texels (RGBA8 words).
        std::vector<uint32_t> RunGfx(nri::Pipeline& pipeline, const GfxOptions& o)
        {
            nri::Texture& target = o.samples > 1 ? m_MsaaTarget : m_ColorTarget;
            nri::CommandBuffer& cmd = m_Cmd;
            cmd.begin(0, true);
            cmd.bindDescriptorHeaps(&m_Resources, &m_Samplers);

            const nri::ResourceState colorWrite{ NRI_ACCESS_COLOR_ATTACHMENT_WRITE, NRI_STAGE_COLOR_ATTACHMENT_OUTPUT };
            const nri::ResourceState depthWrite{ NRI_ACCESS_DEPTH_STENCIL_WRITE | NRI_ACCESS_DEPTH_STENCIL_READ, NRI_STAGE_FRAGMENT_TESTS };
            const nri::ResourceState transferRead{ NRI_ACCESS_TRANSFER_READ, NRI_STAGE_TRANSFER };
            const nri::ResourceState transferWrite{ NRI_ACCESS_TRANSFER_WRITE, NRI_STAGE_TRANSFER };
            std::vector<nri::TextureBarrierDesc> barriers = { { target.handle(), transferRead, colorWrite } };
            if (o.depth)
                barriers.push_back({ m_DepthTarget.handle(), {}, depthWrite });
            cmd.resourceBarriers(barriers, {});

            nri::RenderDesc render{};
            render.renderArea = { kStorageSize, kStorageSize };
            render.colorAttachments.push_back({ .attachment = target.handle(), .loadOp = NRI_LOAD_OP_CLEAR, .storeOp = NRI_STORE_OP_STORE,
                                                .clearColor = { 0.0f, 0.0f, 0.0f, 1.0f } });
            if (o.depth)
            {
                render.depthAttachment = { .attachment = m_DepthTarget.handle(), .loadOp = NRI_LOAD_OP_CLEAR, .storeOp = NRI_STORE_OP_STORE,
                                           .clearDepth = { 1.0f, 0 } };
                render.hasDepthAttachment = true;
            }
            cmd.beginRendering(render);
            // NoxRHI's +Y-up convention (the RenderGraph's FlippedY): a negative height flips Vulkan; D3D12 un-negates it.
            cmd.setViewportWithCount({ 0.0f, static_cast<float>(kStorageSize), static_cast<float>(kStorageSize), -static_cast<float>(kStorageSize) }, 0.0f, 1.0f);
            cmd.setScissorWithCount({ kStorageSize, kStorageSize });
            cmd.setVertexInput();
            cmd.setPrimitiveTopology(NRI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
            cmd.setPrimitiveRestartEnable(false);
            cmd.setRasterizerDiscardEnable(false);
            cmd.setPolygonMode(NRI_POLYGON_MODE_FILL);
            cmd.setCullMode(o.cullBack ? NRI_CULL_MODE_BACK : NRI_CULL_MODE_NONE);
            cmd.setFrontFace(NRI_FRONT_FACE_COUNTER_CLOCKWISE);
            cmd.setDepthBiasEnable(false);
            cmd.setDepthClampEnable(false);
            cmd.setLineWidth(1.0f);
            cmd.setRasterizationSamples(o.samples);
            cmd.setSampleMask(o.samples, 0xFFFFFFFFu);
            cmd.setAlphaToCoverageEnable(false);
            cmd.setAlphaToOneEnable(false);
            cmd.setDepthTestEnable(o.depth);
            cmd.setDepthWriteEnable(o.depth);
            cmd.setDepthCompareOp(NRI_COMPARE_LESS);
            cmd.setDepthBoundsTestEnable(false);
            cmd.setStencilTestEnable(false);
            cmd.setColorBlendEnable(0, o.blend);
            cmd.setColorBlendEquation(0, nri::ColorBlendEquation{ .srcColorBlendFactor = NRI_BLEND_FACTOR_SRC_ALPHA,
                                                                  .dstColorBlendFactor = NRI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                                                  .colorBlendOp = NRI_BLEND_OP_ADD,
                                                                  .srcAlphaBlendFactor = NRI_BLEND_FACTOR_ONE,
                                                                  .dstAlphaBlendFactor = NRI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                                                  .alphaBlendOp = NRI_BLEND_OP_ADD });
            cmd.setColorWriteMask(0, nri::ColorComponent::All);
            cmd.setLogicOpEnable(false);
            cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            Push push = o.push ? *o.push : m_BasePush;
            push.uparams[0] = o.shape;
            cmd.pushData(&push, sizeof(push));
            if (o.mesh)
                nri::drawMeshTasks(cmd, 1);
            else
                cmd.draw(o.vertexCount);
            cmd.endRendering();

            nri::Texture& readFrom = o.samples > 1 ? m_ResolveTarget : m_ColorTarget;
            if (o.samples > 1)
            {
                const nri::TextureBarrierDesc resolveBarriers[2] = { { m_MsaaTarget.handle(), colorWrite, transferRead },
                                                                     { m_ResolveTarget.handle(), transferRead, transferWrite } };
                cmd.resourceBarriers(resolveBarriers, {});
                cmd.resolveImage(m_MsaaTarget, m_ResolveTarget, kStorageSize, kStorageSize);
                const nri::TextureBarrierDesc after[1] = { { m_ResolveTarget.handle(), transferWrite, transferRead } };
                cmd.resourceBarriers(after, {});
            }
            else
            {
                const nri::TextureBarrierDesc after[1] = { { m_ColorTarget.handle(), colorWrite, transferRead } };
                cmd.resourceBarriers(after, {});
            }
            readFrom.copyToBuffer(cmd.handle(), m_StorageReadback.handle(), 0, 0, kStorageSize, kStorageSize);
            cmd.end(0);
            m_Device.submitAndWait(cmd, 0);

            std::vector<uint32_t> image(kStorageSize * kStorageSize);
            std::memcpy(image.data(), m_StorageReadback.map(0, image.size() * 4), image.size() * 4);
            m_StorageReadback.unmap();
            return image;
        }

    private:
        void CreateTextures()
        {
            // The sampled 4 x 4 texture: staging rows 256 bytes apart (both APIs read that pitch when it is given).
            m_SampledTexture = m_Device.createTexture({ .width = kTexSize, .height = kTexSize, .usage = NRI_TEXTURE_USAGE_SHADER_RESOURCE, .format = NRI_FORMAT_RGBA8 });
            auto* staging = static_cast<uint8_t*>(m_Upload.map(0, 256 * kTexSize));
            std::memset(staging, 0, 256 * kTexSize);
            for (uint32_t y = 0; y < kTexSize; ++y)
                for (uint32_t x = 0; x < kTexSize; ++x)
                {
                    uint8_t* texel = staging + y * 256 + x * 4;
                    texel[0] = static_cast<uint8_t>(x * 64 + 10);
                    texel[1] = static_cast<uint8_t>(y * 64 + 20);
                    texel[2] = static_cast<uint8_t>((x + y) * 30);
                    texel[3] = 255;
                }
            m_Upload.unmap();

            m_StorageTexture = m_Device.createTexture({ .width = kStorageSize, .height = kStorageSize, .usage = NRI_TEXTURE_USAGE_STORAGE, .format = NRI_FORMAT_RGBA8 });
            m_StorageReadback = m_Device.createBuffer({ .size = kStorageSize * kStorageSize * 4, .usage = NRI_BUFFER_USAGE_READBACK });
            m_ColorTarget = m_Device.createTexture({ .width = kStorageSize, .height = kStorageSize, .usage = NRI_TEXTURE_USAGE_COLOR_ATTACHMENT, .format = NRI_FORMAT_RGBA8 });
            m_ResolveTarget = m_Device.createTexture({ .width = kStorageSize, .height = kStorageSize, .usage = NRI_TEXTURE_USAGE_COLOR_ATTACHMENT, .format = NRI_FORMAT_RGBA8 });
            m_MsaaTarget = m_Device.createTexture({ .width = kStorageSize, .height = kStorageSize, .sampleCount = 4, .usage = NRI_TEXTURE_USAGE_COLOR_ATTACHMENT, .format = NRI_FORMAT_RGBA8 });
            m_DepthTarget = m_Device.createTexture({ .width = kStorageSize, .height = kStorageSize, .usage = NRI_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT, .format = NRI_FORMAT_NONE });

            nri::CommandBuffer& cmd = m_Cmd;
            cmd.begin(0, true);
            const nri::TextureRegionUpload region{ 0, 0, 0, kTexSize, kTexSize, 256 };
            m_SampledTexture.recordRegionUploads(cmd.handle(), m_Upload.handle(), { &region, 1 });
            const nri::ResourceState uploaded{ NRI_ACCESS_TRANSFER_WRITE, NRI_STAGE_TRANSFER };
            const nri::ResourceState copySource{ NRI_ACCESS_TRANSFER_READ, NRI_STAGE_TRANSFER };
            const nri::TextureBarrierDesc barriers[5] = {
                { m_SampledTexture.handle(), uploaded, { NRI_ACCESS_SHADER_READ, NRI_STAGE_COMPUTE } },
                { m_StorageTexture.handle(), {}, copySource }, { m_ColorTarget.handle(), {}, copySource },
                { m_ResolveTarget.handle(), {}, copySource }, { m_MsaaTarget.handle(), {}, copySource } };
            cmd.resourceBarriers(barriers, {});
            cmd.end(0);
            m_Device.submitAndWait(cmd, 0);

            m_Resources.registerTexture(m_SampledTexture);
            m_BasePush.texSlot = m_Resources.getImageHeapIndexOffset() + m_SampledTexture.getDescriptorSlot();
            m_BasePush.storageTexSlot = m_Resources.registerStorageTextureMip(m_StorageTexture, 0);
        }

        NriGraphicsAPI m_Api;
        std::string m_ShaderPath;
        nri::Device m_Device;
        nri::DescriptorHeap m_Resources, m_Samplers;
        nri::ShaderCompiler m_Compiler;
        nri::Buffer m_In, m_Out, m_Upload, m_Zeros, m_Readback, m_StorageReadback;
        nri::Texture m_SampledTexture, m_StorageTexture, m_ColorTarget, m_ResolveTarget, m_MsaaTarget, m_DepthTarget;
        nri::CommandAllocator m_Allocator;
        nri::CommandBuffer m_Cmd;
        Push m_BasePush;
        std::map<std::string, nri::Pipeline> m_Pipelines;
        nri::Buffer m_Vertices, m_Indices, m_Instances, m_Scratch, m_BlasStorage, m_TlasStorage;
        nri::AccelerationStructure m_Blas, m_Tlas;
        nri::Buffer m_CubeVertices, m_CubeIndices, m_CubeInstances, m_CubeBlasStorage, m_CubeTlasStorage, m_CubeBlasScratch, m_CubeTlasScratch;
        nri::AccelerationStructure m_CubeBlas, m_CubeTlas;
        uint32_t m_CubeInstanceCount = 0;
    };

    // --- The tests -----------------------------------------------------------------------------------------------------------------------

    struct Test
    {
        std::string entry;
        std::string what;
        bool featureGated = false; // a pipeline that does not build means the device lacks the feature: SKIP
        bool exactAcrossApis = true;
        std::function<Result(Suite&, nri::Pipeline&)> run;
        int graphics = 0; // 0: a compute entry point; 1: the raster pipeline (vsGfx / psGfx); 2: the mesh pipeline (tsGfx / msGfx / psGfx);
                          // 3: the raster pipeline with psRayQuery; 4: the mesh pipeline through the payload (tsGfxPayload / msGfxPayload / psGfx)
    };

    std::vector<uint32_t> RandomWords(uint32_t count, uint32_t seed)
    {
        std::mt19937 rng(seed);
        std::vector<uint32_t> words(count);
        for (uint32_t& w : words)
            w = rng();
        return words;
    }

    Result Compare(const std::vector<uint32_t>& got, const std::vector<uint32_t>& want, const char* what)
    {
        Result r;
        r.raw = got;
        for (size_t i = 0; i < want.size(); ++i)
            if (got[i] != want[i])
            {
                r.status = Status::Fail;
                r.detail = std::string(what) + "[" + std::to_string(i) + "] = " + Hex(got[i]) + ", expected " + Hex(want[i]);
                return r;
            }
        r.status = Status::Pass;
        return r;
    }

    // --- graphics checks ----------------------------------------------------------------------------------------------------------------

    uint32_t Rgba(int r, int g, int b, int a) { return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(a) << 24); }
    constexpr uint32_t kBlack = 0xFF000000u, kRed = 0xFF0000FFu, kGreen = 0xFF00FF00u, kBlue = 0xFFFF0000u;

    std::string PixelText(uint32_t v)
    {
        return "(" + std::to_string(v & 0xFF) + "," + std::to_string((v >> 8) & 0xFF) + "," + std::to_string((v >> 16) & 0xFF) + "," + std::to_string(v >> 24) + ")";
    }

    struct PixelCheck
    {
        uint32_t x, y;
        uint32_t want;
        int tolerance; // per channel
        const char* what;
    };

    Result CheckPixels(const std::vector<uint32_t>& image, const std::vector<PixelCheck>& checks)
    {
        Result r;
        r.raw = image;
        for (const PixelCheck& c : checks)
        {
            const uint32_t got = image[c.y * kStorageSize + c.x];
            for (int channel = 0; channel < 4; ++channel)
            {
                const int g = static_cast<int>((got >> (channel * 8)) & 0xFF), w = static_cast<int>((c.want >> (channel * 8)) & 0xFF);
                if (std::abs(g - w) > c.tolerance)
                {
                    r.status = Status::Fail;
                    r.detail = std::string(c.what) + ": pixel (" + std::to_string(c.x) + "," + std::to_string(c.y) + ") = " + PixelText(got) + ", expected " + PixelText(c.want);
                    return r;
                }
            }
        }
        r.status = Status::Pass;
        return r;
    }

    std::vector<Test> MakeTests()
    {
        std::vector<Test> tests;

        tests.push_back({ "csPushLayout", "push data (D3D12 root constants) laid out the same", false, true, [](Suite& s, nri::Pipeline& p)
        {
            Push push = s.BasePush();
            push.texSlot = 11; push.storageTexSlot = 22; push.pointSampler = 33; push.linearSampler = 44; push.count = 55; push.tlasSlot = 66;
            push.params[0] = 1.5f; push.params[1] = -2.25f; push.params[2] = 1e-7f; push.params[3] = 65504.0f;
            push.uparams[0] = 0xDEADBEEFu; push.uparams[1] = 7; push.uparams[2] = 0xFFFFFFFFu; push.uparams[3] = 0x80000000u;
            push.tlasAddress = 0x1122334455667788ull;
            const std::vector<uint32_t> got = s.Run(p, {}, 16, 1, 1, push);
            const std::vector<uint32_t> want = { 11, 22, 33, 44, 55, 66, AsUint(1.5f), AsUint(-2.25f), AsUint(1e-7f), AsUint(65504.0f),
                                                 0xDEADBEEFu, 7, 0xFFFFFFFFu, 0x80000000u, 0x55667788u, 0x11223344u };
            return Compare(got, want, "field");
        } });

        tests.push_back({ "csBufferCopy", "read a buffer, write a buffer", false, true, [](Suite& s, nri::Pipeline& p)
        {
            const uint32_t n = 1000;
            const std::vector<uint32_t> in = RandomWords(n, 1);
            Push push = s.BasePush();
            push.count = n;
            const std::vector<uint32_t> got = s.Run(p, in, n, (n + 63) / 64, 1, push);
            std::vector<uint32_t> want(n);
            for (uint32_t i = 0; i < n; ++i)
                want[i] = in[i] * 3u + 1u;
            return Compare(got, want, "out");
        } });

        tests.push_back({ "csStructLayout", "a struct through nox_gpu.slang's BufferRef: layout, stride, Mat4, LoadByte", false, true, [](Suite& s, nri::Pipeline& p)
        {
            struct LayoutProbe
            {
                float a[4];
                float b[3];
                uint32_t c;
                uint64_t d;
                float e[2];
                uint32_t f, g;
                float m[16]; // column-major
            };
            static_assert(sizeof(LayoutProbe) == 120 && offsetof(LayoutProbe, d) == 32 && offsetof(LayoutProbe, m) == 56, "LayoutProbe must match shader_compat.slang");
            LayoutProbe probes[2]{};
            for (int k = 0; k < 2; ++k)
            {
                LayoutProbe& probe = probes[k];
                const float base = k == 0 ? -100.0f : 1.0f; // (element 0 is a decoy: reading it instead of element 1 fails)
                for (int i = 0; i < 4; ++i) probe.a[i] = base + float(i);
                for (int i = 0; i < 3; ++i) probe.b[i] = base + 10.0f + float(i);
                probe.c = 0xC0DE0000u + uint32_t(k);
                probe.d = 0x0123456789ABCDEFull + uint64_t(k);
                probe.e[0] = base + 20.0f; probe.e[1] = base + 21.0f;
                probe.f = 0xF00Du + uint32_t(k); probe.g = 0x600Du + uint32_t(k);
                for (int i = 0; i < 16; ++i) probe.m[i] = base + 30.0f + float(i);
            }
            std::vector<uint32_t> in(sizeof(probes) / 4);
            std::memcpy(in.data(), probes, sizeof(probes));
            Push push = s.BasePush();
            const std::vector<uint32_t> got = s.Run(p, in, 30, 1, 1, push);

            const LayoutProbe& probe = probes[1];
            std::vector<uint32_t> want;
            for (float v : probe.a) want.push_back(AsUint(v));
            for (float v : probe.b) want.push_back(AsUint(v));
            want.push_back(probe.c);
            want.push_back(uint32_t(probe.d));
            want.push_back(uint32_t(probe.d >> 32));
            want.push_back(AsUint(probe.e[0]));
            want.push_back(AsUint(probe.e[1]));
            want.push_back(probe.f);
            want.push_back(probe.g);
            const float v[4] = { 1, 2, 3, 4 };
            for (int row = 0; row < 4; ++row)
            {
                float sum = 0;
                for (int column = 0; column < 4; ++column)
                    sum += probe.m[column * 4 + row] * v[column];
                want.push_back(AsUint(sum));
            }
            const auto* bytes = reinterpret_cast<const uint8_t*>(probes);
            for (int i = 0; i < 8; ++i)
                want.push_back(bytes[120 + i]);
            return Compare(got, want, "word");
        } });

        tests.push_back({ "csGroupShared", "groupshared memory + barriers (tree sum)", false, true, [](Suite& s, nri::Pipeline& p)
        {
            const uint32_t n = 64 * 20 - 13;
            std::vector<uint32_t> in = RandomWords(n, 2);
            for (uint32_t& w : in)
                w &= 0xFFFFu;
            Push push = s.BasePush();
            push.count = n;
            const uint32_t groups = (n + 63) / 64;
            const std::vector<uint32_t> got = s.Run(p, in, groups, groups, 1, push);
            std::vector<uint32_t> want(groups, 0);
            for (uint32_t i = 0; i < n; ++i)
                want[i / 64] += in[i];
            return Compare(got, want, "group sum");
        } });

        tests.push_back({ "csAtomics", "InterlockedAdd / InterlockedMax on a buffer", false, true, [](Suite& s, nri::Pipeline& p)
        {
            const uint32_t n = 4096;
            std::vector<uint32_t> in = RandomWords(n, 3);
            for (uint32_t& w : in)
                w &= 0xFFFFu;
            Push push = s.BasePush();
            push.count = n;
            const std::vector<uint32_t> got = s.Run(p, in, 3, n / 64, 1, push);
            uint32_t sum = 0, max = 0;
            for (uint32_t v : in)
            {
                sum += v;
                max = std::max(max, v);
            }
            return Compare(got, { sum, max, n }, "atomic");
        } });

        tests.push_back({ "csWave", "wave intrinsics (lane count, prefix sum, active sum)", false, false, [](Suite& s, nri::Pipeline& p)
        {
            const uint32_t threads = 128;
            Push push = s.BasePush();
            const std::vector<uint32_t> got = s.Run(p, {}, 1 + threads * 3, threads / 64, 1, push);
            Result r;
            r.raw = got;
            const uint32_t lanes = got[0];
            if (lanes < 4 || lanes > 128 || (lanes & (lanes - 1)) != 0)
            {
                r.status = Status::Fail;
                r.detail = "wave size " + std::to_string(lanes) + " is not a power of two in 4..128";
                return r;
            }
            const uint32_t perWave = std::min(lanes, 64u); // (a group of 64 fills at most 64 lanes)
            for (uint32_t i = 0; i < threads; ++i)
            {
                const uint32_t lane = got[1 + i * 3], prefix = got[2 + i * 3], active = got[3 + i * 3];
                if (lane >= lanes || prefix != lane % perWave || active != perWave)
                {
                    r.status = Status::Fail;
                    r.detail = "thread " + std::to_string(i) + ": lane " + std::to_string(lane) + ", prefix " + std::to_string(prefix) + ", active " +
                               std::to_string(active) + " (wave size " + std::to_string(lanes) + ")";
                    return r;
                }
            }
            r.status = Status::Pass;
            r.detail = "wave size " + std::to_string(lanes);
            return r;
        } });

        tests.push_back({ "csIntOps", "bit counts, signed division / modulo / shifts, wrapping", false, true, [](Suite& s, nri::Pipeline& p)
        {
            std::vector<uint32_t> in = { 0u, 1u, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu, 12345u, 0xFFFFFFF9u /* -7 */, 0x00F0F000u };
            const std::vector<uint32_t> more = RandomWords(56, 4);
            in.insert(in.end(), more.begin(), more.end());
            const uint32_t n = static_cast<uint32_t>(in.size());
            Push push = s.BasePush();
            push.count = n;
            push.uparams[0] = static_cast<uint32_t>(-7);
            const std::vector<uint32_t> got = s.Run(p, in, n * 10, 1, 1, push);
            std::vector<uint32_t> want;
            for (uint32_t u : in)
            {
                const int32_t v = static_cast<int32_t>(u);
                want.push_back(static_cast<uint32_t>(std::popcount(u)));
                want.push_back(FirstBitHigh(u));
                want.push_back(FirstBitLow(u));
                want.push_back(ReverseBits(u));
                want.push_back(static_cast<uint32_t>(v / -7));
                want.push_back(static_cast<uint32_t>(v % -7));
                want.push_back(static_cast<uint32_t>(v >> 3));
                want.push_back(u >> 3);
                want.push_back(u * 2654435761u);
                want.push_back(v < 0 ? 0u - u : u);
            }
            return Compare(got, want, "op");
        } });

        tests.push_back({ "csFloatMath", "sin, cos, exp2, log2, sqrt, rsqrt, pow, frac, mad, NaN / inf", false, false, [](Suite& s, nri::Pipeline& p)
        {
            const uint32_t n = 64;
            std::vector<uint32_t> in(n);
            for (uint32_t i = 0; i < n; ++i)
                in[i] = AsUint(-3.1f + 6.2f * static_cast<float>(i) / static_cast<float>(n - 1));
            Push push = s.BasePush();
            push.count = n;
            const std::vector<uint32_t> got = s.Run(p, in, n * 10, 1, 1, push);
            Result r;
            r.raw = got;
            struct Check { const char* name; float abs, rel; };
            const Check checks[9] = { { "sin", 2e-3f, 0 }, { "cos", 2e-3f, 0 }, { "exp2", 0, 1e-4f }, { "log2", 1e-4f, 1e-4f }, { "sqrt", 0, 2e-5f },
                                      { "rsqrt", 0, 2e-5f }, { "pow", 1e-5f, 2e-3f }, { "frac", 2e-5f, 0 }, { "mad", 1e-6f, 1e-6f } };
            float worst[9] = {};
            for (uint32_t i = 0; i < n; ++i)
            {
                const float x = AsFloat(in[i]);
                const float scaled = x * 3.7f;
                const float want[9] = { std::sin(x), std::cos(x), std::exp2(x), std::log2(std::fabs(x) + 1.0f), std::sqrt(std::fabs(x)),
                                        1.0f / std::sqrt(std::fabs(x) + 0.5f), std::pow(std::fabs(x) + 0.25f, 1.5f), scaled - std::floor(scaled),
                                        x * 1.25f - 0.5f };
                for (int k = 0; k < 9; ++k)
                {
                    const float g = AsFloat(got[i * 10 + k]);
                    worst[k] = std::max(worst[k], std::fabs(g - want[k]));
                    // (frac near a whole number may land on either side: 0.99999 and 0.0 are both right)
                    const bool fracWrap = k == 7 && std::fabs(std::fabs(g - want[k]) - 1.0f) < 1e-4f;
                    if (!Near(g, want[k], checks[k].abs, checks[k].rel) && !fracWrap)
                    {
                        r.status = Status::Fail;
                        r.detail = std::string(checks[k].name) + "(" + std::to_string(x) + ") = " + std::to_string(g) + ", expected " + std::to_string(want[k]);
                        return r;
                    }
                }
                if (got[i * 10 + 9] != 3u)
                {
                    r.status = Status::Fail;
                    r.detail = "isnan(sqrt(negative)) / isinf(1/0) flags = " + std::to_string(got[i * 10 + 9]) + ", expected 3";
                    return r;
                }
            }
            r.status = Status::Pass;
            char text[160];
            std::snprintf(text, sizeof(text), "largest errors: sin %.1e cos %.1e exp2 %.1e pow %.1e", worst[0], worst[1], worst[2], worst[6]);
            r.detail = text;
            return r;
        } });

        tests.push_back({ "csHalf", "16-bit float math", true, false, [](Suite& s, nri::Pipeline& p)
        {
            const uint32_t n = 64;
            std::vector<uint32_t> in(n);
            for (uint32_t i = 0; i < n; ++i)
                in[i] = AsUint(-20.0f + 40.0f * static_cast<float>(i) / static_cast<float>(n - 1));
            Push push = s.BasePush();
            push.count = n;
            const std::vector<uint32_t> got = s.Run(p, in, n, 1, 1, push);
            Result r;
            r.raw = got;
            for (uint32_t i = 0; i < n; ++i)
            {
                // Narrowing float conversions may round to nearest even or toward zero on both APIs (WARP converts the float input
                // toward zero), and the multiply-add may be fused (one rounding) or not: every combination is right, and every one is
                // a 16-bit value -- math done in 32 bits does not match.
                const float g = AsFloat(got[i]);
                bool matched = false;
                float nearest = 0.0f;
                for (int mode = 0; mode < 2 && !matched; ++mode)
                {
                    const auto round = [mode](float v) { return mode == 0 ? RoundHalf(v) : RoundHalfTowardZero(v); };
                    for (int inputMode = 0; inputMode < 2 && !matched; ++inputMode)
                    {
                        const float a = inputMode == 0 ? RoundHalf(AsFloat(in[i])) : RoundHalfTowardZero(AsFloat(in[i]));
                        const float separate = round(round(a * 1.5f) - 0.25f), fused = round(a * 1.5f - 0.25f);
                        if (mode == 0 && inputMode == 0)
                            nearest = fused;
                        matched = g == separate || g == fused;
                    }
                }
                if (!matched)
                {
                    r.status = Status::Fail;
                    r.detail = "half(" + std::to_string(AsFloat(in[i])) + ") * 1.5 - 0.25 = " + std::to_string(g) + ", expected " + std::to_string(nearest) +
                               " (or the same rounded toward zero)";
                    return r;
                }
            }
            r.status = Status::Pass;
            return r;
        } });

        tests.push_back({ "csInt64", "64-bit integer multiply / add / shift", true, true, [](Suite& s, nri::Pipeline& p)
        {
            const uint32_t n = 64;
            const std::vector<uint32_t> in = RandomWords(n, 5);
            Push push = s.BasePush();
            push.count = n;
            const std::vector<uint32_t> got = s.Run(p, in, n * 2, 1, 1, push);
            std::vector<uint32_t> want;
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint64_t a = (static_cast<uint64_t>(in[i]) << 32) | static_cast<uint64_t>(i * 2654435761u);
                const uint64_t r = a * 6364136223846793005ull + 1442695040888963407ull;
                want.push_back(static_cast<uint32_t>(r));
                want.push_back(static_cast<uint32_t>(r >> 32));
            }
            return Compare(got, want, "u64 word");
        } });

        tests.push_back({ "csMatrix", "float4x4 conventions: mul(M, v), mul(v, M), m[row][col]", false, false, [](Suite& s, nri::Pipeline& p)
        {
            float columns[4][4]; // columns[c][r]: the buffer holds column 0, then column 1, ...
            std::vector<uint32_t> in;
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                {
                    columns[c][r] = static_cast<float>(c * 4 + r + 1) * (r % 2 ? -0.5f : 1.0f);
                    in.push_back(AsUint(columns[c][r]));
                }
            const float v[4] = { 1.0f, -2.0f, 0.5f, 3.0f };
            for (float f : v)
                in.push_back(AsUint(f));
            Push push = s.BasePush();
            const std::vector<uint32_t> got = s.Run(p, in, 12, 1, 1, push);
            Result r;
            r.raw = got;
            for (int i = 0; i < 4; ++i)
            {
                float mv = 0, vm = 0;
                for (int k = 0; k < 4; ++k)
                {
                    mv += columns[k][i] * v[k]; // (M v)_i = sum_k M[i][k] v_k
                    vm += v[k] * columns[i][k]; // (v M)_i = sum_k v_k M[k][i]
                }
                const float row1 = columns[i][1];
                const float g[3] = { AsFloat(got[i]), AsFloat(got[4 + i]), AsFloat(got[8 + i]) };
                const float want[3] = { mv, vm, row1 };
                const char* names[3] = { "mul(M, v)", "mul(v, M)", "M[1]" };
                for (int k = 0; k < 3; ++k)
                    if (!Near(g[k], want[k], 1e-5f, 1e-6f))
                    {
                        r.status = Status::Fail;
                        r.detail = std::string(names[k]) + "[" + std::to_string(i) + "] = " + std::to_string(g[k]) + ", expected " + std::to_string(want[k]);
                        return r;
                    }
            }
            r.status = Status::Pass;
            return r;
        } });

        tests.push_back({ "csTextureSample", "bindless texture + nearest / linear samplers", false, false, [](Suite& s, nri::Pipeline& p)
        {
            Push push = s.BasePush();
            const std::vector<uint32_t> got = s.Run(p, {}, 16 * 8, 1, 1, push);
            Result r;
            r.raw = got;
            const auto texel = [](uint32_t x, uint32_t y, int c)
            {
                const float v[4] = { static_cast<float>(x * 64 + 10), static_cast<float>(y * 64 + 20), static_cast<float>((x + y) * 30), 255.0f };
                return v[c] / 255.0f;
            };
            for (uint32_t i = 0; i < 16; ++i)
            {
                const uint32_t x = i % 4, y = i / 4, lx = std::min(x, 2u), ly = std::min(y, 2u);
                for (int c = 0; c < 4; ++c)
                {
                    const float nearest = AsFloat(got[i * 8 + c]), linear = AsFloat(got[i * 8 + 4 + c]);
                    const float wantLinear = (texel(lx, ly, c) + texel(lx + 1, ly, c) + texel(lx, ly + 1, c) + texel(lx + 1, ly + 1, c)) * 0.25f;
                    if (!Near(nearest, texel(x, y, c), 1e-6f, 0))
                    {
                        r.status = Status::Fail;
                        r.detail = "nearest sample at texel (" + std::to_string(x) + "," + std::to_string(y) + ") channel " + std::to_string(c) + " = " +
                                   std::to_string(nearest) + ", expected " + std::to_string(texel(x, y, c)) + " (is the sampler really nearest?)";
                        return r;
                    }
                    if (!Near(linear, wantLinear, 0.006f, 0))
                    {
                        r.status = Status::Fail;
                        r.detail = "linear sample between texels (" + std::to_string(lx) + "," + std::to_string(ly) + ") channel " + std::to_string(c) +
                                   " = " + std::to_string(linear) + ", expected " + std::to_string(wantLinear);
                        return r;
                    }
                }
            }
            r.status = Status::Pass;
            return r;
        } });

        tests.push_back({ "csStorageTexture", "RWTexture2D writes, copied back", false, true, [](Suite& s, nri::Pipeline& p)
        {
            Push push = s.BasePush();
            push.uparams[0] = push.uparams[1] = kStorageSize;
            std::vector<uint32_t> image;
            s.Run(p, {}, 1, kStorageSize / 8, kStorageSize / 8, push, &image);
            std::vector<uint32_t> want(kStorageSize * kStorageSize);
            for (uint32_t y = 0; y < kStorageSize; ++y)
                for (uint32_t x = 0; x < kStorageSize; ++x)
                    want[y * kStorageSize + x] = (x * 4) | ((y * 4) << 8) | (((x ^ y) & 0xFF) << 16) | (255u << 24);
            return Compare(image, want, "texel");
        } });

        tests.push_back({ "csRayQuery", "inline ray tracing (RayQuery) against a TLAS", true, false, [](Suite& s, nri::Pipeline& p)
        {
            Push push = s.BasePush();
            std::string error;
            if (!s.BuildRayTracingScene(push, error))
            {
                Result r;
                r.status = Status::Skip;
                r.detail = "acceleration structures: " + error;
                return r;
            }
            const std::vector<uint32_t> got = s.Run(p, {}, 64, 1, 1, push);
            std::vector<uint32_t> want(64);
            for (uint32_t y = 0; y < 8; ++y)
                for (uint32_t x = 0; x < 8; ++x)
                {
                    const float ox = -0.23f + (static_cast<float>(x) + 0.5f) * (1.5f / 8.0f), oy = -0.23f + (static_cast<float>(y) + 0.5f) * (1.5f / 8.0f);
                    want[y * 8 + x] = AsUint(ox >= 0.0f && oy >= 0.0f && ox + oy <= 1.0f ? 5.0f : -1.0f);
                }
            // Hit or miss must match exactly; the hit distance within a few float steps (WARP gives 5.0000005: implementations
            // intersect with their own arithmetic, and neither API asks for an exact t).
            Result r;
            r.raw = got;
            for (uint32_t i = 0; i < 64; ++i)
                if ((AsFloat(want[i]) < 0.0f) != (AsFloat(got[i]) < 0.0f) || !Near(AsFloat(got[i]), AsFloat(want[i]), 0.0f, 1e-6f))
                {
                    r.status = Status::Fail;
                    r.detail = "ray[" + std::to_string(i) + "] = " + std::to_string(AsFloat(got[i])) + ", expected " + std::to_string(AsFloat(want[i]));
                    return r;
                }
            r.status = Status::Pass;
            return r;
        } });

        // translation xyz, scale xyz -- rays along +Z from a 16 x 16 grid at z = 0, at least 0.0625 from every box edge
        static const float kInstances[3][6] = { { 0, 0, 10, 1, 1, 1 }, { 3, 0, 6, 0.5f, 2, 0.5f }, { -3, 1, 8, 1.5f, 0.5f, 1 } };
        // The closest box a ray along +Z from (ox, oy) hits: its index and distance (-1: none); `margin`: how near an edge it passes.
        const auto expectedHit = [](float ox, float oy, float& t, float& margin) {
            uint32_t instance = ~0u;
            t = -1.0f;
            margin = 1e9f;
            for (uint32_t i = 0; i < 3; ++i)
            {
                const float* b = kInstances[i];
                margin = std::min({ margin, std::fabs(std::fabs(ox - b[0]) - b[3]), std::fabs(std::fabs(oy - b[1]) - b[4]) });
                if (std::fabs(ox - b[0]) < b[3] && std::fabs(oy - b[1]) < b[4] && (t < 0 || b[2] - b[5] < t))
                {
                    t = b[2] - b[5]; // the front face
                    instance = i;
                }
            }
            return instance;
        };
        const auto instancedRays = [expectedHit](Suite& s, nri::Pipeline& p, bool rebuildFirst)
        {
            Push push = s.BasePush();
            std::string error;
            if (!s.BuildInstancedScene(push, kInstances, 3, error))
            {
                Result r;
                r.status = Status::Skip;
                r.detail = "acceleration structures: " + error;
                return r;
            }
            if (rebuildFirst)
                s.m_PreDispatch = [&s](nri::CommandBuffer& cmd) { s.RecordCubeTlasRebuild(cmd); };
            const std::vector<uint32_t> got = s.Run(p, {}, 256 * 3, 2, 2, push);
            s.m_PreDispatch = nullptr;
            Result r;
            r.raw = got;
            for (uint32_t y = 0; y < 16; ++y)
                for (uint32_t x = 0; x < 16; ++x)
                {
                    float wantT, margin;
                    const uint32_t wantInstance = expectedHit(-5.0f + (x + 0.5f) * (10.0f / 16.0f), -3.0f + (y + 0.5f) * (6.0f / 16.0f), wantT, margin);
                    const uint32_t ray = y * 16 + x;
                    const float gotT = AsFloat(got[ray * 3 + 0]);
                    const bool hitOk = (wantT < 0) == (gotT < 0) && (wantT < 0 || Near(gotT, wantT, 0.0f, 1e-5f));
                    if (!hitOk || got[ray * 3 + 1] != wantInstance || got[ray * 3 + 2] != (wantT < 0 ? 0u : 1u))
                    {
                        r.status = Status::Fail;
                        r.detail = "ray (" + std::to_string(x) + "," + std::to_string(y) + "): t " + std::to_string(gotT) + " instance " +
                                   std::to_string(int(got[ray * 3 + 1])) + " first-hit " + std::to_string(got[ray * 3 + 2]) + ", expected t " +
                                   std::to_string(wantT) + " instance " + std::to_string(int(wantInstance)) + " first-hit " + (wantT < 0 ? "0" : "1");
                        return r;
                    }
                }
            r.status = Status::Pass;
            return r;
        };

        tests.push_back({ "csRayQueryInstances", "ray queries against scaled + translated instances of a cube (fast-build TLAS)", true, false,
                          [instancedRays](Suite& s, nri::Pipeline& p) { return instancedRays(s, p, false); } });
        tests.push_back({ "csRayQueryRebuild", "the same, the TLAS rebuilt in the same command list just before", true, false,
                          [instancedRays](Suite& s, nri::Pipeline& p) { return instancedRays(s, p, true); } });

        // --- graphics (raster pipeline vsGfx / psGfx, mesh pipeline tsGfx / msGfx / psGfx) -------------------------------------------

        const auto orientation = [](Suite& s, nri::Pipeline& p, bool mesh)
        {
            Suite::GfxOptions o;
            o.shape = 0;
            o.mesh = mesh;
            const std::vector<uint32_t> image = s.RunGfx(p, o);
            return CheckPixels(image, { { 4, 4, kRed, 0, "inside the top-left triangle (NDC +Y is up, row 0 is the top)" },
                                        { 60, 4, kBlack, 0, "top right" }, { 4, 60, kBlack, 0, "bottom left" }, { 60, 60, kBlack, 0, "bottom right" } });
        };

        tests.push_back({ "gfxOrientation", "viewport / NDC orientation (+Y up, origin top-left)", false, true,
                          [orientation](Suite& s, nri::Pipeline& p) { return orientation(s, p, false); }, 1 });

        tests.push_back({ "gfxCulling", "winding: back faces culled, counter-clockwise is front", false, true, [](Suite& s, nri::Pipeline& p)
        {
            Suite::GfxOptions o;
            o.shape = 1;
            o.vertexCount = 6;
            o.cullBack = true;
            const std::vector<uint32_t> image = s.RunGfx(p, o);
            return CheckPixels(image, { { 16, 37, kGreen, 0, "the counter-clockwise (front) triangle" },
                                        { 48, 37, kBlack, 0, "the clockwise (back) triangle, culled" } });
        }, 1 });

        tests.push_back({ "gfxDepth", "depth buffer: LESS test and writes (D32)", false, true, [](Suite& s, nri::Pipeline& p)
        {
            Suite::GfxOptions o;
            o.shape = 2;
            o.vertexCount = 18;
            o.depth = true;
            const std::vector<uint32_t> image = s.RunGfx(p, o);
            return CheckPixels(image, { { 8, 32, kBlue, 0, "near blue over far green" }, { 24, 32, kBlue, 0, "near blue over the later middle red" },
                                        { 48, 32, kRed, 0, "middle red over far green" } });
        }, 1 });

        tests.push_back({ "gfxBlend", "alpha blending (src alpha, one minus src alpha)", false, false, [](Suite& s, nri::Pipeline& p)
        {
            Suite::GfxOptions o;
            o.shape = 3;
            o.vertexCount = 6;
            o.blend = true;
            const std::vector<uint32_t> image = s.RunGfx(p, o);
            // (1, 0.5, 0.25) at 0.5 over opaque black: colour halved; alpha 0.5 * 1 + 1 * 0.5 = 1.
            return CheckPixels(image, { { 32, 32, Rgba(128, 64, 32, 255), 1, "blended" } });
        }, 1 });

        tests.push_back({ "gfxInterpolation", "attribute interpolation; flat colour from the first vertex", false, false, [](Suite& s, nri::Pipeline& p)
        {
            Suite::GfxOptions o;
            o.shape = 4;
            const std::vector<uint32_t> image = s.RunGfx(p, o);
            std::vector<PixelCheck> checks;
            for (uint32_t y = 4; y < kStorageSize; y += 18)
                for (uint32_t x = 4; x < kStorageSize; x += 18)
                {
                    // Pixel centre in NDC; barycentrics of the corners (-1,-1) red, (3,-1) green, (-1,3) blue.
                    const float nx = (static_cast<float>(x) + 0.5f) / 32.0f - 1.0f, ny = 1.0f - (static_cast<float>(y) + 0.5f) / 32.0f;
                    const float g = (nx + 1.0f) / 4.0f, b = (ny + 1.0f) / 4.0f, r = 1.0f - g - b;
                    const auto byte = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
                    checks.push_back({ x, y, Rgba(byte(r), byte(g), byte(b), 255), 2, "smooth colour (alpha: the flat colour's red, 255 = first vertex)" });
                }
            return CheckPixels(image, checks);
        }, 1 });

        tests.push_back({ "gfxMsaa", "4x MSAA with the standard sample pattern, resolved", false, false, [](Suite& s, nri::Pipeline& p)
        {
            Suite::GfxOptions o;
            o.shape = 5;
            o.samples = 4;
            const std::vector<uint32_t> image = s.RunGfx(p, o);
            // The standard 4x pattern (D3D and Vulkan alike), from the pixel's top-left corner; the triangle covers y > x (pixels, y down).
            const float samples[4][2] = { { 0.375f, 0.125f }, { 0.875f, 0.375f }, { 0.125f, 0.625f }, { 0.625f, 0.875f } };
            std::vector<PixelCheck> checks;
            uint32_t partial = 0;
            for (uint32_t y = 0; y < kStorageSize; ++y)
                for (uint32_t x = 0; x < kStorageSize; ++x)
                {
                    int covered = 0;
                    for (const auto& sp : samples)
                        covered += (static_cast<float>(y) + sp[1] > static_cast<float>(x) + sp[0]) ? 1 : 0;
                    partial += (covered > 0 && covered < 4) ? 1 : 0;
                    const int v = static_cast<int>(std::lround(255.0 * covered / 4.0));
                    checks.push_back({ x, y, Rgba(v, v, v, 255), 1, covered > 0 && covered < 4 ? "an edge pixel (partly covered)" : "a pixel" });
                }
            Result r = CheckPixels(image, checks);
            if (r.status == Status::Pass)
                r.detail = std::to_string(partial) + " edge pixels averaged";
            return r;
        }, 1 });

        tests.push_back({ "gfxMeshShader", "task + mesh shader (the orientation triangle)", false, true,
                          [orientation](Suite& s, nri::Pipeline& p) { return orientation(s, p, true); }, 2 });
        tests.push_back({ "gfxMeshPayload", "the triangle sent through the task payload (a second task shader, another payload type)", false, true,
                          [orientation](Suite& s, nri::Pipeline& p) { return orientation(s, p, true); }, 4 });

        tests.push_back({ "gfxRayQuery", "ray queries in a pixel shader (the instanced scene, every pixel checked)", true, false,
                          [expectedHit](Suite& s, nri::Pipeline& p)
        {
            Push push = s.BasePush();
            std::string error;
            if (!s.BuildInstancedScene(push, kInstances, 3, error))
            {
                Result r;
                r.status = Status::Skip;
                r.detail = "acceleration structures: " + error;
                return r;
            }
            Suite::GfxOptions o;
            o.shape = 4; // (a fullscreen triangle)
            o.push = &push;
            const std::vector<uint32_t> image = s.RunGfx(p, o);
            std::vector<PixelCheck> checks;
            for (uint32_t y = 0; y < kStorageSize; ++y)
                for (uint32_t x = 0; x < kStorageSize; ++x)
                {
                    float t, margin;
                    const uint32_t instance = expectedHit(-5.0f + (x + 0.5f) * (10.0f / 64.0f), 3.0f - (y + 0.5f) * (6.0f / 64.0f), t, margin);
                    if (margin < 0.02f)
                        continue; // (pixels a box edge passes through: where exactly is up to the implementation)
                    const int red = instance == ~0u ? 0 : int(std::lround((instance + 1) * 0.25f * 255.0f));
                    checks.push_back({ x, y, Rgba(red, instance == ~0u ? 0 : 255, instance == ~0u ? 0 : 255, 255), 1, "a traced pixel" });
                }
            return CheckPixels(image, checks);
        }, 3 });

        return tests;
    }

    std::map<std::string, Result> RunSuite(NriGraphicsAPI api, const std::string& shaderPath, const std::vector<Test>& tests, const std::string& only)
    {
        std::map<std::string, Result> results;
        const char* apiName = api == NRI_GRAPHICS_API_D3D12 ? "D3D12" : api == NRI_GRAPHICS_API_NONE ? "None (no GPU)" : "Vulkan";
        std::printf("\n=== %s ===\n", apiName);
        std::unique_ptr<Suite> suite;
        try
        {
            SetStage(std::string("creating the ") + apiName + " device and resources");
            suite = std::make_unique<Suite>(api, shaderPath);
        }
        catch (const std::exception& e)
        {
            std::printf("  device: %s\n", e.what());
            // (asked for and not there: a failure, not a skip -- a GPU or driver without what NoxRHI needs on this API)
            results["(device)"] = { Status::Fail, std::string("no ") + apiName + " device: " + e.what(), {} };
            return results;
        }
        for (const Test& test : tests)
        {
            if (!only.empty() && test.entry != only)
                continue;
            Result result;
            std::string error;
            const int errorsBefore = g_ValidationErrors;
            if ((test.graphics == 2 || test.graphics == 4) && !suite->SupportsMeshShaders())
            {
                result.detail = "the device has no mesh shaders";
                std::printf("  %-4s %-17s %s -- %s\n", StatusText(result.status), test.entry.c_str(), test.what.c_str(), result.detail.c_str());
                results[test.entry] = std::move(result);
                continue;
            }
            SetStage(std::string(apiName) + ": compiling " + test.entry);
            nri::Pipeline* pipeline = test.graphics == 1 ? suite->GetGfxPipeline(false, error)
                                    : test.graphics == 2 ? suite->GetGfxPipeline(true, error)
                                    : test.graphics == 3 ? suite->GetGfxPipeline(false, error, "psRayQuery")
                                    : test.graphics == 4 ? suite->GetGfxPipeline(true, error, "psGfx", "tsGfxPayload", "msGfxPayload")
                                                         : suite->GetPipeline(test.entry, error);
            if (pipeline)
            {
                try
                {
                    SetStage(std::string(apiName) + ": running " + test.entry);
                    result = test.run(*suite, *pipeline);
                }
                catch (const std::exception& e)
                {
                    result.status = Status::Fail;
                    result.detail = std::string("exception: ") + e.what();
                }
            }
            else
            {
                result.status = test.featureGated ? Status::Skip : Status::Fail;
                result.detail = "pipeline: " + error.substr(0, 300);
            }
            if (result.status == Status::Pass && g_ValidationErrors > errorsBefore)
            {
                result.status = Status::Fail;
                result.detail = "the results matched, but the API reported errors (see above)";
            }
            std::printf("  %-4s %-17s %s%s%s\n", StatusText(result.status), test.entry.c_str(), test.what.c_str(), result.detail.empty() ? "" : " -- ",
                        result.detail.c_str());
            results[test.entry] = std::move(result);
        }
        SetStage(std::string("destroying the ") + apiName + " device");
        return results;
    }
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // (every line out at once: a CI log keeps it even if the process dies)
    InstallDeathReports();
    std::vector<NriGraphicsAPI> apis;
    std::string only;
    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--vk") || !std::strcmp(argv[i], "--vulkan"))
            apis.push_back(NRI_GRAPHICS_API_VULKAN);
        else if (!std::strcmp(argv[i], "--dx12") || !std::strcmp(argv[i], "--d3d12"))
            apis.push_back(NRI_GRAPHICS_API_D3D12);
        else if (!std::strcmp(argv[i], "--all"))
        {
            apis = { NRI_GRAPHICS_API_VULKAN };
#ifdef _WIN32
            apis.push_back(NRI_GRAPHICS_API_D3D12);
#endif
        }
        else if (!std::strcmp(argv[i], "--none"))
            apis.push_back(NRI_GRAPHICS_API_NONE); // (the program's own flow without a GPU: nothing runs, every check fails by design)
        else if (!std::strcmp(argv[i], "--only") && i + 1 < argc)
            only = argv[++i];
        else if (!std::strcmp(argv[i], "--verbose"))
            g_Verbose = true;
        else
        {
            std::printf("usage: ShaderCompat [--vk] [--dx12] [--all] [--none] [--only <entry>] [--verbose]\n");
            g_Finished = true;
            return 2;
        }
    }
    if (apis.empty())
    {
#ifdef _WIN32
        apis = { NRI_GRAPHICS_API_VULKAN, NRI_GRAPHICS_API_D3D12 };
#else
        apis = { NRI_GRAPHICS_API_VULKAN };
#endif
    }

    std::filesystem::path shaderPath = "shader_compat.slang";
    if (!std::filesystem::exists(shaderPath) && argc > 0)
        shaderPath = std::filesystem::path(argv[0]).parent_path() / "shader_compat.slang";

    const std::vector<Test> tests = MakeTests();
    std::vector<std::map<std::string, Result>> perApi;
    for (NriGraphicsAPI api : apis)
        perApi.push_back(RunSuite(api, shaderPath.string(), tests, only));

    int failures = 0;
    for (const auto& results : perApi)
        for (const auto& [entry, result] : results)
            failures += result.status == Status::Fail ? 1 : 0;

    // The same test on both APIs must give the same answer (exactly for integer results; float results within what each was allowed).
    if (perApi.size() == 2)
    {
        std::printf("\n=== Vulkan vs D3D12 ===\n");
        for (const Test& test : tests)
        {
            const auto a = perApi[0].find(test.entry), b = perApi[1].find(test.entry);
            if (a == perApi[0].end() || b == perApi[1].end())
                continue;
            const char* verdict = "n/a ";
            std::string detail;
            if (a->second.status == Status::Pass && b->second.status == Status::Pass)
            {
                if (!test.exactAcrossApis)
                {
                    verdict = "SAME";
                    detail = "both within tolerance";
                    if (test.entry == "csWave" && a->second.detail != b->second.detail)
                        detail += " (" + a->second.detail + " vs " + b->second.detail + ": allowed)";
                }
                else if (a->second.raw == b->second.raw)
                    verdict = "SAME";
                else
                {
                    verdict = "DIFF";
                    ++failures;
                    for (size_t i = 0; i < std::min(a->second.raw.size(), b->second.raw.size()); ++i)
                        if (a->second.raw[i] != b->second.raw[i])
                        {
                            detail = "word " + std::to_string(i) + ": " + Hex(a->second.raw[i]) + " vs " + Hex(b->second.raw[i]);
                            break;
                        }
                }
            }
            else
                detail = std::string("Vulkan ") + StatusText(a->second.status) + ", D3D12 " + StatusText(b->second.status);
            std::printf("  %s %-17s %s\n", verdict, test.entry.c_str(), detail.c_str());
        }
    }

    std::printf("\n%s: %d failure(s)%s\n", failures == 0 ? "OK" : "FAILED", failures, g_ValidationErrors ? " (API errors were reported)" : "");
    g_Finished = true;
    return failures == 0 ? 0 : 1;
}
