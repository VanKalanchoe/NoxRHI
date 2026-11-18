#pragma once
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "VulkanCommon.h"
#include "vk_mem_alloc_raii.hpp" // released sparse page memory (m_sparseRetired)

#include "../Device.h"

namespace NRI
{
    class MemoryAllocatorVK;
    class CommandBufferVK;
    class ShaderCompiler;

    class DeviceVK final : public Device
    {
    public:
        explicit DeviceVK(const DeviceInitDesc& desc);
        ~DeviceVK() override;
        void shutdown() override;

        bool isDeviceInit() const { return m_deviceInitialized; }
        const std::string& getShaderCacheDir() const override { return m_shaderCacheDir; }
        StreamlineContext* getStreamlineContext() const override { return m_streamline.get(); }
        void* getStreamlinePresentFn() const;

        MemoryAllocatorVK& getAllocator() { return *m_allocator; }

        vk::raii::Context& getContext() { return m_context; }
        vk::raii::Instance& getInstance() { return m_instance; }
        vk::raii::PhysicalDevice& getPhysicalDevice() { return m_physicalDevice; }
        vk::SampleCountFlagBits& getMSAASamples() { return m_msaaSamples; }
        uint32_t getMSAASampleCount() const override { return static_cast<uint32_t>(m_msaaSamples); }
        vk::raii::Device& getDevice() { return m_device; }
        bool isShaderObjectExtensionEnabled() const { return m_shaderObjectsEnabled; }
        bool isPipelineStatisticsEnabled() const { return m_pipelineStatisticsEnabled; }
        bool isMemoryBudgetSupported() const override { return m_memoryBudgetEnabled; }
        bool supportsSparseBuffers() const override { return m_sparseQueue != nullptr; }
        uint64_t getSparseBufferReserveLimit() const override { return m_sparseReserveLimit; }
        bool updateSparseBuffer(Buffer& buffer, std::span<const SparsePageRange> commits, std::span<const SparsePageRange> releases) override;
        TimelinePoint getSparseTimelinePoint() const override { return { m_sparseTimeline.get(), m_sparseValue }; }
        enum class CalibratedTimestamps : uint8_t { None, KHR, EXT };
        CalibratedTimestamps getCalibratedTimestamps() const { return m_calibratedTimestamps; }
        vk::TimeDomainKHR getHostTimeDomain() const { return m_hostTimeDomain; }
        bool isDebugUtilsEnabled() const { return m_debugUtilsEnabled; }
        uint32_t getQueueIndex() const { return m_queueIndex; }
        uint32_t getTransferQueueIndex() const { return m_transferQueueIndex; }
        uint32_t getComputeQueueIndex() const { return m_computeQueueIndex; }
        vk::raii::Queue& getQueue() { return m_queue; }
        vk::raii::Queue& getTransferQueue() { return m_transferQueue; }
        vk::raii::Queue& getComputeQueue() { return m_computeQueue; }
        uint32_t getQueueFamily(QueueType queue) const
        {
            if (queue == QueueType::Transfer)
                return m_transferQueueIndex;
            return queue == QueueType::Compute && hasAsyncComputeQueue() ? m_computeQueueIndex : m_queueIndex;
        }
        std::span<const uint32_t> getSharedQueueFamilies() const { return { m_sharedQueueFamilies.data(), m_sharedQueueFamilyCount }; }
        bool hasDedicatedTransferQueue() const override { return m_transferQueueIndex != m_queueIndex; }
        bool hasAsyncComputeQueue() const override { return m_computeQueueIndex != ~0u; }
        GraphicsAPI getAPI() const override { return GraphicsAPI::Vulkan; }
        vk::raii::SurfaceKHR& getSurface() { return m_surface; }
        vk::SurfaceKHR getActiveSurface() const { return m_externalSurface ? m_externalSurface : *m_surface; }
        vk::Format& getDepthFormat() { return m_depthFormat; }
        vk::SurfaceFormatKHR& getSurfaceFormat() { return m_surfaceFormat; }
        void finishPipelineBuilds() override;
        bool isSurfaceFormatBGRA() const override
        {
            return m_surfaceFormat.format == vk::Format::eB8G8R8A8Srgb || m_surfaceFormat.format == vk::Format::eB8G8R8A8Unorm;
        }

