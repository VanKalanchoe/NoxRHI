#include "NRIRenderGraph/RenderGraph.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <format>
#include <stdexcept>

namespace nri
{
    namespace
    {
        void RGAssertCheck(bool cond, const char* msg)
        {
            if (!cond)
                throw std::runtime_error(msg);
        }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // FrameArena

    void* RenderGraph::FrameArena::Allocate(size_t size, size_t alignment)
    {
        const size_t alignMask = alignment > 0 ? alignment - 1 : 0;
        for (;;)
        {
            if (m_BlockIndex >= m_Blocks.size())
            {
                const size_t blockSize = std::max(DefaultBlockSize, size + alignment);
                Block block;
                block.Memory = std::make_unique<std::byte[]>(blockSize);
                block.Size = blockSize;
                m_Blocks.push_back(std::move(block));
                m_Offset = 0;
            }

            Block& current = m_Blocks[m_BlockIndex];
            const size_t aligned = (m_Offset + alignMask) & ~alignMask;
            if (aligned + size <= current.Size)
            {
                m_Offset = aligned + size;
                return current.Memory.get() + aligned;
            }
            ++m_BlockIndex;
            m_Offset = 0;
        }
    }

    void RenderGraph::FrameArena::Reset()
    {
        m_BlockIndex = 0;
        m_Offset = 0;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // RGPassContext

    RGPassContext::RGPassContext(RenderGraph& graph, CommandBuffer& cmd, Extent2D renderArea, uint32_t passIndex)
        : m_Graph(graph), m_Cmd(cmd), m_RenderArea(renderArea), m_PassIndex(passIndex)
    {
    }

    NriTexture RGPassContext::Texture(RGTexture texture) const
    {
        if (m_Graph.m_ValidationEnabled)
        {
            m_Graph.ValidateHandle(texture.Frame, m_PassIndex != RGInvalidIndex ? m_Graph.m_Passes[m_PassIndex].Name : "Texture Inspection");
            m_Graph.ValidateContextAccess(m_PassIndex, texture.Index, true);
        }
        NriTexture resource = m_Graph.GetTexture(texture);
        RGAssertCheck(resource != nullptr, "RGPassContext::Texture: texture is invalid or was not allocated");
        return resource;
    }

    NriBuffer RGPassContext::Buffer(RGBuffer buffer) const
    {
        if (m_Graph.m_ValidationEnabled)
        {
            m_Graph.ValidateHandle(buffer.Frame, m_PassIndex != RGInvalidIndex ? m_Graph.m_Passes[m_PassIndex].Name : "Texture Inspection");
            m_Graph.ValidateContextAccess(m_PassIndex, buffer.Index, false);
        }
        RGAssertCheck(buffer.IsValid() && buffer.Index < m_Graph.m_Buffers.size() && m_Graph.m_Buffers[buffer.Index].Buffer != nullptr,
                      "RGPassContext::Buffer: buffer is invalid or was not allocated");
        return m_Graph.m_Buffers[buffer.Index].Buffer;
    }

    uint64_t RGPassContext::BufferReadRef(RGBuffer buffer) const
    {
        NriBuffer handle = Buffer(buffer);
        if (m_Graph.m_Device && m_Graph.m_Device->getAPI() == NRI_GRAPHICS_API_D3D12)
        {
            const uint32_t slot = m_Graph.m_Buffers[buffer.Index].ReadSlot;
            RGAssertCheck(slot != ~0u, "RGPassContext::BufferReadRef: buffer has no D3D12 SRV slot");
            return static_cast<uint64_t>(slot);
        }
        return nriGetBufferDeviceAddress(handle);
    }

    uint64_t RGPassContext::BufferWriteRef(RGBuffer buffer) const
    {
        NriBuffer handle = Buffer(buffer);
        if (m_Graph.m_Device && m_Graph.m_Device->getAPI() == NRI_GRAPHICS_API_D3D12)
        {
            const uint32_t slot = m_Graph.m_Buffers[buffer.Index].WriteSlot;
            RGAssertCheck(slot != ~0u, "RGPassContext::BufferWriteRef: buffer has no D3D12 UAV slot");
            return static_cast<uint64_t>(slot);
        }
        return nriGetBufferDeviceAddress(handle);
    }

    uint32_t RGPassContext::Slot(RGTexture texture) const
    {
        return nriGetTextureDescriptorSlot(Texture(texture));
    }

    uint32_t RGPassContext::SlotOr(RGTexture texture, uint32_t fallback) const
    {
        if (m_Graph.m_ValidationEnabled && texture.IsValid() && m_Graph.GetTexture(texture))
            m_Graph.ValidateContextAccess(m_PassIndex, texture.Index, true);
        return m_Graph.GetSlotOr(texture, fallback);
    }

    uint32_t RGPassContext::StorageSlot(RGTexture texture, uint32_t mip) const
    {
        if (m_Graph.m_ValidationEnabled)
            m_Graph.ValidateContextAccess(m_PassIndex, texture.Index, true);
        RGAssertCheck(texture.IsValid() && texture.Index < m_Graph.m_Textures.size(), "RGPassContext::StorageSlot: invalid texture");
        const std::vector<uint32_t>* slots = m_Graph.m_Textures[texture.Index].StorageSlots;
        RGAssertCheck(slots && mip < slots->size(), "RGPassContext::StorageSlot: texture has no storage slot for this mip");
        return (*slots)[mip];
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Access states (Precise synchronization)

    namespace
    {
        uint32_t ShaderStages(RGPassFlags flags)
        {
            if (HasFlag(flags, RGPassFlags::RayTracing)) return StageBits::RayTracing;
            // (raster passes draw with vertex shaders as well as task / mesh shaders: a buffer a vertex shader reads needs its stage)
            return HasFlag(flags, RGPassFlags::Raster) ? StageBits::Vertex | StageBits::Task | StageBits::Mesh | StageBits::Fragment : StageBits::Compute;
        }

        ResourceState TextureState(RGTextureAccess access, RGPassFlags flags)
        {
            switch (access)
            {
            case RGTextureAccess::Sampled:
            case RGTextureAccess::StorageRead: return { AccessBits::ShaderRead, ShaderStages(flags) };
            case RGTextureAccess::StorageWrite: return { AccessBits::ShaderWrite, ShaderStages(flags) };
            case RGTextureAccess::ColorTarget: return { AccessBits::ColorAttachmentWrite, StageBits::ColorAttachmentOutput };
            case RGTextureAccess::DepthTarget: return { AccessBits::DepthStencilWrite, StageBits::FragmentTests };
            case RGTextureAccess::CopySource: return { AccessBits::TransferRead, StageBits::Transfer };
            case RGTextureAccess::CopyDestination: return { AccessBits::TransferWrite, StageBits::Transfer };
            }
            return {};
        }

        ResourceState BufferState(RGBufferAccess access, RGPassFlags flags)
        {
            switch (access)
            {
            case RGBufferAccess::Read: return { AccessBits::ShaderRead, ShaderStages(flags) };
            case RGBufferAccess::IndirectRead: return { AccessBits::IndirectRead, StageBits::Indirect };
            case RGBufferAccess::Write: return { AccessBits::ShaderWrite, ShaderStages(flags) };
            case RGBufferAccess::AccelerationStructureBuild: return { AccessBits::AccelerationStructureWrite, StageBits::AccelerationStructureBuild };
            case RGBufferAccess::AccelerationStructureBuildInput: return { AccessBits::AccelerationStructureRead, StageBits::AccelerationStructureBuild };
            case RGBufferAccess::AccelerationStructureRead: return { AccessBits::AccelerationStructureRead | AccessBits::ShaderRead, ShaderStages(flags) };
            case RGBufferAccess::CopySource: return { AccessBits::TransferRead, StageBits::Transfer };
            case RGBufferAccess::CopyDestination: return { AccessBits::TransferWrite, StageBits::Transfer };
            }
            return {};
        }

        constexpr uint32_t ComputeQueueStages = StageBits::Compute | StageBits::Transfer | StageBits::AccelerationStructureBuild |
                                                StageBits::Indirect | StageBits::RayTracing;
        constexpr uint32_t ComputeQueueAccess = AccessBits::ShaderRead | AccessBits::ShaderWrite | AccessBits::TransferRead |
                                                AccessBits::TransferWrite | AccessBits::AccelerationStructureRead |
                                                AccessBits::AccelerationStructureWrite | AccessBits::IndirectRead;

        bool HasWriteAccess(const ResourceState& state)
        {
            constexpr uint32_t writeBits = AccessBits::ShaderWrite | AccessBits::ColorAttachmentWrite | AccessBits::DepthStencilWrite |
                                           AccessBits::TransferWrite | AccessBits::AccelerationStructureWrite;
            return (state.access & writeBits) != 0;
        }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // RGBuilder

    RGTexture RGBuilder::Read(RGTexture texture, RGTextureAccess access)
    {
        if (m_Graph.m_ValidationEnabled)
            m_Graph.ValidateHandle(texture.Frame, m_Graph.m_Passes[m_PassIndex].Name);
        m_Graph.AddAccess(m_PassIndex, texture.Index, true, IsWrite(access), TextureState(access, m_Graph.m_Passes[m_PassIndex].Flags));
        return texture;
    }

    RGTexture RGBuilder::Write(RGTexture texture, RGTextureAccess access)
    {
        if (m_Graph.m_ValidationEnabled)
            m_Graph.ValidateHandle(texture.Frame, m_Graph.m_Passes[m_PassIndex].Name);
        m_Graph.AddAccess(m_PassIndex, texture.Index, true, true, TextureState(access, m_Graph.m_Passes[m_PassIndex].Flags));
        return texture;
    }

    RGBuffer RGBuilder::Read(RGBuffer buffer, RGBufferAccess access)
    {
        if (m_Graph.m_ValidationEnabled)
            m_Graph.ValidateHandle(buffer.Frame, m_Graph.m_Passes[m_PassIndex].Name);
        m_Graph.AddAccess(m_PassIndex, buffer.Index, false, IsWrite(access), BufferState(access, m_Graph.m_Passes[m_PassIndex].Flags));
        return buffer;
    }

    RGBuffer RGBuilder::Write(RGBuffer buffer, RGBufferAccess access)
    {
        if (m_Graph.m_ValidationEnabled)
            m_Graph.ValidateHandle(buffer.Frame, m_Graph.m_Passes[m_PassIndex].Name);
        m_Graph.AddAccess(m_PassIndex, buffer.Index, false, true, BufferState(access, m_Graph.m_Passes[m_PassIndex].Flags));
        return buffer;
    }

    void RGBuilder::ColorTarget(RGTexture texture, LoadOp load, StoreOp store, ClearColor clear)
    {
        if (m_Graph.m_ValidationEnabled)
            m_Graph.ValidateHandle(texture.Frame, m_Graph.m_Passes[m_PassIndex].Name);
        if (load == NRI_LOAD_OP_LOAD)
            m_Graph.AddAccess(m_PassIndex, texture.Index, true, false, { AccessBits::ColorAttachmentRead, StageBits::ColorAttachmentOutput });
        m_Graph.AddAccess(m_PassIndex, texture.Index, true, true, { AccessBits::ColorAttachmentWrite, StageBits::ColorAttachmentOutput });

        RenderGraph::Attachment attachment;
        attachment.Texture = texture.Index;
        attachment.Load = load;
        attachment.Store = store;
        attachment.ClearColorVal = clear;
        m_Graph.m_Passes[m_PassIndex].ColorTargets.push_back(attachment);
    }

    void RGBuilder::DepthTarget(RGTexture texture, LoadOp load, StoreOp store, ClearDepth clear)
    {
        if (m_Graph.m_ValidationEnabled)
            m_Graph.ValidateHandle(texture.Frame, m_Graph.m_Passes[m_PassIndex].Name);
        if (load == NRI_LOAD_OP_LOAD)
            m_Graph.AddAccess(m_PassIndex, texture.Index, true, false, { AccessBits::DepthStencilRead, StageBits::FragmentTests });
        m_Graph.AddAccess(m_PassIndex, texture.Index, true, true, { AccessBits::DepthStencilWrite, StageBits::FragmentTests });

        RenderGraph::Attachment& attachment = m_Graph.m_Passes[m_PassIndex].DepthTarget;
        attachment.Texture = texture.Index;
        attachment.Load = load;
        attachment.Store = store;
        attachment.ClearDepthVal = clear;
    }

    void RGBuilder::SwapchainTarget(const Swapchain& swapchain, uint32_t imageIndex, LoadOp load, StoreOp store, ClearColor clear)
    {
        SwapchainTarget(swapchain.handle(), imageIndex, load, store, clear);
    }

    void RGBuilder::SwapchainTarget(NriSwapchain swapchain, uint32_t imageIndex, LoadOp load, StoreOp store, ClearColor clear)
    {
        RenderGraph::Pass& pass = m_Graph.m_Passes[m_PassIndex];
        pass.Swapchain = swapchain;
        pass.SwapchainImage = imageIndex;
        pass.SwapchainLoad = load;
        pass.SwapchainStore = store;
        pass.SwapchainClear = clear;
    }

    void RGBuilder::SetRenderArea(Extent2D area, RGViewport viewport)
    {
        RenderGraph::Pass& pass = m_Graph.m_Passes[m_PassIndex];
        pass.RenderArea = area;
        pass.Viewport = viewport;
    }

    void RGBuilder::RecordExclusive(const char* key)
    {
        m_Graph.m_Passes[m_PassIndex].ExclusiveMask |= 1u << m_Graph.GetExclusiveKeyBit(key);
    }

    // ---------------------------------------------------------------------------------------------------------------
    // RenderGraph: frame and resources

    RenderGraph::~RenderGraph()
    {
        ReleaseCallbacks();
        if (m_ProfilerHooks.DestroyChunkContext)
        {
            for (void* ctx : m_ProfilerChunkContexts)
            {
                if (ctx)
                    m_ProfilerHooks.DestroyChunkContext(ctx, m_ProfilerHooks.UserData);
            }
        }
        m_ProfilerChunkContexts.clear();
    }

    void RenderGraph::Log(RGLogLevel level, std::string_view message) const
    {
        if (m_LogCallback)
        {
            m_LogCallback(level, message, m_LogUserData);
            return;
        }
        const char* prefix = level == RGLogLevel::Error ? "[NRIRenderGraph][ERROR]" :
                             level == RGLogLevel::Warn  ? "[NRIRenderGraph][WARN]" : "[NRIRenderGraph][INFO]";
        std::fprintf(stderr, "%s %.*s\n", prefix, static_cast<int>(message.size()), message.data());
    }

    void RenderGraph::SetProfilerHooks(const RGProfilerHooks& hooks)
    {
        if (m_ProfilerHooks.DestroyChunkContext)
        {
            for (void* ctx : m_ProfilerChunkContexts)
            {
                if (ctx)
                    m_ProfilerHooks.DestroyChunkContext(ctx, m_ProfilerHooks.UserData);
            }
        }
        m_ProfilerChunkContexts.clear();
        m_ScopeIds.clear();
        m_ProfilerHooks = hooks;

        m_ProfilerChunkContexts.resize(MaxRecordedChunks, nullptr);
        if (m_ProfilerHooks.CreateChunkContext)
        {
            for (void*& ctx : m_ProfilerChunkContexts)
                ctx = m_ProfilerHooks.CreateChunkContext(m_ProfilerHooks.UserData);
        }
    }

    void RenderGraph::Initialize(Device& device, DescriptorHeap& resourceHeap, uint32_t framesInFlight)
    {
        m_Device = &device;
        m_FramesInFlight = framesInFlight;
        m_Pool.Initialize(device, resourceHeap);

        m_Recorders.resize(static_cast<size_t>(MaxRecordedChunks) * framesInFlight);
        m_ChunkScratch.resize(MaxRecordedChunks);
        m_ProfilerChunkContexts.resize(MaxRecordedChunks, nullptr);
        if (m_ProfilerHooks.CreateChunkContext)
        {
            for (void*& ctx : m_ProfilerChunkContexts)
            {
                if (!ctx)
                    ctx = m_ProfilerHooks.CreateChunkContext(m_ProfilerHooks.UserData);
            }
        }

        if (device.hasAsyncComputeQueue())
        {
            m_GraphicsTimeline = device.createTimelineSemaphore(0);
            m_ComputeTimeline = device.createTimelineSemaphore(0);
        }
    }

    void RenderGraph::ReleaseResources()
    {
        m_Textures.clear();
        m_Buffers.clear();
        m_Pool.ReleaseAll();

        m_SubmitList.clear();
        for (CommandRecorder& recorder : m_Recorders)
        {
            recorder.CmdBuffer.reset();
            recorder.Allocator.reset();
        }
    }

    void RenderGraph::Reset(const RGFrameDesc& frame)
    {
        ReleaseCallbacks();
        m_ExecuteArena.Reset();

        m_Frame = frame;
        ++m_FrameId;
        m_Pool.BeginFrame(frame.FrameNumber);

        m_InspectedTexture = RGInvalidIndex;
        m_Inspector = nullptr;
        m_InspectionPass = RGInvalidIndex;
        m_InspectionActive = false;
        m_ValidationMessages.clear();
        m_UnusedWrites.clear();

        m_PassCount = 0;
        m_Events.clear();
        m_GroupNames.clear();
        m_OpenGroupDepth = 0;
        m_Textures.clear();
        m_Buffers.clear();
        m_ImportedTextures.clear();
        m_ImportedBuffers.clear();
        m_Blackboard.NextFrame();

        m_FrameHistoryResets.swap(m_PendingHistoryResets);
        m_PendingHistoryResets.clear();
        m_FrameResetAll = m_PendingResetAll;
        m_PendingResetAll = false;
    }

    RGTextureKey RenderGraph::ResolveKey(const RGTextureDesc& desc) const
    {
        RGTextureKey key;
        switch (desc.Size)
        {
        case RGSize::RenderResolution:
            key.Width = m_Frame.RenderExtent.width;
            key.Height = m_Frame.RenderExtent.height;
            break;
        case RGSize::OutputResolution:
            key.Width = m_Frame.OutputExtent.width;
            key.Height = m_Frame.OutputExtent.height;
            break;
        case RGSize::Absolute:
            key.Width = desc.Width;
            key.Height = desc.Height;
            break;
        }
        key.Format = desc.Format;
        key.Usage = desc.Usage;
        key.MipLevels = desc.MipLevels;
        return key;
    }

    RGTexture RenderGraph::CreateTexture(const char* name, const RGTextureDesc& desc)
    {
        TextureEntry entry;
        entry.Name = name;
        entry.Kind = RGResourceKind::Transient;
        entry.Key = ResolveKey(desc);
        m_Textures.push_back(entry);
        return { static_cast<uint32_t>(m_Textures.size() - 1), m_FrameId };
    }

    RGBuffer RenderGraph::CreateBuffer(const char* name, const RGBufferDesc& desc)
    {
        BufferEntry entry;
        entry.Name = name;
        entry.Kind = RGResourceKind::Transient;
        entry.Key = { desc.Size, desc.Usage };
        m_Buffers.push_back(entry);
        return { static_cast<uint32_t>(m_Buffers.size() - 1), m_FrameId };
    }

    RGTextureHistory RenderGraph::GetHistoryTexture(const char* key, const RGTextureDesc& desc, uint32_t count)
    {
        RGAssertCheck(count > 0 && count <= RGMaxHistoryLength, "RenderGraph::GetHistoryTexture: invalid history length");

        bool created = false;
        RGResourcePool::TextureHistory& history = m_Pool.AcquireTextureHistory(key, ResolveKey(desc), count, created);

        RGTextureHistory result;
        result.Count = count;
        result.WasReset = created || WasHistoryReset(key);
        for (uint32_t i = 0; i < count; ++i)
        {
            TextureEntry entry;
            entry.Name = key;
            entry.Kind = RGResourceKind::History;
            entry.Key = history.Key;
            entry.Texture = history.Textures[i]->Handle.handle();
            entry.StorageSlots = &history.Textures[i]->StorageSlots;
            m_Textures.push_back(entry);
            result.Textures[i] = { static_cast<uint32_t>(m_Textures.size() - 1), m_FrameId };
        }
        return result;
    }

    RGBufferHistory RenderGraph::GetHistoryBuffer(const char* key, const RGBufferDesc& desc, uint32_t count)
    {
        RGAssertCheck(count > 0 && count <= RGMaxHistoryLength, "RenderGraph::GetHistoryBuffer: invalid history length");

        bool created = false;
        RGResourcePool::BufferHistory& history = m_Pool.AcquireBufferHistory(key, { desc.Size, desc.Usage }, count, created);

        RGBufferHistory result;
        result.Count = count;
        result.WasReset = created || WasHistoryReset(key);
        for (uint32_t i = 0; i < count; ++i)
        {
            BufferEntry entry;
            entry.Name = key;
            entry.Kind = RGResourceKind::History;
            entry.Key = history.Key;
            entry.Buffer = history.Buffers[i]->Handle.handle();
            entry.ReadSlot = history.Buffers[i]->ReadSlot;
            entry.WriteSlot = history.Buffers[i]->WriteSlot;
            m_Buffers.push_back(entry);
            result.Buffers[i] = { static_cast<uint32_t>(m_Buffers.size() - 1), m_FrameId };
        }
        return result;
    }

    RGTexture RenderGraph::ImportTexture(const char* name, const Texture& texture, RGImportAccess access)
    {
        return ImportTexture(name, texture.handle(), access);
    }

    RGTexture RenderGraph::ImportTexture(const char* name, NriTexture texture, RGImportAccess access)
    {
        RGAssertCheck(texture != nullptr, "RenderGraph::ImportTexture: null texture");
        auto [found, inserted] = m_ImportedTextures.try_emplace(texture, static_cast<uint32_t>(m_Textures.size()));
        if (inserted)
        {
            TextureEntry entry;
            entry.Name = name;
            entry.Kind = RGResourceKind::Imported;
            entry.ReadOnly = access == RGImportAccess::ReadOnly;
            entry.Key.Width = nriGetTextureWidth(texture);
            entry.Key.Height = nriGetTextureHeight(texture);
            entry.Texture = texture;
            m_Textures.push_back(entry);
        }
        return { found->second, m_FrameId };
    }

    RGBuffer RenderGraph::ImportBuffer(const char* name, const Buffer& buffer, RGImportAccess access, uint32_t readSlot, uint32_t writeSlot)
    {
        return ImportBuffer(name, buffer.handle(), access, readSlot, writeSlot);
    }

    RGBuffer RenderGraph::ImportBuffer(const char* name, NriBuffer buffer, RGImportAccess access, uint32_t readSlot, uint32_t writeSlot)
    {
        RGAssertCheck(buffer != nullptr, "RenderGraph::ImportBuffer: null buffer");
        auto [found, inserted] = m_ImportedBuffers.try_emplace(buffer, static_cast<uint32_t>(m_Buffers.size()));
        if (inserted)
        {
            BufferEntry entry;
            entry.Name = name;
            entry.Kind = RGResourceKind::Imported;
            entry.ReadOnly = access == RGImportAccess::ReadOnly;
            entry.Buffer = buffer;
            entry.ReadSlot = readSlot;
            entry.WriteSlot = writeSlot;
            m_Buffers.push_back(entry);
        }
        else
        {
            BufferEntry& entry = m_Buffers[found->second];
            if (readSlot != ~0u)
                entry.ReadSlot = readSlot;
            if (writeSlot != ~0u)
                entry.WriteSlot = writeSlot;
        }
        return { found->second, m_FrameId };
    }

    void RenderGraph::ResetHistory(const char* key)
    {
        m_PendingHistoryResets.emplace(key);
    }

    void RenderGraph::ResetAllHistory()
    {
        m_PendingResetAll = true;
    }

    bool RenderGraph::WasHistoryReset(const char* key) const
    {
        return m_FrameResetAll || m_FrameHistoryResets.contains(key);
    }

    NriTexture RenderGraph::GetTexture(RGTexture texture) const
    {
        if (!texture.IsValid() || texture.Index >= m_Textures.size())
            return nullptr;
        return m_Textures[texture.Index].Texture;
    }

    uint32_t RenderGraph::GetSlotOr(RGTexture texture, uint32_t fallback) const
    {
        NriTexture resource = GetTexture(texture);
        return resource ? nriGetTextureDescriptorSlot(resource) : fallback;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // RenderGraph: passes

    void RenderGraph::PushGroup(const char* name)
    {
        m_Events.push_back({ EventType::GroupBegin, static_cast<uint32_t>(m_GroupNames.size()) });
        m_GroupNames.push_back(name);
        ++m_OpenGroupDepth;
    }

    void RenderGraph::PopGroup()
    {
        RGAssertCheck(m_OpenGroupDepth > 0, "RenderGraph::PopGroup without PushGroup");
        m_Events.push_back({ EventType::GroupEnd, 0 });
        --m_OpenGroupDepth;
    }

    uint32_t RenderGraph::BeginPass(const char* name, RGPassFlags flags)
    {
        if (m_PassCount == m_Passes.size())
            m_Passes.emplace_back();

        Pass& pass = m_Passes[m_PassCount];
        pass.Name = name;
        pass.Flags = flags;
        pass.Accesses.clear();
        pass.ColorTargets.clear();
        pass.DepthTarget = {};
        pass.Swapchain = nullptr;
        pass.SwapchainImage = 0;
        pass.SwapchainLoad = NRI_LOAD_OP_LOAD;
        pass.SwapchainStore = NRI_STORE_OP_STORE;
        pass.SwapchainClear = {};
        pass.RenderArea = {};
        pass.Viewport = RGViewport::FlippedY;
        pass.Payload = nullptr;
        pass.Invoke = nullptr;
        pass.Destroy = nullptr;
        pass.ExclusiveMask = 0;
        pass.RecordMs = 0.0f;
        pass.Culled = false;
        pass.SynchronizeAfter = false;
        pass.FirstTextureBarrier = 0;
        pass.TextureBarrierCount = 0;
        pass.FirstBufferBarrier = 0;
        pass.BufferBarrierCount = 0;

        m_Events.push_back({ EventType::Pass, m_PassCount });
        return m_PassCount++;
    }

    void RenderGraph::AddAccess(uint32_t passIndex, uint32_t resource, bool isTexture, bool isWrite, ResourceState state)
    {
        RGAssertCheck(resource != RGInvalidIndex, "RenderGraph: pass declares an invalid resource handle");
        m_Passes[passIndex].Accesses.push_back({ resource, isTexture, isWrite, state });
    }

    uint32_t RenderGraph::GetExclusiveKeyBit(const char* key)
    {
        for (uint32_t bit = 0; bit < m_ExclusiveKeys.size(); ++bit)
        {
            if (std::strcmp(m_ExclusiveKeys[bit], key) == 0)
                return bit;
        }
        RGAssertCheck(m_ExclusiveKeys.size() < RGTaskDispatcher::MaxExclusiveKeys, "RenderGraph: too many exclusive recording keys");
        m_ExclusiveKeys.push_back(key);
        return static_cast<uint32_t>(m_ExclusiveKeys.size() - 1);
    }

    bool RenderGraph::HasSideEffects(const Pass& pass) const
    {
        if (HasFlag(pass.Flags, RGPassFlags::NeverCull) || pass.Swapchain)
            return true;

        for (const ResourceAccess& access : pass.Accesses)
        {
            if (!access.IsWrite)
                continue;
            const RGResourceKind kind = access.IsTexture ? m_Textures[access.Resource].Kind : m_Buffers[access.Resource].Kind;
            if (kind != RGResourceKind::Transient)
                return true;
        }
        return false;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // RenderGraph: compile

    void RenderGraph::Compile()
    {
        RGAssertCheck(m_OpenGroupDepth == 0, "RenderGraph::Compile with an unclosed group");

        const size_t textureCount = m_Textures.size();
        auto resourceKey = [textureCount](const ResourceAccess& access)
        {
            return access.IsTexture ? access.Resource : static_cast<uint32_t>(textureCount) + access.Resource;
        };

        m_ResourceRead.assign(textureCount + m_Buffers.size(), false);
        for (uint32_t index = m_PassCount; index-- > 0;)
        {
            Pass& pass = m_Passes[index];
            bool needed = HasSideEffects(pass);
            for (size_t i = 0; !needed && i < pass.Accesses.size(); ++i)
                needed = pass.Accesses[i].IsWrite && m_ResourceRead[resourceKey(pass.Accesses[i])];

            pass.Culled = !needed;
            if (!needed)
                continue;

            for (const ResourceAccess& access : pass.Accesses)
            {
                if (!access.IsWrite)
                    m_ResourceRead[resourceKey(access)] = true;
            }

            if (HasFlag(pass.Flags, RGPassFlags::Raster) && pass.RenderArea.width == 0 && pass.RenderArea.height == 0)
                Log(RGLogLevel::Error, std::format("RenderGraph: raster pass '{}' has no render area", pass.Name));
        }

        for (uint32_t index = 0; index < m_PassCount; ++index)
        {
            const Pass& pass = m_Passes[index];
            if (pass.Culled)
                continue;

            for (const ResourceAccess& access : pass.Accesses)
            {
                uint32_t& firstPass = access.IsTexture ? m_Textures[access.Resource].FirstPass : m_Buffers[access.Resource].FirstPass;
                uint32_t& lastPass = access.IsTexture ? m_Textures[access.Resource].LastPass : m_Buffers[access.Resource].LastPass;
                firstPass = std::min(firstPass, index);
                lastPass = std::max(lastPass, index);
            }
        }
        PlanQueues();
        AllocateTransients();

        if (m_Synchronization == RGSynchronization::Blanket)
            PlanBlanketSynchronization();
        else
            PlanPreciseSynchronization();

        PlaceInspection();
        if (m_ValidationEnabled)
            ValidateCompiledFrame();
    }

    void RenderGraph::PlanBlanketSynchronization()
    {
        const size_t textureCount = m_Textures.size();
        std::vector<uint32_t>& lastWriter = m_LastWriterScratch;
        lastWriter.assign(textureCount + m_Buffers.size(), RGInvalidIndex);
        for (uint32_t index = 0; index < m_PassCount; ++index)
        {
            Pass& pass = m_Passes[index];
            if (pass.Culled)
                continue;

            for (const ResourceAccess& access : pass.Accesses)
            {
                const uint32_t writer = lastWriter[access.IsTexture ? access.Resource : textureCount + access.Resource];
                if (writer != RGInvalidIndex && writer != index)
                    m_Passes[writer].SynchronizeAfter = true;
            }
            for (const ResourceAccess& access : pass.Accesses)
            {
                if (access.IsWrite)
                    lastWriter[access.IsTexture ? access.Resource : textureCount + access.Resource] = index;
            }
        }
    }

    void RenderGraph::PlanPreciseSynchronization()
    {
        m_TextureBarriers.clear();
        m_BufferBarriers.clear();

        const size_t textureCount = m_Textures.size();
        const size_t resourceCount = textureCount + m_Buffers.size();

        auto physical = [&](size_t key) -> const void*
        {
            return key < textureCount ? static_cast<const void*>(m_Textures[key].Texture)
                                      : static_cast<const void*>(m_Buffers[key - textureCount].Buffer);
        };

        std::vector<uint32_t>& slotOf = m_PhysicalSlotScratch;
        std::vector<const void*>& slotResource = m_SlotResourceScratch;
        std::unordered_map<const void*, uint32_t>& slotByResource = m_SlotByResourceScratch;
        slotOf.resize(resourceCount);
        slotResource.clear();
        slotByResource.clear();
        for (size_t key = 0; key < resourceCount; ++key)
        {
            const void* resource = physical(key);
            if (!resource)
            {
                slotOf[key] = static_cast<uint32_t>(slotResource.size());
                slotResource.push_back(nullptr);
                continue;
            }
            auto [entry, added] = slotByResource.try_emplace(resource, static_cast<uint32_t>(slotResource.size()));
            if (added)
                slotResource.push_back(resource);
            slotOf[key] = entry->second;
        }
        const size_t slotCount = slotResource.size();

        std::vector<ResourceState>& lastWrite = m_LastWriteScratch;
        std::vector<ResourceState>& readsSinceWrite = m_ReadsSinceWriteScratch;
        std::vector<ResourceState>(&visible)[2] = m_VisibleScratch;
        std::vector<bool>& accessed = m_AccessedScratch;
        std::vector<bool>& written = m_WrittenScratch;
        lastWrite.assign(slotCount, ResourceState{});
        readsSinceWrite.assign(slotCount, ResourceState{});
        visible[0].assign(slotCount, ResourceState{});
        visible[1].assign(slotCount, ResourceState{});
        accessed.assign(slotCount, false);
        written.assign(slotCount, false);

        for (size_t slot = 0; slot < slotCount; ++slot)
        {
            const void* resource = slotResource[slot];
            auto carried = resource ? m_CarriedStates.find(resource) : m_CarriedStates.end();
            if (carried == m_CarriedStates.end())
                continue;
            lastWrite[slot] = carried->second.LastWrite;
            readsSinceWrite[slot] = carried->second.ReadsSinceWrite;
            visible[0][slot] = carried->second.Visible[0];
            visible[1][slot] = carried->second.Visible[1];
            written[slot] = carried->second.Written;
            accessed[slot] = true;
        }

        auto merge = [](ResourceState& into, const ResourceState& state)
        {
            into.access |= state.access;
            into.stages |= state.stages;
        };
        auto covers = [](const ResourceState& covered, const ResourceState& state)
        {
            return (covered.access & state.access) == state.access && (covered.stages & state.stages) == state.stages;
        };

        for (uint32_t index = 0; index < m_PassCount; ++index)
        {
            Pass& pass = m_Passes[index];
            if (pass.Culled)
                continue;

            std::vector<std::pair<uint32_t, ResourceState>>& passStates = m_PassStatesScratch;
            passStates.clear();
            for (const ResourceAccess& access : pass.Accesses)
            {
                const uint32_t key = access.IsTexture ? access.Resource : static_cast<uint32_t>(textureCount) + access.Resource;
                auto merged = std::find_if(passStates.begin(), passStates.end(), [key](const auto& entry) { return entry.first == key; });
                if (merged == passStates.end())
                {
                    passStates.emplace_back(key, access.State);
                    continue;
                }
                merged->second.access |= access.State.access;
                merged->second.stages |= access.State.stages;
            }

            pass.FirstTextureBarrier = static_cast<uint32_t>(m_TextureBarriers.size());
            pass.FirstBufferBarrier = static_cast<uint32_t>(m_BufferBarriers.size());
            const uint32_t queue = pass.Queue == NRI_QUEUE_COMPUTE ? 1 : 0;
            for (const auto& [key, state] : passStates)
            {
                const uint32_t slot = slotOf[key];
                auto addBarrier = [&](ResourceState before)
                {
                    if (pass.Queue == NRI_QUEUE_COMPUTE)
                    {
                        before.stages &= ComputeQueueStages;
                        before.access &= ComputeQueueAccess;
                        if (before.stages == StageBits::None)
                            return;
                    }
                    if (key < textureCount)
                        m_TextureBarriers.push_back({ m_Textures[key].Texture, before, state });
                    else
                        m_BufferBarriers.push_back({ m_Buffers[key - textureCount].Buffer, before, state });
                };

                if (HasWriteAccess(state))
                {
                    if (accessed[slot])
                    {
                        ResourceState before = lastWrite[slot];
                        merge(before, readsSinceWrite[slot]);
                        addBarrier(before);
                    }
                    lastWrite[slot] = state;
                    readsSinceWrite[slot] = {};
                    visible[queue][slot] = state;
                    visible[1 - queue][slot] = {};
                    written[slot] = true;
                }
                else
                {
                    if (written[slot] && !covers(visible[queue][slot], state))
                    {
                        addBarrier(lastWrite[slot]);
                        merge(visible[queue][slot], state);
                    }
                    merge(readsSinceWrite[slot], state);
                }
                accessed[slot] = true;
            }
            pass.TextureBarrierCount = static_cast<uint32_t>(m_TextureBarriers.size()) - pass.FirstTextureBarrier;
            pass.BufferBarrierCount = static_cast<uint32_t>(m_BufferBarriers.size()) - pass.FirstBufferBarrier;
        }

        m_CarriedStates.clear();
        for (size_t slot = 0; slot < slotCount; ++slot)
        {
            const void* resource = slotResource[slot];
            if (resource && accessed[slot])
                m_CarriedStates[resource] = CarriedState{ lastWrite[slot], readsSinceWrite[slot], { visible[0][slot], visible[1][slot] }, written[slot] };
        }
    }

    void RenderGraph::PlanQueues()
    {
        m_AsyncBlocks.clear();
        m_AsyncComputePassCount = 0;
        std::vector<uint32_t>& executed = m_ExecutedScratch;
        executed.clear();
        for (uint32_t index = 0; index < m_PassCount; ++index)
        {
            m_Passes[index].Queue = NRI_QUEUE_GRAPHICS;
            if (!m_Passes[index].Culled)
                executed.push_back(index);
        }
        if (!m_AsyncComputeEnabled || !m_GraphicsTimeline || m_Synchronization != RGSynchronization::Precise || m_InspectedTexture != RGInvalidIndex)
            return;

        const uint32_t count = static_cast<uint32_t>(executed.size());
        auto pass = [&](uint32_t position) -> Pass& { return m_Passes[executed[position]]; };
        bool anyAsync = false;
        for (uint32_t position = 0; position < count; ++position)
        {
            if (HasFlag(pass(position).Flags, RGPassFlags::AsyncCompute))
            {
                pass(position).Queue = NRI_QUEUE_COMPUTE;
                anyAsync = true;
            }
        }
        if (!anyAsync)
            return;

        const size_t textureCount = m_Textures.size();
        auto resourceKey = [textureCount](const ResourceAccess& access)
        {
            return access.IsTexture ? access.Resource : static_cast<uint32_t>(textureCount) + access.Resource;
        };
        std::vector<uint8_t>& blockAccess = m_BlockAccessScratch;

        for (bool replan = true; replan;)
        {
            replan = false;
            m_AsyncBlocks.clear();
            for (uint32_t first = 0; first < count && !replan;)
            {
                if (pass(first).Queue != NRI_QUEUE_COMPUTE)
                {
                    ++first;
                    continue;
                }
                uint32_t end = first;
                while (end < count && pass(end).Queue == NRI_QUEUE_COMPUTE)
                    ++end;

                blockAccess.assign(textureCount + m_Buffers.size(), 0);
                for (uint32_t position = first; position < end; ++position)
                {
                    for (const ResourceAccess& access : pass(position).Accesses)
                        blockAccess[resourceKey(access)] |= access.IsWrite ? 2 : 1;
                }
                auto conflicts = [&](const Pass& graphics)
                {
                    for (const ResourceAccess& access : graphics.Accesses)
                    {
                        const uint8_t blockUse = blockAccess[resourceKey(access)];
                        if (blockUse != 0 && (access.IsWrite || (blockUse & 2) != 0))
                            return true;
                    }
                    return false;
                };

                uint32_t fork = RGInvalidIndex;
                uint32_t firstGraphics = RGInvalidIndex;
                for (uint32_t position = 0; position < first; ++position)
                {
                    if (pass(position).Queue == NRI_QUEUE_COMPUTE)
                        continue;
                    if (firstGraphics == RGInvalidIndex)
                        firstGraphics = position;
                    if (conflicts(pass(position)))
                        fork = position;
                }
                if (fork == RGInvalidIndex)
                    fork = firstGraphics;
                if (fork != RGInvalidIndex && !m_AsyncBlocks.empty())
                    fork = std::max(fork, m_AsyncBlocks.back().Fork);

                uint32_t join = RGInvalidIndex;
                bool graphicsAfterFork = false;
                for (uint32_t position = fork == RGInvalidIndex ? count : fork + 1; position < count; ++position)
                {
                    if (pass(position).Queue == NRI_QUEUE_COMPUTE)
                        continue;
                    graphicsAfterFork = true;
                    if (position >= end && join == RGInvalidIndex && conflicts(pass(position)))
                        join = position;
                }

                if (fork == RGInvalidIndex || !graphicsAfterFork)
                {
                    for (uint32_t position = first; position < end; ++position)
                        pass(position).Queue = NRI_QUEUE_GRAPHICS;
                    replan = true;
                    break;
                }
                m_AsyncBlocks.push_back({ first, end, fork, join });
                first = end;
            }
        }

        std::vector<uint32_t> boundaries;
        for (const AsyncBlock& block : m_AsyncBlocks)
        {
            boundaries.push_back(block.Fork + 1);
            if (block.Join != RGInvalidIndex)
                boundaries.push_back(block.Join);
        }
        std::sort(boundaries.begin(), boundaries.end());
        boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
        if (boundaries.size() + 1 + m_AsyncBlocks.size() > MaxRecordedChunks)
        {
            for (uint32_t position = 0; position < count; ++position)
                pass(position).Queue = NRI_QUEUE_GRAPHICS;
            m_AsyncBlocks.clear();
            return;
        }

        for (const AsyncBlock& block : m_AsyncBlocks)
        {
            const uint32_t forkPass = executed[block.Fork];
            const uint32_t joinPass = block.Join != RGInvalidIndex ? executed[block.Join] : m_PassCount - 1;
            for (uint32_t position = block.First; position < block.End; ++position)
            {
                for (const ResourceAccess& access : pass(position).Accesses)
                {
                    uint32_t& firstPass = access.IsTexture ? m_Textures[access.Resource].FirstPass : m_Buffers[access.Resource].FirstPass;
                    uint32_t& lastPass = access.IsTexture ? m_Textures[access.Resource].LastPass : m_Buffers[access.Resource].LastPass;
                    firstPass = std::min(firstPass, forkPass);
                    lastPass = std::max(lastPass, joinPass);
                }
            }
            m_AsyncComputePassCount += block.End - block.First;
        }
    }

    void RenderGraph::AllocateTransients()
    {
        std::vector<uint32_t>& order = m_AllocationOrderScratch;

        order.clear();
        for (uint32_t index = 0; index < m_Textures.size(); ++index)
        {
            if (m_Textures[index].Kind == RGResourceKind::Transient && m_Textures[index].FirstPass != RGInvalidIndex)
                order.push_back(index);
        }
        std::sort(order.begin(), order.end(), [this](uint32_t a, uint32_t b) { return m_Textures[a].FirstPass < m_Textures[b].FirstPass; });
        for (uint32_t index : order)
        {
            TextureEntry& entry = m_Textures[index];
            RGResourcePool::TextureEntry& texture = m_Pool.AcquireTransientTexture(entry.Key, entry.FirstPass, entry.LastPass);
            entry.Texture = texture.Handle.handle();
            entry.StorageSlots = &texture.StorageSlots;
        }

        order.clear();
        for (uint32_t index = 0; index < m_Buffers.size(); ++index)
        {
            if (m_Buffers[index].Kind == RGResourceKind::Transient && m_Buffers[index].FirstPass != RGInvalidIndex)
                order.push_back(index);
        }
        std::sort(order.begin(), order.end(), [this](uint32_t a, uint32_t b) { return m_Buffers[a].FirstPass < m_Buffers[b].FirstPass; });
        for (uint32_t index : order)
        {
            BufferEntry& entry = m_Buffers[index];
            RGResourcePool::BufferEntry& buffer = m_Pool.AcquireTransientBuffer(entry.Key, entry.FirstPass, entry.LastPass);
            entry.Buffer = buffer.Handle.handle();
            entry.ReadSlot = buffer.ReadSlot;
            entry.WriteSlot = buffer.WriteSlot;
        }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // RenderGraph: execute

    std::span<const RGSubmission> RenderGraph::Execute(uint32_t frameSlot)
    {
        RGAssertCheck(m_Device && frameSlot < m_FramesInFlight, "RenderGraph::Execute: not initialized or invalid frame slot");

        PlanChunks();
        m_Units.clear();
        if (!m_AsyncBlocks.empty())
        {
            float totalMs = 0.0f;
            for (const uint32_t index : m_ExecutedScratch)
                totalMs += GetEstimatedRecordMs(m_Passes[index]);
            PlanAsyncChunks(totalMs / static_cast<float>(std::max<size_t>(m_Chunks.size(), 1)));
        }
        PrepareRecording(frameSlot);

        const uint32_t chunkCount = static_cast<uint32_t>(m_Chunks.size());
        if (m_ProfilerHooks.BeginGpuFrame)
            m_ProfilerHooks.BeginGpuFrame(frameSlot, m_ProfilerHooks.UserData);

        if (chunkCount == 1)
        {
            RecordChunk(0);
        }
        else
        {
            RGTaskDispatcher* dispatcher = m_TaskDispatcher ? m_TaskDispatcher : &m_DefaultDispatcher;
            dispatcher->RunTasks("Record Command Chunk", std::span<const uint32_t>(m_ChunkExclusiveMasks.data(), chunkCount),
                                 [this](uint32_t chunkIndex) { RecordChunk(chunkIndex); });
        }

        if (m_ProfilerHooks.EndGpuFrame)
            m_ProfilerHooks.EndGpuFrame(std::span<void* const>(m_ProfilerChunkContexts.data(), chunkCount), m_ProfilerHooks.UserData);

        for (uint32_t index = 0; index < m_PassCount; ++index)
        {
            const Pass& pass = m_Passes[index];
            if (pass.Culled)
                continue;

            auto [found, inserted] = m_PassRecordMs.try_emplace(pass.Name, pass.RecordMs);
            if (!inserted)
                found->second += (pass.RecordMs - found->second) * 0.1f;
        }

        m_SubmitList.clear();
        for (uint32_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex)
            m_SubmitList.push_back(m_Recorders[static_cast<size_t>(chunkIndex) * m_FramesInFlight + frameSlot].CmdBuffer.handle());

        m_Submissions.clear();
        if (m_Units.empty())
        {
            m_Submissions.push_back({ NRI_QUEUE_GRAPHICS, std::span<const NriCommandBuffer>(m_SubmitList), {}, {} });
        }
        else
        {
            m_SubmissionWaits.resize(m_Units.size());
            m_SubmissionSignals.resize(m_Units.size());
            std::vector<uint64_t> forkValues(m_AsyncBlocks.size(), 0);
            std::vector<uint64_t> computeValues(m_AsyncBlocks.size(), 0);
            for (size_t unitIndex = 0; unitIndex < m_Units.size(); ++unitIndex)
            {
                const SubmissionUnit& unit = m_Units[unitIndex];
                std::vector<TimelinePoint>& waits = m_SubmissionWaits[unitIndex];
                std::vector<TimelinePoint>& signals = m_SubmissionSignals[unitIndex];
                waits.clear();
                signals.clear();
                if (unit.Queue == NRI_QUEUE_COMPUTE)
                {
                    waits.push_back({ m_GraphicsTimeline.handle(), forkValues[unit.Block] });
                    computeValues[unit.Block] = ++m_ComputeTimelineValue;
                    signals.push_back({ m_ComputeTimeline.handle(), computeValues[unit.Block] });
                }
                else
                {
                    for (const uint32_t block : unit.JoinBlocks)
                        waits.push_back({ m_ComputeTimeline.handle(), computeValues[block] });
                    if (!unit.ForkBlocks.empty())
                    {
                        const uint64_t value = ++m_GraphicsTimelineValue;
                        for (const uint32_t block : unit.ForkBlocks)
                            forkValues[block] = value;
                        signals.push_back({ m_GraphicsTimeline.handle(), value });
                    }
                }
                m_Submissions.push_back({ unit.Queue,
                                          std::span<const NriCommandBuffer>(m_SubmitList.data() + unit.FirstChunk, unit.ChunkCount),
                                          std::span<const TimelinePoint>(waits),
                                          std::span<const TimelinePoint>(signals) });
            }
        }

        if (m_ReportEnabled)
            BuildReport();

        ReleaseCallbacks();
        m_Pool.EndFrame();
        return m_Submissions;
    }

    void RenderGraph::PlanAsyncChunks(float targetMs)
    {
        m_Chunks.clear();
        m_ChunkOpenGroups.clear();
        m_ChunkExclusiveMasks.clear();
        m_ChunkRecordMs.clear();
        m_Units.clear();

        m_PassEvent.assign(m_PassCount, 0);
        for (uint32_t eventIndex = 0; eventIndex < m_Events.size(); ++eventIndex)
        {
            if (m_Events[eventIndex].Type == EventType::Pass)
                m_PassEvent[m_Events[eventIndex].Index] = eventIndex;
        }

        const uint32_t count = static_cast<uint32_t>(m_ExecutedScratch.size());
        std::vector<uint32_t> boundaries{ 0, count };
        for (const AsyncBlock& block : m_AsyncBlocks)
        {
            boundaries.push_back(block.Fork + 1);
            if (block.Join != RGInvalidIndex)
                boundaries.push_back(block.Join);
        }
        std::sort(boundaries.begin(), boundaries.end());
        boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

        std::vector<uint32_t> pendingJoins;
        size_t lastGraphicsUnit = 0;
        for (size_t segment = 0; segment + 1 < boundaries.size(); ++segment)
        {
            const uint32_t begin = boundaries[segment];
            const uint32_t end = boundaries[segment + 1];
            for (uint32_t block = 0; block < m_AsyncBlocks.size(); ++block)
            {
                if (m_AsyncBlocks[block].Join == begin)
                    pendingJoins.push_back(block);
            }
            bool hasGraphics = false;
            for (uint32_t position = begin; position < end && !hasGraphics; ++position)
                hasGraphics = m_Passes[m_ExecutedScratch[position]].Queue == NRI_QUEUE_GRAPHICS;
            if (!hasGraphics)
                continue;

            SubmissionUnit graphics;
            graphics.Queue = NRI_QUEUE_GRAPHICS;
            graphics.FirstChunk = static_cast<uint32_t>(m_Chunks.size());
            graphics.JoinBlocks = std::move(pendingJoins);
            pendingJoins.clear();
            AddChunk(NRI_QUEUE_GRAPHICS, begin, end, targetMs);
            graphics.ChunkCount = static_cast<uint32_t>(m_Chunks.size()) - graphics.FirstChunk;
            for (uint32_t block = 0; block < m_AsyncBlocks.size(); ++block)
            {
                if (m_AsyncBlocks[block].Fork + 1 == end)
                    graphics.ForkBlocks.push_back(block);
            }
            const std::vector<uint32_t> startedBlocks = graphics.ForkBlocks;
            lastGraphicsUnit = m_Units.size();
            m_Units.push_back(std::move(graphics));

            for (const uint32_t block : startedBlocks)
            {
                SubmissionUnit compute;
                compute.Queue = NRI_QUEUE_COMPUTE;
                compute.FirstChunk = static_cast<uint32_t>(m_Chunks.size());
                compute.Block = block;
                AddChunk(NRI_QUEUE_COMPUTE, m_AsyncBlocks[block].First, m_AsyncBlocks[block].End, targetMs);
                compute.ChunkCount = static_cast<uint32_t>(m_Chunks.size()) - compute.FirstChunk;
                m_Units.push_back(std::move(compute));
            }
        }
        for (uint32_t block = 0; block < m_AsyncBlocks.size(); ++block)
        {
            if (m_AsyncBlocks[block].Join == RGInvalidIndex)
                m_Units[lastGraphicsUnit].JoinBlocks.push_back(block);
        }
    }

    void RenderGraph::AddChunk(QueueType queue, uint32_t firstPosition, uint32_t endPosition, float targetMs)
    {
        const uint32_t unitsLeft = static_cast<uint32_t>(m_AsyncBlocks.size()) * 3 + 1;
        bool open = false;
        uint32_t chunkPasses = 0;
        bool previousOwnCommandBuffer = false; // (RGPassFlags::OwnCommandBuffer: a chunk boundary before and after such a pass)
        for (uint32_t position = firstPosition; position < endPosition; ++position)
        {
            Pass& pass = m_Passes[m_ExecutedScratch[position]];
            if (pass.Queue != queue)
                continue;
            const float passMs = GetEstimatedRecordMs(pass);
            const bool ownCommandBuffer = HasFlag(pass.Flags, RGPassFlags::OwnCommandBuffer);
            const bool isolate = (ownCommandBuffer || previousOwnCommandBuffer);
            previousOwnCommandBuffer = ownCommandBuffer;
            const bool split = open && chunkPasses > 0 && (isolate || m_ChunkRecordMs.back() + passMs * 0.5f > targetMs) &&
                               m_Chunks.size() + unitsLeft < MaxRecordedChunks;
            if (!open || split)
            {
                Chunk chunk;
                chunk.FirstEvent = m_PassEvent[m_ExecutedScratch[position]];
                chunk.Queue = queue;
                std::vector<uint32_t>& stack = m_PlanStackScratch;
                stack.clear();
                for (uint32_t eventIndex = 0; eventIndex < chunk.FirstEvent; ++eventIndex)
                {
                    if (m_Events[eventIndex].Type == EventType::GroupBegin)
                        stack.push_back(m_Events[eventIndex].Index);
                    else if (m_Events[eventIndex].Type == EventType::GroupEnd)
                        stack.pop_back();
                }
                chunk.OpenGroupsOffset = static_cast<uint32_t>(m_ChunkOpenGroups.size());
                chunk.OpenGroupsCount = static_cast<uint32_t>(stack.size());
                m_ChunkOpenGroups.insert(m_ChunkOpenGroups.end(), stack.begin(), stack.end());
                m_Chunks.push_back(chunk);
                m_ChunkExclusiveMasks.push_back(0);
                m_ChunkRecordMs.push_back(0.0f);
                open = true;
                chunkPasses = 0;
            }
            pass.Chunk = static_cast<uint32_t>(m_Chunks.size() - 1);
            m_Chunks.back().EndEvent = m_PassEvent[m_ExecutedScratch[position]] + 1;
            m_ChunkExclusiveMasks.back() |= pass.ExclusiveMask;
            m_ChunkRecordMs.back() += passMs;
            ++chunkPasses;
        }
    }

    float RenderGraph::GetEstimatedRecordMs(const Pass& pass) const
    {
        const auto found = m_PassRecordMs.find(pass.Name);
        return found != m_PassRecordMs.end() ? found->second : DefaultPassRecordMs;
    }

    void RenderGraph::PlanChunks()
    {
        float totalMs = 0.0f;
        uint32_t executedPasses = 0;
        for (uint32_t index = 0; index < m_PassCount; ++index)
        {
            if (!m_Passes[index].Culled)
            {
                totalMs += GetEstimatedRecordMs(m_Passes[index]);
                ++executedPasses;
            }
        }

        uint32_t bestCount = 1;
        RGTaskDispatcher* dispatcher = m_TaskDispatcher ? m_TaskDispatcher : &m_DefaultDispatcher;
        const uint32_t workers = std::max(1u, dispatcher->GetWorkerCount());
        if (m_ParallelRecording && workers > 1)
        {
            const uint32_t maxChunks = std::min({ MaxChunks, workers, std::max(executedPasses, 1u) });
            float bestMs = totalMs;
            for (uint32_t chunkCount = 2; chunkCount <= maxChunks; ++chunkCount)
            {
                const float estimatedMs = SplitIntoChunks(chunkCount, totalMs);
                if (estimatedMs < bestMs)
                {
                    bestMs = estimatedMs;
                    bestCount = chunkCount;
                }
            }
        }
        SplitIntoChunks(bestCount, totalMs);
    }

    float RenderGraph::SplitIntoChunks(uint32_t chunkCount, float totalMs)
    {
        m_Chunks.clear();
        m_ChunkOpenGroups.clear();
        m_ChunkExclusiveMasks.clear();
        m_ChunkRecordMs.clear();

        const float targetMs = totalMs / static_cast<float>(chunkCount);

        std::vector<uint32_t>& stack = m_PlanStackScratch;
        stack.clear();
        m_Chunks.push_back({ 0, 0, 0, 0 });
        m_ChunkExclusiveMasks.push_back(0);
        m_ChunkRecordMs.push_back(0.0f);

        uint32_t chunkPasses = 0;
        uint32_t splitEvent = RGInvalidIndex;
        uint32_t splitStackSize = 0;
        bool previousOwnCommandBuffer = false; // (RGPassFlags::OwnCommandBuffer: a chunk boundary before and after such a pass)
        for (uint32_t eventIndex = 0; eventIndex < m_Events.size(); ++eventIndex)
        {
            const Event& event = m_Events[eventIndex];
            if (event.Type == EventType::GroupEnd)
            {
                stack.pop_back();
                continue;
            }
            if (event.Type == EventType::Pass && m_Passes[event.Index].Culled)
                continue;

            if (splitEvent == RGInvalidIndex)
            {
                splitEvent = eventIndex;
                splitStackSize = static_cast<uint32_t>(stack.size());
            }

            if (event.Type == EventType::GroupBegin)
            {
                stack.push_back(event.Index);
                continue;
            }

            const Pass& pass = m_Passes[event.Index];
            const float passMs = GetEstimatedRecordMs(pass);
            const bool ownCommandBuffer = HasFlag(pass.Flags, RGPassFlags::OwnCommandBuffer);
            const bool isolate = chunkPasses > 0 && (ownCommandBuffer || previousOwnCommandBuffer) && m_Chunks.size() < MaxRecordedChunks;
            previousOwnCommandBuffer = ownCommandBuffer;
            if (isolate || (chunkPasses > 0 && m_Chunks.size() < chunkCount && m_ChunkRecordMs.back() + passMs * 0.5f > targetMs))
            {
                m_Chunks.back().EndEvent = splitEvent;
                Chunk next;
                next.FirstEvent = splitEvent;
                next.OpenGroupsOffset = static_cast<uint32_t>(m_ChunkOpenGroups.size());
                next.OpenGroupsCount = splitStackSize;
                m_ChunkOpenGroups.insert(m_ChunkOpenGroups.end(), stack.begin(), stack.begin() + splitStackSize);
                m_Chunks.push_back(next);
                m_ChunkExclusiveMasks.push_back(0);
                m_ChunkRecordMs.push_back(0.0f);
                chunkPasses = 0;
            }

            m_Passes[event.Index].Chunk = static_cast<uint32_t>(m_Chunks.size() - 1);
            m_ChunkRecordMs.back() += passMs;
            ++chunkPasses;
            m_ChunkExclusiveMasks.back() |= pass.ExclusiveMask;
            splitEvent = RGInvalidIndex;
        }
        m_Chunks.back().EndEvent = static_cast<uint32_t>(m_Events.size());

        float wallMs = 0.0f;
        for (float chunkMs : m_ChunkRecordMs)
            wallMs = std::max(wallMs, chunkMs);
        for (uint32_t key = 0; key < m_ExclusiveKeys.size(); ++key)
        {
            float serialMs = 0.0f;
            for (uint32_t chunkIndex = 0; chunkIndex < m_Chunks.size(); ++chunkIndex)
            {
                if ((m_ChunkExclusiveMasks[chunkIndex] & (1u << key)) != 0)
                    serialMs += m_ChunkRecordMs[chunkIndex];
            }
            wallMs = std::max(wallMs, serialMs);
        }
        return wallMs + ChunkOverheadMs * static_cast<float>(m_Chunks.size() - 1);
    }

    void RenderGraph::PrepareRecording(uint32_t frameSlot)
    {
        m_FrameSlot = frameSlot;

        for (uint32_t chunkIndex = 0; chunkIndex < m_Chunks.size(); ++chunkIndex)
        {
            CommandRecorder& recorder = m_Recorders[static_cast<size_t>(chunkIndex) * m_FramesInFlight + frameSlot];
            if (!recorder.Allocator || recorder.Queue != m_Chunks[chunkIndex].Queue)
            {
                recorder.CmdBuffer.reset();
                recorder.Allocator = m_Device->createCommandAllocator(NRI_COMMAND_BUFFER_RESET_WITH_ALLOCATOR, m_Chunks[chunkIndex].Queue);
                recorder.CmdBuffer = recorder.Allocator.allocateCommandBuffer(1);
                recorder.Queue = m_Chunks[chunkIndex].Queue;
            }
            if (m_ProfilerHooks.SetChunkContextAsyncCompute && m_ProfilerChunkContexts[chunkIndex])
            {
                m_ProfilerHooks.SetChunkContextAsyncCompute(
                    m_ProfilerChunkContexts[chunkIndex],
                    m_Chunks[chunkIndex].Queue == NRI_QUEUE_COMPUTE,
                    m_ProfilerHooks.UserData);
            }
        }

        m_GroupScopeIds.resize(m_GroupNames.size());
        for (uint32_t group = 0; group < m_GroupNames.size(); ++group)
            m_GroupScopeIds[group] = GetScopeId(m_GroupNames[group]);
        for (uint32_t index = 0; index < m_PassCount; ++index)
        {
            if (!m_Passes[index].Culled)
                m_Passes[index].ScopeId = GetScopeId(m_Passes[index].Name);
        }
    }

    void RenderGraph::RecordChunk(uint32_t chunkIndex)
    {
        const Chunk& chunk = m_Chunks[chunkIndex];
        const bool firstChunk = chunkIndex == 0;
        const bool lastChunk = chunkIndex + 1 == m_Chunks.size();
        CommandRecorder& recorder = m_Recorders[static_cast<size_t>(chunkIndex) * m_FramesInFlight + m_FrameSlot];
        ChunkScratch& scratch = m_ChunkScratch[chunkIndex];
        void* gpuContext = m_ProfilerChunkContexts[chunkIndex];

        recorder.Allocator.resetPool();
        CommandBuffer& cmd = recorder.CmdBuffer;
        cmd.begin(0, true);

        if (firstChunk && m_ProfilerHooks.RecordGpuFrameBegin)
            m_ProfilerHooks.RecordGpuFrameBegin(cmd, m_ProfilerHooks.UserData);

        if (m_CommandBufferSetup)
            m_CommandBufferSetup(cmd, chunk.Queue);

        std::vector<uint32_t>& groupStack = scratch.GroupStack;
        groupStack.assign(m_ChunkOpenGroups.begin() + chunk.OpenGroupsOffset,
                          m_ChunkOpenGroups.begin() + chunk.OpenGroupsOffset + chunk.OpenGroupsCount);
        uint32_t openedDepth = 0;

        auto closeOpenedGroup = [&]()
        {
            if (m_ProfilerHooks.EndGpuZone)
                m_ProfilerHooks.EndGpuZone(gpuContext, cmd, m_ProfilerHooks.UserData);
            if (m_ProfilerHooks.EndCpuZone)
                m_ProfilerHooks.EndCpuZone(m_GroupScopeIds[groupStack[openedDepth - 1]], m_ProfilerHooks.UserData);
            cmd.endDebugLabel();
            --openedDepth;
        };

        for (uint32_t eventIndex = chunk.FirstEvent; eventIndex < chunk.EndEvent; ++eventIndex)
        {
            const Event& event = m_Events[eventIndex];
            switch (event.Type)
            {
            case EventType::GroupBegin:
                groupStack.push_back(event.Index);
                break;

            case EventType::GroupEnd:
                if (openedDepth == groupStack.size())
                    closeOpenedGroup();
                groupStack.pop_back();
                break;

            case EventType::Pass:
            {
                Pass& pass = m_Passes[event.Index];
                if (pass.Culled || pass.Chunk != chunkIndex)
                    break;

                while (openedDepth < groupStack.size())
                {
                    const char* groupName = m_GroupNames[groupStack[openedDepth]];
                    const uint32_t scopeId = m_GroupScopeIds[groupStack[openedDepth]];
                    cmd.beginDebugLabel(groupName);
                    if (m_ProfilerHooks.BeginCpuZone)
                        m_ProfilerHooks.BeginCpuZone(scopeId, groupName, m_ProfilerHooks.UserData);
                    if (m_ProfilerHooks.BeginGpuZone)
                        m_ProfilerHooks.BeginGpuZone(gpuContext, cmd, scopeId, groupName, m_ProfilerHooks.UserData);
                    ++openedDepth;
                }

                ExecutePass(pass, cmd, scratch, gpuContext);
                break;
            }
            }
        }

        while (openedDepth > 0)
            closeOpenedGroup();

        if (lastChunk && m_InspectionActive && m_InspectionPass == RGInvalidIndex)
            RecordInspection(cmd);

        if (lastChunk && m_ProfilerHooks.RecordGpuFrameEnd)
            m_ProfilerHooks.RecordGpuFrameEnd(cmd, m_ProfilerHooks.UserData);

        cmd.end(0);
    }

    void RenderGraph::ExecutePass(Pass& pass, CommandBuffer& cmd, ChunkScratch& scratch, void* gpuContext)
    {
        const auto start = std::chrono::steady_clock::now();
        const uint32_t passIndex = static_cast<uint32_t>(&pass - m_Passes.data());
        cmd.beginDebugLabel(pass.Name);
        if (m_ProfilerHooks.BeginCpuZone)
            m_ProfilerHooks.BeginCpuZone(pass.ScopeId, pass.Name, m_ProfilerHooks.UserData);
        if (m_ProfilerHooks.BeginGpuZone)
            m_ProfilerHooks.BeginGpuZone(gpuContext, cmd, pass.ScopeId, pass.Name, m_ProfilerHooks.UserData);
        if (HasFlag(pass.Flags, RGPassFlags::Raster) && m_ProfilerHooks.BeginGpuStatistics)
            m_ProfilerHooks.BeginGpuStatistics(gpuContext, cmd, pass.ScopeId, m_ProfilerHooks.UserData);

        if (pass.TextureBarrierCount > 0 || pass.BufferBarrierCount > 0)
        {
            cmd.resourceBarriers(std::span<const TextureBarrierDesc>(m_TextureBarriers.data() + pass.FirstTextureBarrier, pass.TextureBarrierCount),
                                 std::span<const BufferBarrierDesc>(m_BufferBarriers.data() + pass.FirstBufferBarrier, pass.BufferBarrierCount));
        }

        if (pass.Swapchain)
            cmd.transitionSwapchainLayout(pass.Swapchain, pass.SwapchainImage, NRI_TEXTURE_LAYOUT_UNDEFINED, NRI_TEXTURE_LAYOUT_COLOR_ATTACHMENT);

        const bool raster = HasFlag(pass.Flags, RGPassFlags::Raster);
        if (raster)
            BeginRendering(pass, cmd, scratch.RenderDescScratch);

        RGPassContext context(*this, cmd, pass.RenderArea, passIndex);
        pass.Invoke(pass.Payload, context);

        if (raster)
            cmd.endRendering();

        if (pass.Swapchain)
            cmd.transitionSwapchainLayout(pass.Swapchain, pass.SwapchainImage, NRI_TEXTURE_LAYOUT_COLOR_ATTACHMENT, NRI_TEXTURE_LAYOUT_PRESENT);

        if (HasFlag(pass.Flags, RGPassFlags::Raster) && m_ProfilerHooks.EndGpuStatistics)
            m_ProfilerHooks.EndGpuStatistics(gpuContext, cmd, m_ProfilerHooks.UserData);
        if (m_ProfilerHooks.EndGpuZone)
            m_ProfilerHooks.EndGpuZone(gpuContext, cmd, m_ProfilerHooks.UserData);
        if (m_ProfilerHooks.EndCpuZone)
            m_ProfilerHooks.EndCpuZone(pass.ScopeId, m_ProfilerHooks.UserData);
        cmd.endDebugLabel();

        if (pass.SynchronizeAfter)
            cmd.executionBarrier();

        if (m_InspectionActive && m_InspectionPass == passIndex)
            RecordInspection(cmd);

        pass.RecordMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    void RenderGraph::BeginRendering(const Pass& pass, CommandBuffer& cmd, RenderDesc& desc)
    {
        desc.renderArea = pass.RenderArea;
        desc.colorAttachments.clear();
        desc.depthAttachment = {};
        desc.hasDepthAttachment = false;
        desc.isSwapchainPass = false;

        if (pass.Swapchain)
        {
            RenderAttachDesc attachment{};
            attachment.attachmentSwapchain = pass.Swapchain;
            attachment.resolveImageIndex = pass.SwapchainImage;
            attachment.loadOp = pass.SwapchainLoad;
            attachment.storeOp = pass.SwapchainStore;
            attachment.clearColor = pass.SwapchainClear;
            desc.colorAttachments.push_back(attachment);
        }

        for (const Attachment& target : pass.ColorTargets)
        {
            RenderAttachDesc attachment{};
            attachment.attachment = m_Textures[target.Texture].Texture;
            attachment.loadOp = target.Load;
            attachment.storeOp = target.Store;
            attachment.clearColor = target.ClearColorVal;
            desc.colorAttachments.push_back(attachment);
        }

        if (pass.DepthTarget.Texture != RGInvalidIndex)
        {
            desc.hasDepthAttachment = true;
            desc.depthAttachment.attachment = m_Textures[pass.DepthTarget.Texture].Texture;
            desc.depthAttachment.loadOp = pass.DepthTarget.Load;
            desc.depthAttachment.storeOp = pass.DepthTarget.Store;
            desc.depthAttachment.clearDepth = pass.DepthTarget.ClearDepthVal;
        }

        cmd.beginRendering(desc);

        if (pass.Viewport == RGViewport::FlippedY)
        {
            const float width = static_cast<float>(pass.RenderArea.width);
            const float height = static_cast<float>(pass.RenderArea.height);
            cmd.setViewportWithCount({ 0.0f, height, width, -height }, 0.0f, 1.0f);
            cmd.setScissorWithCount(pass.RenderArea);
        }
    }

    void RenderGraph::ReleaseCallbacks()
    {
        for (uint32_t index = 0; index < m_PassCount; ++index)
        {
            Pass& pass = m_Passes[index];
            if (pass.Destroy)
                pass.Destroy(pass.Payload);
            pass.Payload = nullptr;
            pass.Invoke = nullptr;
            pass.Destroy = nullptr;
        }
    }

    uint32_t RenderGraph::GetScopeId(const char* name)
    {
        if (!m_ProfilerHooks.RegisterNamedScope)
            return 0;
        auto [found, inserted] = m_ScopeIds.try_emplace(name, 0u);
        if (inserted)
            found->second = m_ProfilerHooks.RegisterNamedScope(name, m_ProfilerHooks.UserData);
        return found->second;
    }
}
