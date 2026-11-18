#pragma once
#include <memory>
#include <span>
#include <vector>

#include "NRITypes.h"
#include "DeferredDeletionQueue.h"
#include "Swapchain.h"
#include "Pipeline.h"
#include "CommandAllocator.h"
#include "CommandBuffer.h"
#include "Texture.h"
#include "Buffer.h"
#include "DescriptorHeap.h"
#include "AccelerationStructure.h"
#include "GpuProfiler.h"
#include "TimelineSemaphore.h"
#include "QueryPool.h"

namespace NRI
{
    class ShaderCompiler;

    class Device
    {
    public:
        static std::unique_ptr<Device> create(const DeviceInitDesc& desc);

        virtual std::unique_ptr<Swapchain> createSwapchain(const SwapchainDesc& desc) = 0;
        virtual std::unique_ptr<Pipeline> createPipeline(const PipelineDesc& desc, ShaderCompiler& compiler) = 0;
        virtual void precompileShaderModules(const std::vector<std::string>& sources, ShaderCompiler& compiler) {}
        virtual bool compileShaderModules(const std::vector<std::string>& sources, ShaderCompiler& compiler) { return true; }
        virtual std::unique_ptr<CommandAllocator> createCommandAllocator(CommandBufferReset resetMode, QueueType queue = QueueType::Graphics) = 0;
        virtual std::unique_ptr<TimelineSemaphore> createTimelineSemaphore(uint64_t initialValue = 0) = 0;
        virtual bool hasDedicatedTransferQueue() const = 0;
        virtual bool hasAsyncComputeQueue() const { return false; }
        virtual std::unique_ptr<Texture2D> createTexture(const TextureDesc& desc) = 0;
        virtual std::unique_ptr<Buffer> createBuffer(const BufferDesc& desc) = 0;
        virtual std::unique_ptr<DescriptorHeap> createDescriptorHeap(const DescriptorHeapDesc& desc) = 0;
        virtual AccelerationStructureBuildSizes getAccelerationStructureBuildSizes(const AccelerationStructureBuildDesc& desc) = 0;
        virtual std::unique_ptr<AccelerationStructure> createAccelerationStructure(const AccelerationStructureDesc& desc) = 0;
        virtual std::unique_ptr<QueryPool> createQueryPool(const QueryPoolDesc& desc) = 0;
        virtual std::unique_ptr<GpuProfiler> createGpuProfiler(uint32_t framesInFlight) = 0;

        virtual void beginFrame(uint32_t frameNumber) = 0;
        virtual bool supportsSparseBuffers() const { return false; }
        virtual uint64_t getSparseBufferReserveLimit() const { return 0; }
        virtual bool updateSparseBuffer(Buffer& buffer, std::span<const SparsePageRange> commits, std::span<const SparsePageRange> releases)
        {
            return false;
        }
        virtual TimelinePoint getSparseTimelinePoint() const { return {}; }
        virtual bool isSurfaceFormatBGRA() const { return false; }
        virtual void finishPipelineBuilds() {}
        virtual const std::string& getShaderCacheDir() const
        {
            static const std::string empty;
            return empty;
        }
        virtual bool isMemoryBudgetSupported() const = 0;
        virtual void getMemoryStats(std::vector<MemoryHeapStats>& outHeaps) const = 0;
        virtual uint64_t getTextureBytes() const = 0;

        virtual void shutdown() = 0;
        virtual uint32_t getMSAASampleCount() const = 0;
        virtual void submitAndWait(CommandBuffer& cmdBuffer, uint32_t slotIndex = 0) = 0;
        virtual void submitCommandBuffers(std::span<CommandBuffer* const> cmdBuffers, Swapchain& swapchain, uint32_t frameIndex,
                                          uint32_t imageIndex, std::span<const TimelinePoint> timelineWaits = {}) = 0;
        virtual void submit(QueueType queue, std::span<CommandBuffer* const> cmdBuffers, std::span<const TimelinePoint> waits,
                            std::span<const TimelinePoint> signals) = 0;
        virtual void waitIdle() = 0;

        virtual uint32_t getShaderGroupHandleSize() const { return 32; }
        virtual uint32_t getShaderGroupBaseAlignment() const { return 64; }
        virtual GraphicsAPI getAPI() const = 0;

        // Timeline-safe deferred deletion support
        virtual QueueTimelineValues getSubmittedTimelineValues() const { return {}; }
        virtual QueueTimelineValues getCompletedTimelineValues() const { return {}; }
        virtual bool isShuttingDown() const { return m_shuttingDown; }

        template <typename Fn>
        void enqueueDeferredDeletion(Fn&& fn)
        {
            if (m_shuttingDown)
            {
                fn();
                return;
            }
            m_deletionQueue.enqueue(getSubmittedTimelineValues(), std::forward<Fn>(fn));
        }

        void collectDeferredDeletions()
        {
            m_deletionQueue.collect(getCompletedTimelineValues());
        }

        void drainDeferredDeletions()
        {
            m_deletionQueue.drain();
        }

        virtual ~Device() = default;

    protected:
        DeferredDeletionQueue m_deletionQueue;
        bool m_shuttingDown = false;
    };
}
