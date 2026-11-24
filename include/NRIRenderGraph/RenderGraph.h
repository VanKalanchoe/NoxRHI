#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <NRIHPP/NRI.hpp>
#include <NRIHPP/NRIExtensions.hpp>

#include "RGBlackboard.h"
#include "RGFrameReport.h"
#include "RGResourcePool.h"
#include "RGTypes.h"

namespace nri
{
    class RenderGraph;

    class RGPassContext
    {
    public:
        CommandBuffer& Cmd() const { return m_Cmd; }
        NriTexture Texture(RGTexture texture) const;
        NriBuffer Buffer(RGBuffer buffer) const;
        // Cross-API shader buffer reference: device address on Vulkan, raw SRV/UAV descriptor slot on D3D12.
        uint64_t BufferReadRef(RGBuffer buffer) const;
        uint64_t BufferWriteRef(RGBuffer buffer) const;
        uint32_t Slot(RGTexture texture) const;
        uint32_t SlotOr(RGTexture texture, uint32_t fallback) const;
        uint32_t StorageSlot(RGTexture texture, uint32_t mip = 0) const;
        Extent2D RenderArea() const { return m_RenderArea; }

    private:
        friend class RenderGraph;
        RGPassContext(RenderGraph& graph, CommandBuffer& cmd, Extent2D renderArea, uint32_t passIndex);

    private:
        RenderGraph& m_Graph;
        CommandBuffer& m_Cmd;
        Extent2D m_RenderArea;
        uint32_t m_PassIndex;
    };

    class RGBuilder
    {
    public:
        RGTexture Read(RGTexture texture, RGTextureAccess access = RGTextureAccess::Sampled);
        RGTexture Write(RGTexture texture, RGTextureAccess access = RGTextureAccess::StorageWrite);
        RGBuffer Read(RGBuffer buffer, RGBufferAccess access = RGBufferAccess::Read);
        RGBuffer Write(RGBuffer buffer, RGBufferAccess access = RGBufferAccess::Write);

        void ColorTarget(RGTexture texture, LoadOp load, StoreOp store, ClearColor clear = {});
        void DepthTarget(RGTexture texture, LoadOp load, StoreOp store, ClearDepth clear = {});
        void SwapchainTarget(const Swapchain& swapchain, uint32_t imageIndex, LoadOp load, StoreOp store, ClearColor clear = {});
        void SwapchainTarget(NriSwapchain swapchain, uint32_t imageIndex, LoadOp load, StoreOp store, ClearColor clear = {});
        void SetRenderArea(Extent2D area, RGViewport viewport = RGViewport::FlippedY);
        void RecordExclusive(const char* key);

    private:
        friend class RenderGraph;
        RGBuilder(RenderGraph& graph, uint32_t passIndex) : m_Graph(graph), m_PassIndex(passIndex) {}

    private:
        RenderGraph& m_Graph;
        uint32_t m_PassIndex;
    };

    struct RGSubmission
    {
        QueueType Queue = NRI_QUEUE_GRAPHICS;
        std::span<const NriCommandBuffer> CommandBuffers;
        std::span<const TimelinePoint> Waits;
        std::span<const TimelinePoint> Signals;
    };

    struct RGFrameDesc
    {
        uint64_t FrameNumber = 0;
        Extent2D RenderExtent{};
        Extent2D OutputExtent{};
    };

    // Standalone frame render graph built on NRIHPP.
    class RenderGraph
    {
    public:
        RenderGraph() = default;
        ~RenderGraph();

        RenderGraph(const RenderGraph&) = delete;
        RenderGraph& operator=(const RenderGraph&) = delete;

        void Initialize(Device& device, DescriptorHeap& resourceHeap, uint32_t framesInFlight);
        void ReleaseResources();

        // Optional hooks for external profiler, task pool, and logging.
        void SetProfilerHooks(const RGProfilerHooks& hooks);
        void SetTaskDispatcher(RGTaskDispatcher* dispatcher) { m_TaskDispatcher = dispatcher; }
        void SetLogCallback(RGLogCallback callback, void* userData = nullptr)
        {
            m_LogCallback = callback;
            m_LogUserData = userData;
        }