        vk::raii::ImageView createImageView(vk::Image const& image, vk::Format format, vk::ImageAspectFlags aspectFlags, uint32_t mipLevels);
        uint32_t findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties);
        void submitAndWait(CommandBuffer& cmdBuffer, uint32_t slotIndex) override;
        void submitCommandBuffers(std::span<CommandBuffer* const> cmdBuffers, Swapchain& swapchain, uint32_t frameIndex,
                                  uint32_t imageIndex, std::span<const TimelinePoint> timelineWaits) override;
        void submit(QueueType queue, std::span<CommandBuffer* const> cmdBuffers, std::span<const TimelinePoint> waits,
                    std::span<const TimelinePoint> signals) override;
        void waitIdle() override;
        uint32_t getShaderGroupHandleSize() const override { return m_rtPipelineProperties.shaderGroupHandleSize; }
        uint32_t getShaderGroupBaseAlignment() const override { return m_rtPipelineProperties.shaderGroupBaseAlignment; }
        const vk::PhysicalDeviceRayTracingPipelinePropertiesKHR& getRayTracingPipelineProperties() const { return m_rtPipelineProperties; }

        // Factory
        std::unique_ptr<Swapchain> createSwapchain(const SwapchainDesc& desc) override;
        std::unique_ptr<Pipeline> createPipeline(const PipelineDesc& desc, ShaderCompiler& compiler) override;
        void precompileShaderModules(const std::vector<std::string>& sources, ShaderCompiler& compiler) override;
        bool compileShaderModules(const std::vector<std::string>& sources, ShaderCompiler& compiler) override;
        std::unique_ptr<CommandAllocator> createCommandAllocator(CommandBufferReset resetMode, QueueType queue) override;
        std::unique_ptr<TimelineSemaphore> createTimelineSemaphore(uint64_t initialValue) override;
        std::unique_ptr<Texture2D> createTexture(const TextureDesc& desc) override;
        std::unique_ptr<Buffer> createBuffer(const BufferDesc& desc) override;
        std::unique_ptr<DescriptorHeap> createDescriptorHeap(const DescriptorHeapDesc& desc) override;
        AccelerationStructureBuildSizes getAccelerationStructureBuildSizes(const AccelerationStructureBuildDesc& desc) override;
        std::unique_ptr<AccelerationStructure> createAccelerationStructure(const AccelerationStructureDesc& desc) override;
        std::unique_ptr<QueryPool> createQueryPool(const QueryPoolDesc& desc) override;
        std::unique_ptr<GpuProfiler> createGpuProfiler(uint32_t framesInFlight) override;

        void beginFrame(uint32_t frameNumber) override;
        void getMemoryStats(std::vector<MemoryHeapStats>& outHeaps) const override;
        uint64_t getTextureBytes() const override { return m_textureBytes.load(std::memory_order_relaxed); }
        void addTextureBytes(int64_t bytes) { m_textureBytes.fetch_add(static_cast<uint64_t>(bytes), std::memory_order_relaxed); }

        QueueTimelineValues getSubmittedTimelineValues() const override;
        QueueTimelineValues getCompletedTimelineValues() const override;

