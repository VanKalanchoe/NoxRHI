#include "NRI/Extensions/NRIWrapperVK.h"
#include "../../Core/CHandles.h"

#ifdef NRI_ENABLE_VULKAN
#include "../../Core/Vulkan/DeviceVK.h"
#include "../../Core/Vulkan/CommandBufferVK.h"
#include "../../Core/Vulkan/TextureVK.h"
#include "../../Core/Vulkan/BufferVK.h"
#include "../../Core/Vulkan/SwapchainVK.h"
#include "../../Core/Vulkan/TimelineSemaphoreVK.h"
#endif

extern "C" {

NriResult NRI_CALL nriGetDeviceNativeVK(NriDevice device, NriDeviceNativeVK* outNative)
{
    if (!device || !device->impl || !outNative)
        return NRI_ERROR_INVALID_ARGUMENT;
    *outNative = {};
#ifdef NRI_ENABLE_VULKAN
    auto* vkDev = dynamic_cast<NRI::DeviceVK*>(device->impl.get());
    if (!vkDev)
        return NRI_ERROR_UNSUPPORTED;
    outNative->vkInstance = static_cast<void*>(*vkDev->getInstance());
    outNative->vkPhysicalDevice = static_cast<void*>(*vkDev->getPhysicalDevice());
    outNative->vkDevice = static_cast<void*>(*vkDev->getDevice());
    outNative->vkGraphicsQueue = static_cast<void*>(*vkDev->getQueue());
    outNative->vkTransferQueue = static_cast<void*>(*vkDev->getTransferQueue());
    outNative->vkComputeQueue = vkDev->hasAsyncComputeQueue() ? static_cast<void*>(*vkDev->getComputeQueue()) : nullptr;
    outNative->graphicsQueueFamilyIndex = vkDev->getQueueIndex();
    outNative->transferQueueFamilyIndex = vkDev->getTransferQueueIndex();
    outNative->computeQueueFamilyIndex = vkDev->getComputeQueueIndex();
    outNative->vkSurface = reinterpret_cast<uint64_t>(static_cast<VkSurfaceKHR>(vkDev->getActiveSurface()));
    outNative->surfaceVkFormat = static_cast<int32_t>(vkDev->getSurfaceFormat().format);
    outNative->depthVkFormat = static_cast<int32_t>(vkDev->getDepthFormat());
    outNative->debugUtilsEnabled = vkDev->isDebugUtilsEnabled();
    outNative->shaderObjectsEnabled = vkDev->isShaderObjectExtensionEnabled();
    return NRI_SUCCESS;
#else
    return NRI_ERROR_UNSUPPORTED;
#endif
}

void* NRI_CALL nriGetCommandBufferNativeVK(NriCommandBuffer cmd, uint32_t slotIndex)
{
#ifdef NRI_ENABLE_VULKAN
    if (!cmd || !cmd->impl) return nullptr;
    if (auto* vkCmd = dynamic_cast<NRI::CommandBufferVK*>(cmd->impl.get()))
        return static_cast<void*>(*vkCmd->getNativeBuffer(slotIndex));
#endif
    return nullptr;
}

uint64_t NRI_CALL nriGetTextureNativeImageVK(NriTexture texture)
{
#ifdef NRI_ENABLE_VULKAN
    if (!texture || !texture->impl) return 0;
    if (auto* vkTex = dynamic_cast<NRI::TextureVK*>(texture->impl.get()))
        return reinterpret_cast<uint64_t>(static_cast<VkImage>(*vkTex->getNativeImage()));
#endif
    return 0;
}

uint64_t NRI_CALL nriGetTextureNativeViewVK(NriTexture texture)
{
#ifdef NRI_ENABLE_VULKAN
    if (!texture || !texture->impl) return 0;
    if (auto* vkTex = dynamic_cast<NRI::TextureVK*>(texture->impl.get()))
        return reinterpret_cast<uint64_t>(static_cast<VkImageView>(*vkTex->getNativeView()));
#endif
    return 0;
}

int32_t NRI_CALL nriGetTextureNativeFormatVK(NriTexture texture)
{
#ifdef NRI_ENABLE_VULKAN
    if (!texture || !texture->impl) return 0;
    if (auto* vkTex = dynamic_cast<NRI::TextureVK*>(texture->impl.get()))
        return static_cast<int32_t>(vkTex->getFormat());
#endif
    return 0;
}

uint64_t NRI_CALL nriGetBufferNativeVK(NriBuffer buffer)
{
#ifdef NRI_ENABLE_VULKAN
    if (!buffer || !buffer->impl) return 0;
    if (auto* vkBuf = dynamic_cast<NRI::BufferVK*>(buffer->impl.get()))
        return reinterpret_cast<uint64_t>(static_cast<VkBuffer>(*vkBuf->getNativeBuffer()));
#endif
    return 0;
}

uint64_t NRI_CALL nriGetSwapchainNativeVK(NriSwapchain swapchain)
{
#ifdef NRI_ENABLE_VULKAN
    if (!swapchain || !swapchain->impl) return 0;
    if (auto* vkSwap = dynamic_cast<NRI::SwapchainVK*>(swapchain->impl.get()))
        return reinterpret_cast<uint64_t>(static_cast<VkSwapchainKHR>(vkSwap->getNativeSwapchain()));
#endif
    return 0;
}

uint64_t NRI_CALL nriGetSwapchainNativeImageVK(NriSwapchain swapchain, uint32_t imageIndex)
{
#ifdef NRI_ENABLE_VULKAN
    if (!swapchain || !swapchain->impl) return 0;
    if (auto* vkSwap = dynamic_cast<NRI::SwapchainVK*>(swapchain->impl.get()))
        return reinterpret_cast<uint64_t>(static_cast<VkImage>(vkSwap->getNativeImage(imageIndex)));
#endif
    return 0;
}

uint64_t NRI_CALL nriGetSwapchainNativeViewVK(NriSwapchain swapchain, uint32_t imageIndex)
{
#ifdef NRI_ENABLE_VULKAN
    if (!swapchain || !swapchain->impl) return 0;
    if (auto* vkSwap = dynamic_cast<NRI::SwapchainVK*>(swapchain->impl.get()))
        return reinterpret_cast<uint64_t>(static_cast<VkImageView>(vkSwap->getNativeView(imageIndex)));
#endif
    return 0;
}

uint64_t NRI_CALL nriGetTimelineSemaphoreNativeVK(NriTimelineSemaphore semaphore)
{
#ifdef NRI_ENABLE_VULKAN
    if (!semaphore || !semaphore->impl) return 0;
    if (auto* vkSem = dynamic_cast<NRI::TimelineSemaphoreVK*>(semaphore->impl.get()))
        return reinterpret_cast<uint64_t>(static_cast<VkSemaphore>(*vkSem->getNativeSemaphore()));
#endif
    return 0;
}

} // extern "C"
