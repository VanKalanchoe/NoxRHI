#pragma once

#include "../NRI.h"
#include "NRIDeviceCreation.h"

NRI_EXTERN_C_BEGIN

typedef enum NriFrameResult
{
    NRI_FRAME_RESULT_SUCCESS = 0,
    NRI_FRAME_RESULT_RESIZE_REQUIRED = 1
} NriFrameResult;

typedef struct NriSwapchainDesc
{
    uint32_t width;
    uint32_t height;
    bool vsync;
    NriNativeWindowHandle window; // If hwnd == NULL && vkSurface == 0, uses the device's primary window
    // Without vsync: false (the default) presents the newest finished frame without tearing (Vulkan MAILBOX, D3D12 Present(0, 0));
    // true allows tearing (Vulkan IMMEDIATE, D3D12 DXGI_PRESENT_ALLOW_TEARING). The choice NVIDIA's NRI makes (SwapChainBits::ALLOW_TEARING).
    bool allowTearing;
} NriSwapchainDesc;

NRI_API NriResult NRI_CALL nriCreateSwapchain(NriDevice device, const NriSwapchainDesc* desc, NriSwapchain* outSwapchain);
NRI_API void NRI_CALL nriDestroySwapchain(NriSwapchain swapchain);
NRI_API NriExtent2D NRI_CALL nriGetSwapchainExtent(NriSwapchain swapchain);
NRI_API NriFrameResult NRI_CALL nriAcquireNextSwapchainImage(NriSwapchain swapchain, uint32_t frameIndex, uint32_t* outImageIndex);
NRI_API NriFrameResult NRI_CALL nriPresentSwapchain(NriSwapchain swapchain, uint32_t frameIndex, uint32_t imageIndex);

NRI_EXTERN_C_END
