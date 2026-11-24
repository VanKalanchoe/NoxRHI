#include "CommandBufferD3D12.h"
#include "DeviceD3D12.h"
#include "CommandAllocatorD3D12.h"
#include "PipelineD3D12.h"
#include "TextureD3D12.h"
#include "BufferD3D12.h"
#include "DescriptorHeapD3D12.h"
#include "AccelerationStructureD3D12.h"
#include "QueryPoolD3D12.h"
#include "SwapchainD3D12.h"
#include <algorithm>

namespace NRI
{
    namespace
    {
        D3D12_RESOURCE_STATES TranslateTextureLayoutToD3D12(TextureLayout layout)
        {
            switch (layout)
            {
            case TextureLayout::Undefined: return D3D12_RESOURCE_STATE_COMMON;
            case TextureLayout::ColorAttachment: return D3D12_RESOURCE_STATE_RENDER_TARGET;
            case TextureLayout::DepthAttachment: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
            case TextureLayout::ShaderResource: return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            case TextureLayout::TransferSrc: return D3D12_RESOURCE_STATE_COPY_SOURCE;
            case TextureLayout::TransferDst: return D3D12_RESOURCE_STATE_COPY_DEST;
            case TextureLayout::Present: return D3D12_RESOURCE_STATE_PRESENT;
            case TextureLayout::General: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            default: return D3D12_RESOURCE_STATE_COMMON;
            }
        }

