#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "NRI.hpp"
#include <NRI/Extensions/NRIDeviceCreation.h>
#include <NRI/Extensions/NRISwapchain.h>
#include <NRI/Extensions/NRIRayTracing.h>
#include <NRI/Extensions/NRIMeshShader.h>
#include <NRI/Extensions/NRIGpuProfiler.h>
#include <NRI/Extensions/NRIShaderCompiler.h>
#include <NRI/Extensions/NRIImgui.h>
#include <NRI/Extensions/NRIUpscaler.h>
#include <NRI/Extensions/NRIDenoiser.h>
#include <NRI/Extensions/NRIWrapperVK.h>
#include <NRI/Extensions/NRIWrapperD3D12.h>

namespace nri
{
    using WindowSystemType = NriWindowSystemType;
    using NativeWindowHandle = NriNativeWindowHandle;
    using FrameResult = NriFrameResult;
    using AccelerationStructureType = NriAccelerationStructureType;
    using AccelerationStructureBuildSizes = NriAccelerationStructureBuildSizes;
    using AccelerationStructureTrianglesDesc = NriAccelerationStructureTrianglesDesc;
    using AccelerationStructureInstancesDesc = NriAccelerationStructureInstancesDesc;
    using AccelerationStructureInstance = NriAccelerationStructureInstance;
    using StridedDeviceAddressRegion = NriStridedDeviceAddressRegion;
    using ShaderTableState = NriShaderTableState;
    using AccelerationStructureBarrierType = NriAccelerationStructureBarrierType;
    using GpuTimestamp = NriGpuTimestamp;
    using GpuPipelineStatistics = NriGpuPipelineStatistics;
    using DeviceNativeVK = NriDeviceNativeVK;
    using DeviceNativeD3D12 = NriDeviceNativeD3D12;

    struct DeviceDesc
    {
        GraphicsAPI api = NRI_GRAPHICS_API_VULKAN;
        NativeWindowHandle window{};
        bool enableValidation =
#ifdef NDEBUG
            false;
#else
            true;
#endif
        std::string shaderCacheDir = "shader_cache";
        std::vector<std::string> extraInstanceExtensions;
        std::vector<std::string> extraDeviceExtensions;
        LogCallback logCallback = nullptr;
        void* logUserData = nullptr;
        bool enableStreamline = false;
        uint32_t streamlineAppId = 0;
        std::string streamlineProjectId;
    };

    inline Device createDevice(const DeviceDesc& desc)
    {
        std::vector<const char*> instExts;
        instExts.reserve(desc.extraInstanceExtensions.size());
        for (const auto& s : desc.extraInstanceExtensions) instExts.push_back(s.c_str());

        std::vector<const char*> devExts;
        devExts.reserve(desc.extraDeviceExtensions.size());
        for (const auto& s : desc.extraDeviceExtensions) devExts.push_back(s.c_str());

        const NriDeviceDesc cDesc{
            .api = desc.api,
            .window = desc.window,
            .enableValidation = desc.enableValidation,
            .shaderCacheDir = desc.shaderCacheDir.empty() ? nullptr : desc.shaderCacheDir.c_str(),
            .extraInstanceExtensions = instExts.empty() ? nullptr : instExts.data(),
            .extraInstanceExtensionCount = static_cast<uint32_t>(instExts.size()),
            .extraDeviceExtensions = devExts.empty() ? nullptr : devExts.data(),
            .extraDeviceExtensionCount = static_cast<uint32_t>(devExts.size()),
            .logCallback = desc.logCallback,
            .logUserData = desc.logUserData,
            .enableStreamline = desc.enableStreamline,
            .streamlineAppId = desc.streamlineAppId,
            .streamlineProjectId = desc.streamlineProjectId.empty() ? nullptr : desc.streamlineProjectId.c_str()
        };
        NriDevice dev = nullptr;
        checkResult(nriCreateDevice(&cDesc, &dev), "nriCreateDevice");
        return Device(dev);
    }

