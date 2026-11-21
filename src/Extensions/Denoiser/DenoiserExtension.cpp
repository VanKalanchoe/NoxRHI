#include "DenoiserExtension.h"
#include "NrdIntegrationHost.h"

#include <cstring>
#include <stdexcept>
#include <vector>

#include "../../Core/Log.h"
#include "../../Core/CommandBuffer.h"
#include "../../Core/Texture.h"

#if defined(NRI_ENABLE_VULKAN)
    #include "../../Core/Vulkan/DeviceVK.h"
    #include "../../Core/Vulkan/CommandBufferVK.h"
    #include "../../Core/Vulkan/TextureVK.h"
#endif

#if defined(NRI_ENABLE_D3D12) && defined(_WIN32)
    #include "../../Core/D3D12/DeviceD3D12.h"
    #include "../../Core/D3D12/CommandBufferD3D12.h"
    #include "../../Core/D3D12/TextureD3D12.h"
#endif

namespace NRI
{
    struct DenoiserExtension::Impl
    {
        NrdIntegrationHost host;
    };

    DenoiserExtension::DenoiserExtension(std::shared_ptr<Device> device, const NriDenoiserDesc& desc)
        : m_device(std::move(device)), m_impl(std::make_unique<Impl>())
    {
        if (!m_device)
            throw std::runtime_error("DenoiserExtension requires a valid device");

#if defined(NRI_ENABLE_NRD)
        std::vector<NrdInstanceSpec> specs;
        if (desc.instances && desc.instanceCount > 0)
        {
            specs.reserve(desc.instanceCount);
            for (uint32_t i = 0; i < desc.instanceCount; ++i)
                specs.push_back({ desc.instances[i].identifier, static_cast<uint32_t>(desc.instances[i].method) });
        }

        m_impl->host.configureInstances(
            specs.empty() ? nullptr : specs.data(),
            static_cast<uint32_t>(specs.size()),
            desc.queuedFrameNum,
            desc.enableWholeLifetimeDescriptorCaching);

        if (!recreate(desc.width, desc.height))
            throw std::runtime_error("Failed to initialize NRD denoiser integration");
#else
        (void)desc;
        throw std::runtime_error("NRD extension was not built (NRI_ENABLE_NRD=OFF)");
#endif
    }

    DenoiserExtension::~DenoiserExtension()
    {
        if (m_impl && m_impl->host.isInitialized())
        {
            if (m_device && !m_device->isShuttingDown())
                m_device->waitIdle();
            m_impl->host.destroy();
        }
    }

    bool DenoiserExtension::isAvailable(const Device& device)
    {
#if defined(NRI_ENABLE_NRD)
        return device.getAPI() == GraphicsAPI::Vulkan || device.getAPI() == GraphicsAPI::D3D12;
#else
        (void)device;
        return false;
#endif
    }

    bool DenoiserExtension::recreate(uint32_t width, uint32_t height)
    {
#if defined(NRI_ENABLE_NRD)
        if (width == 0 || height == 0)
            return false;

        if (m_impl->host.isInitialized() && m_impl->host.width() == width && m_impl->host.height() == height)
            return true;

        if (m_impl->host.isInitialized())
        {
            m_device->waitIdle();
            m_impl->host.destroy();
        }

        uint32_t errorCode = 1;
        bool ok = false;
        if (m_device->getAPI() == GraphicsAPI::Vulkan)
        {
#if defined(NRI_ENABLE_VULKAN)
            auto& devVK = static_cast<DeviceVK&>(*m_device);
            NrdVulkanInitDesc vkInit{};
            vkInit.vkInstance = static_cast<void*>(static_cast<VkInstance>(*devVK.getInstance()));
            vkInit.vkPhysicalDevice = static_cast<void*>(static_cast<VkPhysicalDevice>(*devVK.getPhysicalDevice()));
            vkInit.vkDevice = static_cast<void*>(static_cast<VkDevice>(*devVK.getDevice()));
            vkInit.graphicsQueueFamily = devVK.getQueueIndex();
            vkInit.computeQueueFamily = devVK.getComputeQueueIndex();
            vkInit.hasAsyncCompute = devVK.hasAsyncComputeQueue();
            ok = m_impl->host.recreateVK(width, height, vkInit, errorCode);
#endif
        }
        else if (m_device->getAPI() == GraphicsAPI::D3D12)
        {
#if defined(NRI_ENABLE_D3D12) && defined(_WIN32)
            auto& devD3D12 = static_cast<DeviceD3D12&>(*m_device);
            NrdD3D12InitDesc d3dInit{};
            d3dInit.d3d12Device = devD3D12.getNativeDevice();
            d3dInit.graphicsQueue = devD3D12.getGraphicsQueue();
            d3dInit.computeQueue = devD3D12.getComputeQueue();
            d3dInit.hasAsyncCompute = devD3D12.hasAsyncComputeQueue();
            ok = m_impl->host.recreateD3D12(width, height, d3dInit, errorCode);
#endif
        }

        if (!ok)
        {
            NRI_LOG_ERROR("[NRD] Recreate failed (error code {}).", errorCode);
            return false;
        }

        NRI_LOG_INFO("[NRD] Initialized denoiser suite ({}x{}, {} instances).",
                     width, height, m_impl->host.instanceCount());
        return true;
#else
        (void)width;
        (void)height;
        return false;
#endif
    }

