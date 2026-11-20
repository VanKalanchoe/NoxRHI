#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../Device.h"

namespace NRI
{
    /**
     * Null / No-GPU RHI device backend (NRI_GRAPHICS_API_NONE) for dedicated servers
     * and non-graphical runtimes where Vulkan/D3D12 drivers or GPUs may be absent.
     */
    class DeviceNone final : public Device
    {
    public:
        explicit DeviceNone(const DeviceInitDesc& desc);
        ~DeviceNone() override;

        GraphicsAPI getAPI() const override { return GraphicsAPI::None; }

        std::unique_ptr<Swapchain> createSwapchain(const SwapchainDesc& desc) override;
        std::unique_ptr<Pipeline> createPipeline(const PipelineDesc& desc, ShaderCompiler& compiler) override;
        std::unique_ptr<Pipeline> createPipelineNoCompiler(const PipelineDesc& desc) override;
        std::unique_ptr<CommandAllocator> createCommandAllocator(CommandBufferReset resetMode, QueueType queue) override;
        std::unique_ptr<TimelineSemaphore> createTimelineSemaphore(uint64_t initialValue) override;
        bool hasDedicatedTransferQueue() const override { return false; }
        bool hasAsyncComputeQueue() const override { return false; }
        std::unique_ptr<Texture2D> createTexture(const TextureDesc& desc) override;
        std::unique_ptr<Buffer> createBuffer(const BufferDesc& desc) override;
        std::unique_ptr<DescriptorHeap> createDescriptorHeap(const DescriptorHeapDesc& desc) override;
        AccelerationStructureBuildSizes getAccelerationStructureBuildSizes(const AccelerationStructureBuildDesc& desc) override;
        std::unique_ptr<AccelerationStructure> createAccelerationStructure(const AccelerationStructureDesc& desc) override;
        std::unique_ptr<QueryPool> createQueryPool(const QueryPoolDesc& desc) override;
        std::unique_ptr<GpuProfiler> createGpuProfiler(uint32_t framesInFlight) override;

        void beginFrame(uint32_t frameNumber) override;
        const std::string& getShaderCacheDir() const override { return m_shaderCacheDir; }
        bool isMemoryBudgetSupported() const override { return false; }
        bool supportsMeshShaders() const override { return false; }
        void getMemoryStats(std::vector<MemoryHeapStats>& outHeaps) const override;
        uint64_t getTextureBytes() const override { return m_textureBytes.load(std::memory_order_relaxed); }
        void addTextureBytes(int64_t delta) { m_textureBytes.fetch_add(static_cast<uint64_t>(delta), std::memory_order_relaxed); }

        void shutdown() override;
        uint32_t getMSAASampleCount() const override { return 1; }
        void submitAndWait(CommandBuffer& cmdBuffer, uint32_t slotIndex = 0) override;
        void submitCommandBuffers(std::span<CommandBuffer* const> cmdBuffers, Swapchain& swapchain, uint32_t frameIndex,
                                  uint32_t imageIndex, std::span<const TimelinePoint> timelineWaits = {}) override;
        void submit(QueueType queue, std::span<CommandBuffer* const> cmdBuffers, std::span<const TimelinePoint> waits,
                    std::span<const TimelinePoint> signals) override;
        void waitIdle() override;

    private:
        std::string m_shaderCacheDir;
        std::atomic<uint64_t> m_textureBytes{ 0 };
    };
}
