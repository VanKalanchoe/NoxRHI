#pragma once
#include <array>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <span>
#include "D3D12Common.h"
#include "../CommandBuffer.h"
#include "../AccelerationStructure.h"

namespace NRI
{
    class DeviceD3D12;
    class CommandAllocatorD3D12;
    class PipelineD3D12;
    class BufferD3D12;

    class CommandBufferD3D12 final : public CommandBuffer
    {
    public:
        CommandBufferD3D12(DeviceD3D12& device, CommandAllocatorD3D12& allocator);
        ~CommandBufferD3D12() override = default;

        ID3D12GraphicsCommandList6* getNativeCommandList() const { return m_cmdList.Get(); }
        D3D12_COMMAND_LIST_TYPE getCommandListType() const;

        void begin(uint32_t index = 0, bool oneTimeSubmit = false) override;
        void end(uint32_t index = 0) override;
        void beginRendering(RenderDesc& desc) override;
        void endRendering() override;
        void bindPipeline(PipelineBindPoint bindPoint, Pipeline& pipeline) override;

        void setViewport(Extent2D swapChainExtent) override;
        void setScissor(Extent2D swapChainExtent) override;
        void setViewportWithCount(const ViewportBounds& bounds, float minDepth, float maxDepth) override;
        void setScissorWithCount(const Extent2D& swapChainExtent) override;
        void setScissorRect(int32_t x, int32_t y, uint32_t width, uint32_t height) override;
        void clearColorRects(uint32_t colorAttachmentIndex, const ClearColor& color, const Rect2D* rects, uint32_t rectCount) override;
        void setVertexInput() override {}
        void setPrimitiveTopology(const PrimitiveTopology& topology) override;
        void setPrimitiveRestartEnable(bool enable) override {}
        void setRasterizerDiscardEnable(bool enable) override {}
        void setPolygonMode(const PolygonMode& polygon) override { m_polygonMode = polygon; }
        void setCullMode(const CullMode& cullMode) override { m_cullMode = cullMode; }
        void setFrontFace(const FrontFace& frontFace) override { m_frontFace = frontFace; }
        void setDepthBiasEnable(bool enable) override {}
        // D3D12 keeps depth bias in the pipeline's rasterizer state (ID3D12GraphicsCommandList9::RSSetDepthBias is the dynamic form);
        // like setDepthBiasEnable above, not wired yet: the shadow map passes draw without the raster slope bias here.
        void setDepthBias(float constantFactor, float clamp, float slopeFactor) override {}
        void setDepthClampEnable(bool enable) override { m_depthClampEnable = enable; }
        void setLineWidth(float lineWidth) override {}
        void setRasterizationSamples(uint32_t sampleCount) override { m_rasterizationSamples = sampleCount > 0 ? sampleCount : 1; }
        void setSampleMask(uint32_t sampleCount, uint32_t sampleMask) override {}
        void setAlphaToCoverageEnable(bool enable) override {}
        void setAlphaToOneEnableEXT(bool enable) override {}
        void setDepthTestEnable(bool enable) override { m_depthTestEnable = enable; }
        void setDepthWriteEnable(bool enable) override { m_depthWriteEnable = enable; }
        void setDepthCompareOp(const CompareOp& compareOp) override { m_depthCompareOp = compareOp; }
        void setDepthBoundsTestEnable(bool enable) override {}
        void setStencilTestEnable(bool enable) override {}
        void setColorBlendEnable(uint32_t firstAttachment, bool enable) override
        {
            if (firstAttachment < m_colorBlendEnable.size())
                m_colorBlendEnable[firstAttachment] = enable;
        }
        void setColorBlendEquation(uint32_t firstAttachment, const ColorBlendEquation& blendEquation) override
        {
            if (firstAttachment < m_colorBlendEquation.size())
                m_colorBlendEquation[firstAttachment] = blendEquation;
        }
        void setColorWriteMask(uint32_t firstAttachment, uint32_t colorWriteMask) override
        {
            if (firstAttachment < m_colorWriteMask.size())
                m_colorWriteMask[firstAttachment] = colorWriteMask;
        }
        void setLogicOpEnable(bool enable) override {}