    private:
        void initVulkan(const DeviceInitDesc& desc);
        void createInstance(const DeviceInitDesc& desc);
        void setupDebugMessenger();
        void createSurface(const NativeWindowHandle& window);
        bool isDeviceSuitable(vk::raii::PhysicalDevice const& physicalDevice);
        void pickPhysicalDevice();
        void createLogicalDevice(const DeviceInitDesc& desc);
        void initDeviceCapabilities();
        vk::SampleCountFlagBits getMaxUsableSampleCount();
        std::vector<char const*> getRequiredInstanceExtensions(const DeviceInitDesc& desc);
        static vk::Bool32 debugCallback(vk::DebugUtilsMessageSeverityFlagBitsEXT severity,
                                        vk::DebugUtilsMessageTypeFlagsEXT type,
                                        const vk::DebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                        void* pUserData);
        vk::Format findSupportedFormat(const std::vector<vk::Format>& candidates, vk::ImageTiling tiling, vk::FormatFeatureFlags features);
        vk::Format findDepthFormat();
        vk::SurfaceFormatKHR chooseSurfaceFormat();
        vk::SurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<vk::SurfaceFormatKHR>& availableFormats);
        void freeRetiredSparseMemory();

    private:
        bool m_deviceInitialized = false;
        bool m_headless = false;
        bool m_validationEnabled = false;
        std::string m_shaderCacheDir = "shader_cache";
        std::unique_ptr<StreamlineContext> m_streamline;

        vk::raii::Context m_context;
        vk::raii::Instance m_instance = nullptr;
        vk::raii::DebugUtilsMessengerEXT m_debugMessenger = nullptr;
        vk::raii::PhysicalDevice m_physicalDevice = nullptr;
        vk::SampleCountFlagBits m_msaaSamples = vk::SampleCountFlagBits::e1;
        vk::raii::Device m_device = nullptr;
        bool m_shaderObjectsEnabled = false;
        bool m_pipelineStatisticsEnabled = false;
        bool m_memoryBudgetEnabled = false;
        CalibratedTimestamps m_calibratedTimestamps = CalibratedTimestamps::None;
        vk::TimeDomainKHR m_hostTimeDomain = vk::TimeDomainKHR::eDevice;
        std::atomic<uint64_t> m_textureBytes{ 0 };
        bool m_debugUtilsEnabled = false;
        uint32_t m_queueIndex = ~0;
        vk::raii::Queue m_queue = nullptr;
        uint32_t m_transferQueueIndex = ~0;
        vk::raii::Queue m_transferQueue = nullptr;
        uint32_t m_computeQueueIndex = ~0u;
        vk::raii::Queue* m_sparseQueue = nullptr;
        bool m_sparseFeatures = false;
        uint64_t m_sparseReserveLimit = 0;
        std::unique_ptr<TimelineSemaphore> m_sparseTimeline;
        uint64_t m_sparseValue = 0;
        std::vector<std::pair<std::vector<vma::raii::Allocation>, uint64_t>> m_sparseRetired;
        std::vector<vk::SparseMemoryBind> m_sparseBindScratch;

        vk::raii::Queue m_computeQueue = nullptr;
        std::array<uint32_t, 3> m_sharedQueueFamilies{};
        uint32_t m_sharedQueueFamilyCount = 1;
        vk::raii::Fence m_submitFence = nullptr;
        std::vector<vk::CommandBufferSubmitInfo> m_submitScratch;
        std::vector<vk::SemaphoreSubmitInfo> m_waitScratch;
        std::vector<vk::SemaphoreSubmitInfo> m_signalScratch;

        // Per-queue timeline semaphores for deferred resource deletion [Graphics, Transfer, Compute]
        std::array<vk::raii::Semaphore, 3> m_retirementSemaphores{ nullptr, nullptr, nullptr };
        std::array<std::atomic<uint64_t>, 3> m_submittedValues{};

        vk::raii::SurfaceKHR m_surface = nullptr;
        vk::SurfaceKHR m_externalSurface = {};
        vk::Format m_depthFormat = vk::Format::eUndefined;
        vk::SurfaceFormatKHR m_surfaceFormat = {};

        std::unique_ptr<MemoryAllocatorVK> m_allocator;

        const std::vector<char const*> validationLayers = {
            "VK_LAYER_KHRONOS_validation"
        };

        vk::PhysicalDeviceRayTracingPipelinePropertiesKHR m_rtPipelineProperties{};
    };
}
