#pragma once

#include <cstdint>
#include <memory>

namespace NRI
{
    struct NrdInstanceSpec
    {
        uint32_t identifier = 0;
        uint32_t method = 0; // Matches NriDenoiserMethod enum values 0..6
    };

    struct NrdCommonSettingsSpec
    {
        float view[16]{};
        float proj[16]{};
        float prevView[16]{};
        float prevProj[16]{};
        float motionVectorScale[3]{};
        float cameraJitter[2]{};
        float cameraJitterPrev[2]{};
        uint32_t frameIndex = 0;
        float denoisingRange = 0.0f;
        bool resetHistory = false;
    };

    struct NrdSigmaSettingsSpec
    {
        float lightDirection[3]{};
        float planeDistanceSensitivity = 0.0f;
        uint32_t maxStabilizedFrameNum = 0;
    };

    struct NrdReblurSettingsSpec
    {
        float hitDistanceA = 0.0f;
        float hitDistanceB = 0.0f;
        float hitDistanceC = 0.0f;
        float hitDistanceD = 0.0f;
        uint32_t maxAccumulatedFrameNum = 0;
        float diffusePrepassBlurRadius = 0.0f;
        float specularPrepassBlurRadius = 0.0f;
        float planeDistanceSensitivity = 0.0f;
        bool disableAntiFirefly = false;
        bool disableArea5x5HitDistanceReconstruction = false;
    };

    struct NrdRelaxSettingsSpec
    {
        float diffusePrepassBlurRadius = 0.0f;
        float specularPrepassBlurRadius = 0.0f;
        uint32_t diffuseMaxAccumulatedFrameNum = 0;
        uint32_t specularMaxAccumulatedFrameNum = 0;
        bool disableAntiFirefly = false;
    };

    struct NrdVulkanInitDesc
    {
        void* vkInstance = nullptr;
        void* vkPhysicalDevice = nullptr;
        void* vkDevice = nullptr;
        uint32_t graphicsQueueFamily = 0;
        uint32_t computeQueueFamily = 0;
        bool hasAsyncCompute = false;
    };

    struct NrdD3D12InitDesc
    {
        void* d3d12Device = nullptr;
        void* graphicsQueue = nullptr;
        void* computeQueue = nullptr;
        bool hasAsyncCompute = false;
    };

    struct NrdNativeTextureDesc
    {
        uint64_t vkImage = 0;
        void* d3d12Resource = nullptr;
        uint32_t format = 0; // VkFormat or DXGI_FORMAT
    };

    struct NrdNativeDispatchDesc
    {
        bool restoreInitialState = false;
        void* vkCommandBuffer = nullptr;
        bool vkComputeQueue = false;
        void* d3d12CommandList = nullptr;

        NrdNativeTextureDesc inMotionVectors{};
        NrdNativeTextureDesc inNormalRoughness{};
        NrdNativeTextureDesc inViewZ{};
        NrdNativeTextureDesc inPenumbra{};
        NrdNativeTextureDesc outShadowTranslucent{};
        NrdNativeTextureDesc inSpecRadianceHitDist{};
        NrdNativeTextureDesc outSpecRadianceHitDist{};
        NrdNativeTextureDesc inDiffRadianceHitDist{};
        NrdNativeTextureDesc outDiffRadianceHitDist{};
    };

    class NrdIntegrationHost
    {
    public:
        NrdIntegrationHost();
        ~NrdIntegrationHost();

        NrdIntegrationHost(const NrdIntegrationHost&) = delete;
        NrdIntegrationHost& operator=(const NrdIntegrationHost&) = delete;

        void configureInstances(const NrdInstanceSpec* instances, uint32_t count,
                                uint32_t queuedFrameNum, bool enableWholeLifetimeCaching);

        void destroy();

        bool recreateVK(uint32_t width, uint32_t height, const NrdVulkanInitDesc& vkInit, uint32_t& outErrorCode);
        bool recreateD3D12(uint32_t width, uint32_t height, const NrdD3D12InitDesc& d3dInit, uint32_t& outErrorCode);

        bool isInitialized() const;
        uint32_t width() const;
        uint32_t height() const;
        size_t instanceCount() const;

        bool beginFrame(const NrdCommonSettingsSpec& settings);
        void setSigmaSettings(uint32_t identifier, const NrdSigmaSettingsSpec& settings);
        void setReblurSettings(uint32_t identifier, const NrdReblurSettingsSpec& settings);
        void setRelaxSettings(uint32_t identifier, const NrdRelaxSettingsSpec& settings);

        bool denoiseVK(const uint32_t* identifiers, uint32_t count, const NrdNativeDispatchDesc& desc);
        bool denoiseD3D12(const uint32_t* identifiers, uint32_t count, const NrdNativeDispatchDesc& desc);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