        void SetCommandBufferSetup(std::function<void(CommandBuffer&, QueueType)> setup) { m_CommandBufferSetup = std::move(setup); }
        void SetParallelRecording(bool enabled) { m_ParallelRecording = enabled; }
        bool IsParallelRecording() const { return m_ParallelRecording; }
        void SetAsyncCompute(bool enabled) { m_AsyncComputeEnabled = enabled; }
        bool IsAsyncComputeEnabled() const { return m_AsyncComputeEnabled; }
        bool IsAsyncComputeAvailable() const { return static_cast<bool>(m_GraphicsTimeline); }
        uint32_t GetAsyncComputePassCount() const { return m_AsyncComputePassCount; }
        void SetSynchronization(RGSynchronization synchronization) { m_Synchronization = synchronization; }
        RGSynchronization GetSynchronization() const { return m_Synchronization; }

        void Reset(const RGFrameDesc& frame);

        RGTexture CreateTexture(const char* name, const RGTextureDesc& desc);
        RGBuffer CreateBuffer(const char* name, const RGBufferDesc& desc);
        RGTextureHistory GetHistoryTexture(const char* key, const RGTextureDesc& desc, uint32_t count);
        RGBufferHistory GetHistoryBuffer(const char* key, const RGBufferDesc& desc, uint32_t count);
        RGTexture ImportTexture(const char* name, const Texture& texture, RGImportAccess access = RGImportAccess::ReadWrite);
        RGTexture ImportTexture(const char* name, NriTexture texture, RGImportAccess access = RGImportAccess::ReadWrite);
        RGBuffer ImportBuffer(const char* name, const Buffer& buffer, RGImportAccess access = RGImportAccess::ReadWrite,
                              uint32_t readSlot = ~0u, uint32_t writeSlot = ~0u);
        RGBuffer ImportBuffer(const char* name, NriBuffer buffer, RGImportAccess access = RGImportAccess::ReadWrite,
                              uint32_t readSlot = ~0u, uint32_t writeSlot = ~0u);

        void ResetHistory(const char* key);
        void ResetAllHistory();
        bool WasHistoryReset(const char* key) const;

        template <typename SetupCallback, typename ExecuteCallback>
        void AddPass(const char* name, RGPassFlags flags, SetupCallback&& setup, ExecuteCallback&& execute)
        {
            const uint32_t passIndex = BeginPass(name, flags);
            RGBuilder builder(*this, passIndex);
            setup(builder);
            StoreExecute(passIndex, std::forward<ExecuteCallback>(execute));
        }

        void PushGroup(const char* name);
        void PopGroup();

        RGBlackboard& GetBlackboard() { return m_Blackboard; }
        const RGResourcePool& GetResourcePool() const { return m_Pool; }

        void Compile();
        std::span<const RGSubmission> Execute(uint32_t frameSlot);

        NriTexture GetTexture(RGTexture texture) const;
        uint32_t GetSlotOr(RGTexture texture, uint32_t fallback) const;

        RGTexture FindTexture(std::string_view name, uint32_t occurrence) const;
        const RGTextureKey& GetTextureKey(RGTexture texture) const;
        void InspectTexture(RGTexture texture, std::function<void(RGPassContext& context)> inspector);
        bool IsInspecting() const { return m_InspectionActive; }

        void SetValidationEnabled(bool enabled) { m_ValidationEnabled = enabled; }
        bool IsValidationEnabled() const { return m_ValidationEnabled; }

        void SetReportEnabled(bool enabled) { m_ReportEnabled = enabled; }
        const RGFrameReport& GetReport() const { return m_Report; }

    private:
        friend class RGBuilder;
        friend class RGPassContext;