    NriResult DenoiserExtension::resize(uint32_t width, uint32_t height)
    {
        return recreate(width, height) ? NRI_SUCCESS : NRI_ERROR_INTERNAL;
    }

    bool DenoiserExtension::beginFrame(const NriDenoiserCommonSettings& settings)
    {
        NrdCommonSettingsSpec spec{};
        std::memcpy(spec.view, settings.view, sizeof(spec.view));
        std::memcpy(spec.proj, settings.proj, sizeof(spec.proj));
        std::memcpy(spec.prevView, settings.prevView, sizeof(spec.prevView));
        std::memcpy(spec.prevProj, settings.prevProj, sizeof(spec.prevProj));
        std::memcpy(spec.motionVectorScale, settings.motionVectorScale, sizeof(spec.motionVectorScale));
        std::memcpy(spec.cameraJitter, settings.cameraJitter, sizeof(spec.cameraJitter));
        std::memcpy(spec.cameraJitterPrev, settings.cameraJitterPrev, sizeof(spec.cameraJitterPrev));
        spec.frameIndex = settings.frameIndex;
        spec.denoisingRange = settings.denoisingRange;
        spec.resetHistory = settings.resetHistory;
        return m_impl->host.beginFrame(spec);
    }

    void DenoiserExtension::setSigmaSettings(uint32_t identifier, const NriDenoiserSigmaSettings& settings)
    {
        NrdSigmaSettingsSpec spec{};
        std::memcpy(spec.lightDirection, settings.lightDirection, sizeof(spec.lightDirection));
        spec.planeDistanceSensitivity = settings.planeDistanceSensitivity;
        spec.maxStabilizedFrameNum = settings.maxStabilizedFrameNum;
        m_impl->host.setSigmaSettings(identifier, spec);
    }

    void DenoiserExtension::setReblurSettings(uint32_t identifier, const NriDenoiserReblurSettings& settings)
    {
        NrdReblurSettingsSpec spec{};
        spec.hitDistanceA = settings.hitDistanceA;
        spec.hitDistanceB = settings.hitDistanceB;
        spec.hitDistanceC = settings.hitDistanceC;
        spec.hitDistanceD = settings.hitDistanceD;
        spec.maxAccumulatedFrameNum = settings.maxAccumulatedFrameNum;
        spec.diffusePrepassBlurRadius = settings.diffusePrepassBlurRadius;
        spec.specularPrepassBlurRadius = settings.specularPrepassBlurRadius;
        spec.planeDistanceSensitivity = settings.planeDistanceSensitivity;
        spec.disableAntiFirefly = settings.disableAntiFirefly;
        spec.disableArea5x5HitDistanceReconstruction = settings.disableArea5x5HitDistanceReconstruction;
        m_impl->host.setReblurSettings(identifier, spec);
    }

    void DenoiserExtension::setRelaxSettings(uint32_t identifier, const NriDenoiserRelaxSettings& settings)
    {
        NrdRelaxSettingsSpec spec{};
        spec.diffusePrepassBlurRadius = settings.diffusePrepassBlurRadius;
        spec.specularPrepassBlurRadius = settings.specularPrepassBlurRadius;
        spec.diffuseMaxAccumulatedFrameNum = settings.diffuseMaxAccumulatedFrameNum;
        spec.specularMaxAccumulatedFrameNum = settings.specularMaxAccumulatedFrameNum;
        spec.disableAntiFirefly = settings.disableAntiFirefly;
        m_impl->host.setRelaxSettings(identifier, spec);
    }

