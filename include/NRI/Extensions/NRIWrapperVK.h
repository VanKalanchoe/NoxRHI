#pragma once

#include "../NRI.h"

NRI_EXTERN_C_BEGIN

// Dispatchable handles (VkInstance, VkPhysicalDevice, VkDevice, VkQueue, VkCommandBuffer) are void*.
// Non-dispatchable handles (VkImage, VkImageView, VkBuffer, VkSurfaceKHR, VkSwapchainKHR, VkSemaphore) are uint64_t.

typedef struct NriDeviceNativeVK
{
    void* vkInstance;
    void* vkPhysicalDevice;
    void* vkDevice;
    void* vkGraphicsQueue;
    void* vkTransferQueue;
    void* vkComputeQueue;
    uint32_t graphicsQueueFamilyIndex;
    uint32_t transferQueueFamilyIndex;
    uint32_t computeQueueFamilyIndex;
    uint64_t vkSurface;
    int32_t surfaceVkFormat;
    int32_t depthVkFormat;
    bool debugUtilsEnabled;
    bool shaderObjectsEnabled;
} NriDeviceNativeVK;

NRI_API NriResult NRI_CALL nriGetDeviceNativeVK(NriDevice device, NriDeviceNativeVK* outNative);
NRI_API void* NRI_CALL nriGetCommandBufferNativeVK(NriCommandBuffer cmd, uint32_t slotIndex);
NRI_API uint64_t NRI_CALL nriGetTextureNativeImageVK(NriTexture texture);
NRI_API uint64_t NRI_CALL nriGetTextureNativeViewVK(NriTexture texture);
NRI_API int32_t NRI_CALL nriGetTextureNativeFormatVK(NriTexture texture);
NRI_API uint64_t NRI_CALL nriGetBufferNativeVK(NriBuffer buffer);
NRI_API uint64_t NRI_CALL nriGetSwapchainNativeVK(NriSwapchain swapchain);
NRI_API uint64_t NRI_CALL nriGetSwapchainNativeImageVK(NriSwapchain swapchain, uint32_t imageIndex);
NRI_API uint64_t NRI_CALL nriGetSwapchainNativeViewVK(NriSwapchain swapchain, uint32_t imageIndex);
NRI_API uint64_t NRI_CALL nriGetTimelineSemaphoreNativeVK(NriTimelineSemaphore semaphore);

NRI_EXTERN_C_END