        D3D12_RESOURCE_STATES TranslateResourceStateToD3D12(const ResourceState& state, bool isComputeList = false)
        {
            if (state.access == AccessBits::None)
                return D3D12_RESOURCE_STATE_COMMON;
            if (state.access & (AccessBits::ShaderWrite | AccessBits::AccelerationStructureWrite))
                return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            if (state.access & (AccessBits::ColorAttachmentWrite | AccessBits::ColorAttachmentRead))
                return D3D12_RESOURCE_STATE_RENDER_TARGET;
            if (state.access & AccessBits::DepthStencilWrite)
                return D3D12_RESOURCE_STATE_DEPTH_WRITE;
            if (state.access & AccessBits::TransferWrite)
                return D3D12_RESOURCE_STATE_COPY_DEST;

            // Read-only states are combinable bitflags in D3D12 (e.g. a buffer read both by shaders and ExecuteIndirect).
            D3D12_RESOURCE_STATES readStates = D3D12_RESOURCE_STATE_COMMON;
            if (!isComputeList && (state.access & AccessBits::DepthStencilRead))
                readStates |= D3D12_RESOURCE_STATE_DEPTH_READ;
            if (state.access & AccessBits::ShaderRead)
            {
                const bool nonPixelOnlyStages = (state.stages != StageBits::None) &&
                                                ((state.stages & StageBits::Fragment) == 0);
                readStates |= (isComputeList || nonPixelOnlyStages)
                    ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                    : (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            if (state.access & AccessBits::AccelerationStructureRead)
                readStates |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            if (state.access & AccessBits::TransferRead)
                readStates |= D3D12_RESOURCE_STATE_COPY_SOURCE;
            if (state.access & AccessBits::IndirectRead)
                readStates |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
            return readStates;
        }

        bool isReadOnlySubset(D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
        {
            return before != D3D12_RESOURCE_STATE_COMMON &&
                   after != D3D12_RESOURCE_STATE_COMMON &&
                   (before & ~D3D12_RESOURCE_STATE_GENERIC_READ) == 0 &&
                   (before & after) == after;
        }
    }

    CommandBufferD3D12::CommandBufferD3D12(DeviceD3D12& device, CommandAllocatorD3D12& allocator)
        : m_deviceD3D12(device), m_allocator(allocator)
    {
        ThrowIfFailed(m_deviceD3D12.getNativeDevice()->CreateCommandList(
            0, m_allocator.getCommandListType(), m_allocator.getNativeAllocator(),
            nullptr, IID_PPV_ARGS(&m_cmdList)), "CreateCommandList");
        m_cmdList->Close();

        // Create temporary RTV and DSV descriptor heaps for dynamic rendering
        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDesc.NumDescriptors = 8;
        m_deviceD3D12.getNativeDevice()->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap));
        m_rtvDescriptorSize = m_deviceD3D12.getNativeDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
        dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvDesc.NumDescriptors = 1;
        m_deviceD3D12.getNativeDevice()->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&m_dsvHeap));
        m_dsvDescriptorSize = m_deviceD3D12.getNativeDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    }

    void CommandBufferD3D12::begin(uint32_t index, bool oneTimeSubmit)
    {
        (void)index;
        (void)oneTimeSubmit;
        ThrowIfFailed(m_cmdList->Reset(m_allocator.getNativeAllocator(), nullptr), "CommandList Reset");
        m_isRecording = true;
        m_debugPassList.clear();
        m_localTexStates.clear();
        m_firstTexStates.clear();
        m_localBufStates.clear();
        m_firstBufStates.clear();
        m_pendingDiscardTextures.clear();
        m_discardSeen.clear();
        m_markInitializedTextures.clear();
        m_currentPipeline = nullptr;
        m_boundPso = nullptr;
        m_boundResourceHeap = m_boundSamplerHeap = nullptr;
        m_currentBindPoint = m_allocator.getCommandListType() == D3D12_COMMAND_LIST_TYPE_COMPUTE
            ? PipelineBindPoint::Compute
            : PipelineBindPoint::Graphics;
        m_hasDepthAttachment = false;
        m_depthTestEnable = false;
        m_depthWriteEnable = false;
        m_depthClampEnable = false;
        m_depthCompareOp = CompareOp::GreaterOrEqual;
        m_cullMode = CullMode::Back;
        m_frontFace = FrontFace::CounterClockWise;
        m_polygonMode = PolygonMode::Fill;
    }

    void CommandBufferD3D12::end(uint32_t index)
    {
        if (!m_debugPassList.empty())
        {
            std::wstring wname;
            wname.reserve(m_debugPassList.size());
            for (char c : m_debugPassList)
                wname.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
            m_cmdList->SetName(wname.c_str());
        }
        ThrowIfFailed(m_cmdList->Close(), "CommandList Close");
        m_isRecording = false;
    }

    void CommandBufferD3D12::beginRendering(RenderDesc& desc)
    {
        std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvHandles;
        D3D12_CPU_DESCRIPTOR_HANDLE rtvStart = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

        for (size_t i = 0; i < desc.colorAttachments.size() && i < 8; ++i)
        {
            auto& attach = desc.colorAttachments[i];
            TextureD3D12* tex = nullptr;
            if (attach.attachment)
                tex = dynamic_cast<TextureD3D12*>(attach.attachment);
            else if (attach.attachmentSwapchain)
                tex = static_cast<SwapchainD3D12*>(attach.attachmentSwapchain)->getBackBuffer(attach.resolveImageIndex);

            if (tex)
            {
                initializeTarget(*tex);
                transitionTextureState(*tex, D3D12_RESOURCE_STATE_RENDER_TARGET);

                D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvStart;
                rtvHandle.ptr += i * m_rtvDescriptorSize;

                D3D12_RENDER_TARGET_VIEW_DESC rtvViewDesc{};
                rtvViewDesc.Format = tex->getDxgiFormat();
                rtvViewDesc.ViewDimension = tex->getDesc().sampleCount > 1
                    ? D3D12_RTV_DIMENSION_TEXTURE2DMS
                    : D3D12_RTV_DIMENSION_TEXTURE2D;
                m_deviceD3D12.getNativeDevice()->CreateRenderTargetView(tex->getNativeResource(), &rtvViewDesc, rtvHandle);
                rtvHandles.push_back(rtvHandle);

                if (attach.loadOP == LoadOP::clear)
                {
                    const float clearColor[4] = { attach.clearColor.r, attach.clearColor.g, attach.clearColor.b, attach.clearColor.a };
                    m_cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
                }
            }
        }

        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{ 0 };
        bool hasDepth = false;
        if (desc.depthAttachment.attachment)
        {
            auto* depthTex = dynamic_cast<TextureD3D12*>(desc.depthAttachment.attachment);
            initializeTarget(*depthTex);
            transitionTextureState(*depthTex, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            dsvHandle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
            // (an explicit view: the resource is typeless -- TextureD3D12 -- and may be multisampled)
            D3D12_DEPTH_STENCIL_VIEW_DESC dsvViewDesc{};
            dsvViewDesc.Format = DXGI_FORMAT_D32_FLOAT;
            dsvViewDesc.ViewDimension = depthTex->getDesc().sampleCount > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
            m_deviceD3D12.getNativeDevice()->CreateDepthStencilView(depthTex->getNativeResource(), &dsvViewDesc, dsvHandle);
            hasDepth = true;

            if (desc.depthAttachment.loadOP == LoadOP::clear)
            {
                m_cmdList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, desc.depthAttachment.clearDepth.depth, 0, 0, nullptr);
            }
        }

        m_hasDepthAttachment = hasDepth;
        m_cmdList->OMSetRenderTargets(
            static_cast<UINT>(rtvHandles.size()),
            rtvHandles.data(),
            FALSE,
            hasDepth ? &dsvHandle : nullptr
        );

        // Viewport and Scissor
        D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(desc.renderArea.width), static_cast<float>(desc.renderArea.height), 0.0f, 1.0f };
        D3D12_RECT sci{ 0, 0, static_cast<LONG>(desc.renderArea.width), static_cast<LONG>(desc.renderArea.height) };
        m_cmdList->RSSetViewports(1, &vp);
        m_cmdList->RSSetScissorRects(1, &sci);
    }

    void CommandBufferD3D12::endRendering()
    {
        m_hasDepthAttachment = false;
    }

    D3D12_COMMAND_LIST_TYPE CommandBufferD3D12::getCommandListType() const
    {
        return m_allocator.getCommandListType();
    }

    void CommandBufferD3D12::ensureGraphicsPipelineState()
    {
        if (!m_currentPipeline || m_currentBindPoint != PipelineBindPoint::Graphics)
            return;
        ID3D12PipelineState* desired = m_currentPipeline->resolveGraphicsPipeline(
            m_hasDepthAttachment,
            m_depthTestEnable,
            m_depthWriteEnable,
            m_depthCompareOp,
            m_cullMode,
            m_frontFace,
            m_polygonMode,
            m_colorBlendEnable,
            m_colorBlendEquation,
            m_colorWriteMask,
            m_rasterizationSamples,
            m_depthClampEnable);
        if (desired && desired != m_boundPso)
        {
            m_cmdList->SetPipelineState(desired);
            m_boundPso = desired;
        }
    }

    void CommandBufferD3D12::bindPipeline(PipelineBindPoint bindPoint, Pipeline& pipeline)
    {
        auto& pipeD3D12 = static_cast<PipelineD3D12&>(pipeline);
        m_currentBindPoint = bindPoint;
        m_currentPipeline = &pipeD3D12;
        if (bindPoint == PipelineBindPoint::Graphics)
        {
            // Dynamic states (cull mode, depth test/write, blend enable/equation/write mask) are often set after bindPipeline
            // and before draw*; resolve the D3D12 PSO variant lazily in ensureGraphicsPipelineState() at draw time.
            m_boundPso = nullptr;
        }
        else if (pipeD3D12.getNativePipeline())
        {
            if (pipeD3D12.getNativePipeline() != m_boundPso)
            {
                m_cmdList->SetPipelineState(pipeD3D12.getNativePipeline());
                m_boundPso = pipeD3D12.getNativePipeline();
            }
        }
    }

    void CommandBufferD3D12::setViewport(Extent2D swapChainExtent)
    {
        D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(swapChainExtent.width), static_cast<float>(swapChainExtent.height), 0.0f, 1.0f };
        m_cmdList->RSSetViewports(1, &vp);
    }

    void CommandBufferD3D12::setScissor(Extent2D swapChainExtent)
    {
        D3D12_RECT sci{ 0, 0, static_cast<LONG>(swapChainExtent.width), static_cast<LONG>(swapChainExtent.height) };
        m_cmdList->RSSetScissorRects(1, &sci);
    }

    void CommandBufferD3D12::setViewportWithCount(const ViewportBounds& bounds, float minDepth, float maxDepth)
    {
        // D3D12 NDC Y is already +Y up and forbids negative viewport heights (used on Vulkan for Y flip).
        float y = bounds.y;
        float h = bounds.height;
        if (h < 0.0f)
        {
            y += h;
            h = -h;
        }
        D3D12_VIEWPORT vp{ bounds.x, y, bounds.width, h, minDepth, maxDepth };
        m_cmdList->RSSetViewports(1, &vp);
    }

    void CommandBufferD3D12::setScissorWithCount(const Extent2D& swapChainExtent)
    {
        setScissor(swapChainExtent);
    }

    void CommandBufferD3D12::setScissorRect(int32_t x, int32_t y, uint32_t width, uint32_t height)
    {
        const D3D12_RECT rect{ x, y, x + static_cast<LONG>(width), y + static_cast<LONG>(height) };
        m_cmdList->RSSetScissorRects(1, &rect);
    }

    void CommandBufferD3D12::setPrimitiveTopology(const PrimitiveTopology& topology)
    {
        m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }

    void CommandBufferD3D12::restoreBindingsAfterExternalWork()
    {
        // NRD sets its own descriptor heaps, root signature and pipeline states on this list, and Streamline (manual hooking, no
        // command-list state tracking) restores nothing: the heaps and NoxRHI's root signature go back, and the cached pipeline is
        // forgotten -- otherwise binding the pipeline that was bound before would be skipped while another one is set on the list.
        m_boundPso = nullptr;
        m_currentPipeline = nullptr;
        if (m_boundResourceHeap || m_boundSamplerHeap)
            bindDescriptorHeaps(m_boundResourceHeap, m_boundSamplerHeap);
    }

    void CommandBufferD3D12::bindDescriptorHeaps(DescriptorHeap* resourceHeap, DescriptorHeap* samplerHeap)
    {
        if (resourceHeap)
            m_boundResourceHeap = resourceHeap;
        if (samplerHeap)
            m_boundSamplerHeap = samplerHeap;
        ID3D12DescriptorHeap* heaps[2]{};
        UINT count = 0;
        if (resourceHeap)
            heaps[count++] = static_cast<DescriptorHeapD3D12*>(resourceHeap)->getNativeHeap();
        if (samplerHeap)
            heaps[count++] = static_cast<DescriptorHeapD3D12*>(samplerHeap)->getNativeHeap();

        if (count > 0)
            m_cmdList->SetDescriptorHeaps(count, heaps);

        if (resourceHeap && samplerHeap &&
            (m_allocator.getCommandListType() == D3D12_COMMAND_LIST_TYPE_DIRECT ||
             m_allocator.getCommandListType() == D3D12_COMMAND_LIST_TYPE_COMPUTE))
        {
            ID3D12RootSignature* rootSig = m_deviceD3D12.getGlobalRootSignature();
            if (rootSig)
            {
                if (m_allocator.getCommandListType() == D3D12_COMMAND_LIST_TYPE_DIRECT)
                    m_cmdList->SetGraphicsRootSignature(rootSig);
                m_cmdList->SetComputeRootSignature(rootSig);
            }
        }
    }

    void CommandBufferD3D12::pushData(const void* data, uint32_t size)
    {
        UINT numValues = size / 4;
        // Vulkan shares one push-constant block across graphics & compute (so callers like drawMeshletBucket may call
        // pushData before bindPipeline). Mirror that on DIRECT lists by updating both graphics & compute root constants.
        if (m_allocator.getCommandListType() == D3D12_COMMAND_LIST_TYPE_DIRECT)
        {
            m_cmdList->SetGraphicsRoot32BitConstants(0, numValues, data, 0);
            m_cmdList->SetComputeRoot32BitConstants(0, numValues, data, 0);
        }
        else
        {
            m_cmdList->SetComputeRoot32BitConstants(0, numValues, data, 0);
        }
    }

    void CommandBufferD3D12::bindVertexBuffers(uint32_t firstBinding, Buffer& buffer, uint64_t offset)
    {
        D3D12_VERTEX_BUFFER_VIEW view{};
        view.BufferLocation = buffer.getDeviceAddress() + offset;
        view.SizeInBytes = static_cast<UINT>(buffer.getSize() - offset);
        view.StrideInBytes = 36;
        m_cmdList->IASetVertexBuffers(firstBinding, 1, &view);
    }

    void CommandBufferD3D12::bindIndexBuffer(Buffer& buffer, uint64_t offet)
    {
        auto* bufD3D12 = static_cast<BufferD3D12*>(&buffer);
        transitionBufferState(*bufD3D12, D3D12_RESOURCE_STATE_INDEX_BUFFER);
        D3D12_INDEX_BUFFER_VIEW view{};
        view.BufferLocation = buffer.getDeviceAddress() + offet;
        view.SizeInBytes = static_cast<UINT>(buffer.getSize() - offet);
        view.Format = DXGI_FORMAT_R32_UINT;
        m_cmdList->IASetIndexBuffer(&view);
    }

    void CommandBufferD3D12::draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
    {
        ensureGraphicsPipelineState();
        m_cmdList->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
    }

    void CommandBufferD3D12::drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
    {
        ensureGraphicsPipelineState();
        m_cmdList->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    }

    void CommandBufferD3D12::drawMeshTasks(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
    {
        ensureGraphicsPipelineState();
        m_cmdList->DispatchMesh(groupCountX, groupCountY, groupCountZ);
    }

    // Indirect mesh draws: ExecuteIndirect with a DispatchMesh command signature. The argument records are Vulkan's
    // VkDrawMeshTasksIndirectCommandEXT bytes (groupCountX/Y/Z), so one buffer serves both APIs. The buffer must be in
    // INDIRECT_ARGUMENT state (AccessBits::IndirectRead; the render graph's RGBufferAccess::IndirectRead), the count buffer too.
    void CommandBufferD3D12::drawMeshTasksIndirect(Buffer& indirectBuffer, uint64_t offset, uint32_t drawCount, uint32_t stride)
    {
        if (drawCount == 0)
            return;
        ID3D12CommandSignature* signature = m_deviceD3D12.getDispatchMeshSignature(stride ? stride : uint32_t(sizeof(D3D12_DISPATCH_MESH_ARGUMENTS)));
        if (!signature)
            return;
        ensureGraphicsPipelineState();
        auto& d3dBuffer = static_cast<BufferD3D12&>(indirectBuffer);
        m_cmdList->ExecuteIndirect(signature, drawCount, d3dBuffer.getNativeBuffer(), offset, nullptr, 0);
    }

    void CommandBufferD3D12::drawMeshTasksIndirect(uint64_t indirectBufferDeviceAddress, uint64_t offset, uint32_t drawCount, uint32_t stride)
    {
        // ExecuteIndirect takes a resource and an offset, not a GPU address: the address form cannot be expressed on D3D12 (Vulkan's
        // drawMeshTasksIndirect2EXT is untested too). Draw with the Buffer& form.
        (void)indirectBufferDeviceAddress; (void)offset; (void)drawCount; (void)stride;
        NRI_LOG_ERROR("CommandBufferD3D12::drawMeshTasksIndirect by device address is not supported on D3D12: use the Buffer form");
    }

    void CommandBufferD3D12::drawMeshTasksIndirectCount(Buffer& indirectBuffer, uint64_t offset, Buffer& countBuffer, uint64_t countOffset, uint32_t maxDrawCount, uint32_t stride)
    {
        if (maxDrawCount == 0)
            return;
        ID3D12CommandSignature* signature = m_deviceD3D12.getDispatchMeshSignature(stride ? stride : uint32_t(sizeof(D3D12_DISPATCH_MESH_ARGUMENTS)));
        if (!signature)
            return;
        ensureGraphicsPipelineState();
        auto& d3dBuffer = static_cast<BufferD3D12&>(indirectBuffer);
        auto& d3dCount = static_cast<BufferD3D12&>(countBuffer);
        transitionBufferState(d3dBuffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        transitionBufferState(d3dCount, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        m_cmdList->ExecuteIndirect(signature, maxDrawCount, d3dBuffer.getNativeBuffer(), offset, d3dCount.getNativeBuffer(), countOffset);
    }

    void CommandBufferD3D12::drawMeshTasksIndirectCountWithDrawId(Buffer& indirectBuffer, uint64_t offset, Buffer& countBuffer, uint64_t countOffset, uint32_t maxDrawCount, uint32_t stride)
    {
        if (maxDrawCount == 0)
            return;
        ID3D12CommandSignature* signature = m_deviceD3D12.getDispatchMeshWithDrawIdSignature(stride ? stride : 16u);
        if (!signature)
            return;
        ensureGraphicsPipelineState();
        auto& d3dBuffer = static_cast<BufferD3D12&>(indirectBuffer);
        auto& d3dCount = static_cast<BufferD3D12&>(countBuffer);
        transitionBufferState(d3dBuffer, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        transitionBufferState(d3dCount, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        m_cmdList->ExecuteIndirect(signature, maxDrawCount, d3dBuffer.getNativeBuffer(), offset, d3dCount.getNativeBuffer(), countOffset);
    }

    void CommandBufferD3D12::dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
    {
        m_cmdList->Dispatch(groupCountX, groupCountY, groupCountZ);
    }

    void CommandBufferD3D12::traceRays(const StridedDeviceAddressRegion& rayGen, const StridedDeviceAddressRegion& miss,
                                       const StridedDeviceAddressRegion& hitGroup, const StridedDeviceAddressRegion& callable,
                                       uint32_t width, uint32_t height, uint32_t depth)
    {
        D3D12_DISPATCH_RAYS_DESC desc{};
        desc.RayGenerationShaderRecord.StartAddress = rayGen.deviceAddress;
        desc.RayGenerationShaderRecord.SizeInBytes = rayGen.size;

        desc.MissShaderTable.StartAddress = miss.deviceAddress;
        desc.MissShaderTable.SizeInBytes = miss.size;
        desc.MissShaderTable.StrideInBytes = miss.stride;

        desc.HitGroupTable.StartAddress = hitGroup.deviceAddress;
        desc.HitGroupTable.SizeInBytes = hitGroup.size;
        desc.HitGroupTable.StrideInBytes = hitGroup.stride;

        desc.CallableShaderTable.StartAddress = callable.deviceAddress;
        desc.CallableShaderTable.SizeInBytes = callable.size;
        desc.CallableShaderTable.StrideInBytes = callable.stride;

        desc.Width = width;
        desc.Height = height;
        desc.Depth = depth;

        m_cmdList->DispatchRays(&desc);
    }

    void CommandBufferD3D12::copyBuffer(Buffer& srcBuffer, Buffer& dstBuffer, const BufferCopyRegion& region)
    {
        auto* srcD3D12 = static_cast<BufferD3D12*>(&srcBuffer);
        auto* dstD3D12 = static_cast<BufferD3D12*>(&dstBuffer);
        transitionBufferState(*srcD3D12, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transitionBufferState(*dstD3D12, D3D12_RESOURCE_STATE_COPY_DEST);
        uint64_t size = region.size > 0 ? region.size : std::min(srcBuffer.getSize(), dstBuffer.getSize());
        m_cmdList->CopyBufferRegion(dstD3D12->getNativeBuffer(), region.dstOffset, srcD3D12->getNativeBuffer(), region.srcOffset, size);
    }

    void CommandBufferD3D12::fillBuffer(Buffer& buffer, uint64_t offset, uint64_t size, uint32_t value)
    {
        if (m_allocator.getCommandListType() == D3D12_COMMAND_LIST_TYPE_COPY)
        {
            NRI_LOG_WARN("CommandBufferD3D12::fillBuffer is not supported on a COPY command list");
            return;
        }
        const uint64_t bufSize = buffer.getSize();
        if (offset >= bufSize)
            return;
        const uint64_t effectiveSize = (size == ~0ull || offset + size > bufSize) ? (bufSize - offset) : size;
        const uint64_t totalDwords = effectiveSize / 4;
        if (totalDwords == 0)
            return;

        ID3D12RootSignature* rootSig = nullptr;
        ID3D12PipelineState* pso = nullptr;
        if (!m_deviceD3D12.getFillBufferPipeline(rootSig, pso))
            return;

        auto& bufD3D12 = static_cast<BufferD3D12&>(buffer);
        transitionBufferState(bufD3D12, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        m_cmdList->SetComputeRootSignature(rootSig);
        m_cmdList->SetPipelineState(pso);
        m_cmdList->SetComputeRootUnorderedAccessView(1, bufD3D12.getDeviceAddress());

        constexpr uint32_t kMaxChunkDwords = 65535u * 64u;
        uint64_t curDword = offset / 4;
        uint64_t remaining = totalDwords;
        while (remaining > 0)
        {
            const uint32_t chunk = static_cast<uint32_t>(std::min<uint64_t>(remaining, kMaxChunkDwords));
            const uint32_t constants[3] = { static_cast<uint32_t>(curDword), chunk, value };
            m_cmdList->SetComputeRoot32BitConstants(0, 3, constants, 0);
            m_cmdList->Dispatch((chunk + 63u) / 64u, 1, 1);
            curDword += chunk;
            remaining -= chunk;
        }

        // Restore NoxRHI's global root signature, descriptor heaps, and invalidate cached PSO.
        restoreBindingsAfterExternalWork();
    }

    void CommandBufferD3D12::copyBuffer(Buffer& srcBuffer, Buffer& dstBuffer, std::span<const BufferCopyRegion> regions)
    {
        auto* srcD3D12 = static_cast<BufferD3D12*>(&srcBuffer);
        auto* dstD3D12 = static_cast<BufferD3D12*>(&dstBuffer);
        transitionBufferState(*srcD3D12, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transitionBufferState(*dstD3D12, D3D12_RESOURCE_STATE_COPY_DEST);
        for (const auto& reg : regions)
        {
            m_cmdList->CopyBufferRegion(dstD3D12->getNativeBuffer(), reg.dstOffset, srcD3D12->getNativeBuffer(), reg.srcOffset, reg.size);
        }
    }

    void CommandBufferD3D12::transitionTextureLayout(Texture& texture, TextureLayout oldLayout, TextureLayout newLayout)
    {
        (void)oldLayout;
        auto* texD3D12 = static_cast<TextureD3D12*>(&texture);
        transitionTextureState(*texD3D12, TranslateTextureLayoutToD3D12(newLayout));
    }

    void CommandBufferD3D12::transitionSwapchainLayout(Swapchain& swapchain, uint32_t imageIndex, TextureLayout oldLayout, TextureLayout newLayout)
    {
        auto* swapD3D12 = static_cast<SwapchainD3D12*>(&swapchain);
        TextureD3D12* backBuffer = swapD3D12->getBackBuffer(imageIndex);
        if (backBuffer)
        {
            transitionTextureLayout(*backBuffer, oldLayout, newLayout);
        }
    }

    void CommandBufferD3D12::resolveImage(Texture& srcTexture, Texture& dstTexture, uint32_t width, uint32_t height)
    {
        auto* srcD3D12 = static_cast<TextureD3D12*>(&srcTexture);
        auto* dstD3D12 = static_cast<TextureD3D12*>(&dstTexture);
        initializeTarget(*srcD3D12);
        initializeTarget(*dstD3D12); // (a resolve is not one of the operations that initialize a target)
        transitionTextureState(*srcD3D12, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        transitionTextureState(*dstD3D12, D3D12_RESOURCE_STATE_RESOLVE_DEST);
        m_cmdList->ResolveSubresource(
            dstD3D12->getNativeResource(), 0,
            srcD3D12->getNativeResource(), 0,
            srcD3D12->getDxgiFormat());
    }

    void CommandBufferD3D12::copyTexture(Texture& srcTexture, Texture& dstTexture, uint32_t width, uint32_t height)
    {
        auto* srcD3D12 = static_cast<TextureD3D12*>(&srcTexture);
        auto* dstD3D12 = static_cast<TextureD3D12*>(&dstTexture);
        initializeTarget(*srcD3D12);
        if (m_deferredStateTracking)
            m_markInitializedTextures.push_back(dstD3D12);
        else
            dstD3D12->markContentsInitialized(); // (a copy initializes its destination)

        transitionTextureState(*srcD3D12, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transitionTextureState(*dstD3D12, D3D12_RESOURCE_STATE_COPY_DEST);

        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource = dstD3D12->getNativeResource();
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = srcD3D12->getNativeResource();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLoc.SubresourceIndex = 0;

        D3D12_BOX box{ 0, 0, 0, width, height, 1 };
        m_cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &box);
    }

    void CommandBufferD3D12::copyTextureMips(Texture& srcTexture, uint32_t srcFirstMip, Texture& dstTexture, uint32_t dstFirstMip, uint32_t mipCount)
    {
        auto* srcD3D12 = static_cast<TextureD3D12*>(&srcTexture);
        auto* dstD3D12 = static_cast<TextureD3D12*>(&dstTexture);
        initializeTarget(*srcD3D12);
        if (m_deferredStateTracking)
            m_markInitializedTextures.push_back(dstD3D12);
        else
            dstD3D12->markContentsInitialized();

        if (m_allocator.getCommandListType() != D3D12_COMMAND_LIST_TYPE_COPY)
        {
            transitionTextureState(*srcD3D12, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transitionTextureState(*dstD3D12, D3D12_RESOURCE_STATE_COPY_DEST);
        }

        for (uint32_t i = 0; i < mipCount; ++i)
        {
            const uint32_t srcMip = srcFirstMip + i;
            const uint32_t dstMip = dstFirstMip + i;

            D3D12_TEXTURE_COPY_LOCATION dstLoc{};
            dstLoc.pResource = dstD3D12->getNativeResource();
            dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dstLoc.SubresourceIndex = dstMip;

            D3D12_TEXTURE_COPY_LOCATION srcLoc{};
            srcLoc.pResource = srcD3D12->getNativeResource();
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            srcLoc.SubresourceIndex = srcMip;

            // Pass nullptr for pSrcBox to copy the entire mip subresource cleanly (including BC-compressed 2x2 and 1x1 mips,
            // where an explicit non-multiple-of-4 D3D12_BOX fails D3D12 validation).
            m_cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
        }
    }

    void CommandBufferD3D12::buildAccelerationStructure(const AccelerationStructureBuildDesc& buildDesc, uint64_t scratchAddress, AccelerationStructure& dstAS)
    {
        const D3D12AccelerationStructureInputs build(buildDesc, false);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
        desc.DestAccelerationStructureData = dstAS.getDeviceAddress();
        desc.ScratchAccelerationStructureData = scratchAddress;
        desc.Inputs = build.inputs;
        m_cmdList->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
    }

    void CommandBufferD3D12::updateAccelerationStructure(const AccelerationStructureBuildDesc& buildDesc, uint64_t scratchAddress, AccelerationStructure& srcAS, AccelerationStructure& dstAS)
    {
        // (the structure must have been built with AllowUpdate: the D3D12 debug layer reports it otherwise)
        const D3D12AccelerationStructureInputs build(buildDesc, true);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
        desc.SourceAccelerationStructureData = srcAS.getDeviceAddress();
        desc.DestAccelerationStructureData = dstAS.getDeviceAddress();
        desc.ScratchAccelerationStructureData = scratchAddress;
        desc.Inputs = build.inputs;
        m_cmdList->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
    }

    void CommandBufferD3D12::accelerationStructureBarrier(AccelerationStructureBarrierType barrierType)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = nullptr;
        m_cmdList->ResourceBarrier(1, &b);
    }

    void CommandBufferD3D12::resetQueries(QueryPool& pool, uint32_t first, uint32_t count)
    {
    }

    void CommandBufferD3D12::writeCompactedSizes(std::span<AccelerationStructure* const> structures, QueryPool& pool, uint32_t firstQuery)
    {
        auto* qp = dynamic_cast<QueryPoolD3D12*>(&pool);
        if (!qp || !qp->getReadbackBuffer() || !qp->getUavBuffer() || structures.empty())
            return;

        const uint64_t byteOffset = static_cast<uint64_t>(firstQuery) * sizeof(uint64_t);
        const uint64_t byteSize = static_cast<uint64_t>(structures.size()) * sizeof(uint64_t);

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC postbuildDesc{};
        postbuildDesc.DestBuffer = qp->getUavBuffer()->GetGPUVirtualAddress() + byteOffset;
        postbuildDesc.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;

        std::vector<D3D12_GPU_VIRTUAL_ADDRESS> addresses;
        addresses.reserve(structures.size());
        for (auto* s : structures)
            addresses.push_back(s->getDeviceAddress());

        m_cmdList->EmitRaytracingAccelerationStructurePostbuildInfo(&postbuildDesc, static_cast<UINT>(addresses.size()), addresses.data());

        D3D12_RESOURCE_BARRIER toCopy{};
        toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopy.Transition.pResource = qp->getUavBuffer();
        toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_cmdList->ResourceBarrier(1, &toCopy);

        m_cmdList->CopyBufferRegion(qp->getReadbackBuffer(), byteOffset, qp->getUavBuffer(), byteOffset, byteSize);

        D3D12_RESOURCE_BARRIER toUav{};
        toUav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toUav.Transition.pResource = qp->getUavBuffer();
        toUav.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        toUav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toUav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_cmdList->ResourceBarrier(1, &toUav);
    }

    void CommandBufferD3D12::copyAccelerationStructure(AccelerationStructure& src, AccelerationStructure& dst, bool compact)
    {
        m_cmdList->CopyRaytracingAccelerationStructure(
            dst.getDeviceAddress(), src.getDeviceAddress(),
            compact ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE);
    }

    void CommandBufferD3D12::executionBarrier()
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = nullptr;
        m_cmdList->ResourceBarrier(1, &b);
    }

    void CommandBufferD3D12::transferBarrier()
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = nullptr;
        m_cmdList->ResourceBarrier(1, &b);
    }

    void CommandBufferD3D12::initializeTarget(TextureD3D12& texture)
    {
        if (m_deferredStateTracking)
        {
            if (m_discardSeen.insert(&texture).second)
                m_pendingDiscardTextures.push_back(&texture);
            return;
        }
        if (!texture.needsInitialDiscard())
            return;
        // DiscardResource wants a render target in RENDER_TARGET and a depth target in DEPTH_WRITE.
        const D3D12_RESOURCE_STATES state = texture.getUsage() == TextureUsage::DepthStencilAttachment ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                                                                                       : D3D12_RESOURCE_STATE_RENDER_TARGET;
        transitionTextureState(texture, state);
        m_cmdList->DiscardResource(texture.getNativeResource(), nullptr);
        texture.markContentsInitialized();
    }

    D3D12_RESOURCE_STATES CommandBufferD3D12::getTrackedTextureState(const TextureD3D12& texture) const
    {
        if (m_deferredStateTracking)
        {
            if (auto it = m_localTexStates.find(const_cast<TextureD3D12*>(&texture)); it != m_localTexStates.end())
                return it->second;
        }
        return texture.getCurrentState();
    }

    void CommandBufferD3D12::transitionTextureState(TextureD3D12& texture, D3D12_RESOURCE_STATES afterState, bool uavBarrierIfSame)
    {
        const bool isComputeList = (m_allocator.getCommandListType() == D3D12_COMMAND_LIST_TYPE_COMPUTE);
        if (m_deferredStateTracking)
        {
            auto it = m_localTexStates.find(&texture);
            if (it == m_localTexStates.end())
            {
                m_firstTexStates.emplace_back(&texture, afterState);
                m_localTexStates.emplace(&texture, afterState);
                if (uavBarrierIfSame && afterState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                {
                    D3D12_RESOURCE_BARRIER b{};
                    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                    b.UAV.pResource = texture.getNativeResource();
                    m_cmdList->ResourceBarrier(1, &b);
                }
                return;
            }
            const D3D12_RESOURCE_STATES beforeState = it->second;
            if (beforeState != afterState && !(isComputeList && isReadOnlySubset(beforeState, afterState)))
            {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = texture.getNativeResource();
                b.Transition.StateBefore = beforeState;
                b.Transition.StateAfter = afterState;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                m_cmdList->ResourceBarrier(1, &b);
                it->second = afterState;
            }
            else if (uavBarrierIfSame && beforeState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                b.UAV.pResource = texture.getNativeResource();
                m_cmdList->ResourceBarrier(1, &b);
            }
            return;
        }

        const D3D12_RESOURCE_STATES beforeState = texture.getCurrentState();
        if (beforeState != afterState && !(isComputeList && isReadOnlySubset(beforeState, afterState)))
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = texture.getNativeResource();
            b.Transition.StateBefore = beforeState;
            b.Transition.StateAfter = afterState;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            m_cmdList->ResourceBarrier(1, &b);
            texture.setCurrentState(afterState);
        }
        else if (uavBarrierIfSame && beforeState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            b.UAV.pResource = texture.getNativeResource();
            m_cmdList->ResourceBarrier(1, &b);
        }
    }

    void CommandBufferD3D12::transitionBufferState(BufferD3D12& buffer, D3D12_RESOURCE_STATES afterState, bool uavBarrierIfSame)
    {
        if (buffer.isUploadHeap() || buffer.isReadbackHeap())
            return;
        // On a D3D12 COPY command list, buffers implicitly promote from COMMON and decay back to COMMON on execution end.
        if (m_allocator.getCommandListType() == D3D12_COMMAND_LIST_TYPE_COPY)
        {
            buffer.setCurrentState(D3D12_RESOURCE_STATE_COMMON);
            return;
        }
        if (buffer.getInitialState() == D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)
        {
            if (uavBarrierIfSame)
            {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                b.UAV.pResource = buffer.getNativeBuffer();
                m_cmdList->ResourceBarrier(1, &b);
            }
            return;
        }

        if (m_deferredStateTracking)
        {
            auto it = m_localBufStates.find(&buffer);
            if (it == m_localBufStates.end())
            {
                if (afterState != D3D12_RESOURCE_STATE_COMMON)
                {
                    m_firstBufStates.emplace_back(&buffer, afterState);
                    m_localBufStates.emplace(&buffer, afterState);
                }
                if (uavBarrierIfSame && afterState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                {
                    D3D12_RESOURCE_BARRIER b{};
                    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                    b.UAV.pResource = buffer.getNativeBuffer();
                    m_cmdList->ResourceBarrier(1, &b);
                }
                return;
            }
            const D3D12_RESOURCE_STATES beforeState = it->second;
            if (beforeState != D3D12_RESOURCE_STATE_COMMON && afterState != D3D12_RESOURCE_STATE_COMMON &&
                (beforeState & ~D3D12_RESOURCE_STATE_GENERIC_READ) == 0 &&
                (afterState & ~D3D12_RESOURCE_STATE_GENERIC_READ) == 0)
            {
                afterState |= beforeState;
            }
            if (beforeState != afterState && afterState != D3D12_RESOURCE_STATE_COMMON)
            {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = buffer.getNativeBuffer();
                b.Transition.StateBefore = beforeState;
                b.Transition.StateAfter = afterState;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                m_cmdList->ResourceBarrier(1, &b);
                it->second = afterState;
            }
            else if (uavBarrierIfSame)
            {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                b.UAV.pResource = buffer.getNativeBuffer();
                m_cmdList->ResourceBarrier(1, &b);
            }
            return;
        }

        const D3D12_RESOURCE_STATES beforeState = buffer.getCurrentState();
        if (beforeState != D3D12_RESOURCE_STATE_COMMON && afterState != D3D12_RESOURCE_STATE_COMMON &&
            (beforeState & ~D3D12_RESOURCE_STATE_GENERIC_READ) == 0 &&
            (afterState & ~D3D12_RESOURCE_STATE_GENERIC_READ) == 0)
        {
            afterState |= beforeState;
        }
        if (beforeState != afterState && afterState != D3D12_RESOURCE_STATE_COMMON)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = buffer.getNativeBuffer();
            b.Transition.StateBefore = beforeState;
            b.Transition.StateAfter = afterState;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            m_cmdList->ResourceBarrier(1, &b);
            buffer.setCurrentState(afterState);
        }
        else if (uavBarrierIfSame)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            b.UAV.pResource = buffer.getNativeBuffer();
            m_cmdList->ResourceBarrier(1, &b);
        }
    }

    void CommandBufferD3D12::resourceBarriers(std::span<const TextureBarrierDesc> textures, std::span<const BufferBarrierDesc> buffers)
    {
        const bool isComputeList = (m_allocator.getCommandListType() == D3D12_COMMAND_LIST_TYPE_COMPUTE);
        for (const auto& tb : textures)
        {
            if (!tb.texture)
                continue;
            auto* texD3D12 = static_cast<TextureD3D12*>(tb.texture);
            const D3D12_RESOURCE_STATES afterState = TranslateResourceStateToD3D12(tb.after, isComputeList);
            transitionTextureState(*texD3D12, afterState, (tb.after.access & AccessBits::ShaderWrite) != 0);
        }
        for (const auto& bb : buffers)
        {
            if (!bb.buffer)
                continue;
            auto* bufD3D12 = static_cast<BufferD3D12*>(bb.buffer);
            const D3D12_RESOURCE_STATES afterState = TranslateResourceStateToD3D12(bb.after, isComputeList);
            constexpr uint32_t writes = AccessBits::ShaderWrite | AccessBits::AccelerationStructureWrite;
            const bool fixedState = bufD3D12->getInitialState() == D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
            const bool needsUav = ((bb.before.access & writes) != 0) || ((bb.after.access & writes) != 0) ||
                                  (fixedState && (((bb.before.access | bb.after.access) & AccessBits::AccelerationStructureRead) != 0));
            transitionBufferState(*bufD3D12, afterState, needsUav);
        }
    }

    bool CommandBufferD3D12::resolveDeferredBarriers(CommandBuffer& fixupCmdBase)
    {
        auto& fixup = static_cast<CommandBufferD3D12&>(fixupCmdBase);
        bool recorded = false;
        auto ensureOpen = [&]()
        {
            if (!recorded)
            {
                fixup.m_allocator.reset();
                fixup.setDeferredStateTracking(false);
                fixup.begin(0, true);
                recorded = true;
            }
        };

        // 1. Placed render/depth targets that still need initial DiscardResource before first use.
        for (TextureD3D12* tex : m_pendingDiscardTextures)
        {
            if (tex && tex->needsInitialDiscard())
            {
                ensureOpen();
                fixup.initializeTarget(*tex);
            }
        }

        // 2. Resolve first-use texture transitions and commit each texture's final state from this chunk.
        const bool isComputeFixup = (fixup.m_allocator.getCommandListType() == D3D12_COMMAND_LIST_TYPE_COMPUTE);
        for (const auto& [tex, firstState] : m_firstTexStates)
        {
            const D3D12_RESOURCE_STATES globalState = tex->getCurrentState();
            const D3D12_RESOURCE_STATES finalState = m_localTexStates[tex];
            if (globalState != firstState && !(isComputeFixup && finalState == firstState && isReadOnlySubset(globalState, firstState)))
            {
                ensureOpen();
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = tex->getNativeResource();
                b.Transition.StateBefore = globalState;
                b.Transition.StateAfter = firstState;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                fixup.m_cmdList->ResourceBarrier(1, &b);
                tex->setCurrentState(finalState);
            }
            else if (finalState != firstState)
            {
                tex->setCurrentState(finalState);
            }
        }

        // 3. Resolve first-use buffer transitions and commit each buffer's final state from this chunk.
        for (const auto& [buf, firstState] : m_firstBufStates)
        {
            if (buf->isUploadHeap() || buf->isReadbackHeap() ||
                buf->getInitialState() == D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)
                continue;
            const D3D12_RESOURCE_STATES globalState = buf->getCurrentState();
            const D3D12_RESOURCE_STATES finalState = m_localBufStates[buf];
            const bool canSkipReadSubset = (finalState == firstState) &&
                                           globalState != D3D12_RESOURCE_STATE_COMMON &&
                                           firstState != D3D12_RESOURCE_STATE_COMMON &&
                                           (globalState & ~D3D12_RESOURCE_STATE_GENERIC_READ) == 0 &&
                                           (firstState & ~D3D12_RESOURCE_STATE_GENERIC_READ) == 0 &&
                                           (globalState & firstState) == firstState;
            if (globalState != firstState && firstState != D3D12_RESOURCE_STATE_COMMON && !canSkipReadSubset)
            {
                ensureOpen();
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = buf->getNativeBuffer();
                b.Transition.StateBefore = globalState;
                b.Transition.StateAfter = firstState;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                fixup.m_cmdList->ResourceBarrier(1, &b);
                buf->setCurrentState(finalState);
            }
            else if (finalState != firstState)
            {
                buf->setCurrentState(finalState);
            }
        }

        for (TextureD3D12* tex : m_markInitializedTextures)
        {
            if (tex)
                tex->markContentsInitialized();
        }

        m_deferredStateTracking = false;
        if (recorded)
            fixup.end(0);
        return recorded;
    }

    void CommandBufferD3D12::beginDebugLabel(const char* label)
    {
        if (m_cmdList && label && label[0] != '\0')
        {
            if (!m_debugPassList.empty())
                m_debugPassList += " | ";
            m_debugPassList += label;
            // Metadata = 0 (PIX_EVENT_UNICODE_VERSION): required by D3D12 DRED BreadcrumbContexts.
            std::wstring wlabel;
            for (const char* p = label; *p; ++p)
                wlabel.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
            m_cmdList->BeginEvent(0, wlabel.c_str(), static_cast<UINT>((wlabel.size() + 1) * sizeof(wchar_t)));
        }
    }

    void CommandBufferD3D12::endDebugLabel()
    {
        if (m_cmdList)
            m_cmdList->EndEvent();
    }
}
