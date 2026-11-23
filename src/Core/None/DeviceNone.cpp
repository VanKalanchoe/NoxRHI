#include "DeviceNone.h"
#include "../Log.h"

#include <algorithm>
#include <cstring>

namespace NRI
{
    namespace
    {
        class TimelineSemaphoreNone final : public TimelineSemaphore
        {
        public:
            explicit TimelineSemaphoreNone(uint64_t initialValue)
                : m_value(initialValue)
            {
            }

            uint64_t getValue() const override
            {
                return m_value.load(std::memory_order_relaxed);
            }

            void wait(uint64_t value) const override
            {
                (void)value;
            }

            void advanceTo(uint64_t value)
            {
                uint64_t cur = m_value.load(std::memory_order_relaxed);
                while (cur < value && !m_value.compare_exchange_weak(cur, value, std::memory_order_relaxed))
                {
                }
            }

        private:
            std::atomic<uint64_t> m_value{ 0 };
        };

        class BufferNone final : public Buffer
        {
        public:
            explicit BufferNone(const BufferDesc& desc)
                : m_desc(desc)
            {
                if (isHostMappable(desc.usage) && !desc.sparse && desc.size > 0 && desc.size <= kMaxAutoCpuBackingBytes)
                    m_data.resize(static_cast<size_t>(desc.size));
            }

            void* map(uint64_t offset, uint64_t size) override
            {
                if (m_desc.sparse || m_desc.size == 0 || offset >= m_desc.size)
                    return nullptr;
                const uint64_t avail = m_desc.size - offset;
                if (size > 0 && size > avail)
                    return nullptr;
                if (m_data.size() < m_desc.size)
                    m_data.resize(static_cast<size_t>(m_desc.size));
                return m_data.data() + offset;
            }

            void unmap() override {}

            uint64_t getDeviceAddress() const override
            {
                return m_data.empty() ? 0ull : reinterpret_cast<uint64_t>(m_data.data());
            }

            uint64_t getSize() const override { return m_desc.size; }
            uint64_t getSparsePageSize() const override { return m_desc.sparse ? 65536ull : 0ull; }

            std::vector<uint8_t>* tryGetCpuBacking()
            {
                if (m_desc.sparse || m_desc.size == 0)
                    return nullptr;
                if (m_data.size() < m_desc.size && m_desc.size <= kMaxAutoCpuBackingBytes)
                    m_data.resize(static_cast<size_t>(m_desc.size));
                return m_data.empty() ? nullptr : &m_data;
            }

        private:
            static constexpr uint64_t kMaxAutoCpuBackingBytes = 64ull * 1024ull * 1024ull;

            static bool isHostMappable(BufferUsage usage)
            {
                switch (usage)
                {
                case BufferUsage::Staging:
                case BufferUsage::Uniform:
                case BufferUsage::DescriptorHeap:
                case BufferUsage::AccelerationStructureInstance:
                case BufferUsage::ShaderBindingTable:
                case BufferUsage::Readback:
                    return true;
                default:
                    return false;
                }
            }

            BufferDesc m_desc{};
            std::vector<uint8_t> m_data;
        };

        class TextureNone final : public Texture2D
        {
        public:
            TextureNone(DeviceNone& device, const TextureDesc& desc)
                : m_device(device)
                , m_desc(desc)
                , m_estimatedBytes(estimateTextureBytes(desc))
            {
                m_device.addTextureBytes(static_cast<int64_t>(m_estimatedBytes));
            }

            ~TextureNone() override
            {
                m_device.addTextureBytes(-static_cast<int64_t>(m_estimatedBytes));
            }

            uint32_t GetWidth() const override { return m_desc.width; }
            uint32_t GetHeight() const override { return m_desc.height; }
            uint32_t GetDescriptorIndexSlot() const override { return m_descriptorSlot; }
            void setDescriptorSlot(uint32_t slot) { m_descriptorSlot = slot; }