    struct SwapchainDesc
    {
        uint32_t width = 0;
        uint32_t height = 0;
        bool vsync = true;
        NativeWindowHandle window{};
    };

    class Swapchain
    {
    public:
        Swapchain() = default;
        explicit Swapchain(NriSwapchain handle) : m_handle(handle) {}
        ~Swapchain() { reset(); }

        Swapchain(const Swapchain&) = delete;
        Swapchain& operator=(const Swapchain&) = delete;
        Swapchain(Swapchain&& other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}
        Swapchain& operator=(Swapchain&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                m_handle = std::exchange(other.m_handle, nullptr);
            }
            return *this;
        }

        void reset(NriSwapchain h = nullptr)
        {
            if (m_handle)
                nriDestroySwapchain(m_handle);
            m_handle = h;
        }
        NriSwapchain release() { return std::exchange(m_handle, nullptr); }
        NriSwapchain handle() const { return m_handle; }
        explicit operator bool() const { return m_handle != nullptr; }

        Extent2D getExtent() const { return nriGetSwapchainExtent(m_handle); }
        FrameResult acquireNextImage(uint32_t frameIndex, uint32_t& outImageIndex)
        {
            return nriAcquireNextSwapchainImage(m_handle, frameIndex, &outImageIndex);
        }
        FrameResult present(uint32_t frameIndex, uint32_t imageIndex)
        {
            return nriPresentSwapchain(m_handle, frameIndex, imageIndex);
        }

