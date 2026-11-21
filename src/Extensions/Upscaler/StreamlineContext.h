#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../../Core/NRITypes.h"
#include "NRI/Extensions/NRIUpscaler.h"

namespace NRI
{
    class Device;
    class CommandBuffer;
    class Texture2D;

    struct UpscalerTextures
    {
        Texture2D* inputColor = nullptr;
        Texture2D* outputColor = nullptr;
        Texture2D* depth = nullptr;
        Texture2D* motionVectors = nullptr;
        Texture2D* albedo = nullptr;
        Texture2D* specularAlbedo = nullptr;
        Texture2D* normal = nullptr;
        Texture2D* roughness = nullptr;
        Texture2D* specularHitDistance = nullptr;
        Texture2D* specularMotionVectors = nullptr;
        Texture2D* colorBeforeTransparency = nullptr;
    };

    // Streamline's proxies for the Vulkan functions its manual-hooking guide calls mandatory (sl_hooks.h: present, swapchain, surface,
    // device wait idle). NoxRHI runs Streamline with eUseManualHooking, so these must be called instead of the native functions: going
    // through them is what runs Streamline's presentCommon() every frame (its per-frame bookkeeping and garbage collection -- without it
    // Streamline logs "presentCommon() was not observed") and lets it track the swapchain. PFN_* values, as void* (no Vulkan header here).
    struct StreamlineVulkanHooks
    {
        void* vkCreateWin32SurfaceKHR = nullptr;
        void* vkDestroySurfaceKHR = nullptr;
        void* vkCreateSwapchainKHR = nullptr;
        void* vkDestroySwapchainKHR = nullptr;
        void* vkGetSwapchainImagesKHR = nullptr;
        void* vkAcquireNextImageKHR = nullptr;
        void* vkQueuePresentKHR = nullptr;
        void* vkDeviceWaitIdle = nullptr;
    };

    // Wraps NVIDIA Streamline SDK lifecycle and feature evaluation across Vulkan and D3D12.
    class StreamlineContext
    {
    public:
        StreamlineContext();
        ~StreamlineContext();

        bool initBeforeDevice(GraphicsAPI api, const DeviceInitDesc& desc);
        void getRequiredVulkanExtensions(std::vector<std::string>& outInstanceExts,
                                         std::vector<std::string>& outDeviceExts) const;

        bool registerVulkanDevice(void* vkInstance,
                                  void* vkPhysicalDevice,
                                  void* vkDevice,
                                  uint32_t graphicsQueueFamily,
                                  uint32_t graphicsQueueIndex,
                                  uint32_t computeQueueFamily,
                                  uint32_t computeQueueIndex);
        bool registerD3D12Device(void* id3d12Device, const uint8_t* adapterLuid, uint32_t luidSize);
        void shutdown();

        bool isInitialized() const { return m_initialized; }
        bool isUpscalerSupported(NriUpscalerType type) const;
        // Vulkan: Streamline's proxies for its mandatory hooks, or null when Streamline is off or one of them is missing.
        const StreamlineVulkanHooks* getVulkanHooks() const { return m_vkHooksComplete ? &m_vkHooks : nullptr; }
        // D3D12: replaces *iface (an ID3D12Device / IDXGIFactory, with its reference) by Streamline's proxy for it (slUpgradeInterface).
        // The device's command queues and the swapchain's factory go through it, as the manual-hooking guide asks.
        bool upgradeD3D12Interface(void** iface) const;
        // D3D12: the native interface behind one of Streamline's proxies, with a reference (slGetNativeInterface). The guide: use native
        // interfaces everywhere except for the hooked calls.
        bool nativeD3D12Interface(void* proxy, void** native) const;
        // The frame's Streamline token, from the device's frame number (slGetNewFrameToken with the host's index), once per frame at its
        // start -- every frame, not only those that evaluate DLSS: Streamline's present hook counts every frame.
        void beginFrame(uint32_t frameNumber);

        NriResult getOptimalSettings(NriUpscalerType type,
                                     NriUpscalerMode mode,
                                     Extent2D outputSize,
                                     NriUpscalerOptimalSettings& outSettings) const;
        void resetViewport();
        bool evaluate(Device& device,
                      CommandBuffer& cmd,
                      NriUpscalerType type,
                      const UpscalerTextures& textures,
                      const NriUpscalerEvaluateDesc& desc);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
        bool m_initialized = false;
        StreamlineVulkanHooks m_vkHooks{};
        bool m_vkHooksComplete = false;
    };
}