            void uploadFromBuffer(CommandBuffer&, Buffer&, uint32_t, uint32_t, uint32_t, const std::vector<size_t>&) override {}
            void recordUpload(CommandBuffer&, Buffer&, uint64_t, const std::vector<size_t>&) override {}
            void recordInitialLayout(CommandBuffer&) override {}
            void recordRegionUploads(CommandBuffer&, Buffer&, std::span<const TextureRegionUpload>) override {}
            void copyImageToBuffer(CommandBuffer&, Buffer&, uint32_t, uint32_t, uint32_t, uint32_t) override {}
            void blitTo(CommandBuffer&, Texture2D&) override {}
            void generateMipmaps(CommandBuffer&) override {}

            uint32_t getMipLevels() const override { return m_desc.mipLevels; }
            uint32_t getArrayLayers() const override { return m_desc.arrayLayers; }
            TextureUsage getUsage() const override { return m_desc.usage; }

        private:
            DeviceNone& m_device;
            TextureDesc m_desc{};
            uint64_t m_estimatedBytes = 0;
            uint32_t m_descriptorSlot = 0;
        };

        class DescriptorHeapNone final : public DescriptorHeap
        {
        public:
            void registerTexture(Texture& texture, TextureUsage) override
            {
                if (auto* texNone = dynamic_cast<TextureNone*>(&texture))
                    texNone->setDescriptorSlot(m_nextImageSlot.fetch_add(1, std::memory_order_relaxed));
            }

            uint32_t registerStorageTextureMip(Texture&, uint32_t) override
            {
                return m_nextImageSlot.fetch_add(1, std::memory_order_relaxed);
            }

            void unregisterTexture(uint32_t) override {}

            uint32_t registerBuffer(Buffer&, uint64_t) override
            {
                return m_nextBufferSlot.fetch_add(1, std::memory_order_relaxed);
            }

            void unregisterBuffer(uint32_t) override {}

            uint32_t registerAccelerationStructure(AccelerationStructure&, uint32_t slot) override
            {
                return slot != ~0u ? slot : m_nextBufferSlot.fetch_add(1, std::memory_order_relaxed);
            }

            uint32_t getImageHeapIndexOffset() const override { return 0; }
            uint64_t getSize() const override { return 0; }
            uint64_t getReservedRangeOffset() const override { return 0; }
            uint64_t getReservedRangeSize() const override { return 0; }
            uint64_t getGPUAddress() const override { return 0; }

        private:
            std::atomic<uint32_t> m_nextBufferSlot{ 0 };
            std::atomic<uint32_t> m_nextImageSlot{ 0 };
        };

        class AccelerationStructureNone final : public AccelerationStructure
        {
        public:
            explicit AccelerationStructureNone(const AccelerationStructureDesc& desc)
                : m_desc(desc)
            {
            }

            uint64_t getDeviceAddress() const override { return 0; }
            uint64_t getSize() const override { return m_desc.size; }
            AccelerationStructureType getType() const override { return m_desc.type; }
            Buffer* getBuffer() const override { return m_desc.storageBuffer; }

        private:
            AccelerationStructureDesc m_desc{};
        };

        class QueryPoolNone final : public QueryPool
        {
        public:
            explicit QueryPoolNone(uint32_t capacity)
                : m_capacity(capacity)
            {
            }

            uint32_t getCapacity() const override { return m_capacity; }

            bool getResults(uint32_t first, std::span<uint64_t> out) const override
            {
                if (first + out.size() > m_capacity)
                    return false;
                std::fill(out.begin(), out.end(), 0ull);
                return true;
            }

        private:
            uint32_t m_capacity = 0;
        };

        class GpuProfilerNone final : public GpuProfiler
        {
        public:
            bool isSupported() const override { return false; }
            float getTimestampPeriod() const override { return 1.0f; }
            uint32_t getTimestampValidBits() const override { return 64; }
            bool calibrate(uint64_t&, int64_t&) override { return false; }

