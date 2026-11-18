#include "NRI/Extensions/NRIWrapperD3D12.h"
#include "../../Core/CHandles.h"

#if defined(_WIN32) && defined(NRI_ENABLE_D3D12)
#include "../../Core/D3D12/DeviceD3D12.h"
#include "../../Core/D3D12/CommandBufferD3D12.h"
#include "../../Core/D3D12/TextureD3D12.h"
#include "../../Core/D3D12/BufferD3D12.h"
#include "../../Core/D3D12/DescriptorHeapD3D12.h"
#include "../../Core/D3D12/SwapchainD3D12.h"
#include "../../Core/D3D12/TimelineSemaphoreD3D12.h"
#endif

extern "C" {

NriResult NRI_CALL nriGetDeviceNativeD3D12(NriDevice device, NriDeviceNativeD3D12* outNative)
{
    if (!device || !device->impl || !outNative)
        return NRI_ERROR_INVALID_ARGUMENT;
    *outNative = {};
#if defined(_WIN32) && defined(NRI_ENABLE_D3D12)
    auto* d3dDev = dynamic_cast<NRI::DeviceD3D12*>(device->impl.get());
    if (!d3dDev)
        return NRI_ERROR_UNSUPPORTED;
    outNative->device = d3dDev->getNativeDevice();
    outNative->factory = d3dDev->getNativeFactory();
    outNative->adapter = d3dDev->getNativeAdapter();
    outNative->graphicsQueue = d3dDev->getGraphicsQueue();
    outNative->computeQueue = d3dDev->getComputeQueue();
    outNative->transferQueue = d3dDev->getTransferQueue();
    outNative->globalRootSignature = d3dDev->getGlobalRootSignature();
    outNative->hwnd = d3dDev->getWindowHandle();
    return NRI_SUCCESS;
#else
    return NRI_ERROR_UNSUPPORTED;
#endif
}

void* NRI_CALL nriGetCommandBufferNativeD3D12(NriCommandBuffer cmd)
{
#if defined(_WIN32) && defined(NRI_ENABLE_D3D12)
    if (!cmd || !cmd->impl) return nullptr;
    if (auto* d3dCmd = dynamic_cast<NRI::CommandBufferD3D12*>(cmd->impl.get()))
        return d3dCmd->getNativeCommandList();
#endif
    return nullptr;
}

void* NRI_CALL nriGetTextureNativeD3D12(NriTexture texture)
{
#if defined(_WIN32) && defined(NRI_ENABLE_D3D12)
    if (!texture || !texture->impl) return nullptr;
    if (auto* d3dTex = dynamic_cast<NRI::TextureD3D12*>(texture->impl.get()))
        return d3dTex->getNativeResource();
#endif
    return nullptr;
}

int32_t NRI_CALL nriGetTextureNativeFormatD3D12(NriTexture texture)
{
#if defined(_WIN32) && defined(NRI_ENABLE_D3D12)
    if (!texture || !texture->impl) return 0;
    if (auto* d3dTex = dynamic_cast<NRI::TextureD3D12*>(texture->impl.get()))
        return static_cast<int32_t>(d3dTex->getDxgiFormat());
#endif
    return 0;
}

void* NRI_CALL nriGetBufferNativeD3D12(NriBuffer buffer)
{
#if defined(_WIN32) && defined(NRI_ENABLE_D3D12)
    if (!buffer || !buffer->impl) return nullptr;
    if (auto* d3dBuf = dynamic_cast<NRI::BufferD3D12*>(buffer->impl.get()))
        return d3dBuf->getNativeBuffer();
#endif
    return nullptr;
}

void* NRI_CALL nriGetDescriptorHeapNativeD3D12(NriDescriptorHeap heap)
{
#if defined(_WIN32) && defined(NRI_ENABLE_D3D12)
    if (!heap || !heap->impl) return nullptr;
    if (auto* d3dHeap = dynamic_cast<NRI::DescriptorHeapD3D12*>(heap->impl.get()))
        return d3dHeap->getNativeHeap();
#endif
    return nullptr;
}

void* NRI_CALL nriGetSwapchainNativeD3D12(NriSwapchain swapchain)
{
#if defined(_WIN32) && defined(NRI_ENABLE_D3D12)
    if (!swapchain || !swapchain->impl) return nullptr;
    if (auto* d3dSwap = dynamic_cast<NRI::SwapchainD3D12*>(swapchain->impl.get()))
        return d3dSwap->getNativeSwapchain();
#endif
    return nullptr;
}

void* NRI_CALL nriGetTimelineSemaphoreNativeD3D12(NriTimelineSemaphore semaphore)
{
#if defined(_WIN32) && defined(NRI_ENABLE_D3D12)
    if (!semaphore || !semaphore->impl) return nullptr;
    if (auto* d3dSem = dynamic_cast<NRI::TimelineSemaphoreD3D12*>(semaphore->impl.get()))
        return d3dSem->getNativeFence();
#endif
    return nullptr;
}

} // extern "C"
