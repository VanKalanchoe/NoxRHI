#include "NrdIntegrationHost.h"
#include "../../Core/Log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(NRI_ENABLE_NRD)
    #if defined(_WIN32)
        #ifndef WIN32_LEAN_AND_MEAN
            #define WIN32_LEAN_AND_MEAN
        #endif
        #ifndef NOMINMAX
            #define NOMINMAX
        #endif
        #include <windows.h>
        #if defined(NRI_ENABLE_D3D12)
            #include <d3d12.h>
            #include <d3d12sdklayers.h>
            #include <wrl/client.h>
        #endif
    #endif

    // Resolve NVIDIA NRI symbols dynamically from NRI.dll so NoxRHI's own C exports (e.g. nriDestroyDevice) never collide.
#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wunused-function"
#endif
    #define NRI_API static
    #define nriGetInterface nv_nriGetInterface
    #define nriDestroyDevice nv_nriDestroyDevice
    #define nriCreateDeviceFromD3D12Device nv_nriCreateDeviceFromD3D12Device
    #define nriCreateDeviceFromVKDevice nv_nriCreateDeviceFromVKDevice

    #include <NRD.h>
    #include <NRDSettings.h>
    #include "../../../vendors/NRD/Include/NRI.h"
    #include "../../../vendors/NRD/Include/Extensions/NRIHelper.h"
    #include "../../../vendors/NRD/Include/Extensions/NRIRayTracing.h"
    #if defined(NRI_ENABLE_VULKAN)
        #include "../../../vendors/NRD/Include/Extensions/NRIWrapperVK.h"
    #endif
    #if defined(NRI_ENABLE_D3D12) && defined(_WIN32)
        #include "../../../vendors/NRD/Include/Extensions/NRIWrapperD3D12.h"
    #endif
#ifdef __clang__
    #pragma clang diagnostic pop