            void beginFrame(uint32_t) override {}
            std::span<const GpuTimestamp> getReadbackTimestamps() const override { return {}; }
            void resetQueries(CommandBuffer&) override {}

            uint32_t allocateQueryPair() override { return InvalidQueryId; }
            void writeTimestamp(CommandBuffer&, uint32_t) override {}

            bool isPipelineStatisticsSupported() const override { return false; }
            uint32_t allocateStatisticsQuery() override { return InvalidQueryId; }
            void beginStatistics(CommandBuffer&, uint32_t) override {}
            void endStatistics(CommandBuffer&, uint32_t) override {}
            std::span<const GpuPipelineStatistics> getReadbackStatistics() const override { return {}; }
        };

        class PipelineNone final : public Pipeline
        {
        };

        class SwapchainNone final : public Swapchain
        {
        public:
            explicit SwapchainNone(const SwapchainDesc& desc)
                : m_extent{ desc.Width, desc.Height }
            {
            }

            Extent2D getExtent() const override { return m_extent; }

            FrameResult acquireNextImage(uint32_t, uint32_t& outImageIndex) override
            {
                outImageIndex = 0;
                return FrameResult::Success;
            }

            FrameResult present(uint32_t, uint32_t) override
            {
                return FrameResult::Success;
            }

        private:
            Extent2D m_extent{};
        };

        class CommandBufferNone final : public CommandBuffer
        {
        public:
            void begin(uint32_t, bool) override {}
            void end(uint32_t) override {}
            void beginRendering(RenderDesc&) override {}
            void endRendering() override {}
            void bindPipeline(PipelineBindPoint, Pipeline&) override {}

            void setViewport(Extent2D) override {}
            void setScissor(Extent2D) override {}
            void setViewportWithCount(const ViewportBounds&, float, float) override {}
            void setScissorWithCount(const Extent2D&) override {}
            void setScissorRect(int32_t, int32_t, uint32_t, uint32_t) override {}
            void setVertexInput() override {}
            void setPrimitiveTopology(const PrimitiveTopology&) override {}
            void setPrimitiveRestartEnable(bool) override {}
            void setRasterizerDiscardEnable(bool) override {}
            void setPolygonMode(const PolygonMode&) override {}
            void setCullMode(const CullMode&) override {}
            void setFrontFace(const FrontFace&) override {}
            void setDepthBiasEnable(bool) override {}
            void setDepthBias(float, float, float) override {}
            void setDepthClampEnable(bool) override {}
            void setLineWidth(float) override {}
            void setRasterizationSamples(uint32_t) override {}
            void setSampleMask(uint32_t, uint32_t) override {}
            void setAlphaToCoverageEnable(bool) override {}
            void setAlphaToOneEnableEXT(bool) override {}
            void setDepthTestEnable(bool) override {}
            void setDepthWriteEnable(bool) override {}
            void setDepthCompareOp(const CompareOp&) override {}
            void setDepthBoundsTestEnable(bool) override {}
            void setStencilTestEnable(bool) override {}
            void setColorBlendEnable(uint32_t, bool) override {}
            void setColorBlendEquation(uint32_t, const ColorBlendEquation&) override {}
            void setColorWriteMask(uint32_t, uint32_t) override {}
            void setLogicOpEnable(bool) override {}

            void bindDescriptorHeaps(DescriptorHeap*, DescriptorHeap*) override {}
            void pushData(const void*, uint32_t) override {}
            void bindVertexBuffers(uint32_t, Buffer&, uint64_t) override {}
            void bindIndexBuffer(Buffer&, uint64_t) override {}
            void draw(uint32_t, uint32_t, uint32_t, uint32_t) override {}
            void drawIndexed(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) override {}
            void drawMeshTasks(uint32_t, uint32_t, uint32_t) override {}
            void drawMeshTasksIndirect(Buffer&, uint64_t, uint32_t, uint32_t) override {}
            void drawMeshTasksIndirect(uint64_t, uint64_t, uint32_t, uint32_t) override {}
            void drawMeshTasksIndirectCount(Buffer&, uint64_t, Buffer&, uint64_t, uint32_t, uint32_t) override {}
            void drawMeshTasksIndirectCountWithDrawId(Buffer&, uint64_t, Buffer&, uint64_t, uint32_t, uint32_t) override {}
            void dispatch(uint32_t, uint32_t, uint32_t) override {}
            void traceRays(const StridedDeviceAddressRegion&, const StridedDeviceAddressRegion&,
                           const StridedDeviceAddressRegion&, const StridedDeviceAddressRegion&,
                           uint32_t, uint32_t, uint32_t) override {}

