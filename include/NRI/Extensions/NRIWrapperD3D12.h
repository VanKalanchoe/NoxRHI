#pragma once

#include "../NRI.h"

NRI_EXTERN_C_BEGIN

typedef struct NriDeviceNativeD3D12
{
    void* device;             // ID3D12Device5*
    void* factory;            // IDXGIFactory6*
    void* adapter;            // IDXGIAdapter4*
    void* graphicsQueue;      // ID3D12CommandQueue*
    void* computeQueue;       // ID3D12CommandQueue*
    void* transferQueue;      // ID3D12CommandQueue*
    void* globalRootSignature;// ID3D12RootSignature*
    void* hwnd;               // HWND
} NriDeviceNativeD3D12;

NRI_API NriResult NRI_CALL nriGetDeviceNativeD3D12(NriDevice device, NriDeviceNativeD3D12* outNative);
NRI_API void* NRI_CALL nriGetCommandBufferNativeD3D12(NriCommandBuffer cmd);     // ID3D12GraphicsCommandList6*
NRI_API void* NRI_CALL nriGetTextureNativeD3D12(NriTexture texture);             // ID3D12Resource*
NRI_API int32_t NRI_CALL nriGetTextureNativeFormatD3D12(NriTexture texture);     // DXGI_FORMAT
NRI_API void* NRI_CALL nriGetBufferNativeD3D12(NriBuffer buffer);                // ID3D12Resource*
NRI_API void* NRI_CALL nriGetDescriptorHeapNativeD3D12(NriDescriptorHeap heap);  // ID3D12DescriptorHeap*
NRI_API void* NRI_CALL nriGetSwapchainNativeD3D12(NriSwapchain swapchain);       // IDXGISwapChain4*
NRI_API void* NRI_CALL nriGetTimelineSemaphoreNativeD3D12(NriTimelineSemaphore semaphore); // ID3D12Fence*

NRI_EXTERN_C_END
