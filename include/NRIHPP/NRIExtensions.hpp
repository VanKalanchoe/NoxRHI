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
            .logUserData = desc.logUserData
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
}