        void bindDescriptorHeaps(DescriptorHeap* resourceHeap, DescriptorHeap* samplerHeap) override;
        void restoreBindingsAfterExternalWork() override;
        void pushData(const void* data, uint32_t size) override;
        void bindVertexBuffers(uint32_t firstBinding, Buffer& buffer, uint64_t offset) override;
        void bindIndexBuffer(Buffer& buffer, uint64_t offet) override;
        void draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override;
        void drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) override;
        void drawMeshTasks(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override;
        void drawMeshTasksIndirect(Buffer& indirectBuffer, uint64_t offset, uint32_t drawCount, uint32_t stride) override;
        void drawMeshTasksIndirect(uint64_t indirectBufferDeviceAddress, uint64_t offset, uint32_t drawCount, uint32_t stride) override;
        void drawMeshTasksIndirectCount(Buffer& indirectBuffer, uint64_t offset, Buffer& countBuffer, uint64_t countOffset, uint32_t maxDrawCount, uint32_t stride) override;
        void drawMeshTasksIndirectCountWithDrawId(Buffer& indirectBuffer, uint64_t offset, Buffer& countBuffer, uint64_t countOffset, uint32_t maxDrawCount, uint32_t stride) override;
        void dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override;
        void traceRays(const StridedDeviceAddressRegion& rayGen, const StridedDeviceAddressRegion& miss,
                       const StridedDeviceAddressRegion& hitGroup, const StridedDeviceAddressRegion& callable,
                       uint32_t width, uint32_t height, uint32_t depth = 1) override;

        void copyBuffer(Buffer& srcBuffer, Buffer& dstBuffer, const BufferCopyRegion& region = {}) override;
        void fillBuffer(Buffer& buffer, uint64_t offset, uint64_t size, uint32_t value) override;
        void copyBuffer(Buffer& srcBuffer, Buffer& dstBuffer, std::span<const BufferCopyRegion> regions) override;

        void transitionTextureLayout(Texture& texture, TextureLayout oldLayout, TextureLayout newLayout) override;
        void transitionSwapchainLayout(Swapchain& swapchain, uint32_t imageIndex, TextureLayout oldLayout, TextureLayout newLayout) override;
        void resolveImage(Texture& srcTexture, Texture& dstTexture, uint32_t width, uint32_t height) override;
        void copyTexture(Texture& srcTexture, Texture& dstTexture, uint32_t width, uint32_t height) override;
        void copyTextureMips(Texture& srcTexture, uint32_t srcFirstMip, Texture& dstTexture, uint32_t dstFirstMip, uint32_t mipCount) override;

        void buildAccelerationStructure(const AccelerationStructureBuildDesc& buildDesc, uint64_t scratchAddress, AccelerationStructure& dstAS) override;
        void updateAccelerationStructure(const AccelerationStructureBuildDesc& buildDesc, uint64_t scratchAddress, AccelerationStructure& srcAS, AccelerationStructure& dstAS) override;
        void accelerationStructureBarrier(AccelerationStructureBarrierType barrierType = AccelerationStructureBarrierType::BuildToShaderRead) override;

        void resetQueries(QueryPool& pool, uint32_t first, uint32_t count) override;
        void writeCompactedSizes(std::span<AccelerationStructure* const> structures, QueryPool& pool, uint32_t firstQuery) override;
        void copyAccelerationStructure(AccelerationStructure& src, AccelerationStructure& dst, bool compact) override;

        void executionBarrier() override;
        void transferBarrier() override;
        void resourceBarriers(std::span<const TextureBarrierDesc> textures, std::span<const BufferBarrierDesc> buffers) override;

        void beginDebugLabel(const char* label) override;
        void endDebugLabel() override;

        void setDeferredStateTracking(bool enable) override { m_deferredStateTracking = enable; }
        bool resolveDeferredBarriers(CommandBuffer& fixupCmd) override;

        // Discards a render-target / depth texture in placed memory before its first use (see TextureD3D12::needsInitialDiscard);
        // nothing for any other texture. Recording marks it initialized.
        void initializeTarget(class TextureD3D12& texture);
        void transitionTextureState(class TextureD3D12& texture, D3D12_RESOURCE_STATES afterState, bool uavBarrierIfSame = false);
        D3D12_RESOURCE_STATES getTrackedTextureState(const class TextureD3D12& texture) const;

        // Keeps temporary staging buffers created during recording alive until this command list is submitted and retired.
        void retainTempBuffer(std::shared_ptr<BufferD3D12> buffer) { m_tempBuffers.push_back(std::move(buffer)); }
        std::vector<std::shared_ptr<BufferD3D12>> takeTempBuffers() { return std::move(m_tempBuffers); }

    private:
        void ensureGraphicsPipelineState();
        void transitionBufferState(class BufferD3D12& buffer, D3D12_RESOURCE_STATES afterState, bool uavBarrierIfSame = false);

    private:
        DeviceD3D12& m_deviceD3D12;
        CommandAllocatorD3D12& m_allocator;
        ComPtr<ID3D12GraphicsCommandList6> m_cmdList;
        bool m_isRecording = false;
        PipelineD3D12* m_currentPipeline = nullptr;
        ID3D12PipelineState* m_boundPso = nullptr;
        // The heaps bound last in this recording (restoreBindingsAfterExternalWork binds them and the root signature again)
        DescriptorHeap* m_boundResourceHeap = nullptr;
        DescriptorHeap* m_boundSamplerHeap = nullptr;
        PipelineBindPoint m_currentBindPoint = PipelineBindPoint::Graphics;
        uint32_t m_rasterizationSamples = 1; // (a pipeline variant per sample count: D3D12 bakes it into the PSO)
        bool m_hasDepthAttachment = false;
        bool m_depthTestEnable = false;
        bool m_depthWriteEnable = false;
        bool m_depthClampEnable = false;
        CompareOp m_depthCompareOp = CompareOp::GreaterOrEqual;
        CullMode m_cullMode = CullMode::Back;
        FrontFace m_frontFace = FrontFace::CounterClockWise;
        PolygonMode m_polygonMode = PolygonMode::Fill;
        std::array<bool, 8> m_colorBlendEnable{};
        std::array<ColorBlendEquation, 8> m_colorBlendEquation{ {
            { BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha, BlendOp::Add, BlendFactor::One, BlendFactor::Zero, BlendOp::Add },
            { BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha, BlendOp::Add, BlendFactor::One, BlendFactor::Zero, BlendOp::Add },
            { BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha, BlendOp::Add, BlendFactor::One, BlendFactor::Zero, BlendOp::Add },
            { BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha, BlendOp::Add, BlendFactor::One, BlendFactor::Zero, BlendOp::Add },
            { BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha, BlendOp::Add, BlendFactor::One, BlendFactor::Zero, BlendOp::Add },
            { BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha, BlendOp::Add, BlendFactor::One, BlendFactor::Zero, BlendOp::Add },
            { BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha, BlendOp::Add, BlendFactor::One, BlendFactor::Zero, BlendOp::Add },
            { BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha, BlendOp::Add, BlendFactor::One, BlendFactor::Zero, BlendOp::Add }
        } };
        std::array<uint32_t, 8> m_colorWriteMask{
            ColorComponent::All, ColorComponent::All, ColorComponent::All, ColorComponent::All,
            ColorComponent::All, ColorComponent::All, ColorComponent::All, ColorComponent::All
        };

        // Temporary RTV/DSV descriptor storage for dynamic rendering
        ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
        ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
        uint32_t m_rtvDescriptorSize = 0;
        uint32_t m_dsvDescriptorSize = 0;

        std::vector<std::shared_ptr<BufferD3D12>> m_tempBuffers;
        std::string m_debugPassList;

        // Parallel recording deferred state tracking (resolved sequentially on the main thread before ExecuteCommandLists).
        bool m_deferredStateTracking = false;
        std::unordered_map<class TextureD3D12*, D3D12_RESOURCE_STATES> m_localTexStates;
        std::vector<std::pair<class TextureD3D12*, D3D12_RESOURCE_STATES>> m_firstTexStates;
        std::unordered_map<BufferD3D12*, D3D12_RESOURCE_STATES> m_localBufStates;
        std::vector<std::pair<BufferD3D12*, D3D12_RESOURCE_STATES>> m_firstBufStates;
        std::vector<class TextureD3D12*> m_pendingDiscardTextures;
        std::unordered_set<class TextureD3D12*> m_discardSeen;
        std::vector<class TextureD3D12*> m_markInitializedTextures;
        // The render target views of the rendering in progress, by colour attachment index (clearColorRects).
        D3D12_CPU_DESCRIPTOR_HANDLE m_boundRtv[8]{};
        bool m_boundRtvValid[8]{};
    };
}