#endif

    namespace nri
    {
        namespace detail
        {
            inline void* loadNriProc(const char* name)
            {
#if defined(_WIN32)
                static HMODULE s_nriModule = []() -> HMODULE
                {
                    HMODULE h = GetModuleHandleA("NRI.dll");
                    if (!h)
                        h = LoadLibraryA("NRI.dll");
                    return h;
                }();
                return s_nriModule ? reinterpret_cast<void*>(GetProcAddress(s_nriModule, name)) : nullptr;
#else
                (void)name;
                return nullptr;
#endif
            }
        }

        static Result NRI_CALL nv_nriGetInterface(const Device& device, const char* interfaceName, size_t interfaceSize, void* interfacePtr)
        {
            using Fn = Result(NRI_CALL*)(const Device&, const char*, size_t, void*);
            static Fn fn = reinterpret_cast<Fn>(detail::loadNriProc("nriGetInterface"));
            return fn ? fn(device, interfaceName, interfaceSize, interfacePtr) : Result::FAILURE;
        }

        static void NRI_CALL nv_nriDestroyDevice(Device* device)
        {
            using Fn = void(NRI_CALL*)(Device*);
            static Fn fn = reinterpret_cast<Fn>(detail::loadNriProc("nriDestroyDevice"));
            if (fn)
                fn(device);
        }

#if defined(NRI_ENABLE_VULKAN)
        static Result NRI_CALL nv_nriCreateDeviceFromVKDevice(const DeviceCreationVKDesc& deviceDesc, Device*& device)
        {
            using Fn = Result(NRI_CALL*)(const DeviceCreationVKDesc&, Device*&);
            static Fn fn = reinterpret_cast<Fn>(detail::loadNriProc("nriCreateDeviceFromVKDevice"));
            return fn ? fn(deviceDesc, device) : Result::FAILURE;
        }
#endif

#if defined(NRI_ENABLE_D3D12) && defined(_WIN32)
        static Result NRI_CALL nv_nriCreateDeviceFromD3D12Device(const DeviceCreationD3D12Desc& deviceDesc, Device*& device)
        {
            using Fn = Result(NRI_CALL*)(const DeviceCreationD3D12Desc&, Device*&);
            static Fn fn = reinterpret_cast<Fn>(detail::loadNriProc("nriCreateDeviceFromD3D12Device"));
            return fn ? fn(deviceDesc, device) : Result::FAILURE;
        }
#endif
    }

    #ifndef NRD_INTEGRATION_ASSERT
        #define NRD_INTEGRATION_ASSERT(expr, msg)                                                   \
            do                                                                                      \
            {                                                                                       \
                if (!(expr))                                                                        \
                    NRI_LOG_ERROR("[NRD] Assert failed: {} ({})", (msg), #expr);                    \
            } while (false)
    #endif

    #include <NRDIntegration.hpp>
#endif

namespace NRI
{
#if defined(NRI_ENABLE_NRD)
    namespace
    {
        void NRI_CALL onNvNriMessage(::nri::Message messageType, const char* file, uint32_t line, const char* message, void*)
        {
            const uint8_t type = static_cast<uint8_t>(messageType);
            const char* safeFile = file ? file : "<unknown>";
            const char* safeMsg = message ? message : "";
            if (type >= 2)
                NRI_LOG_ERROR("[NRD/NRI] {}:{} - {}", safeFile, line, safeMsg);
            else if (type == 1)
                NRI_LOG_WARN("[NRD/NRI] {}:{} - {}", safeFile, line, safeMsg);
            else
                NRI_LOG_INFO("[NRD/NRI] {}:{} - {}", safeFile, line, safeMsg);
        }

        void NRI_CALL onNvNriAbort(void*)
        {
            NRI_LOG_ERROR("[NRD/NRI] AbortExecution triggered (suppressed; returning error to caller).");
        }

        ::nrd::Denoiser toNrdDenoiser(uint32_t method)
        {
            switch (method)
            {
            case 0:  return ::nrd::Denoiser::SIGMA_SHADOW;
            case 1:  return ::nrd::Denoiser::REBLUR_SPECULAR;
            case 2:  return ::nrd::Denoiser::RELAX_SPECULAR;
            case 3:  return ::nrd::Denoiser::REBLUR_DIFFUSE;
            case 4:  return ::nrd::Denoiser::RELAX_DIFFUSE;
            case 5:  return ::nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR;
            case 6:  return ::nrd::Denoiser::RELAX_DIFFUSE_SPECULAR;
            default: return ::nrd::Denoiser::REBLUR_SPECULAR;
            }
        }
    }
#endif

    struct NrdIntegrationHost::Impl
    {
#if defined(NRI_ENABLE_NRD)
        ::nrd::Integration integration;
        std::vector<NrdInstanceSpec> instances;
        std::vector<::nrd::Identifier> clearedDenoisers;
        ::nrd::CommonSettings commonSettings{};
        uint32_t currentFrameIndex = 0xFFFFFFFFu;
        uint32_t nrdFrameIndex = 0xFFFFFFFFu;
#endif
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t queuedFrameNum = 3;
        bool enableWholeLifetimeCaching = false;
        bool initialized = false;
    };

    NrdIntegrationHost::NrdIntegrationHost()
        : m_impl(std::make_unique<Impl>())
    {
    }

    NrdIntegrationHost::~NrdIntegrationHost()
    {
        destroy();
    }

    void NrdIntegrationHost::configureInstances(const NrdInstanceSpec* instances, uint32_t count,
                                                uint32_t queuedFrameNum, bool enableWholeLifetimeCaching)
    {
#if defined(NRI_ENABLE_NRD)
        m_impl->queuedFrameNum = queuedFrameNum > 0 ? queuedFrameNum : 3;
        m_impl->enableWholeLifetimeCaching = enableWholeLifetimeCaching;
        if (instances && count > 0)
        {
            m_impl->instances.assign(instances, instances + count);
        }
        else
        {
            m_impl->instances = {
                { 0u, 0u },
                { 1u, 1u },
                { 2u, 2u },
                { 3u, 3u },
                { 4u, 4u },
                { 5u, 5u },
                { 6u, 6u }
            };
        }
#else
        (void)instances;
        (void)count;
        (void)queuedFrameNum;
        (void)enableWholeLifetimeCaching;
#endif
    }

    void NrdIntegrationHost::destroy()
    {
#if defined(NRI_ENABLE_NRD)
        if (m_impl && m_impl->initialized)
        {
            m_impl->integration.Destroy();
            m_impl->initialized = false;
        }
#endif
    }

    bool NrdIntegrationHost::isInitialized() const { return m_impl->initialized; }
    uint32_t NrdIntegrationHost::width() const { return m_impl->width; }
    uint32_t NrdIntegrationHost::height() const { return m_impl->height; }

    size_t NrdIntegrationHost::instanceCount() const
    {
#if defined(NRI_ENABLE_NRD)
        return m_impl->instances.size();
#else
        return 0;
#endif
    }

    bool NrdIntegrationHost::recreateVK(uint32_t width, uint32_t height, const NrdVulkanInitDesc& vkInit, uint32_t& outErrorCode)
    {
#if defined(NRI_ENABLE_NRD) && defined(NRI_ENABLE_VULKAN)
        if (width == 0 || height == 0)
            return false;

        m_impl->width = width;
        m_impl->height = height;
        m_impl->currentFrameIndex = 0xFFFFFFFFu;
        m_impl->nrdFrameIndex = 0xFFFFFFFFu;
        m_impl->clearedDenoisers.clear();

        ::nrd::IntegrationCreationDesc integrationDesc{};
        std::snprintf(integrationDesc.name, sizeof(integrationDesc.name), "NoxRHI-NRD");
        integrationDesc.residencyPriority = 0.0f;
        integrationDesc.resourceWidth = static_cast<uint16_t>(width);
        integrationDesc.resourceHeight = static_cast<uint16_t>(height);
        integrationDesc.queuedFrameNum = static_cast<uint8_t>(std::clamp(m_impl->queuedFrameNum, 1u, 3u));
        integrationDesc.enableWholeLifetimeDescriptorCaching = m_impl->enableWholeLifetimeCaching;
        integrationDesc.autoWaitForIdle = true;

        std::vector<::nrd::DenoiserDesc> nrdDescs(m_impl->instances.size());
        for (size_t i = 0; i < m_impl->instances.size(); ++i)
        {
            nrdDescs[i].identifier = m_impl->instances[i].identifier;
            nrdDescs[i].denoiser = toNrdDenoiser(m_impl->instances[i].method);
        }

        ::nrd::InstanceCreationDesc instanceDesc{};
        instanceDesc.denoisers = nrdDescs.data();
        instanceDesc.denoisersNum = static_cast<uint32_t>(nrdDescs.size());

        ::nri::QueueFamilyVKDesc queueFamilies[2]{};
        queueFamilies[0].familyIndex = vkInit.graphicsQueueFamily;
        queueFamilies[0].queueType = ::nri::QueueType::GRAPHICS;
        queueFamilies[0].queueNum = 1;
        uint32_t queueFamilyCount = 1;
        if (vkInit.hasAsyncCompute)
        {
            queueFamilies[1].familyIndex = vkInit.computeQueueFamily;
            queueFamilies[1].queueType = ::nri::QueueType::COMPUTE;
            queueFamilies[1].queueNum = 1;
            queueFamilyCount = 2;
        }

        ::nri::DeviceCreationVKDesc vkDesc{};
        vkDesc.vkInstance = static_cast<VKHandle>(vkInit.vkInstance);
        vkDesc.vkDevice = static_cast<VKHandle>(vkInit.vkDevice);
        vkDesc.vkPhysicalDevice = static_cast<VKHandle>(vkInit.vkPhysicalDevice);
        vkDesc.queueFamilies = queueFamilies;
        vkDesc.queueFamilyNum = queueFamilyCount;
        vkDesc.minorVersion = 4;
        vkDesc.callbackInterface.MessageCallback = &onNvNriMessage;
        vkDesc.callbackInterface.AbortExecution = &onNvNriAbort;

        const ::nrd::Result res = m_impl->integration.RecreateVK(integrationDesc, instanceDesc, vkDesc);
        outErrorCode = static_cast<uint32_t>(res);
        if (res != ::nrd::Result::SUCCESS)
            return false;

        m_impl->initialized = true;
        return true;
#else
        (void)width;
        (void)height;
        (void)vkInit;
        outErrorCode = 1;
        return false;
#endif
    }

    bool NrdIntegrationHost::recreateD3D12(uint32_t width, uint32_t height, const NrdD3D12InitDesc& d3dInit, uint32_t& outErrorCode)
    {
#if defined(NRI_ENABLE_NRD) && defined(NRI_ENABLE_D3D12) && defined(_WIN32)
        if (width == 0 || height == 0)
            return false;

        m_impl->width = width;
        m_impl->height = height;
        m_impl->currentFrameIndex = 0xFFFFFFFFu;
        m_impl->nrdFrameIndex = 0xFFFFFFFFu;
        m_impl->clearedDenoisers.clear();

        ::nrd::IntegrationCreationDesc integrationDesc{};
        std::snprintf(integrationDesc.name, sizeof(integrationDesc.name), "NoxRHI-NRD");
        integrationDesc.residencyPriority = 0.0f;
        integrationDesc.resourceWidth = static_cast<uint16_t>(width);
        integrationDesc.resourceHeight = static_cast<uint16_t>(height);
        integrationDesc.queuedFrameNum = static_cast<uint8_t>(std::clamp(m_impl->queuedFrameNum, 1u, 3u));
        integrationDesc.enableWholeLifetimeDescriptorCaching = m_impl->enableWholeLifetimeCaching;
        integrationDesc.autoWaitForIdle = true;

        std::vector<::nrd::DenoiserDesc> nrdDescs(m_impl->instances.size());
        for (size_t i = 0; i < m_impl->instances.size(); ++i)
        {
            nrdDescs[i].identifier = m_impl->instances[i].identifier;
            nrdDescs[i].denoiser = toNrdDenoiser(m_impl->instances[i].method);
        }

        ::nrd::InstanceCreationDesc instanceDesc{};
        instanceDesc.denoisers = nrdDescs.data();
        instanceDesc.denoisersNum = static_cast<uint32_t>(nrdDescs.size());

        ID3D12CommandQueue* graphicsQueues[] = { static_cast<ID3D12CommandQueue*>(d3dInit.graphicsQueue) };
        ID3D12CommandQueue* computeQueues[] = { static_cast<ID3D12CommandQueue*>(d3dInit.computeQueue) };

        ::nri::QueueFamilyD3D12Desc queueFamilies[2]{};
        queueFamilies[0].d3d12Queues = graphicsQueues;
        queueFamilies[0].queueNum = 1;
        queueFamilies[0].queueType = ::nri::QueueType::GRAPHICS;
        uint32_t queueFamilyCount = 1;
        if (d3dInit.hasAsyncCompute && d3dInit.computeQueue != d3dInit.graphicsQueue)
        {
            queueFamilies[1].d3d12Queues = computeQueues;
            queueFamilies[1].queueNum = 1;
            queueFamilies[1].queueType = ::nri::QueueType::COMPUTE;
            queueFamilyCount = 2;
        }

        ::nri::DeviceCreationD3D12Desc d3dDesc{};
        d3dDesc.d3d12Device = static_cast<ID3D12Device*>(d3dInit.d3d12Device);
        d3dDesc.queueFamilies = queueFamilies;
        d3dDesc.queueFamilyNum = queueFamilyCount;
        d3dDesc.disableD3D12EnhancedBarriers = true;
        d3dDesc.disableNVAPIInitialization = true;
        d3dDesc.callbackInterface.MessageCallback = &onNvNriMessage;
        d3dDesc.callbackInterface.AbortExecution = &onNvNriAbort;

        if (d3dDesc.d3d12Device)
        {
            const HRESULT removedReason = d3dDesc.d3d12Device->GetDeviceRemovedReason();
            if (FAILED(removedReason))
                NRI_LOG_ERROR("[NRD] D3D12 device already removed before RecreateD3D12: 0x{:08X}", static_cast<uint32_t>(removedReason));
        }

        const ::nrd::Result res = m_impl->integration.RecreateD3D12(integrationDesc, instanceDesc, d3dDesc);
        outErrorCode = static_cast<uint32_t>(res);
        if (res != ::nrd::Result::SUCCESS)
            return false;

        m_impl->initialized = true;
        return true;
#else
        (void)width;
        (void)height;
        (void)d3dInit;
        outErrorCode = 1;
        return false;
#endif
    }

    bool NrdIntegrationHost::beginFrame(const NrdCommonSettingsSpec& settings)
    {
#if defined(NRI_ENABLE_NRD)
        if (!m_impl->initialized)
            return false;

        if (m_impl->currentFrameIndex == settings.frameIndex)
            return true;

        m_impl->integration.NewFrame();
        m_impl->currentFrameIndex = settings.frameIndex;
        m_impl->nrdFrameIndex = (m_impl->nrdFrameIndex == 0xFFFFFFFFu) ? 0u : (m_impl->nrdFrameIndex + 1u);

        ::nrd::CommonSettings nrdCommon{};
        std::memcpy(nrdCommon.viewToClipMatrix, settings.proj, sizeof(float) * 16);
        std::memcpy(nrdCommon.viewToClipMatrixPrev, settings.prevProj, sizeof(float) * 16);
        std::memcpy(nrdCommon.worldToViewMatrix, settings.view, sizeof(float) * 16);
        std::memcpy(nrdCommon.worldToViewMatrixPrev, settings.prevView, sizeof(float) * 16);

        nrdCommon.motionVectorScale[0] = settings.motionVectorScale[0] != 0.0f ? settings.motionVectorScale[0] : 1.0f;
        nrdCommon.motionVectorScale[1] = settings.motionVectorScale[1] != 0.0f ? settings.motionVectorScale[1] : 1.0f;
        nrdCommon.motionVectorScale[2] = settings.motionVectorScale[2];
        nrdCommon.cameraJitter[0] = settings.cameraJitter[0];
        nrdCommon.cameraJitter[1] = settings.cameraJitter[1];
        nrdCommon.cameraJitterPrev[0] = settings.cameraJitterPrev[0];
        nrdCommon.cameraJitterPrev[1] = settings.cameraJitterPrev[1];

        nrdCommon.resourceSize[0] = static_cast<uint16_t>(m_impl->width);
        nrdCommon.resourceSize[1] = static_cast<uint16_t>(m_impl->height);
        nrdCommon.resourceSizePrev[0] = static_cast<uint16_t>(m_impl->width);
        nrdCommon.resourceSizePrev[1] = static_cast<uint16_t>(m_impl->height);
        nrdCommon.rectSize[0] = static_cast<uint16_t>(m_impl->width);
        nrdCommon.rectSize[1] = static_cast<uint16_t>(m_impl->height);
        nrdCommon.rectSizePrev[0] = static_cast<uint16_t>(m_impl->width);
        nrdCommon.rectSizePrev[1] = static_cast<uint16_t>(m_impl->height);

        nrdCommon.denoisingRange = settings.denoisingRange > 0.0f ? settings.denoisingRange : 5000.0f;
        nrdCommon.frameIndex = m_impl->nrdFrameIndex;
        nrdCommon.accumulationMode = settings.resetHistory
            ? ::nrd::AccumulationMode::RESTART
            : ::nrd::AccumulationMode::CONTINUE;

        m_impl->commonSettings = nrdCommon;
        m_impl->integration.SetCommonSettings(nrdCommon);
        return true;
#else
        (void)settings;
        return false;
#endif
    }

    void NrdIntegrationHost::setSigmaSettings(uint32_t identifier, const NrdSigmaSettingsSpec& settings)
    {
#if defined(NRI_ENABLE_NRD)
        if (!m_impl->initialized)
            return;
        ::nrd::SigmaSettings s{};
        s.lightDirection[0] = settings.lightDirection[0];
        s.lightDirection[1] = settings.lightDirection[1];
        s.lightDirection[2] = settings.lightDirection[2];
        s.planeDistanceSensitivity = settings.planeDistanceSensitivity > 0.0f ? settings.planeDistanceSensitivity : 0.02f;
        s.maxStabilizedFrameNum = settings.maxStabilizedFrameNum > 0 ? settings.maxStabilizedFrameNum : 5u;
        m_impl->integration.SetDenoiserSettings(identifier, &s);
#else
        (void)identifier;
        (void)settings;
#endif
    }

    void NrdIntegrationHost::setReblurSettings(uint32_t identifier, const NrdReblurSettingsSpec& settings)
    {
#if defined(NRI_ENABLE_NRD)
        if (!m_impl->initialized)
            return;
        ::nrd::ReblurSettings s{};
        s.hitDistanceParameters.A = settings.hitDistanceA > 0.0f ? settings.hitDistanceA : 3.0f;
        s.hitDistanceParameters.B = settings.hitDistanceB > 0.0f ? settings.hitDistanceB : 0.1f;
        s.hitDistanceParameters.C = settings.hitDistanceC >= 1.0f ? settings.hitDistanceC : 20.0f;
        (void)settings.hitDistanceD;
        s.enableAntiFirefly = !settings.disableAntiFirefly;
        s.hitDistanceReconstructionMode = settings.disableArea5x5HitDistanceReconstruction
            ? ::nrd::HitDistanceReconstructionMode::OFF
            : ::nrd::HitDistanceReconstructionMode::AREA_5X5;
        s.maxAccumulatedFrameNum = settings.maxAccumulatedFrameNum > 0 ? settings.maxAccumulatedFrameNum : 50u;
        s.diffusePrepassBlurRadius = settings.diffusePrepassBlurRadius > 0.0f ? settings.diffusePrepassBlurRadius : 15.0f;
        s.specularPrepassBlurRadius = settings.specularPrepassBlurRadius > 0.0f ? settings.specularPrepassBlurRadius : 40.0f;
        s.planeDistanceSensitivity = settings.planeDistanceSensitivity > 0.0f ? settings.planeDistanceSensitivity : 0.02f;
        m_impl->integration.SetDenoiserSettings(identifier, &s);
#else
        (void)identifier;
        (void)settings;
#endif
    }

    void NrdIntegrationHost::setRelaxSettings(uint32_t identifier, const NrdRelaxSettingsSpec& settings)
    {
#if defined(NRI_ENABLE_NRD)
        if (!m_impl->initialized)
            return;
        ::nrd::RelaxSettings s{};
        s.diffusePrepassBlurRadius = settings.diffusePrepassBlurRadius > 0.0f ? settings.diffusePrepassBlurRadius : 15.0f;
        s.specularPrepassBlurRadius = settings.specularPrepassBlurRadius > 0.0f ? settings.specularPrepassBlurRadius : 30.0f;
        s.diffuseMaxAccumulatedFrameNum = settings.diffuseMaxAccumulatedFrameNum > 0 ? settings.diffuseMaxAccumulatedFrameNum : 30u;
        s.specularMaxAccumulatedFrameNum = settings.specularMaxAccumulatedFrameNum > 0 ? settings.specularMaxAccumulatedFrameNum : 30u;
        s.enableAntiFirefly = !settings.disableAntiFirefly;
        m_impl->integration.SetDenoiserSettings(identifier, &s);
#else
        (void)identifier;
        (void)settings;
#endif
    }

    bool NrdIntegrationHost::denoiseVK(const uint32_t* identifiers, uint32_t count, const NrdNativeDispatchDesc& desc)
    {
#if defined(NRI_ENABLE_NRD) && defined(NRI_ENABLE_VULKAN)
        if (!m_impl->initialized || !identifiers || count == 0 || !desc.vkCommandBuffer)
            return false;

        const ::nri::StageBits stages = desc.vkComputeQueue
            ? ::nri::StageBits::COMPUTE_SHADER
            : ::nri::StageBits::COMPUTE_SHADER | ::nri::StageBits::FRAGMENT_SHADER | ::nri::StageBits::COLOR_ATTACHMENT;

        ::nrd::ResourceSnapshot snapshot{};
        snapshot.restoreInitialState = desc.restoreInitialState;

        auto bindVK = [&](::nrd::ResourceType slot, const NrdNativeTextureDesc& tex)
        {
            if (tex.vkImage == 0)
                return;
            ::nrd::Resource res{};
            res.vk.image = static_cast<VKNonDispatchableHandle>(tex.vkImage);
            res.vk.format = static_cast<VKEnum>(tex.format);
            res.state.access = ::nri::AccessBits::SHADER_RESOURCE_STORAGE;
            res.state.layout = ::nri::Layout::GENERAL;
            res.state.stages = stages;
            snapshot.SetResource(slot, res);
        };

        bindVK(::nrd::ResourceType::IN_MV, desc.inMotionVectors);
        bindVK(::nrd::ResourceType::IN_NORMAL_ROUGHNESS, desc.inNormalRoughness);
        bindVK(::nrd::ResourceType::IN_VIEWZ, desc.inViewZ);
        bindVK(::nrd::ResourceType::IN_PENUMBRA, desc.inPenumbra);
        bindVK(::nrd::ResourceType::OUT_SHADOW_TRANSLUCENCY, desc.outShadowTranslucent);
        bindVK(::nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST, desc.inSpecRadianceHitDist);
        bindVK(::nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST, desc.outSpecRadianceHitDist);
        bindVK(::nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST, desc.inDiffRadianceHitDist);
        bindVK(::nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST, desc.outDiffRadianceHitDist);

        ::nri::CommandBufferVKDesc cmdDesc{};
        cmdDesc.vkCommandBuffer = static_cast<VKHandle>(desc.vkCommandBuffer);
        cmdDesc.queueType = desc.vkComputeQueue ? ::nri::QueueType::COMPUTE : ::nri::QueueType::GRAPHICS;

        for (uint32_t i = 0; i < count; ++i)
        {
            const ::nrd::Identifier id = identifiers[i];
            const bool firstDispatch = std::find(m_impl->clearedDenoisers.begin(), m_impl->clearedDenoisers.end(), id) == m_impl->clearedDenoisers.end();
            if (firstDispatch)
            {
                ::nrd::CommonSettings clearing = m_impl->commonSettings;
                clearing.accumulationMode = ::nrd::AccumulationMode::CLEAR_AND_RESTART;
                m_impl->integration.SetCommonSettings(clearing);
                m_impl->clearedDenoisers.push_back(id);
            }

            m_impl->integration.DenoiseVK(&id, 1, cmdDesc, snapshot);

            if (firstDispatch)
                m_impl->integration.SetCommonSettings(m_impl->commonSettings);
        }
        return true;
#else
        (void)identifiers;
        (void)count;
        (void)desc;
        return false;
#endif
    }

    bool NrdIntegrationHost::denoiseD3D12(const uint32_t* identifiers, uint32_t count, const NrdNativeDispatchDesc& desc)
    {
#if defined(NRI_ENABLE_NRD) && defined(NRI_ENABLE_D3D12) && defined(_WIN32)
        if (!m_impl->initialized || !identifiers || count == 0 || !desc.d3d12CommandList)
            return false;

        ::nrd::ResourceSnapshot snapshot{};
        snapshot.restoreInitialState = desc.restoreInitialState;

        auto bindD3D12 = [&](::nrd::ResourceType slot, const NrdNativeTextureDesc& tex, bool isOutput)
        {
            if (!tex.d3d12Resource)
                return;
            ::nrd::Resource res{};
            res.d3d12.resource = static_cast<ID3D12Resource*>(tex.d3d12Resource);
            res.d3d12.format = static_cast<DXGIFormat>(tex.format);
            if (isOutput)
            {
                res.state.access = ::nri::AccessBits::SHADER_RESOURCE_STORAGE;
                res.state.layout = ::nri::Layout::SHADER_RESOURCE_STORAGE;
                res.state.stages = ::nri::StageBits::COMPUTE_SHADER;
            }
            else
            {
                res.state.access = ::nri::AccessBits::SHADER_RESOURCE;
                res.state.layout = ::nri::Layout::SHADER_RESOURCE;
                res.state.stages = ::nri::StageBits::COMPUTE_SHADER;
            }
            snapshot.SetResource(slot, res);
        };

        bindD3D12(::nrd::ResourceType::IN_MV, desc.inMotionVectors, false);
        bindD3D12(::nrd::ResourceType::IN_NORMAL_ROUGHNESS, desc.inNormalRoughness, false);
        bindD3D12(::nrd::ResourceType::IN_VIEWZ, desc.inViewZ, false);
        bindD3D12(::nrd::ResourceType::IN_PENUMBRA, desc.inPenumbra, false);
        bindD3D12(::nrd::ResourceType::OUT_SHADOW_TRANSLUCENCY, desc.outShadowTranslucent, true);
        bindD3D12(::nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST, desc.inSpecRadianceHitDist, false);
        bindD3D12(::nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST, desc.outSpecRadianceHitDist, true);
        bindD3D12(::nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST, desc.inDiffRadianceHitDist, false);
        bindD3D12(::nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST, desc.outDiffRadianceHitDist, true);

        ::nri::CommandBufferD3D12Desc cmdDesc{};
        cmdDesc.d3d12CommandList = static_cast<ID3D12GraphicsCommandList*>(desc.d3d12CommandList);

        for (uint32_t i = 0; i < count; ++i)
        {
            const ::nrd::Identifier id = identifiers[i];
            const bool firstDispatch = std::find(m_impl->clearedDenoisers.begin(), m_impl->clearedDenoisers.end(), id) == m_impl->clearedDenoisers.end();
            if (firstDispatch)
            {
                ::nrd::CommonSettings clearing = m_impl->commonSettings;
                clearing.accumulationMode = ::nrd::AccumulationMode::CLEAR_AND_RESTART;
                m_impl->integration.SetCommonSettings(clearing);
                m_impl->clearedDenoisers.push_back(id);
            }

            m_impl->integration.DenoiseD3D12(&id, 1, cmdDesc, snapshot);

            if (firstDispatch)
                m_impl->integration.SetCommonSettings(m_impl->commonSettings);
        }
        return true;
#else
        (void)identifiers;
        (void)count;
        (void)desc;
        return false;
#endif
    }
}
