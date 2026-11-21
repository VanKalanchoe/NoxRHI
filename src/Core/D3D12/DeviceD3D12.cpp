#include "DeviceD3D12.h"
#include "SwapchainD3D12.h"
#include "PipelineD3D12.h"
#include "CommandAllocatorD3D12.h"
#include "CommandBufferD3D12.h"
#include "TextureD3D12.h"
#include "BufferD3D12.h"
#include "DescriptorHeapD3D12.h"
#include "AccelerationStructureD3D12.h"
#include "QueryPoolD3D12.h"
#include "GpuProfilerD3D12.h"
#include "TimelineSemaphoreD3D12.h"
#include "MemoryAllocatorD3D12.h"
#include "../../Extensions/Upscaler/StreamlineContext.h"

namespace NRI
{
    static DeviceD3D12* s_reportingDevice = nullptr; // (the device g_OnD3D12DeviceRemoved reports for)

    namespace
    {
        void CALLBACK onD3D12ValidationMessage(D3D12_MESSAGE_CATEGORY,
                                               D3D12_MESSAGE_SEVERITY severity,
                                               D3D12_MESSAGE_ID,
                                               LPCSTR pDescription,
                                               void*)
        {
            if (!pDescription)
                return;
            if (severity <= D3D12_MESSAGE_SEVERITY_ERROR)
                NRI_LOG_ERROR("D3D12 validation: {}", pDescription);
            else if (severity == D3D12_MESSAGE_SEVERITY_WARNING)
                NRI_LOG_WARN("D3D12 validation: {}", pDescription);
            else
                NRI_LOG_INFO("D3D12 validation: {}", pDescription);
        }
    }

    DeviceD3D12::DeviceD3D12(const DeviceInitDesc& desc)
        : m_shaderCacheDir(desc.shaderCacheDir.empty() ? "shader_cache" : desc.shaderCacheDir)
    {
        initD3D12(desc);
        createGlobalRootSignature();
    }

    DeviceD3D12::~DeviceD3D12()
    {
        shutdown();
    }

