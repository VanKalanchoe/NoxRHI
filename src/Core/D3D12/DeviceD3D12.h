#pragma once
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <span>
#include <string>
#include <vector>

#include "D3D12Common.h"
#include "../Device.h"

namespace NRI
{
    class SwapchainD3D12;
    class MemoryAllocatorD3D12;

    class DeviceD3D12 final : public Device
    {
    public:
        explicit DeviceD3D12(const DeviceInitDesc& desc);
        ~DeviceD3D12() override;

        GraphicsAPI getAPI() const override { return GraphicsAPI::D3D12; }

        ID3D12Device5* getNativeDevice() const { return m_device.Get(); }
        IDXGIFactory6* getNativeFactory() const { return m_factory.Get(); }
        IDXGIAdapter4* getNativeAdapter() const { return m_adapter.Get(); }
        ID3D12CommandQueue* getGraphicsQueue() const { return m_directQueue.Get(); }
        // The queue a swapchain is created on: Streamline's proxy of the direct queue when Streamline is up, else the direct queue.
        ID3D12CommandQueue* getSwapchainQueue() const { return m_directQueueProxy ? m_directQueueProxy.Get() : m_directQueue.Get(); }
        ID3D12CommandQueue* getComputeQueue() const { return m_computeQueue ? m_computeQueue.Get() : m_directQueue.Get(); }
        ID3D12CommandQueue* getTransferQueue() const { return m_copyQueue ? m_copyQueue.Get() : m_directQueue.Get(); }
        ID3D12RootSignature* getGlobalRootSignature() const { return m_globalRootSignature.Get(); }
        MemoryAllocatorD3D12* getAllocator() const { return m_allocator.get(); }

        void shutdown() override;
        uint32_t getMSAASampleCount() const override { return 1; }
        void submitAndWait(CommandBuffer& cmdBuffer, uint32_t slotIndex = 0) override;
        void submitCommandBuffers(std::span<CommandBuffer* const> cmdBuffers, Swapchain& swapchain, uint32_t frameIndex,
                                  uint32_t imageIndex, std::span<const TimelinePoint> timelineWaits = {}) override;
        void submit(QueueType queue, std::span<CommandBuffer* const> cmdBuffers, std::span<const TimelinePoint> waits,
                    std::span<const TimelinePoint> signals) override;
        void waitIdle() override;

        std::unique_ptr<Swapchain> createSwapchain(const SwapchainDesc& desc) override;
        std::unique_ptr<Pipeline> createPipeline(const PipelineDesc& desc, ShaderCompiler& compiler) override;
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

        bool hasDedicatedTransferQueue() const override { return m_copyQueue != nullptr; }
        bool hasAsyncComputeQueue() const override { return m_computeQueue != nullptr; }
        bool isMemoryBudgetSupported() const override { return true; }
        HWND getWindowHandle() const { return m_hwnd; }
        const std::string& getShaderCacheDir() const override { return m_shaderCacheDir; }
        StreamlineContext* getStreamlineContext() const override { return m_streamline.get(); }

        uint32_t getShaderGroupHandleSize() const override { return D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT; }
        uint32_t getShaderGroupBaseAlignment() const override { return D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT; }

        QueueTimelineValues getSubmittedTimelineValues() const override;
        QueueTimelineValues getCompletedTimelineValues() const override;

        bool supportsMeshShaders() const override { return m_meshShaders; }

        // ExecuteIndirect's command signature for indirect mesh draws: one D3D12_DISPATCH_MESH_ARGUMENTS (three uint32 group counts, the
        // same bytes as Vulkan's VkDrawMeshTasksIndirectCommandEXT) every `stride` bytes. Created once per stride, kept for the device.
        ID3D12CommandSignature* getDispatchMeshSignature(uint32_t stride);

        // Logs why the device was removed: GetDeviceRemovedReason and, with DRED on, the unfinished command lists' breadcrumbs and any
        // page fault. Once per device.
        void reportDeviceRemoved();

    private:
        void initD3D12(const DeviceInitDesc& desc);
        void createGlobalRootSignature();
        void signalRetirementFence(QueueType queue);

    private:
        HWND m_hwnd = nullptr;
        std::string m_shaderCacheDir = "shader_cache";
        std::unique_ptr<StreamlineContext> m_streamline;
        ComPtr<IDXGIFactory6> m_factory;
        ComPtr<IDXGIAdapter4> m_adapter;
        ComPtr<ID3D12Device5> m_device;

        ComPtr<ID3D12CommandQueue> m_directQueue;
        ComPtr<ID3D12CommandQueue> m_directQueueProxy; // (Streamline's proxy of m_directQueue: only to create the swapchain on)
        ComPtr<ID3D12CommandQueue> m_computeQueue;
        ComPtr<ID3D12CommandQueue> m_copyQueue;

        ComPtr<ID3D12RootSignature> m_globalRootSignature;
        std::mutex m_commandSignatureMutex;
        std::unordered_map<uint32_t, ComPtr<ID3D12CommandSignature>> m_dispatchMeshSignatures;
        DWORD m_infoQueueCookie = 0;

        ComPtr<ID3D12Fence> m_waitIdleFence;
        uint64_t m_waitIdleFenceValue = 0;
        HANDLE m_waitIdleEvent = nullptr;

        // Per-queue retirement fences for timeline-safe deferred deletion
        std::array<ComPtr<ID3D12Fence>, 3> m_retirementFences;
        std::array<std::atomic<uint64_t>, 3> m_submittedValues{};

        std::atomic<uint64_t> m_textureBytes = 0;

        std::unique_ptr<MemoryAllocatorD3D12> m_allocator;
        bool m_dredEnabled = false;
        bool m_removalReported = false;
        bool m_meshShaders = false;
    };
}