            void copyBuffer(Buffer& srcBuffer, Buffer& dstBuffer, const BufferCopyRegion& region) override
            {
                auto* src = dynamic_cast<BufferNone*>(&srcBuffer);
                auto* dst = dynamic_cast<BufferNone*>(&dstBuffer);
                if (!src || !dst)
                    return;
                auto* srcData = src->tryGetCpuBacking();
                auto* dstData = dst->tryGetCpuBacking();
                if (!srcData || !dstData)
                    return;
                const uint64_t bytes = region.size > 0
                    ? region.size
                    : std::min<uint64_t>(src->getSize() - std::min(region.srcOffset, src->getSize()),
                                           dst->getSize() - std::min(region.dstOffset, dst->getSize()));
                if (region.srcOffset + bytes <= srcData->size() && region.dstOffset + bytes <= dstData->size())
                    std::memcpy(dstData->data() + region.dstOffset, srcData->data() + region.srcOffset, static_cast<size_t>(bytes));
            }

            void fillBuffer(Buffer& buffer, uint64_t offset, uint64_t size, uint32_t value) override
            {
                auto* buf = dynamic_cast<BufferNone*>(&buffer);
                if (!buf)
                    return;
                auto* data = buf->tryGetCpuBacking();
                if (!data || offset >= data->size())
                    return;
                const uint64_t bytes = std::min<uint64_t>(size, data->size() - offset);
                for (uint64_t i = 0; i + 4 <= bytes; i += 4)
                    std::memcpy(data->data() + offset + i, &value, 4);
            }

            void copyBuffer(Buffer& srcBuffer, Buffer& dstBuffer, std::span<const BufferCopyRegion> regions) override
            {
                for (const BufferCopyRegion& r : regions)
                    copyBuffer(srcBuffer, dstBuffer, r);
            }

            void transitionTextureLayout(Texture&, TextureLayout, TextureLayout) override {}
            void transitionSwapchainLayout(Swapchain&, uint32_t, TextureLayout, TextureLayout) override {}
            void resolveImage(Texture&, Texture&, uint32_t, uint32_t) override {}
            void copyTexture(Texture&, Texture&, uint32_t, uint32_t) override {}
            void copyTextureMips(Texture&, uint32_t, Texture&, uint32_t, uint32_t) override {}

            void buildAccelerationStructure(const AccelerationStructureBuildDesc&, uint64_t, AccelerationStructure&) override {}
            void updateAccelerationStructure(const AccelerationStructureBuildDesc&, uint64_t, AccelerationStructure&, AccelerationStructure&) override {}
            void accelerationStructureBarrier(AccelerationStructureBarrierType) override {}
            void resetQueries(QueryPool&, uint32_t, uint32_t) override {}
            void writeCompactedSizes(std::span<AccelerationStructure* const>, QueryPool&, uint32_t) override {}
            void copyAccelerationStructure(AccelerationStructure&, AccelerationStructure&, bool) override {}
            void executionBarrier() override {}
            void transferBarrier() override {}
            void resourceBarriers(std::span<const TextureBarrierDesc>, std::span<const BufferBarrierDesc>) override {}

            void beginDebugLabel(const char*) override {}
            void endDebugLabel() override {}
        };