    void DeviceD3D12::initD3D12(const DeviceInitDesc& desc)
    {
        m_hwnd = static_cast<HWND>(desc.window.hwnd);

        if (desc.enableStreamline)
        {
            m_streamline = std::make_unique<StreamlineContext>();
            m_streamline->initBeforeDevice(GraphicsAPI::D3D12, desc);
        }

        UINT dxgiFlags = 0;
        if (desc.enableValidation)
        {
            ComPtr<ID3D12Debug> debugController;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
            {
                debugController->EnableDebugLayer();
                dxgiFlags |= DXGI_CREATE_FACTORY_DEBUG;
                // NOXRHI_D3D12_GBV=1: GPU-based validation -- the debug layer also checks descriptors, resource states and accesses
                // while shaders run (slow; for finding what the CPU-side validation cannot see).
                char gbvValue[8]{};
                ComPtr<ID3D12Debug1> debug1;
                if (GetEnvironmentVariableA("NOXRHI_D3D12_GBV", gbvValue, sizeof(gbvValue)) > 0 && gbvValue[0] == '1' && SUCCEEDED(debugController.As(&debug1)))
                {
                    debug1->SetEnableGPUBasedValidation(TRUE);
                    NRI_LOG_INFO("D3D12: GPU-based validation on");
                }
            }
        }

        // DRED (Device Removed Extended Data): with validation, or NOXRHI_D3D12_DRED=1. Must be set before the device is created.
        char dredValue[8]{};
        if (desc.enableValidation || (GetEnvironmentVariableA("NOXRHI_D3D12_DRED", dredValue, sizeof(dredValue)) > 0 && dredValue[0] == '1'))
        {
            ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dred;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred))))
            {
                dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                dred->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                m_dredEnabled = true;
            }
        }

        ThrowIfFailed(CreateDXGIFactory2(dxgiFlags, IID_PPV_ARGS(&m_factory)), "CreateDXGIFactory2");

        ComPtr<IDXGIAdapter1> adapter1;
        SIZE_T maxVram = 0;
        // NOXRHI_D3D12_WARP=1: Microsoft's software adapter (WARP) instead of the largest hardware GPU -- for machines without one, such
        // as CI runners, where it runs the same shaders (ShaderCompat --dx12).
        char warpValue[8]{};
        if (GetEnvironmentVariableA("NOXRHI_D3D12_WARP", warpValue, sizeof(warpValue)) > 0 && warpValue[0] == '1')
        {
            ComPtr<IDXGIAdapter1> warp;
            if (SUCCEEDED(m_factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) &&
                SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_12_1, _uuidof(ID3D12Device5), nullptr)))
                warp.As(&m_adapter);
            else
                throw std::runtime_error("D3D12: NOXRHI_D3D12_WARP=1 but the WARP adapter does not support D3D_FEATURE_LEVEL_12_1");
        }
        for (UINT i = 0; !m_adapter && m_factory->EnumAdapters1(i, &adapter1) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 adapterDesc{};
            adapter1->GetDesc1(&adapterDesc);
            if ((adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 && adapterDesc.DedicatedVideoMemory > maxVram)
            {
                if (SUCCEEDED(D3D12CreateDevice(adapter1.Get(), D3D_FEATURE_LEVEL_12_1, _uuidof(ID3D12Device5), nullptr)))
                {
                    maxVram = adapterDesc.DedicatedVideoMemory;
                    adapter1.As(&m_adapter);
                }
            }
        }

        if (!m_adapter)
        {
            throw std::runtime_error("D3D12: No suitable hardware GPU found supporting D3D_FEATURE_LEVEL_12_1");
        }

        ThrowIfFailed(D3D12CreateDevice(m_adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&m_device)), "D3D12CreateDevice");

        s_reportingDevice = this;
        g_OnD3D12DeviceRemoved = [] {
            if (s_reportingDevice)
                s_reportingDevice->reportDeviceRemoved();
        };

        if (desc.enableValidation)
        {
            ComPtr<ID3D12InfoQueue1> infoQueue;
            if (SUCCEEDED(m_device.As(&infoQueue)))
            {
                infoQueue->RegisterMessageCallback(&onD3D12ValidationMessage, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &m_infoQueueCookie);
            }
        }

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_directQueue)), "Create Direct CommandQueue");

        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_computeQueue));

        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_copyQueue));

        ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_waitIdleFence)), "CreateFence");
        m_waitIdleEvent = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);

        for (auto& fence : m_retirementFences)
        {
            ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Create Retirement Fence");
        }

        m_allocator = std::make_unique<MemoryAllocatorD3D12>(*this);

        DXGI_ADAPTER_DESC1 chosenDesc{};
        m_adapter->GetDesc1(&chosenDesc);
        char adapterName[256]{};
        WideCharToMultiByte(CP_UTF8, 0, chosenDesc.Description, -1, adapterName, sizeof(adapterName), nullptr, nullptr);
        NRI_LOG_INFO("D3D12 Adapter: {} (VRAM: {} MB)", adapterName, static_cast<uint64_t>(chosenDesc.DedicatedVideoMemory >> 20));
        NRI_LOG_INFO("Transfer queue: {}", m_copyQueue ? "dedicated COPY queue" : "shared with DIRECT");
        NRI_LOG_INFO("Async compute queue: {}", m_computeQueue ? "dedicated COMPUTE queue" : "shared with DIRECT");

        D3D12_FEATURE_DATA_D3D12_OPTIONS opts{};
        m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts));
        NRI_LOG_INFO("Sparse buffers (TiledResourcesTier): {}",
                     opts.TiledResourcesTier == D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED ? "off" :
                     opts.TiledResourcesTier == D3D12_TILED_RESOURCES_TIER_1 ? "Tier 1" :
                     opts.TiledResourcesTier == D3D12_TILED_RESOURCES_TIER_2 ? "Tier 2" : "Tier 3+");

        D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts5{};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opts5, sizeof(opts5))))
        {
            NRI_LOG_INFO("RT Pipeline Properties: tier={}, recordStride={}, tableAlign={}",
                         opts5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED ? "off" :
                         opts5.RaytracingTier == D3D12_RAYTRACING_TIER_1_0 ? "1.0" : "1.1",
                         D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT,
                         D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS7 opts7{};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opts7, sizeof(opts7))))
        {
            m_meshShaders = opts7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
            NRI_LOG_INFO("Mesh shaders: {}", m_meshShaders ? "Tier 1" : "off");
        }

        if (m_streamline && m_streamline->isInitialized())
        {
            m_streamline->registerD3D12Device(m_device.Get(),
                                              reinterpret_cast<const uint8_t*>(&chosenDesc.AdapterLuid),
                                              sizeof(chosenDesc.AdapterLuid));
        }
    }

    void DeviceD3D12::createGlobalRootSignature()
    {
        D3D12_ROOT_PARAMETER1 rootParam{};
        rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParam.Constants.ShaderRegister = 0;
        rootParam.Constants.RegisterSpace = 0;
        rootParam.Constants.Num32BitValues = 32;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc{};
        rootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rootSigDesc.Desc_1_1.NumParameters = 1;
        rootSigDesc.Desc_1_1.pParameters = &rootParam;
        rootSigDesc.Desc_1_1.NumStaticSamplers = 0;
        rootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
        rootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                                     D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

        ComPtr<ID3DBlob> signatureBlob;
        ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = D3D12SerializeVersionedRootSignature(&rootSigDesc, &signatureBlob, &errorBlob);
        if (FAILED(hr))
        {
            rootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
            ThrowIfFailed(D3D12SerializeVersionedRootSignature(&rootSigDesc, &signatureBlob, &errorBlob), "SerializeRootSignature");
        }

        ThrowIfFailed(m_device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(),
                                                    IID_PPV_ARGS(&m_globalRootSignature)), "CreateRootSignature");
    }

    namespace
    {
        const char* BreadcrumbOpName(D3D12_AUTO_BREADCRUMB_OP op)
        {
            switch (op)
            {
            case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return "SetMarker";
            case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return "BeginEvent";
            case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return "EndEvent";
            case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DrawInstanced";
            case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "DrawIndexedInstanced";
            case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return "ExecuteIndirect";
            case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "Dispatch";
            case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "CopyBufferRegion";
            case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "CopyTextureRegion";
            case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "CopyResource";
            case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: return "ResolveSubresource";
            case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return "ClearRenderTargetView";
            case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return "ClearUnorderedAccessView";
            case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return "ClearDepthStencilView";
            case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "ResourceBarrier";
            case D3D12_AUTO_BREADCRUMB_OP_EXECUTEBUNDLE: return "ExecuteBundle";
            case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return "Present";
            case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA: return "ResolveQueryData";
            case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION: return "BeginSubmission";
            case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION: return "EndSubmission";
            case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE: return "BuildRaytracingAccelerationStructure";
            case D3D12_AUTO_BREADCRUMB_OP_EMITRAYTRACINGACCELERATIONSTRUCTUREPOSTBUILDINFO: return "EmitRaytracingAccelerationStructurePostbuildInfo";
            case D3D12_AUTO_BREADCRUMB_OP_COPYRAYTRACINGACCELERATIONSTRUCTURE: return "CopyRaytracingAccelerationStructure";
            case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS: return "DispatchRays";
            case D3D12_AUTO_BREADCRUMB_OP_DISPATCHMESH: return "DispatchMesh";
            case D3D12_AUTO_BREADCRUMB_OP_BARRIER: return "Barrier";
            default: return nullptr;
            }
        }

        std::string Narrow(const wchar_t* text)
        {
            std::string out;
            for (; text && *text; ++text)
                out += *text < 128 ? static_cast<char>(*text) : '?';
            return out;
        }
    }

    void DeviceD3D12::reportDeviceRemoved()
    {
        if (m_removalReported || !m_device)
            return;
        m_removalReported = true;

        const HRESULT reason = m_device->GetDeviceRemovedReason();
        const char* reasonName = reason == DXGI_ERROR_DEVICE_HUNG ? "DEVICE_HUNG (the GPU stopped responding)"
                               : reason == DXGI_ERROR_DEVICE_REMOVED ? "DEVICE_REMOVED"
                               : reason == DXGI_ERROR_DEVICE_RESET ? "DEVICE_RESET (bad commands)"
                               : reason == DXGI_ERROR_DRIVER_INTERNAL_ERROR ? "DRIVER_INTERNAL_ERROR"
                               : reason == DXGI_ERROR_INVALID_CALL ? "INVALID_CALL" : "other";
        NRI_LOG_ERROR("D3D12 device removed: reason 0x{:08X} {}", static_cast<unsigned int>(reason), reasonName);
        if (!m_dredEnabled)
        {
            NRI_LOG_ERROR("D3D12: run with validation on (or NOXRHI_D3D12_DRED=1) for DRED breadcrumbs and page-fault data");
            return;
        }

        ComPtr<ID3D12DeviceRemovedExtendedData> dred;
        if (FAILED(m_device.As(&dred)))
            return;

        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs{};
        if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs)))
        {
            for (const D3D12_AUTO_BREADCRUMB_NODE* node = breadcrumbs.pHeadAutoBreadcrumbNode; node; node = node->pNext)
            {
                const UINT completed = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
                if (completed >= node->BreadcrumbCount)
                    continue; // (this command list finished)
                const std::string list = node->pCommandListDebugNameA ? node->pCommandListDebugNameA : Narrow(node->pCommandListDebugNameW);
                NRI_LOG_ERROR("D3D12 DRED: command list '{}': {} of {} operations completed; around where the GPU stopped:",
                              list.empty() ? "(unnamed)" : list, completed, node->BreadcrumbCount);
                const UINT first = completed > 4 ? completed - 4 : 0, last = std::min(node->BreadcrumbCount, completed + 4);
                for (UINT i = first; i < last; ++i)
                {
                    const char* name = BreadcrumbOpName(node->pCommandHistory[i]);
                    NRI_LOG_ERROR("D3D12 DRED:   {} [{}] {}{}", i < completed ? "done   " : (i == completed ? "FAILED?" : "pending"), i,
                                  name ? name : "op", name ? "" : std::to_string(static_cast<int>(node->pCommandHistory[i])));
                }
            }
        }

        D3D12_DRED_PAGE_FAULT_OUTPUT pageFault{};
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pageFault)) && pageFault.PageFaultVA)
        {
            NRI_LOG_ERROR("D3D12 DRED: page fault at GPU address 0x{:016X}", static_cast<unsigned long long>(pageFault.PageFaultVA));
            for (const D3D12_DRED_ALLOCATION_NODE* node = pageFault.pHeadExistingAllocationNode; node; node = node->pNext)
                NRI_LOG_ERROR("D3D12 DRED:   allocation there: {} (type {})", node->ObjectNameA ? node->ObjectNameA : Narrow(node->ObjectNameW),
                              static_cast<int>(node->AllocationType));
            for (const D3D12_DRED_ALLOCATION_NODE* node = pageFault.pHeadRecentFreedAllocationNode; node; node = node->pNext)
                NRI_LOG_ERROR("D3D12 DRED:   recently freed there: {} (type {})", node->ObjectNameA ? node->ObjectNameA : Narrow(node->ObjectNameW),
                              static_cast<int>(node->AllocationType));
        }
    }

    void DeviceD3D12::shutdown()
    {
        if (s_reportingDevice == this)
        {
            s_reportingDevice = nullptr;
            g_OnD3D12DeviceRemoved = nullptr;
        }
        if (m_shuttingDown)
            return;
        waitIdle();
        m_shuttingDown = true;
        drainDeferredDeletions();
        if (m_streamline)
        {
            m_streamline->shutdown();
            m_streamline.reset();
        }
        if (m_infoQueueCookie != 0 && m_device)
        {
            ComPtr<ID3D12InfoQueue1> infoQueue;
            if (SUCCEEDED(m_device.As(&infoQueue)))
                infoQueue->UnregisterMessageCallback(m_infoQueueCookie);
            m_infoQueueCookie = 0;
        }
        if (m_waitIdleEvent)
        {
            CloseHandle(m_waitIdleEvent);
            m_waitIdleEvent = nullptr;
        }
        {
            std::lock_guard lock(m_commandSignatureMutex);
            m_dispatchMeshSignatures.clear();
        }
        m_allocator.reset();
    }

    ID3D12CommandSignature* DeviceD3D12::getDispatchMeshSignature(uint32_t stride)
    {
        std::lock_guard lock(m_commandSignatureMutex);
        ComPtr<ID3D12CommandSignature>& signature = m_dispatchMeshSignatures[stride];
        if (!signature)
        {
            // Only the dispatch argument, no root arguments: no root signature needed (D3D12MeshShaders' indirect sample does the same).
            D3D12_INDIRECT_ARGUMENT_DESC argument{};
            argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
            D3D12_COMMAND_SIGNATURE_DESC desc{};
            desc.ByteStride = stride;
            desc.NumArgumentDescs = 1;
            desc.pArgumentDescs = &argument;
            const HRESULT result = m_device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&signature));
            if (FAILED(result))
            {
                NRI_LOG_ERROR("DeviceD3D12: CreateCommandSignature (DispatchMesh, stride {}) failed: 0x{:08X}", stride, uint32_t(result));
                signature.Reset();
                return nullptr;
            }
        }
        return signature.Get();
    }

    void DeviceD3D12::waitIdle()
    {
        if (!m_waitIdleFence || !m_waitIdleEvent)
            return;

        auto waitQueue = [this](ID3D12CommandQueue* queue)
        {
            if (!queue)
                return;
            m_waitIdleFenceValue++;
            queue->Signal(m_waitIdleFence.Get(), m_waitIdleFenceValue);
            if (m_waitIdleFence->GetCompletedValue() < m_waitIdleFenceValue)
            {
                m_waitIdleFence->SetEventOnCompletion(m_waitIdleFenceValue, m_waitIdleEvent);
                WaitForSingleObject(m_waitIdleEvent, INFINITE);
            }
        };

        waitQueue(m_directQueue.Get());
        waitQueue(m_computeQueue.Get());
        waitQueue(m_copyQueue.Get());
    }

    void DeviceD3D12::signalRetirementFence(QueueType queue)
    {
        const size_t idx = static_cast<size_t>(queue);
        ID3D12CommandQueue* targetQueue = m_directQueue.Get();
        if (queue == QueueType::Compute && m_computeQueue)
            targetQueue = m_computeQueue.Get();
        else if (queue == QueueType::Transfer && m_copyQueue)
            targetQueue = m_copyQueue.Get();

        if (targetQueue && m_retirementFences[idx])
        {
            const uint64_t val = m_submittedValues[idx].fetch_add(1, std::memory_order_relaxed) + 1;
            targetQueue->Signal(m_retirementFences[idx].Get(), val);
        }
    }

    QueueTimelineValues DeviceD3D12::getSubmittedTimelineValues() const
    {
        return {
            m_submittedValues[static_cast<size_t>(QueueType::Graphics)].load(std::memory_order_relaxed),
            m_submittedValues[static_cast<size_t>(QueueType::Transfer)].load(std::memory_order_relaxed),
            m_submittedValues[static_cast<size_t>(QueueType::Compute)].load(std::memory_order_relaxed)
        };
    }

    QueueTimelineValues DeviceD3D12::getCompletedTimelineValues() const
    {
        QueueTimelineValues completed{};
        if (m_retirementFences[0])
            completed.graphics = m_retirementFences[0]->GetCompletedValue();
        if (m_retirementFences[1])
            completed.transfer = m_retirementFences[1]->GetCompletedValue();
        if (m_retirementFences[2])
            completed.compute = m_retirementFences[2]->GetCompletedValue();
        return completed;
    }

    void DeviceD3D12::submitAndWait(CommandBuffer& cmdBuffer, uint32_t slotIndex)
    {
        auto* cmdD3D12 = dynamic_cast<CommandBufferD3D12*>(&cmdBuffer);
        if (!cmdD3D12)
            return;

        ID3D12CommandList* lists[] = { cmdD3D12->getNativeCommandList() };
        m_directQueue->ExecuteCommandLists(1, lists);
        signalRetirementFence(QueueType::Graphics);
        waitIdle();
        collectDeferredDeletions();
    }

    void DeviceD3D12::submitCommandBuffers(std::span<CommandBuffer* const> cmdBuffers, Swapchain& swapchain,
                                           uint32_t frameIndex, uint32_t imageIndex, std::span<const TimelinePoint> timelineWaits)
    {
        for (const auto& w : timelineWaits)
        {
            if (auto* fence = dynamic_cast<TimelineSemaphoreD3D12*>(w.semaphore))
                m_directQueue->Wait(fence->getNativeFence(), w.value);
        }

        std::vector<ID3D12CommandList*> lists;
        lists.reserve(cmdBuffers.size());
        for (auto* cb : cmdBuffers)
        {
            if (auto* cmdD3D12 = dynamic_cast<CommandBufferD3D12*>(cb))
                lists.push_back(cmdD3D12->getNativeCommandList());
        }

        if (!lists.empty())
        {
            m_directQueue->ExecuteCommandLists(static_cast<UINT>(lists.size()), lists.data());
        }
        static_cast<SwapchainD3D12&>(swapchain).signalFrame(m_directQueue.Get(), frameIndex);
        signalRetirementFence(QueueType::Graphics);
    }

    void DeviceD3D12::submit(QueueType queue, std::span<CommandBuffer* const> cmdBuffers,
                             std::span<const TimelinePoint> waits, std::span<const TimelinePoint> signals)
    {
        ID3D12CommandQueue* targetQueue = m_directQueue.Get();
        QueueType effectiveQueue = QueueType::Graphics;
        if (queue == QueueType::Compute && m_computeQueue)
        {
            targetQueue = m_computeQueue.Get();
            effectiveQueue = QueueType::Compute;
        }
        else if (queue == QueueType::Transfer && m_copyQueue)
        {
            targetQueue = m_copyQueue.Get();
            effectiveQueue = QueueType::Transfer;
        }

        for (const auto& w : waits)
        {
            if (auto* fence = dynamic_cast<TimelineSemaphoreD3D12*>(w.semaphore))
                targetQueue->Wait(fence->getNativeFence(), w.value);
        }

        std::vector<ID3D12CommandList*> lists;
        for (auto* cb : cmdBuffers)
        {
            if (auto* cmdD3D12 = dynamic_cast<CommandBufferD3D12*>(cb))
                lists.push_back(cmdD3D12->getNativeCommandList());
        }

        if (!lists.empty())
            targetQueue->ExecuteCommandLists(static_cast<UINT>(lists.size()), lists.data());

        for (const auto& s : signals)
        {
            if (auto* fence = dynamic_cast<TimelineSemaphoreD3D12*>(s.semaphore))
                targetQueue->Signal(fence->getNativeFence(), s.value);
        }

        signalRetirementFence(effectiveQueue);
    }

    std::unique_ptr<Swapchain> DeviceD3D12::createSwapchain(const SwapchainDesc& desc)
    {
        return std::make_unique<SwapchainD3D12>(*this, desc);
    }

    std::unique_ptr<Pipeline> DeviceD3D12::createPipeline(const PipelineDesc& desc, ShaderCompiler& compiler)
    {
        return std::make_unique<PipelineD3D12>(*this, desc, compiler);
    }

    std::unique_ptr<CommandAllocator> DeviceD3D12::createCommandAllocator(CommandBufferReset resetMode, QueueType queue)
    {
        D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (queue == QueueType::Compute) type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        else if (queue == QueueType::Transfer) type = D3D12_COMMAND_LIST_TYPE_COPY;
        return std::make_unique<CommandAllocatorD3D12>(*this, resetMode, type);
    }

    std::unique_ptr<TimelineSemaphore> DeviceD3D12::createTimelineSemaphore(uint64_t initialValue)
    {
        return std::make_unique<TimelineSemaphoreD3D12>(*this, initialValue);
    }

    std::unique_ptr<Texture2D> DeviceD3D12::createTexture(const TextureDesc& desc)
    {
        return std::make_unique<TextureD3D12>(*this, desc);
    }

    std::unique_ptr<Buffer> DeviceD3D12::createBuffer(const BufferDesc& desc)
    {
        return std::make_unique<BufferD3D12>(*this, desc);
    }

    std::unique_ptr<DescriptorHeap> DeviceD3D12::createDescriptorHeap(const DescriptorHeapDesc& desc)
    {
        return std::make_unique<DescriptorHeapD3D12>(*this, desc);
    }

    AccelerationStructureBuildSizes DeviceD3D12::getAccelerationStructureBuildSizes(const AccelerationStructureBuildDesc& desc)
    {
        const D3D12AccelerationStructureInputs build(desc, false);
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo{};
        m_device->GetRaytracingAccelerationStructurePrebuildInfo(&build.inputs, &prebuildInfo);

        AccelerationStructureBuildSizes sizes{};
        sizes.accelerationStructureSize = prebuildInfo.ResultDataMaxSizeInBytes;
        sizes.buildScratchSize = prebuildInfo.ScratchDataSizeInBytes;
        sizes.updateScratchSize = prebuildInfo.UpdateScratchDataSizeInBytes;
        return sizes;
    }

    std::unique_ptr<AccelerationStructure> DeviceD3D12::createAccelerationStructure(const AccelerationStructureDesc& desc)
    {
        return std::make_unique<AccelerationStructureD3D12>(*this, desc);
    }

    std::unique_ptr<QueryPool> DeviceD3D12::createQueryPool(const QueryPoolDesc& desc)
    {
        return std::make_unique<QueryPoolD3D12>(*this, desc);
    }

    std::unique_ptr<GpuProfiler> DeviceD3D12::createGpuProfiler(uint32_t framesInFlight)
    {
        return std::make_unique<GpuProfilerD3D12>(*this, framesInFlight);
    }

    void DeviceD3D12::beginFrame(uint32_t frameNumber)
    {
        if (m_allocator)
        {
            m_allocator->setCurrentFrameIndex(frameNumber);
        }
        collectDeferredDeletions();
    }

    void DeviceD3D12::getMemoryStats(std::vector<MemoryHeapStats>& outHeaps) const
    {
        if (m_allocator)
        {
            m_allocator->getHeapStats(outHeaps);
            return;
        }

        outHeaps.clear();
        DXGI_QUERY_VIDEO_MEMORY_INFO videoMemoryInfo{};
        if (m_adapter && SUCCEEDED(m_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &videoMemoryInfo)))
        {
            MemoryHeapStats local{};
            local.deviceLocal = true;
            local.usage = videoMemoryInfo.CurrentUsage;
            local.budget = videoMemoryInfo.Budget;
            outHeaps.push_back(local);
        }
    }
}
