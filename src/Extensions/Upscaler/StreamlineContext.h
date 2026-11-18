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
        void* getVkQueuePresentFn() const { return m_vkQueuePresentFn; }

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
        void* m_vkQueuePresentFn = nullptr;
    };
}