    bool DenoiserExtension::denoise(CommandBuffer& cmd,
                                    const uint32_t* identifiers,
                                    uint32_t identifierCount,
                                    const DenoiserTextures& textures,
                                    bool restoreInitialState)
    {
#if defined(NRI_ENABLE_NRD)
        if (!m_impl->host.isInitialized() || !identifiers || identifierCount == 0)
            return false;

        NrdNativeDispatchDesc dispatch{};
        dispatch.restoreInitialState = restoreInitialState;

        if (m_device->getAPI() == GraphicsAPI::Vulkan)
        {
#if defined(NRI_ENABLE_VULKAN)
            auto& devVK = static_cast<DeviceVK&>(*m_device);
            auto& cmdVK = static_cast<CommandBufferVK&>(cmd);
            dispatch.vkCommandBuffer = static_cast<void*>(static_cast<VkCommandBuffer>(*cmdVK.getActiveNativeBuffer()));
            dispatch.vkComputeQueue = devVK.hasAsyncComputeQueue() &&
                                      cmdVK.getQueueFamily() == devVK.getComputeQueueIndex() &&
                                      devVK.getComputeQueueIndex() != devVK.getQueueIndex();

            auto fillVK = [](NrdNativeTextureDesc& dst, Texture2D* tex)
            {
                if (!tex)
                    return;
                auto* texVK = static_cast<TextureVK*>(tex);
                dst.vkImage = reinterpret_cast<uint64_t>(static_cast<VkImage>(*texVK->getNativeImage()));
                dst.format = static_cast<uint32_t>(static_cast<VkFormat>(texVK->getFormat()));
            };

            fillVK(dispatch.inMotionVectors, textures.inMotionVectors);
            fillVK(dispatch.inNormalRoughness, textures.inNormalRoughness);
            fillVK(dispatch.inViewZ, textures.inViewZ);
            fillVK(dispatch.inPenumbra, textures.inPenumbra);
            fillVK(dispatch.outShadowTranslucent, textures.outShadowTranslucent);
            fillVK(dispatch.inSpecRadianceHitDist, textures.inSpecRadianceHitDist);
            fillVK(dispatch.outSpecRadianceHitDist, textures.outSpecRadianceHitDist);
            fillVK(dispatch.inDiffRadianceHitDist, textures.inDiffRadianceHitDist);
            fillVK(dispatch.outDiffRadianceHitDist, textures.outDiffRadianceHitDist);

            const bool denoised = m_impl->host.denoiseVK(identifiers, identifierCount, dispatch);
            cmd.restoreBindingsAfterExternalWork(); // (NRD bound its own pipelines and descriptor sets)
            return denoised;
#endif
        }
        else if (m_device->getAPI() == GraphicsAPI::D3D12)
        {
#if defined(NRI_ENABLE_D3D12) && defined(_WIN32)
            auto& cmdD3D12 = static_cast<CommandBufferD3D12&>(cmd);
            ID3D12GraphicsCommandList* list = cmdD3D12.getNativeCommandList();
            dispatch.d3d12CommandList = list;

            auto prepD3D12 = [&](NrdNativeTextureDesc& dst, Texture2D* tex, bool isOutput)
            {
                if (!tex)
                    return;
                auto* texD3D12 = static_cast<TextureD3D12*>(tex);
                const D3D12_RESOURCE_STATES targetState = isOutput
                    ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                    : (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

                if (texD3D12->getCurrentState() != targetState)
                {
                    D3D12_RESOURCE_BARRIER b{};
                    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b.Transition.pResource = texD3D12->getNativeResource();
                    b.Transition.StateBefore = texD3D12->getCurrentState();
                    b.Transition.StateAfter = targetState;
                    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    list->ResourceBarrier(1, &b);
                    texD3D12->setCurrentState(targetState);
                }

                dst.d3d12Resource = texD3D12->getNativeResource();
                dst.format = static_cast<uint32_t>(texD3D12->getDxgiFormat());
            };

            prepD3D12(dispatch.inMotionVectors, textures.inMotionVectors, false);
            prepD3D12(dispatch.inNormalRoughness, textures.inNormalRoughness, false);
            prepD3D12(dispatch.inViewZ, textures.inViewZ, false);
            prepD3D12(dispatch.inPenumbra, textures.inPenumbra, false);
            prepD3D12(dispatch.outShadowTranslucent, textures.outShadowTranslucent, true);
            prepD3D12(dispatch.inSpecRadianceHitDist, textures.inSpecRadianceHitDist, false);
            prepD3D12(dispatch.outSpecRadianceHitDist, textures.outSpecRadianceHitDist, true);
            prepD3D12(dispatch.inDiffRadianceHitDist, textures.inDiffRadianceHitDist, false);
            prepD3D12(dispatch.outDiffRadianceHitDist, textures.outDiffRadianceHitDist, true);

            const bool denoised = m_impl->host.denoiseD3D12(identifiers, identifierCount, dispatch);
            cmd.restoreBindingsAfterExternalWork(); // (NRD set its own heaps, root signature and pipelines)
            return denoised;
#endif
        }
        return false;
#else
        (void)cmd;
        (void)identifiers;
        (void)identifierCount;
        (void)textures;
        (void)restoreInitialState;
        return false;
#endif
    }
}