    private:
        NriSwapchain m_handle = nullptr;
    };

    inline Swapchain createSwapchain(const Device& device, const SwapchainDesc& desc)
    {
        const NriSwapchainDesc cDesc{
            .width = desc.width,
            .height = desc.height,
            .vsync = desc.vsync,
            .window = desc.window
        };
        NriSwapchain sc = nullptr;
        checkResult(nriCreateSwapchain(device.handle(), &cDesc, &sc), "nriCreateSwapchain");
        return Swapchain(sc);
    }

    class ShaderCompiler
    {
    public:
        ShaderCompiler() = default;
        explicit ShaderCompiler(NriShaderCompiler handle) : m_handle(handle) {}
        ~ShaderCompiler() { reset(); }

        ShaderCompiler(const ShaderCompiler&) = delete;
        ShaderCompiler& operator=(const ShaderCompiler&) = delete;
        ShaderCompiler(ShaderCompiler&& other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}
        ShaderCompiler& operator=(ShaderCompiler&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                m_handle = std::exchange(other.m_handle, nullptr);
            }
            return *this;
        }

        void reset(NriShaderCompiler h = nullptr)
        {
            if (m_handle)
                nriDestroyShaderCompiler(m_handle);
            m_handle = h;
        }
        NriShaderCompiler release() { return std::exchange(m_handle, nullptr); }
        NriShaderCompiler handle() const { return m_handle; }
        explicit operator bool() const { return m_handle != nullptr; }

        void setTargetAPI(GraphicsAPI api) { nriShaderCompilerSetTargetAPI(m_handle, api); }
        void setDefine(const char* name, const char* value) { nriShaderCompilerSetDefine(m_handle, name, value); }
        GraphicsAPI getTargetAPI() const { return nriShaderCompilerGetTargetAPI(m_handle); }
        void setSearchPaths(std::span<const std::string_view> paths)
        {
            std::vector<std::string> owned;
            std::vector<const char*> ptrs;
            owned.reserve(paths.size());
            ptrs.reserve(paths.size());
            for (auto p : paths)
            {
                owned.emplace_back(p);
                ptrs.push_back(owned.back().c_str());
            }
            nriShaderCompilerSetSearchPaths(m_handle, ptrs.data(), static_cast<uint32_t>(ptrs.size()));
        }
        void addSearchPath(std::string_view path)
        {
            std::string owned(path);
            nriShaderCompilerAddSearchPath(m_handle, owned.c_str());
        }
        bool canCompile() const { return nriShaderCompilerCanCompile(m_handle); }
        uint64_t sourceHash(std::string_view path) const
        {
            std::string owned(path);
            return nriShaderCompilerSourceHash(m_handle, owned.c_str());
        }
        std::vector<char> compile(std::string_view path)
        {
            std::string owned(path);
            char* bytes = nullptr;
            size_t size = 0;
            checkResult(nriShaderCompilerCompile(m_handle, owned.c_str(), &bytes, &size), "nriShaderCompilerCompile");
            std::vector<char> out(bytes, bytes + size);
            nriFreeCompiledShaderBytes(bytes);
            return out;
        }

    private:
        NriShaderCompiler m_handle = nullptr;
    };

    inline ShaderCompiler createSlangCompiler()
    {
        NriShaderCompiler sc = nullptr;
        checkResult(nriCreateSlangCompiler(&sc), "nriCreateSlangCompiler");
        return ShaderCompiler(sc);
    }

    class AccelerationStructure
    {
    public:
        AccelerationStructure() = default;
        explicit AccelerationStructure(NriAccelerationStructure handle) : m_handle(handle) {}
        ~AccelerationStructure() { reset(); }

        AccelerationStructure(const AccelerationStructure&) = delete;
        AccelerationStructure& operator=(const AccelerationStructure&) = delete;
        AccelerationStructure(AccelerationStructure&& other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}
        AccelerationStructure& operator=(AccelerationStructure&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                m_handle = std::exchange(other.m_handle, nullptr);
            }
            return *this;
        }

        void reset(NriAccelerationStructure h = nullptr)
        {
            if (m_handle)
                nriDestroyAccelerationStructure(m_handle);
            m_handle = h;
        }
        NriAccelerationStructure release() { return std::exchange(m_handle, nullptr); }
        NriAccelerationStructure handle() const { return m_handle; }
        explicit operator bool() const { return m_handle != nullptr; }

        uint64_t getDeviceAddress() const { return nriGetAccelerationStructureDeviceAddress(m_handle); }
        uint64_t getSize() const { return nriGetAccelerationStructureSize(m_handle); }
        AccelerationStructureType getType() const { return nriGetAccelerationStructureType(m_handle); }

    private:
        NriAccelerationStructure m_handle = nullptr;
    };

    class GpuProfiler
    {
    public:
        static constexpr uint32_t InvalidQueryId = NRI_GPU_PROFILER_INVALID_QUERY_ID;

        GpuProfiler() = default;
        explicit GpuProfiler(NriGpuProfiler handle) : m_handle(handle) {}
        ~GpuProfiler() { reset(); }

        GpuProfiler(const GpuProfiler&) = delete;
        GpuProfiler& operator=(const GpuProfiler&) = delete;
        GpuProfiler(GpuProfiler&& other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}
        GpuProfiler& operator=(GpuProfiler&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                m_handle = std::exchange(other.m_handle, nullptr);
            }
            return *this;
        }

        void reset(NriGpuProfiler h = nullptr)
        {
            if (m_handle)
                nriDestroyGpuProfiler(m_handle);
            m_handle = h;
        }
        NriGpuProfiler release() { return std::exchange(m_handle, nullptr); }
        NriGpuProfiler handle() const { return m_handle; }
        explicit operator bool() const { return m_handle != nullptr; }

        bool isSupported() const { return nriGpuProfilerIsSupported(m_handle); }
        bool isComputeQueueSupported() const { return nriGpuProfilerIsComputeQueueSupported(m_handle); }
        float getTimestampPeriod() const { return nriGpuProfilerGetTimestampPeriod(m_handle); }
        uint32_t getTimestampValidBits() const { return nriGpuProfilerGetTimestampValidBits(m_handle); }
        bool calibrate(uint64_t& outGpuTicks, int64_t& outHostNanoseconds)
        {
            return nriGpuProfilerCalibrate(m_handle, &outGpuTicks, &outHostNanoseconds);
        }
        void beginFrame(uint32_t frameSlot) { nriGpuProfilerBeginFrame(m_handle, frameSlot); }
        std::span<const GpuTimestamp> getReadbackTimestamps() const
        {
            uint32_t count = 0;
            const NriGpuTimestamp* data = nriGpuProfilerGetReadbackTimestamps(m_handle, &count);
            return { data, count };
        }
        void resetQueries(const CommandBuffer& cmd) { nriGpuProfilerResetQueries(m_handle, cmd.handle()); }
        uint32_t allocateQueryPair() { return nriGpuProfilerAllocateQueryPair(m_handle); }
        void writeTimestamp(const CommandBuffer& cmd, uint32_t queryId) { nriGpuProfilerWriteTimestamp(m_handle, cmd.handle(), queryId); }
        bool isPipelineStatisticsSupported() const { return nriGpuProfilerIsPipelineStatisticsSupported(m_handle); }
        uint32_t allocateStatisticsQuery() { return nriGpuProfilerAllocateStatisticsQuery(m_handle); }
        void beginStatistics(const CommandBuffer& cmd, uint32_t queryId) { nriGpuProfilerBeginStatistics(m_handle, cmd.handle(), queryId); }
        void endStatistics(const CommandBuffer& cmd, uint32_t queryId) { nriGpuProfilerEndStatistics(m_handle, cmd.handle(), queryId); }
        std::span<const GpuPipelineStatistics> getReadbackStatistics() const
        {
            uint32_t count = 0;
            const NriGpuPipelineStatistics* data = nriGpuProfilerGetReadbackStatistics(m_handle, &count);
            return { data, count };
        }
        bool isTransferQueueSupported() const { return nriGpuProfilerIsTransferQueueSupported(m_handle); }
        uint32_t beginTransferQuery(const CommandBuffer& cmd) { return nriGpuProfilerBeginTransferQuery(m_handle, cmd.handle()); }
        void endTransferQuery(const CommandBuffer& cmd, uint32_t queryId) { nriGpuProfilerEndTransferQuery(m_handle, cmd.handle(), queryId); }
        bool readTransferQuery(uint32_t queryId, uint64_t& outBeginTicks, uint64_t& outEndTicks)
        {
            return nriGpuProfilerReadTransferQuery(m_handle, queryId, &outBeginTicks, &outEndTicks);
        }

    private:
        NriGpuProfiler m_handle = nullptr;
    };

    inline GpuProfiler createGpuProfiler(const Device& device, uint32_t framesInFlight)
    {
        NriGpuProfiler gp = nullptr;
        checkResult(nriCreateGpuProfiler(device.handle(), framesInFlight, &gp), "nriCreateGpuProfiler");
        return GpuProfiler(gp);
    }

    inline DeviceNativeVK getDeviceNativeVK(const Device& device)
    {
        DeviceNativeVK out{};
        checkResult(nriGetDeviceNativeVK(device.handle(), &out), "nriGetDeviceNativeVK");
        return out;
    }

    inline DeviceNativeD3D12 getDeviceNativeD3D12(const Device& device)
    {
        DeviceNativeD3D12 out{};
        checkResult(nriGetDeviceNativeD3D12(device.handle(), &out), "nriGetDeviceNativeD3D12");
        return out;
    }

    // Mesh shader helpers
    inline void drawMeshTasks(const CommandBuffer& cmd, uint32_t gx, uint32_t gy = 1, uint32_t gz = 1)
    {
        nriCmdDrawMeshTasks(cmd.handle(), gx, gy, gz);
    }
    inline void drawMeshTasksIndirect(const CommandBuffer& cmd, const Buffer& indirectBuffer, uint64_t offset, uint32_t drawCount, uint32_t stride)
    {
        nriCmdDrawMeshTasksIndirect(cmd.handle(), indirectBuffer.handle(), offset, drawCount, stride);
    }
    inline void drawMeshTasksIndirectCount(const CommandBuffer& cmd, const Buffer& indirectBuffer, uint64_t offset,
                                           const Buffer& countBuffer, uint64_t countOffset, uint32_t maxDrawCount, uint32_t stride)
    {
        nriCmdDrawMeshTasksIndirectCount(cmd.handle(), indirectBuffer.handle(), offset, countBuffer.handle(), countOffset, maxDrawCount, stride);
    }

    // ImGui extension
    using ImguiVertex = NriImguiVertex;
    using ImguiDrawCmd = NriImguiDrawCmd;
    using ImguiDrawList = NriImguiDrawList;
    using ImguiDrawDataDesc = NriImguiDrawDataDesc;
    using ImguiViewportDesc = NriImguiViewportDesc;

    struct ImguiDesc
    {
        const DescriptorHeap* resourceHeap = nullptr;
        const DescriptorHeap* samplerHeap = nullptr;
        uint32_t samplerSlot = 0;
        ImageFormat colorFormat = NRI_FORMAT_SURFACE;
        uint32_t framesInFlight = 2;
        bool linearColorOutput = true;
    };

    class Imgui
    {
    public:
        Imgui() = default;
        explicit Imgui(NriImgui handle) : m_handle(handle) {}
        ~Imgui() { reset(); }

        Imgui(const Imgui&) = delete;
        Imgui& operator=(const Imgui&) = delete;
        Imgui(Imgui&& other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}
        Imgui& operator=(Imgui&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                m_handle = std::exchange(other.m_handle, nullptr);
            }
            return *this;
        }

        void reset(NriImgui h = nullptr)
        {
            if (m_handle)
                nriDestroyImgui(m_handle);
            m_handle = h;
        }
        NriImgui release() { return std::exchange(m_handle, nullptr); }
        NriImgui handle() const { return m_handle; }
        explicit operator bool() const { return m_handle != nullptr; }

        uint64_t createTexture(uint32_t width, uint32_t height, const void* rgbaPixels) const
        {
            uint64_t outId = 0;
            checkResult(nriImguiCreateTexture(m_handle, width, height, rgbaPixels, &outId), "nriImguiCreateTexture");
            return outId;
        }

        void updateTexture(uint64_t textureId, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                           const void* fullRgbaPixels, uint32_t texWidth, uint32_t texHeight) const
        {
            checkResult(nriImguiUpdateTexture(m_handle, textureId, x, y, width, height, fullRgbaPixels, texWidth, texHeight),
                        "nriImguiUpdateTexture");
        }

        void destroyTexture(uint64_t textureId) const
        {
            nriImguiDestroyTexture(m_handle, textureId);
        }

        void copyData(const CommandBuffer& cmd, const NriImguiDrawDataDesc& drawData) const
        {
            nriCmdCopyImguiData(cmd.handle(), m_handle, &drawData);
        }

        void draw(const CommandBuffer& cmd, const NriImguiDrawDataDesc& drawData) const
        {
            nriCmdDrawImgui(cmd.handle(), m_handle, &drawData);
        }

        uint64_t createViewport(const NriImguiViewportDesc& desc) const
        {
            uint64_t handle = 0;
            checkResult(nriImguiCreateViewport(m_handle, &desc, &handle), "nriImguiCreateViewport");
            return handle;
        }

        void resizeViewport(uint64_t viewportHandle, uint32_t width, uint32_t height) const
        {
            nriImguiResizeViewport(m_handle, viewportHandle, width, height);
        }

        void destroyViewport(uint64_t viewportHandle) const
        {
            nriImguiDestroyViewport(m_handle, viewportHandle);
        }

        uint32_t getViewportSlot(uint64_t viewportHandle) const
        {
            return nriImguiGetViewportSlot(m_handle, viewportHandle);
        }

        void renderViewport(uint64_t viewportHandle, const NriImguiDrawDataDesc& drawData,
                            ClearColor clearColor = { 0.0f, 0.0f, 0.0f, 1.0f }) const
        {
            nriImguiRenderViewport(m_handle, viewportHandle, &drawData, clearColor.r, clearColor.g, clearColor.b, clearColor.a);
        }

        void presentViewport(uint64_t viewportHandle) const
        {
            nriImguiPresentViewport(m_handle, viewportHandle);
        }

    private:
        NriImgui m_handle = nullptr;
    };

    inline Imgui createImgui(const Device& device, const ImguiDesc& desc = {})
    {
        const NriImguiDesc cDesc{
            .resourceHeap = desc.resourceHeap ? desc.resourceHeap->handle() : nullptr,
            .samplerHeap = desc.samplerHeap ? desc.samplerHeap->handle() : nullptr,
            .samplerSlot = desc.samplerSlot,
            .colorFormat = desc.colorFormat,
            .framesInFlight = desc.framesInFlight,
            .linearColorOutput = desc.linearColorOutput
        };
        NriImgui imgui = nullptr;
        checkResult(nriCreateImgui(device.handle(), &cDesc, &imgui), "nriCreateImgui");
        return Imgui(imgui);
    }

    // Upscaler extension (Streamline / DLSS Super Resolution / DLSS-RR)
    using UpscalerType = NriUpscalerType;
    using UpscalerMode = NriUpscalerMode;
    using UpscalerDesc = NriUpscalerDesc;
    using UpscalerOptimalSettings = NriUpscalerOptimalSettings;
    using UpscalerEvaluateDesc = NriUpscalerEvaluateDesc;

    inline bool isStreamlineInitialized(const Device& device) { return nriIsStreamlineInitialized(device.handle()); }
    inline bool isUpscalerSupported(const Device& device, UpscalerType type) { return nriIsUpscalerSupported(device.handle(), type); }

    class Upscaler
    {
    public:
        Upscaler() = default;
        explicit Upscaler(NriUpscaler handle) : m_handle(handle) {}
        ~Upscaler() { reset(); }

        Upscaler(const Upscaler&) = delete;
        Upscaler& operator=(const Upscaler&) = delete;
        Upscaler(Upscaler&& other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}
        Upscaler& operator=(Upscaler&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                m_handle = std::exchange(other.m_handle, nullptr);
            }
            return *this;
        }

        void reset(NriUpscaler h = nullptr)
        {
            if (m_handle)
                nriDestroyUpscaler(m_handle);
            m_handle = h;
        }
        NriUpscaler release() { return std::exchange(m_handle, nullptr); }
        NriUpscaler handle() const { return m_handle; }
        explicit operator bool() const { return m_handle != nullptr; }

        UpscalerOptimalSettings getOptimalSettings(UpscalerMode mode, Extent2D outputSize) const
        {
            UpscalerOptimalSettings out{};
            checkResult(nriUpscalerGetOptimalSettings(m_handle, mode, outputSize, &out), "nriUpscalerGetOptimalSettings");
            return out;
        }

        void resetViewport() const { nriUpscalerReset(m_handle); }

        bool evaluate(const CommandBuffer& cmd, const UpscalerEvaluateDesc& desc) const
        {
            return nriCmdEvaluateUpscaler(cmd.handle(), m_handle, &desc);
        }

    private:
        NriUpscaler m_handle = nullptr;
    };

    inline Upscaler createUpscaler(const Device& device, UpscalerType type = NRI_UPSCALER_TYPE_DLSS_SR)
    {
        const NriUpscalerDesc desc{ .type = type };
        NriUpscaler up = nullptr;
        checkResult(nriCreateUpscaler(device.handle(), &desc, &up), "nriCreateUpscaler");
        return Upscaler(up);
    }

    // Denoiser extension (NVIDIA Real-Time Denoisers: SIGMA / REBLUR / RELAX)
    using DenoiserMethod = NriDenoiserMethod;
    using DenoiserInstanceDesc = NriDenoiserInstanceDesc;
    using DenoiserCommonSettings = NriDenoiserCommonSettings;
    using DenoiserSigmaSettings = NriDenoiserSigmaSettings;
    using DenoiserReblurSettings = NriDenoiserReblurSettings;
    using DenoiserRelaxSettings = NriDenoiserRelaxSettings;
    using DenoiserResourcesDesc = NriDenoiserResourcesDesc;

    struct DenoiserDesc
    {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<DenoiserInstanceDesc> instances;
        uint32_t queuedFrameNum = 3;
        bool enableWholeLifetimeDescriptorCaching = false;
    };

    inline bool isDenoiserAvailable(const Device& device) { return nriIsDenoiserAvailable(device.handle()); }

    class Denoiser
    {
    public:
        Denoiser() = default;
        explicit Denoiser(NriDenoiser handle) : m_handle(handle) {}
        ~Denoiser() { reset(); }

        Denoiser(const Denoiser&) = delete;
        Denoiser& operator=(const Denoiser&) = delete;
        Denoiser(Denoiser&& other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}
        Denoiser& operator=(Denoiser&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                m_handle = std::exchange(other.m_handle, nullptr);
            }
            return *this;
        }

        void reset(NriDenoiser h = nullptr)
        {
            if (m_handle)
                nriDestroyDenoiser(m_handle);
            m_handle = h;
        }
        NriDenoiser release() { return std::exchange(m_handle, nullptr); }
        NriDenoiser handle() const { return m_handle; }
        explicit operator bool() const { return m_handle != nullptr; }

        void resize(uint32_t width, uint32_t height) const
        {
            checkResult(nriResizeDenoiser(m_handle, width, height), "nriResizeDenoiser");
        }

        bool beginFrame(const DenoiserCommonSettings& settings) const
        {
            return nriDenoiserBeginFrame(m_handle, &settings);
        }

        void setSigmaSettings(uint32_t identifier, const DenoiserSigmaSettings& settings) const
        {
            nriDenoiserSetSigmaSettings(m_handle, identifier, &settings);
        }

        void setReblurSettings(uint32_t identifier, const DenoiserReblurSettings& settings) const
        {
            nriDenoiserSetReblurSettings(m_handle, identifier, &settings);
        }

        void setRelaxSettings(uint32_t identifier, const DenoiserRelaxSettings& settings) const
        {
            nriDenoiserSetRelaxSettings(m_handle, identifier, &settings);
        }

        bool denoise(const CommandBuffer& cmd, std::span<const uint32_t> identifiers, const DenoiserResourcesDesc& resources) const
        {
            return nriCmdDenoise(cmd.handle(), m_handle, identifiers.data(), static_cast<uint32_t>(identifiers.size()), &resources);
        }

        bool denoise(const CommandBuffer& cmd, uint32_t identifier, const DenoiserResourcesDesc& resources) const
        {
            return nriCmdDenoise(cmd.handle(), m_handle, &identifier, 1, &resources);
        }

    private:
        NriDenoiser m_handle = nullptr;
    };

    inline Denoiser createDenoiser(const Device& device, const DenoiserDesc& desc)
    {
        const NriDenoiserDesc cDesc{
            .width = desc.width,
            .height = desc.height,
            .instances = desc.instances.empty() ? nullptr : desc.instances.data(),
            .instanceCount = static_cast<uint32_t>(desc.instances.size()),
            .queuedFrameNum = desc.queuedFrameNum,
            .enableWholeLifetimeDescriptorCaching = desc.enableWholeLifetimeDescriptorCaching
        };
        NriDenoiser den = nullptr;
        checkResult(nriCreateDenoiser(device.handle(), &cDesc, &den), "nriCreateDenoiser");
        return Denoiser(den);
    }
}
