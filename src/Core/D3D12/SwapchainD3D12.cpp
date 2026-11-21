#include "SwapchainD3D12.h"
#include "DeviceD3D12.h"
#include "../../Extensions/Upscaler/StreamlineContext.h"

namespace NRI
{
    SwapchainD3D12::SwapchainD3D12(DeviceD3D12& device, const SwapchainDesc& desc)
        : m_deviceD3D12(device), m_desc(desc), m_extent{ desc.Width, desc.Height }
    {
        ThrowIfFailed(m_deviceD3D12.getNativeDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_frameFence)),
                      "Create Swapchain Frame Fence");
        m_frameFenceEvent = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        createSwapChain();
    }

    SwapchainD3D12::~SwapchainD3D12()
    {
        if (m_frameFenceEvent)
        {
            CloseHandle(m_frameFenceEvent);
            m_frameFenceEvent = nullptr;
        }
    }

    void SwapchainD3D12::createSwapChain()
    {
        HWND hwnd = m_desc.Window.hwnd ? static_cast<HWND>(m_desc.Window.hwnd) : m_deviceD3D12.getWindowHandle();
        if (!hwnd)
        {
            throw std::runtime_error("SwapchainD3D12: HWND is null");
        }

        DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
        swapChainDesc.Width = m_extent.width;
        swapChainDesc.Height = m_extent.height;
        swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDesc.Stereo = FALSE;
        swapChainDesc.SampleDesc.Count = 1;
        swapChainDesc.SampleDesc.Quality = 0;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.BufferCount = m_bufferCount;
        swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        // With Streamline (manual hooking) the swapchain comes from its proxy factory (slUpgradeInterface): the swapchain is then its proxy,
        // and Present / GetBuffer / GetCurrentBackBufferIndex -- mandatory hooks -- go through it, which runs its presentCommon() every frame.
        ComPtr<IDXGIFactory6> factory = m_deviceD3D12.getNativeFactory();
        // (The factory proxy takes its own reference on the native factory and starts at one reference -- ours, so no AddRef first; it is
        // released after the swapchain's creation, which keeps nothing of it.)
        if (StreamlineContext* streamline = m_deviceD3D12.getStreamlineContext(); streamline && streamline->isInitialized())
        {
            void* proxy = factory.Get();
            if (streamline->upgradeD3D12Interface(&proxy) && proxy != factory.Get())
            {
                factory.Reset();
                factory.Attach(static_cast<IDXGIFactory6*>(proxy));
            }
        }

        ComPtr<IDXGISwapChain1> swapChain1;
        ThrowIfFailed(factory->CreateSwapChainForHwnd(
            m_deviceD3D12.getSwapchainQueue(),
            hwnd,
            &swapChainDesc,
            nullptr,
            nullptr,
            &swapChain1
        ), "CreateSwapChainForHwnd");

        ThrowIfFailed(swapChain1.As(&m_swapChain), "Query IDXGISwapChain4");

        // Disable Alt+Enter fullscreen handling by DXGI (SDL handles it)
        m_deviceD3D12.getNativeFactory()->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

        // Create Texture2D wrappers for each backbuffer
        m_backBuffers.resize(m_bufferCount);
        for (uint32_t i = 0; i < m_bufferCount; ++i)
        {
            ComPtr<ID3D12Resource> backBuffer;
            ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffer)), "Swapchain GetBuffer");

            TextureDesc texDesc{};
            texDesc.width = m_extent.width;
            texDesc.height = m_extent.height;
            texDesc.format = ImageFormat::Surface;
            texDesc.usage = TextureUsage::ColorAttachment;
            texDesc.mipLevels = 1;
            texDesc.arrayLayers = 1;

            m_backBuffers[i] = std::make_unique<TextureD3D12>(m_deviceD3D12, std::move(backBuffer), texDesc);
        }
    }

    void SwapchainD3D12::waitForFrameSlot(uint32_t frameIndex)
    {
        if (!m_frameFence || !m_frameFenceEvent)
            return;
        const uint64_t waitValue = m_frameFenceValues[frameIndex % m_frameFenceValues.size()];
        if (waitValue > 0 && m_frameFence->GetCompletedValue() < waitValue)
        {
            ThrowIfFailed(m_frameFence->SetEventOnCompletion(waitValue, m_frameFenceEvent), "Swapchain SetEventOnCompletion");
            WaitForSingleObject(m_frameFenceEvent, INFINITE);
        }
    }

    void SwapchainD3D12::signalFrame(ID3D12CommandQueue* queue, uint32_t frameIndex)
    {
        if (!queue || !m_frameFence)
            return;
        const uint64_t val = ++m_nextFenceValue;
        m_frameFenceValues[frameIndex % m_frameFenceValues.size()] = val;
        ThrowIfFailed(queue->Signal(m_frameFence.Get(), val), "Swapchain Signal Frame Fence");
    }

    FrameResult SwapchainD3D12::acquireNextImage(uint32_t frameIndex, uint32_t& outImageIndex)
    {
        waitForFrameSlot(frameIndex);
        outImageIndex = m_swapChain->GetCurrentBackBufferIndex();
        return FrameResult::Success;
    }

    FrameResult SwapchainD3D12::present(uint32_t frameIndex, uint32_t imageIndex)
    {
        UINT syncInterval = m_desc.VSync ? 1 : 0;
        UINT flags = (!m_desc.VSync) ? DXGI_PRESENT_ALLOW_TEARING : 0;
        HRESULT hr = m_swapChain->Present(syncInterval, flags);

        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        {
            return FrameResult::ResizeRequired;
        }
        return FrameResult::Success;
    }
}