        class FrameArena
        {
        public:
            static constexpr size_t DefaultBlockSize = 256 * 1024;
            void* Allocate(size_t size, size_t alignment);
            template <typename T, typename... Args>
            T* New(Args&&... args)
            {
                return new (Allocate(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
            }
            void Reset();

        private:
            struct Block
            {
                std::unique_ptr<std::byte[]> Memory;
                size_t Size = 0;
            };
            std::vector<Block> m_Blocks;
            size_t m_BlockIndex = 0;
            size_t m_Offset = 0;
        };

        using InvokeFn = void (*)(void* payload, RGPassContext& context);
        using DestroyFn = void (*)(void* payload);

        enum class EventType : uint8_t
        {
            Pass,
            GroupBegin,
            GroupEnd
        };

        struct Event
        {
            EventType Type;
            uint32_t Index;
        };

        struct TextureEntry
        {
            const char* Name = nullptr;
            RGResourceKind Kind = RGResourceKind::Imported;
            bool ReadOnly = false;
            RGTextureKey Key;
            NriTexture Texture = nullptr;
            const std::vector<uint32_t>* StorageSlots = nullptr;
            uint32_t FirstPass = RGInvalidIndex;
            uint32_t LastPass = 0;
        };

        struct BufferEntry
        {
            const char* Name = nullptr;
            RGResourceKind Kind = RGResourceKind::Imported;
            bool ReadOnly = false;
            RGBufferKey Key;
            NriBuffer Buffer = nullptr;
            uint32_t ReadSlot = ~0u;
            uint32_t WriteSlot = ~0u;
            uint32_t FirstPass = RGInvalidIndex;
            uint32_t LastPass = 0;
        };

        struct ResourceAccess
        {
            uint32_t Resource = RGInvalidIndex;
            bool IsTexture = true;
            bool IsWrite = false;
            ResourceState State{};
        };

        struct Attachment
        {
            uint32_t Texture = RGInvalidIndex;
            LoadOp Load = NRI_LOAD_OP_DONT_CARE;
            StoreOp Store = NRI_STORE_OP_DONT_CARE;
            ClearColor ClearColorVal{};
            ClearDepth ClearDepthVal{};
        };

        struct Pass
        {
            const char* Name = nullptr;
            RGPassFlags Flags = RGPassFlags::None;
            std::vector<ResourceAccess> Accesses;
            std::vector<Attachment> ColorTargets;
            Attachment DepthTarget;
            NriSwapchain Swapchain = nullptr;
            uint32_t SwapchainImage = 0;
            LoadOp SwapchainLoad = NRI_LOAD_OP_LOAD;
            StoreOp SwapchainStore = NRI_STORE_OP_STORE;
            ClearColor SwapchainClear{};
            Extent2D RenderArea{};
            RGViewport Viewport = RGViewport::FlippedY;
            void* Payload = nullptr;
            InvokeFn Invoke = nullptr;
            DestroyFn Destroy = nullptr;
            uint32_t ExclusiveMask = 0;
            uint32_t ScopeId = 0;
            uint32_t Chunk = 0;
            QueueType Queue = NRI_QUEUE_GRAPHICS;
            float RecordMs = 0.0f;
            bool Culled = false;
            bool SynchronizeAfter = false;
            uint32_t FirstTextureBarrier = 0;
            uint32_t TextureBarrierCount = 0;
            uint32_t FirstBufferBarrier = 0;
            uint32_t BufferBarrierCount = 0;
            uint32_t FirstPostTextureBarrier = 0;
            uint32_t PostTextureBarrierCount = 0;
            uint32_t FirstPostBufferBarrier = 0;
            uint32_t PostBufferBarrierCount = 0;
        };

        struct Chunk
        {
            uint32_t FirstEvent = 0;
            uint32_t EndEvent = 0;
            uint32_t OpenGroupsOffset = 0;
            uint32_t OpenGroupsCount = 0;
            QueueType Queue = NRI_QUEUE_GRAPHICS;
        };

        struct CommandRecorder
        {
            CommandAllocator Allocator;
            CommandBuffer CmdBuffer;
            CommandAllocator FixupAllocator;
            CommandBuffer FixupCmdBuffer;
            QueueType Queue = NRI_QUEUE_GRAPHICS;
        };

        struct AsyncBlock
        {
            uint32_t First = 0;
            uint32_t End = 0;
            uint32_t Fork = RGInvalidIndex;
            uint32_t Join = RGInvalidIndex;
        };

        struct SubmissionUnit
        {
            QueueType Queue = NRI_QUEUE_GRAPHICS;
            uint32_t FirstChunk = 0;
            uint32_t ChunkCount = 0;
            std::vector<uint32_t> JoinBlocks;
            std::vector<uint32_t> ForkBlocks;
            uint32_t Block = RGInvalidIndex;
        };

        struct alignas(64) ChunkScratch
        {
            std::vector<uint32_t> GroupStack;
            RenderDesc RenderDescScratch;
        };

        uint32_t BeginPass(const char* name, RGPassFlags flags);

        template <typename ExecuteCallback>
        void StoreExecute(uint32_t passIndex, ExecuteCallback&& execute)
        {
            using Function = std::decay_t<ExecuteCallback>;
            Pass& pass = m_Passes[passIndex];
            pass.Payload = m_ExecuteArena.New<Function>(std::forward<ExecuteCallback>(execute));
            pass.Invoke = [](void* payload, RGPassContext& context) { (*static_cast<Function*>(payload))(context); };
            pass.Destroy = [](void* payload) { static_cast<Function*>(payload)->~Function(); };
        }

        void Log(RGLogLevel level, std::string_view message) const;
        RGTextureKey ResolveKey(const RGTextureDesc& desc) const;
        void AddAccess(uint32_t passIndex, uint32_t resource, bool isTexture, bool isWrite, ResourceState state);
        void PlanQueues();
        void PlanAsyncChunks(float targetMs);
        void AddChunk(QueueType queue, uint32_t firstPosition, uint32_t endPosition, float targetMs);
        void PlanBlanketSynchronization();
        void PlanPreciseSynchronization();
        void ValidateHandle(uint32_t handleFrame, const char* passName);
        void ValidateContextAccess(uint32_t passIndex, uint32_t resource, bool isTexture);
        void ValidateCompiledFrame();
        void ReportValidation(std::string message);
        void PlaceInspection();
        void RecordInspection(CommandBuffer& cmd);
        void BuildReport();
        uint32_t GetExclusiveKeyBit(const char* key);
        bool HasSideEffects(const Pass& pass) const;
        void AllocateTransients();
        float GetEstimatedRecordMs(const Pass& pass) const;
        void PlanChunks();
        float SplitIntoChunks(uint32_t chunkCount, float totalMs);
        void PrepareRecording(uint32_t frameSlot);
        void RecordChunk(uint32_t chunkIndex);
        void ExecutePass(Pass& pass, CommandBuffer& cmd, ChunkScratch& scratch, void* gpuContext);
        void BeginRendering(const Pass& pass, CommandBuffer& cmd, RenderDesc& desc);
        void ReleaseCallbacks();
        uint32_t GetScopeId(const char* name);

    private:
        static constexpr float ChunkOverheadMs = 0.05f;
        static constexpr uint32_t MaxChunks = 8;
        static constexpr uint32_t MaxRecordedChunks = 16;
        static constexpr float DefaultPassRecordMs = 0.02f;

        Device* m_Device = nullptr;
        uint32_t m_FramesInFlight = 0;
        RGFrameDesc m_Frame;
        uint32_t m_FrameId = 0;
        RGResourcePool m_Pool;
        FrameArena m_ExecuteArena;

        RGProfilerHooks m_ProfilerHooks{};
        RGTaskDispatcher m_DefaultDispatcher;
        RGTaskDispatcher* m_TaskDispatcher = nullptr;
        RGLogCallback m_LogCallback = nullptr;
        void* m_LogUserData = nullptr;

        std::vector<Pass> m_Passes;
        uint32_t m_PassCount = 0;
        std::vector<Event> m_Events;
        std::vector<const char*> m_GroupNames;
        std::vector<uint32_t> m_GroupScopeIds;
        uint32_t m_OpenGroupDepth = 0;
        std::vector<const char*> m_ExclusiveKeys;

        std::vector<TextureEntry> m_Textures;
        std::vector<BufferEntry> m_Buffers;
        std::unordered_map<const void*, uint32_t> m_ImportedTextures;
        std::unordered_map<const void*, uint32_t> m_ImportedBuffers;

        std::unordered_set<std::string> m_PendingHistoryResets;
        std::unordered_set<std::string> m_FrameHistoryResets;
        bool m_PendingResetAll = false;
        bool m_FrameResetAll = false;

        RGBlackboard m_Blackboard;

        std::function<void(CommandBuffer&, QueueType)> m_CommandBufferSetup;
        bool m_ParallelRecording = true;
        RGSynchronization m_Synchronization = RGSynchronization::Precise;
        std::vector<TextureBarrierDesc> m_TextureBarriers;
        std::vector<BufferBarrierDesc> m_BufferBarriers;
        uint32_t m_FrameSlot = 0;
        std::vector<CommandRecorder> m_Recorders;
        std::vector<Chunk> m_Chunks;
        std::vector<uint32_t> m_ChunkOpenGroups;
        std::vector<uint32_t> m_ChunkExclusiveMasks;
        std::vector<float> m_ChunkRecordMs;
        std::vector<ChunkScratch> m_ChunkScratch;
        std::vector<void*> m_ProfilerChunkContexts;
        std::vector<NriCommandBuffer> m_SubmitList;

        bool m_AsyncComputeEnabled = false;
        uint32_t m_AsyncComputePassCount = 0;
        TimelineSemaphore m_GraphicsTimeline;
        TimelineSemaphore m_ComputeTimeline;
        uint64_t m_GraphicsTimelineValue = 0;
        uint64_t m_ComputeTimelineValue = 0;
        std::vector<uint32_t> m_ExecutedScratch;
        std::vector<uint8_t> m_BlockAccessScratch;
        std::vector<uint32_t> m_PassEvent;
        std::vector<AsyncBlock> m_AsyncBlocks;
        std::vector<SubmissionUnit> m_Units;
        std::vector<RGSubmission> m_Submissions;
        std::vector<std::vector<TimelinePoint>> m_SubmissionWaits;
        std::vector<std::vector<TimelinePoint>> m_SubmissionSignals;
        std::unordered_map<const char*, float> m_PassRecordMs;

        uint32_t m_InspectedTexture = RGInvalidIndex;
        std::function<void(RGPassContext& context)> m_Inspector;
        uint32_t m_InspectionPass = RGInvalidIndex;
        bool m_InspectionActive = false;

#ifndef NDEBUG
        bool m_ValidationEnabled = true;
#else
        bool m_ValidationEnabled = false;
#endif
        std::mutex m_ValidationMutex;
        std::vector<std::string> m_ValidationMessages;
        std::unordered_set<std::string> m_LoggedValidationMessages;
        std::vector<RGFrameReport::UnusedWrite> m_UnusedWrites;
        bool m_ReportEnabled = false;
        RGFrameReport m_Report;

        std::vector<bool> m_ResourceRead;
        std::vector<uint32_t> m_LastWriterScratch;
        std::vector<ResourceState> m_LastWriteScratch;
        std::vector<ResourceState> m_ReadsSinceWriteScratch;
        std::vector<ResourceState> m_VisibleScratch[2];
        std::vector<bool> m_AccessedScratch;
        std::vector<bool> m_WrittenScratch;
        std::vector<uint32_t> m_PhysicalSlotScratch;
        std::vector<const void*> m_SlotResourceScratch;
        std::unordered_map<const void*, uint32_t> m_SlotByResourceScratch;

        struct CarriedState
        {
            ResourceState LastWrite;
            ResourceState ReadsSinceWrite;
            ResourceState Visible[2];
            bool Written = false;
        };
        std::unordered_map<const void*, CarriedState> m_CarriedStates;
        std::vector<std::pair<uint32_t, ResourceState>> m_PassStatesScratch;
        std::vector<uint32_t> m_AllocationOrderScratch;
        std::vector<uint32_t> m_PlanStackScratch;
        std::unordered_map<const char*, uint32_t> m_ScopeIds;
    };
}