        class CommandAllocatorNone final : public CommandAllocator
        {
        public:
            std::unique_ptr<CommandBuffer> allocateCommandBuffer(uint32_t) override
            {
                return std::make_unique<CommandBufferNone>();
            }

            void reset() override {}
        };
    }

    DeviceNone::DeviceNone(const DeviceInitDesc& desc)
        : m_shaderCacheDir(desc.shaderCacheDir.empty() ? "shader_cache" : desc.shaderCacheDir)
    {
        NRI_LOG_INFO("None (no-GPU headless) backend active.");
    }

    DeviceNone::~DeviceNone()
    {
        shutdown();
    }

    std::unique_ptr<Swapchain> DeviceNone::createSwapchain(const SwapchainDesc& desc)
    {
        return std::make_unique<SwapchainNone>(desc);
    }

    std::unique_ptr<Pipeline> DeviceNone::createPipeline(const PipelineDesc&, ShaderCompiler&)
    {
        return std::make_unique<PipelineNone>();
    }

    std::unique_ptr<Pipeline> DeviceNone::createPipelineNoCompiler(const PipelineDesc&)
    {
        return std::make_unique<PipelineNone>();
    }

    std::unique_ptr<CommandAllocator> DeviceNone::createCommandAllocator(CommandBufferReset, QueueType)
    {
        return std::make_unique<CommandAllocatorNone>();
    }

    std::unique_ptr<TimelineSemaphore> DeviceNone::createTimelineSemaphore(uint64_t initialValue)
    {
        return std::make_unique<TimelineSemaphoreNone>(initialValue);
    }

    std::unique_ptr<Texture2D> DeviceNone::createTexture(const TextureDesc& desc)
    {
        return std::make_unique<TextureNone>(*this, desc);
    }

    std::unique_ptr<Buffer> DeviceNone::createBuffer(const BufferDesc& desc)
    {
        return std::make_unique<BufferNone>(desc);
    }

    std::unique_ptr<DescriptorHeap> DeviceNone::createDescriptorHeap(const DescriptorHeapDesc&)
    {
        return std::make_unique<DescriptorHeapNone>();
    }

    AccelerationStructureBuildSizes DeviceNone::getAccelerationStructureBuildSizes(const AccelerationStructureBuildDesc&)
    {
        return {};
    }

    std::unique_ptr<AccelerationStructure> DeviceNone::createAccelerationStructure(const AccelerationStructureDesc& desc)
    {
        return std::make_unique<AccelerationStructureNone>(desc);
    }

    std::unique_ptr<QueryPool> DeviceNone::createQueryPool(const QueryPoolDesc& desc)
    {
        return std::make_unique<QueryPoolNone>(desc.capacity);
    }

    std::unique_ptr<GpuProfiler> DeviceNone::createGpuProfiler(uint32_t)
    {
        return std::make_unique<GpuProfilerNone>();
    }

    void DeviceNone::beginFrame(uint32_t)
    {
        collectDeferredDeletions();
    }

    void DeviceNone::getMemoryStats(std::vector<MemoryHeapStats>& outHeaps) const
    {
        outHeaps.clear();
    }

    void DeviceNone::shutdown()
    {
        if (m_shuttingDown)
            return;
        waitIdle();
        m_shuttingDown = true;
    }

    void DeviceNone::submitAndWait(CommandBuffer&, uint32_t)
    {
    }

    void DeviceNone::submitCommandBuffers(std::span<CommandBuffer* const>, Swapchain&, uint32_t, uint32_t,
                                          std::span<const TimelinePoint>)
    {
    }

    void DeviceNone::submit(QueueType, std::span<CommandBuffer* const>, std::span<const TimelinePoint>,
                            std::span<const TimelinePoint> signals)
    {
        for (const TimelinePoint& sig : signals)
        {
            if (auto* sem = dynamic_cast<TimelineSemaphoreNone*>(sig.semaphore))
                sem->advanceTo(sig.value);
        }
    }

    void DeviceNone::waitIdle()
    {
        drainDeferredDeletions();
    }
}
