#include "StreamlineContext.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string_view>

#include "../../Core/Log.h"
#include "../../Core/Device.h"
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

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

#if defined(NRI_ENABLE_STREAMLINE)
    #include <sl.h>
    #include <sl_consts.h>
    #include <sl_dlss.h>
    #include <sl_dlss_d.h>
    #if defined(NRI_ENABLE_VULKAN)
        #include <sl_helpers_vk.h>
    #endif
#endif

namespace NRI
{
    namespace
    {
#if defined(NRI_ENABLE_STREAMLINE)
        constexpr uint32_t kDefaultStreamlineAppId = 231313u;
        constexpr const char* kDefaultStreamlineProjectId = "09481010-0ae3-4697-92e9-7ba7e9ee7ead";

        sl::DLSSMode toSlDlssMode(NriUpscalerMode mode)
        {
            switch (mode)
            {
            case NRI_UPSCALER_MODE_OFF:               return sl::DLSSMode::eOff;
            case NRI_UPSCALER_MODE_DLAA:              return sl::DLSSMode::eDLAA;
            case NRI_UPSCALER_MODE_QUALITY:           return sl::DLSSMode::eMaxQuality;
            case NRI_UPSCALER_MODE_BALANCED:          return sl::DLSSMode::eBalanced;
            case NRI_UPSCALER_MODE_PERFORMANCE:       return sl::DLSSMode::eMaxPerformance;
            case NRI_UPSCALER_MODE_ULTRA_PERFORMANCE: return sl::DLSSMode::eUltraPerformance;
            default:                                  return sl::DLSSMode::eDLAA;
            }
        }

        // Column-major 4x4 matrix helpers (avoid pulling glm into NoxRHI core)
        bool isFiniteMatrix(const float m[16])
        {
            for (int i = 0; i < 16; ++i)
            {
                if (!std::isfinite(m[i]))
                    return false;
            }
            return true;
        }

        bool isFiniteVec3(const float v[3])
        {
            return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
        }

        float dot3(const float a[3], const float b[3])
        {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        }

        void normalize3(const float v[3], float out[3])
        {
            const float len = std::sqrt(dot3(v, v));
            if (len > 1e-12f)
            {
                out[0] = v[0] / len;
                out[1] = v[1] / len;
                out[2] = v[2] / len;
            }
            else
            {
                out[0] = out[1] = out[2] = 0.0f;
            }
        }

        void cross3(const float a[3], const float b[3], float out[3])
        {
            out[0] = a[1] * b[2] - a[2] * b[1];
            out[1] = a[2] * b[0] - a[0] * b[2];
            out[2] = a[0] * b[1] - a[1] * b[0];
        }

        void mulMat4(const float a[16], const float b[16], float out[16])
        {
            for (int c = 0; c < 4; ++c)
            {
                for (int r = 0; r < 4; ++r)
                {
                    out[c * 4 + r] =
                        a[0 * 4 + r] * b[c * 4 + 0] +
                        a[1 * 4 + r] * b[c * 4 + 1] +
                        a[2 * 4 + r] * b[c * 4 + 2] +
                        a[3 * 4 + r] * b[c * 4 + 3];
                }
            }
        }

        bool invertMat4(const float m[16], float out[16], float* outDet = nullptr)
        {
            float inv[16]{};
            inv[0] = m[5]  * m[10] * m[15] - m[5]  * m[11] * m[14] - m[9]  * m[6]  * m[15] +
                     m[9]  * m[7]  * m[14] + m[13] * m[6]  * m[11] - m[13] * m[7]  * m[10];
            inv[4] = -m[4]  * m[10] * m[15] + m[4]  * m[11] * m[14] + m[8]  * m[6]  * m[15] -
                     m[8]  * m[7]  * m[14] - m[12] * m[6]  * m[11] + m[12] * m[7]  * m[10];
            inv[8] = m[4]  * m[9] * m[15] - m[4]  * m[11] * m[13] - m[8]  * m[5] * m[15] +
                     m[8]  * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
            inv[12] = -m[4]  * m[9] * m[14] + m[4]  * m[10] * m[13] + m[8]  * m[5] * m[14] -
                      m[8]  * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
            inv[1] = -m[1]  * m[10] * m[15] + m[1]  * m[11] * m[14] + m[9]  * m[2] * m[15] -
                     m[9]  * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
            inv[5] = m[0]  * m[10] * m[15] - m[0]  * m[11] * m[14] - m[8]  * m[2] * m[15] +
                     m[8]  * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
            inv[9] = -m[0]  * m[9] * m[15] + m[0]  * m[11] * m[13] + m[8]  * m[1] * m[15] -
                     m[8]  * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
            inv[13] = m[0]  * m[9] * m[14] - m[0]  * m[10] * m[13] - m[8]  * m[1] * m[14] +
                      m[8]  * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
            inv[2] = m[1]  * m[6] * m[15] - m[1]  * m[7] * m[14] - m[5]  * m[2] * m[15] +
                     m[5]  * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
            inv[6] = -m[0]  * m[6] * m[15] + m[0]  * m[7] * m[14] + m[4]  * m[2] * m[15] -
                     m[4]  * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
            inv[10] = m[0]  * m[5] * m[15] - m[0]  * m[7] * m[13] - m[4]  * m[1] * m[15] +
                      m[4]  * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
            inv[14] = -m[0]  * m[5] * m[14] + m[0]  * m[6] * m[13] + m[4]  * m[1] * m[14] -
                      m[4]  * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
            inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
                     m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
            inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
                     m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
            inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
                      m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
            inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
                      m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

            const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
            if (outDet)
                *outDet = det;
            if (std::abs(det) <= 1e-24f || !std::isfinite(det))
                return false;

            const float invDet = 1.0f / det;
            for (int i = 0; i < 16; ++i)
                out[i] = inv[i] * invDet;
            return true;
        }

        sl::float4x4 toSlTransposedMat4(const float colMajor[16])
        {
            sl::float4x4 out{};
            for (int row = 0; row < 4; ++row)
            {
                out[row] = sl::float4{
                    colMajor[0 * 4 + row],
                    colMajor[1 * 4 + row],
                    colMajor[2 * 4 + row],
                    colMajor[3 * 4 + row]
                };
            }
            return out;
        }

        void logStreamlineMessage(sl::LogType type, const char* msg)
        {
            if (!msg)
                return;
            if (type == sl::LogType::eError)
            {
                NRI_LOG_ERROR("[Streamline] {}", msg);
            }
            else if (type == sl::LogType::eWarn)
            {
                std::string_view sv(msg);
                if (sv.find("before slInit()") != std::string_view::npos)
                    return;
                NRI_LOG_WARN("[Streamline] {}", msg);
            }
            else if (type == sl::LogType::eInfo)
            {
                std::string_view sv(msg);
                if (sv.find("PerfSection") != std::string_view::npos)
                    return;
                if (sv.find("DLSS") != std::string_view::npos ||
                    sv.find("NGX") != std::string_view::npos ||
                    sv.find("extents") != std::string_view::npos ||
                    sv.find("optimal") != std::string_view::npos)
                {
                    NRI_LOG_INFO("[Streamline] {}", msg);
                }
            }
        }
#endif
    }

    struct StreamlineContext::Impl
    {
        GraphicsAPI api = GraphicsAPI::Vulkan;
        bool dlssSupported = false;
        bool dlssRRSupported = false;
        bool dlssContextEvaluated = false;
        bool dlssEvaluated = false;   // (DLSS-SR evaluated since the last reset: its resources exist)
        bool dlssRREvaluated = false; // (DLSS-RR likewise)
        uint32_t frameIndex = 0;

#if defined(NRI_ENABLE_VULKAN)
        bool dummyDescriptorSetReady = false;
        vk::raii::DescriptorSetLayout dummySetLayout = nullptr;
        vk::raii::DescriptorPool dummyPool = nullptr;
        vk::raii::PipelineLayout dummyPipelineLayout = nullptr;
        vk::DescriptorSet dummySet = {};

        void ensureDummyDescriptorSet(vk::raii::Device& vkDevice)
        {
            if (dummyDescriptorSetReady)
                return;
            vk::DescriptorSetLayoutCreateInfo layoutInfo{ .bindingCount = 0, .pBindings = nullptr };
            dummySetLayout = vk::raii::DescriptorSetLayout(vkDevice, layoutInfo);
            vk::DescriptorPoolCreateInfo poolInfo{ .maxSets = 1, .poolSizeCount = 0, .pPoolSizes = nullptr };
            dummyPool = vk::raii::DescriptorPool(vkDevice, poolInfo);
            vk::DescriptorSetAllocateInfo allocInfo{
                .descriptorPool = *dummyPool,
                .descriptorSetCount = 1,
                .pSetLayouts = &*dummySetLayout
            };
            std::vector<vk::DescriptorSet> sets = (*vkDevice).allocateDescriptorSets(allocInfo);
            dummySet = sets.front();
            vk::PipelineLayoutCreateInfo pipelineLayoutInfo{
                .setLayoutCount = 1,
                .pSetLayouts = &*dummySetLayout
            };
            dummyPipelineLayout = vk::raii::PipelineLayout(vkDevice, pipelineLayoutInfo);
            dummyDescriptorSetReady = true;
        }
#endif
    };

    StreamlineContext::StreamlineContext()
        : m_impl(std::make_unique<Impl>())
    {
    }

    StreamlineContext::~StreamlineContext()
    {
        shutdown();
    }

    bool StreamlineContext::initBeforeDevice(GraphicsAPI api, const DeviceInitDesc& desc)
    {
#if defined(NRI_ENABLE_STREAMLINE) && defined(_WIN32)
        m_impl->api = api;

        // Check whether sl.interposer.dll is present before invoking delay-loaded slInit
        HMODULE interposer = LoadLibraryA("sl.interposer.dll");
        if (!interposer)
        {
            NRI_LOG_WARN("[Streamline] sl.interposer.dll not found alongside executable; Streamline disabled.");
            m_initialized = false;
            return false;
        }

        sl::Preferences pref{};
        pref.showConsole = false;
        pref.logLevel = sl::LogLevel::eVerbose;
        pref.pathsToPlugins = nullptr;
        pref.numPathsToPlugins = 0;
        pref.applicationId = desc.streamlineAppId != 0 ? desc.streamlineAppId : kDefaultStreamlineAppId;
        pref.engine = sl::EngineType::eCustom;
        pref.engineVersion = "1.0.0";
        pref.projectId = desc.streamlineProjectId.empty() ? kDefaultStreamlineProjectId : desc.streamlineProjectId.c_str();
        pref.renderAPI = (api == GraphicsAPI::D3D12) ? sl::RenderAPI::eD3D12 : sl::RenderAPI::eVulkan;
        pref.flags = sl::PreferenceFlags::eDisableCLStateTracking
                   | sl::PreferenceFlags::eDisableDebugText
                   | sl::PreferenceFlags::eUseManualHooking
                   | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
        pref.logMessageCallback = logStreamlineMessage;

        static const sl::Feature s_features[] = {
            sl::kFeatureDLSS,
            sl::kFeatureDLSS_RR
        };
        pref.featuresToLoad = s_features;
        pref.numFeaturesToLoad = static_cast<uint32_t>(std::size(s_features));

        const sl::Result initRes = slInit(pref, sl::kSDKVersion);
        if (initRes == sl::Result::eOk)
        {
            m_initialized = true;
            NRI_LOG_INFO("[Streamline] Initialized successfully ({}).",
                         api == GraphicsAPI::D3D12 ? "D3D12" : "Vulkan");
            return true;
        }

        NRI_LOG_WARN("[Streamline] slInit failed with result: {}", static_cast<int>(initRes));
        m_initialized = false;
        return false;
#else
        (void)api;
        (void)desc;
        m_initialized = false;
        return false;
#endif
    }

    void StreamlineContext::getRequiredVulkanExtensions(std::vector<std::string>& outInstanceExts,
                                                        std::vector<std::string>& outDeviceExts) const
    {
#if defined(NRI_ENABLE_STREAMLINE)
        if (!m_initialized)
            return;

        auto queryFeature = [&](sl::Feature feature, const char* name)
        {
            sl::FeatureRequirements reqs{};
            const sl::Result res = slGetFeatureRequirements(feature, reqs);
            if (res != sl::Result::eOk)
            {
                NRI_LOG_INFO("[Streamline] slGetFeatureRequirements({}) returned {}", name, static_cast<int>(res));
                return;
            }
            for (uint32_t i = 0; i < reqs.vkNumInstanceExtensions; ++i)
            {
                if (reqs.vkInstanceExtensions[i])
                    outInstanceExts.emplace_back(reqs.vkInstanceExtensions[i]);
            }
            for (uint32_t i = 0; i < reqs.vkNumDeviceExtensions; ++i)
            {
                if (reqs.vkDeviceExtensions[i])
                    outDeviceExts.emplace_back(reqs.vkDeviceExtensions[i]);
            }
        };
        queryFeature(sl::kFeatureDLSS, "DLSS");
        queryFeature(sl::kFeatureDLSS_RR, "DLSS_RR");
#else
        (void)outInstanceExts;
        (void)outDeviceExts;
#endif
    }

    bool StreamlineContext::registerVulkanDevice(void* vkInstance,
                                                 void* vkPhysicalDevice,
                                                 void* vkDevice,
                                                 uint32_t graphicsQueueFamily,
                                                 uint32_t graphicsQueueIndex,
                                                 uint32_t computeQueueFamily,
                                                 uint32_t computeQueueIndex)
    {
#if defined(NRI_ENABLE_STREAMLINE) && defined(NRI_ENABLE_VULKAN)
        if (!m_initialized)
            return false;

        sl::VulkanInfo vkInfo{};
        vkInfo.device = static_cast<VkDevice>(vkDevice);
        vkInfo.instance = static_cast<VkInstance>(vkInstance);
        vkInfo.physicalDevice = static_cast<VkPhysicalDevice>(vkPhysicalDevice);
        vkInfo.graphicsQueueFamily = graphicsQueueFamily;
        vkInfo.graphicsQueueIndex = graphicsQueueIndex;
        vkInfo.computeQueueFamily = computeQueueFamily;
        vkInfo.computeQueueIndex = computeQueueIndex;

        const sl::Result vkRes = slSetVulkanInfo(vkInfo);
        if (vkRes != sl::Result::eOk)
        {
            NRI_LOG_WARN("[Streamline] slSetVulkanInfo failed with result: {}", static_cast<int>(vkRes));
            return false;
        }
        NRI_LOG_INFO("[Streamline] Vulkan device registered successfully.");

#ifdef _WIN32
        // The proxies of the manual-hooking guide's mandatory functions (sl_hooks.h), from the interposer's own vkGet*ProcAddr: they return
        // vulkan-1.dll's functions except for those Streamline intercepts.
        m_vkHooks = {};
        m_vkHooksComplete = false;
        if (HMODULE interposer = GetModuleHandleA("sl.interposer.dll"))
        {
            const auto getDeviceProc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(interposer, "vkGetDeviceProcAddr"));
            const auto getInstanceProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(interposer, "vkGetInstanceProcAddr"));
            if (getDeviceProc && getInstanceProc)
            {
                const VkInstance instance = static_cast<VkInstance>(vkInstance);
                const VkDevice device = static_cast<VkDevice>(vkDevice);
                m_vkHooks.vkCreateWin32SurfaceKHR = reinterpret_cast<void*>(getInstanceProc(instance, "vkCreateWin32SurfaceKHR"));
                m_vkHooks.vkDestroySurfaceKHR = reinterpret_cast<void*>(getInstanceProc(instance, "vkDestroySurfaceKHR"));
                m_vkHooks.vkCreateSwapchainKHR = reinterpret_cast<void*>(getDeviceProc(device, "vkCreateSwapchainKHR"));
                m_vkHooks.vkDestroySwapchainKHR = reinterpret_cast<void*>(getDeviceProc(device, "vkDestroySwapchainKHR"));
                m_vkHooks.vkGetSwapchainImagesKHR = reinterpret_cast<void*>(getDeviceProc(device, "vkGetSwapchainImagesKHR"));
                m_vkHooks.vkAcquireNextImageKHR = reinterpret_cast<void*>(getDeviceProc(device, "vkAcquireNextImageKHR"));
                m_vkHooks.vkQueuePresentKHR = reinterpret_cast<void*>(getDeviceProc(device, "vkQueuePresentKHR"));
                m_vkHooks.vkDeviceWaitIdle = reinterpret_cast<void*>(getDeviceProc(device, "vkDeviceWaitIdle"));
                m_vkHooksComplete = m_vkHooks.vkCreateWin32SurfaceKHR && m_vkHooks.vkDestroySurfaceKHR && m_vkHooks.vkCreateSwapchainKHR &&
                                    m_vkHooks.vkDestroySwapchainKHR && m_vkHooks.vkGetSwapchainImagesKHR && m_vkHooks.vkAcquireNextImageKHR &&
                                    m_vkHooks.vkQueuePresentKHR && m_vkHooks.vkDeviceWaitIdle;
            }
        }
        if (m_vkHooksComplete)
            NRI_LOG_INFO("[Streamline] Vulkan: present, swapchain, surface and device-wait-idle go through Streamline's proxies (manual hooking)");
        else
            NRI_LOG_WARN("[Streamline] Vulkan: Streamline's proxies for its mandatory hooks were not all found; presentCommon() will not run");
#endif

        sl::AdapterInfo adapterInfo{};
        adapterInfo.vkPhysicalDevice = vkPhysicalDevice;

        bool dlssLoaded = false;
        slIsFeatureLoaded(sl::kFeatureDLSS, dlssLoaded);
        const sl::Result dlssRes = slIsFeatureSupported(sl::kFeatureDLSS, adapterInfo);
        m_impl->dlssSupported = (dlssRes == sl::Result::eOk);

        bool dlssRRLoaded = false;
        slIsFeatureLoaded(sl::kFeatureDLSS_RR, dlssRRLoaded);
        const sl::Result dlssRRRes = slIsFeatureSupported(sl::kFeatureDLSS_RR, adapterInfo);
        m_impl->dlssRRSupported = (dlssRRRes == sl::Result::eOk);

        NRI_LOG_INFO("[Streamline] DLSS Super Resolution - Loaded: {}, Supported: {} (code: {})",
                     dlssLoaded, m_impl->dlssSupported, static_cast<int>(dlssRes));
        NRI_LOG_INFO("[Streamline] DLSS Ray Reconstruction - Loaded: {}, Supported: {} (code: {})",
                     dlssRRLoaded, m_impl->dlssRRSupported, static_cast<int>(dlssRRRes));
        return true;
#else
        (void)vkInstance;
        (void)vkPhysicalDevice;
        (void)vkDevice;
        (void)graphicsQueueFamily;
        (void)graphicsQueueIndex;
        (void)computeQueueFamily;
        (void)computeQueueIndex;
        return false;
#endif
    }

    bool StreamlineContext::registerD3D12Device(void* id3d12Device, const uint8_t* adapterLuid, uint32_t luidSize)
    {
#if defined(NRI_ENABLE_STREAMLINE)
        if (!m_initialized || !id3d12Device)
            return false;

        const sl::Result d3dRes = slSetD3DDevice(id3d12Device);
        if (d3dRes != sl::Result::eOk)
        {
            NRI_LOG_WARN("[Streamline] slSetD3DDevice failed with result: {}", static_cast<int>(d3dRes));
            return false;
        }
        NRI_LOG_INFO("[Streamline] D3D12 device registered successfully.");

        sl::AdapterInfo adapterInfo{};
        adapterInfo.deviceLUID = const_cast<uint8_t*>(adapterLuid);
        adapterInfo.deviceLUIDSizeInBytes = luidSize;

        bool dlssLoaded = false;
        slIsFeatureLoaded(sl::kFeatureDLSS, dlssLoaded);
        const sl::Result dlssRes = slIsFeatureSupported(sl::kFeatureDLSS, adapterInfo);
        m_impl->dlssSupported = (dlssRes == sl::Result::eOk);

        bool dlssRRLoaded = false;
        slIsFeatureLoaded(sl::kFeatureDLSS_RR, dlssRRLoaded);
        const sl::Result dlssRRRes = slIsFeatureSupported(sl::kFeatureDLSS_RR, adapterInfo);
        m_impl->dlssRRSupported = (dlssRRRes == sl::Result::eOk);

        NRI_LOG_INFO("[Streamline] DLSS Super Resolution - Loaded: {}, Supported: {} (code: {})",
                     dlssLoaded, m_impl->dlssSupported, static_cast<int>(dlssRes));
        NRI_LOG_INFO("[Streamline] DLSS Ray Reconstruction - Loaded: {}, Supported: {} (code: {})",
                     dlssRRLoaded, m_impl->dlssRRSupported, static_cast<int>(dlssRRRes));
        return true;
#else
        (void)id3d12Device;
        (void)adapterLuid;
        (void)luidSize;
        return false;
#endif
    }

    bool StreamlineContext::upgradeD3D12Interface(void** iface) const
    {
#if defined(NRI_ENABLE_STREAMLINE) && defined(_WIN32)
        if (!m_initialized || !iface || !*iface)
            return false;
        const sl::Result result = slUpgradeInterface(iface);
        if (result != sl::Result::eOk)
        {
            NRI_LOG_WARN("[Streamline] slUpgradeInterface failed: {}", static_cast<int>(result));
            return false;
        }
        return true;
#else
        (void)iface;
        return false;
#endif
    }

    void StreamlineContext::shutdown()
    {
#if defined(NRI_ENABLE_VULKAN)
        if (m_impl)
        {
            m_impl->dummyPipelineLayout = nullptr;
            m_impl->dummyPool = nullptr;
            m_impl->dummySetLayout = nullptr;
            m_impl->dummyDescriptorSetReady = false;
        }
#endif
#if defined(NRI_ENABLE_STREAMLINE)
        if (m_initialized)
        {
            slShutdown();
            m_initialized = false;
        }
#endif
        m_vkHooks = {};
        m_vkHooksComplete = false;
    }

    bool StreamlineContext::isUpscalerSupported(NriUpscalerType type) const
    {
        if (!m_initialized || !m_impl)
            return false;
        return type == NRI_UPSCALER_TYPE_DLSS_RR ? m_impl->dlssRRSupported : m_impl->dlssSupported;
    }

    NriResult StreamlineContext::getOptimalSettings(NriUpscalerType type,
                                                    NriUpscalerMode mode,
                                                    Extent2D outputSize,
                                                    NriUpscalerOptimalSettings& outSettings) const
    {
        outSettings.renderSize = { outputSize.width, outputSize.height };
        outSettings.minRenderSize = { outputSize.width, outputSize.height };
        outSettings.maxRenderSize = { outputSize.width, outputSize.height };
        outSettings.optimalSharpness = 0.0f;

#if defined(NRI_ENABLE_STREAMLINE)
        if (!m_initialized || !m_impl->dlssSupported || mode == NRI_UPSCALER_MODE_OFF ||
            outputSize.width == 0 || outputSize.height == 0)
        {
            return NRI_SUCCESS;
        }

        if (type == NRI_UPSCALER_TYPE_DLSS_RR && m_impl->dlssRRSupported)
        {
            sl::DLSSDOptions opts{};
            opts.mode = toSlDlssMode(mode);
            opts.outputWidth = outputSize.width;
            opts.outputHeight = outputSize.height;
            sl::DLSSDOptimalSettings settings{};
            const sl::Result res = slDLSSDGetOptimalSettings(opts, settings);
            if (res == sl::Result::eOk && settings.optimalRenderWidth > 0 && settings.optimalRenderHeight > 0)
            {
                outSettings.renderSize = { settings.optimalRenderWidth, settings.optimalRenderHeight };
                outSettings.minRenderSize = { settings.renderWidthMin, settings.renderHeightMin };
                outSettings.maxRenderSize = { settings.renderWidthMax, settings.renderHeightMax };
                outSettings.optimalSharpness = settings.optimalSharpness;
                return NRI_SUCCESS;
            }
            NRI_LOG_WARN("[Streamline] slDLSSDGetOptimalSettings failed: {}", static_cast<int>(res));
        }
        else
        {
            sl::DLSSOptions opts{};
            opts.mode = toSlDlssMode(mode);
            opts.outputWidth = outputSize.width;
            opts.outputHeight = outputSize.height;
            sl::DLSSOptimalSettings settings{};
            const sl::Result res = slDLSSGetOptimalSettings(opts, settings);
            if (res == sl::Result::eOk && settings.optimalRenderWidth > 0 && settings.optimalRenderHeight > 0)
            {
                outSettings.renderSize = { settings.optimalRenderWidth, settings.optimalRenderHeight };
                outSettings.minRenderSize = { settings.renderWidthMin, settings.renderHeightMin };
                outSettings.maxRenderSize = { settings.renderWidthMax, settings.renderHeightMax };
                outSettings.optimalSharpness = settings.optimalSharpness;
                return NRI_SUCCESS;
            }
            NRI_LOG_WARN("[Streamline] slDLSSGetOptimalSettings failed: {}", static_cast<int>(res));
        }
        return NRI_SUCCESS;
#else
        (void)type;
        return NRI_ERROR_UNSUPPORTED;
#endif
    }

    void StreamlineContext::resetViewport()
    {
#if defined(NRI_ENABLE_STREAMLINE)
        if (!m_initialized || !m_impl->dlssContextEvaluated)
            return;
        m_impl->dlssContextEvaluated = false;
        // Only the features that were evaluated have resources to free: freeing DLSS-SR after only DLSS-RR ran returned
        // eErrorInvalidParameter (25) at shutdown.
        const sl::ViewportHandle viewport{ 0 };
        const auto release = [&](sl::Feature feature, bool& evaluated, const char* name) {
            if (!evaluated)
                return;
            evaluated = false;
            const sl::Result res = slFreeResources(feature, viewport);
            if (res != sl::Result::eOk)
                NRI_LOG_WARN("[Streamline] slFreeResources({}) failed: {}", name, static_cast<int>(res));
        };
        release(sl::kFeatureDLSS, m_impl->dlssEvaluated, "DLSS");
        release(sl::kFeatureDLSS_RR, m_impl->dlssRREvaluated, "DLSS-RR");
#endif
    }

    bool StreamlineContext::evaluate(Device& device,
                                     CommandBuffer& cmd,
                                     NriUpscalerType type,
                                     const UpscalerTextures& textures,
                                     const NriUpscalerEvaluateDesc& desc)
    {
#if defined(NRI_ENABLE_STREAMLINE)
        if (!m_initialized || !m_impl->dlssSupported)
            return false;

        if (!textures.inputColor || !textures.outputColor || !textures.depth || !textures.motionVectors)
            return false;

        Texture2D* inputTex = textures.inputColor;
        Texture2D* outputTex = textures.outputColor;
        Texture2D* depthTex = textures.depth;
        Texture2D* mvecTex = textures.motionVectors;

        sl::FrameToken* frameToken = nullptr;
        m_impl->frameIndex++;
        const sl::Result tokenRes = slGetNewFrameToken(frameToken, &m_impl->frameIndex);
        if (tokenRes != sl::Result::eOk || !frameToken)
        {
            NRI_LOG_WARN("[Streamline] slGetNewFrameToken failed: {}", static_cast<int>(tokenRes));
            return false;
        }

        const uint32_t outW = outputTex->GetWidth();
        const uint32_t outH = outputTex->GetHeight();
        const float aspect = outH > 0 ? (static_cast<float>(outW) / static_cast<float>(outH)) : 1.777f;

        float invProj[16]{};
        float currVP[16]{};
        float prevVP[16]{};
        float invCurrVP[16]{};
        float clipToPrevClip[16]{};
        float prevClipToClip[16]{};

        mulMat4(desc.nonJitteredProj, desc.view, currVP);
        mulMat4(desc.prevNonJitteredProj, desc.prevView, prevVP);

        float rightN[3]{}, upN[3]{}, fwdN[3]{}, crossRU[3]{};
        normalize3(desc.cameraRight, rightN);
        normalize3(desc.cameraUp, upN);
        normalize3(desc.cameraFwd, fwdN);
        cross3(rightN, upN, crossRU);
        const float frameOrtho = std::abs(dot3(crossRU, fwdN));

        float viewDet = 0.0f;
        float invView[16]{};
        const bool validMatrices =
            isFiniteMatrix(desc.nonJitteredProj) && isFiniteMatrix(desc.view) &&
            isFiniteMatrix(desc.prevNonJitteredProj) && isFiniteMatrix(desc.prevView) &&
            invertMat4(desc.nonJitteredProj, invProj) &&
            invertMat4(desc.view, invView, &viewDet) && std::abs(viewDet) > 1e-12f &&
            invertMat4(currVP, invCurrVP);

        bool validClip = false;
        if (validMatrices)
        {
            mulMat4(prevVP, invCurrVP, clipToPrevClip);
            validClip = invertMat4(clipToPrevClip, prevClipToClip);
        }

        const bool cameraValid = validMatrices && validClip &&
                                 isFiniteVec3(desc.cameraPos) &&
                                 std::isfinite(frameOrtho) && frameOrtho > 0.5f &&
                                 std::isfinite(desc.cameraFovRad) && desc.cameraFovRad > 0.0f &&
                                 std::isfinite(aspect) && aspect > 0.0f;
        if (!cameraValid)
        {
            static std::mutex warnMutex;
            static std::chrono::steady_clock::time_point lastWarn{};
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard lock(warnMutex);
            if (now - lastWarn > std::chrono::seconds(5))
            {
                lastWarn = now;
                NRI_LOG_WARN("[Streamline] DLSS skipped due to invalid camera matrices or basis.");
            }
            return false;
        }

        sl::Constants consts{};
        consts.cameraViewToClip = toSlTransposedMat4(desc.nonJitteredProj);
        consts.clipToCameraView = toSlTransposedMat4(invProj);
        consts.clipToPrevClip = toSlTransposedMat4(clipToPrevClip);
        consts.prevClipToClip = toSlTransposedMat4(prevClipToClip);

        consts.jitterOffset = { desc.jitterOffset[0], desc.jitterOffset[1] };
        const float mvx = desc.motionVectorScale[0] != 0.0f ? desc.motionVectorScale[0] : 1.0f;
        const float mvy = desc.motionVectorScale[1] != 0.0f ? desc.motionVectorScale[1] : 1.0f;
        consts.mvecScale = { mvx, mvy };
        consts.cameraPinholeOffset = { 0.0f, 0.0f };

        consts.cameraPos = { desc.cameraPos[0], desc.cameraPos[1], desc.cameraPos[2] };
        consts.cameraUp = { desc.cameraUp[0], desc.cameraUp[1], desc.cameraUp[2] };
        consts.cameraRight = { desc.cameraRight[0], desc.cameraRight[1], desc.cameraRight[2] };
        consts.cameraFwd = { desc.cameraFwd[0], desc.cameraFwd[1], desc.cameraFwd[2] };

        consts.cameraNear = desc.cameraNear > 0.0f ? desc.cameraNear : 0.1f;
        consts.cameraFar = desc.cameraFar > 0.0f ? desc.cameraFar : 10000.0f;
        consts.cameraFOV = desc.cameraFovRad;
        consts.cameraAspectRatio = aspect;

        consts.depthInverted = sl::Boolean::eTrue;
        consts.cameraMotionIncluded = sl::Boolean::eTrue;
        consts.motionVectors3D = sl::Boolean::eFalse;
        consts.motionVectorsJittered = sl::Boolean::eFalse;
        consts.motionVectorsDilated = sl::Boolean::eFalse;
        consts.orthographicProjection = sl::Boolean::eFalse;
        consts.reset = desc.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;

        const sl::ViewportHandle viewport{ 0 };
        const sl::Result constRes = slSetConstants(consts, *frameToken, viewport);
        if (constRes != sl::Result::eOk)
        {
            NRI_LOG_WARN("[Streamline] slSetConstants failed: {}", static_cast<int>(constRes));
            return false;
        }

        const bool useRayReconstruction = (type == NRI_UPSCALER_TYPE_DLSS_RR) && m_impl->dlssRRSupported;
        if (useRayReconstruction)
        {
            sl::DLSSDOptions opts{};
            opts.mode = toSlDlssMode(desc.mode);
            opts.outputWidth = outW;
            opts.outputHeight = outH;
            opts.colorBuffersHDR = desc.disableHDR ? sl::Boolean::eFalse : sl::Boolean::eTrue;
            opts.preExposure = 1.0f;
            opts.exposureScale = 1.0f;
            opts.normalRoughnessMode = desc.normalRoughnessPacked
                ? sl::DLSSDNormalRoughnessMode::ePacked
                : sl::DLSSDNormalRoughnessMode::eUnpacked;
            opts.alphaUpscalingEnabled = sl::Boolean::eFalse;
            opts.worldToCameraView = toSlTransposedMat4(desc.view);
            opts.cameraViewToWorld = toSlTransposedMat4(invView);
            opts.dlaaPreset = sl::DLSSDPreset::ePresetE;
            opts.qualityPreset = sl::DLSSDPreset::ePresetE;
            opts.balancedPreset = sl::DLSSDPreset::ePresetE;
            opts.performancePreset = sl::DLSSDPreset::ePresetE;
            opts.ultraPerformancePreset = sl::DLSSDPreset::ePresetE;

            const sl::Result optRes = slDLSSDSetOptions(viewport, opts);
            if (optRes != sl::Result::eOk)
            {
                NRI_LOG_WARN("[Streamline] slDLSSDSetOptions failed: {}", static_cast<int>(optRes));
                return false;
            }
        }
        else
        {
            sl::DLSSOptions opts{};
            opts.mode = toSlDlssMode(desc.mode);
            opts.outputWidth = outW;
            opts.outputHeight = outH;
            opts.colorBuffersHDR = desc.disableHDR ? sl::Boolean::eFalse : sl::Boolean::eTrue;
            opts.preExposure = 1.0f;
            opts.exposureScale = 1.0f;
            opts.useAutoExposure = sl::Boolean::eTrue;
            opts.dlaaPreset = sl::DLSSPreset::ePresetK;
            opts.qualityPreset = sl::DLSSPreset::ePresetK;
            opts.balancedPreset = sl::DLSSPreset::ePresetK;
            opts.performancePreset = sl::DLSSPreset::ePresetM;
            opts.ultraPerformancePreset = sl::DLSSPreset::ePresetL;

            const sl::Result optRes = slDLSSSetOptions(viewport, opts);
            if (optRes != sl::Result::eOk)
            {
                NRI_LOG_WARN("[Streamline] slDLSSSetOptions failed: {}", static_cast<int>(optRes));
                return false;
            }
        }

        std::vector<sl::ResourceTag> tags;
        tags.reserve(12);

        const sl::Extent inputExtent{ 0, 0, inputTex->GetWidth(), inputTex->GetHeight() };
        const sl::Extent outputExtent{ 0, 0, outW, outH };
        const sl::Extent depthExtent{ 0, 0, depthTex->GetWidth(), depthTex->GetHeight() };
        const sl::Extent mvecExtent{ 0, 0, mvecTex->GetWidth(), mvecTex->GetHeight() };

        sl::Resource colorIn{}, colorOut{}, depthRes{}, mvecRes{};
        sl::Resource albedoRes{}, specAlbedoRes{}, normalRes{}, roughRes{}, hitDistRes{}, specMvecRes{}, preTransRes{};
        sl::CommandBuffer* slCmd = nullptr;

        if (device.getAPI() == GraphicsAPI::Vulkan)
        {
#if defined(NRI_ENABLE_VULKAN)
            auto& devVK = static_cast<DeviceVK&>(device);
            auto& cmdVK = static_cast<CommandBufferVK&>(cmd);
            VkCommandBuffer vkCmd = static_cast<VkCommandBuffer>(*cmdVK.getActiveNativeBuffer());
            slCmd = reinterpret_cast<sl::CommandBuffer*>(vkCmd);

            auto wrapVK = [](TextureVK* t, uint32_t usageFlags) -> sl::Resource
            {
                sl::Resource r{};
                r.type = sl::ResourceType::eTex2d;
                r.native = static_cast<VkImage>(*t->getNativeImage());
                r.view = static_cast<VkImageView>(*t->getNativeView());
                r.nativeFormat = static_cast<uint32_t>(t->getFormat());
                r.width = t->GetWidth();
                r.height = t->GetHeight();
                r.state = VK_IMAGE_LAYOUT_GENERAL;
                r.mipLevels = 1;
                r.arrayLayers = 1;
                r.usage = usageFlags;
                return r;
            };

            constexpr uint32_t kColorUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
            auto* depthVK = static_cast<TextureVK*>(depthTex);
            const bool nativeDepth = depthVK->getFormat() != vk::Format::eR32Sfloat;
            static sl::SubresourceRange depthRange{};
            if (nativeDepth)
            {
                depthRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                depthRange.baseMipLevel = 0;
                depthRange.levelCount = 1;
                depthRange.baseArrayLayer = 0;
                depthRange.layerCount = 1;
            }

            colorIn = wrapVK(static_cast<TextureVK*>(inputTex), kColorUsage);
            colorOut = wrapVK(static_cast<TextureVK*>(outputTex), kColorUsage);
            depthRes = wrapVK(depthVK, nativeDepth
                                           ? (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
                                           : (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));
            depthRes.next = nativeDepth ? &depthRange : nullptr;
            mvecRes = wrapVK(static_cast<TextureVK*>(mvecTex), kColorUsage);

            tags.push_back(sl::ResourceTag(&colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent));
            tags.push_back(sl::ResourceTag(&colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &outputExtent));
            tags.push_back(sl::ResourceTag(&depthRes, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &depthExtent));
            tags.push_back(sl::ResourceTag(&mvecRes, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate, &mvecExtent));

            if (useRayReconstruction && textures.albedo && textures.normal &&
                (desc.normalRoughnessPacked || textures.roughness))
            {
                albedoRes = wrapVK(static_cast<TextureVK*>(textures.albedo), kColorUsage);
                specAlbedoRes = wrapVK(static_cast<TextureVK*>(textures.specularAlbedo ? textures.specularAlbedo : textures.albedo), kColorUsage);
                normalRes = wrapVK(static_cast<TextureVK*>(textures.normal), kColorUsage);
                tags.push_back(sl::ResourceTag(&albedoRes, sl::kBufferTypeAlbedo, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent));
                tags.push_back(sl::ResourceTag(&specAlbedoRes, sl::kBufferTypeSpecularAlbedo, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent));
                tags.push_back(sl::ResourceTag(&normalRes,
                    desc.normalRoughnessPacked ? sl::kBufferTypeNormalRoughness : sl::kBufferTypeNormals,
                    sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent));
                if (!desc.normalRoughnessPacked && textures.roughness)
                {
                    roughRes = wrapVK(static_cast<TextureVK*>(textures.roughness), kColorUsage);
                    tags.push_back(sl::ResourceTag(&roughRes, sl::kBufferTypeRoughness, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent));
                }
                if (textures.specularHitDistance)
                {
                    hitDistRes = wrapVK(static_cast<TextureVK*>(textures.specularHitDistance), kColorUsage);
                    tags.push_back(sl::ResourceTag(&hitDistRes, sl::kBufferTypeSpecularHitDistance, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent));
                }
                else if (textures.specularMotionVectors)
                {
                    specMvecRes = wrapVK(static_cast<TextureVK*>(textures.specularMotionVectors), kColorUsage);
                    tags.push_back(sl::ResourceTag(&specMvecRes, sl::kBufferTypeSpecularMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent));
                }
                if (textures.colorBeforeTransparency)
                {
                    preTransRes = wrapVK(static_cast<TextureVK*>(textures.colorBeforeTransparency), kColorUsage);
                    tags.push_back(sl::ResourceTag(&preTransRes, sl::kBufferTypeColorBeforeTransparency, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent));
                }
            }

            m_impl->ensureDummyDescriptorSet(devVK.getDevice());
            vk::CommandBuffer(vkCmd).bindDescriptorSets(vk::PipelineBindPoint::eCompute, *m_impl->dummyPipelineLayout, 0, { m_impl->dummySet }, {});
#endif
        }
        else if (device.getAPI() == GraphicsAPI::D3D12)
        {
#if defined(NRI_ENABLE_D3D12) && defined(_WIN32)
            auto& cmdD3D12 = static_cast<CommandBufferD3D12&>(cmd);
            slCmd = reinterpret_cast<sl::CommandBuffer*>(cmdD3D12.getNativeCommandList());

            auto wrapD3D12 = [](TextureD3D12* t) -> sl::Resource
            {
                sl::Resource r{};
                r.type = sl::ResourceType::eTex2d;
                r.native = t->getNativeResource();
                r.nativeFormat = static_cast<uint32_t>(t->getDxgiFormat());
                r.width = t->GetWidth();
                r.height = t->GetHeight();
                r.state = static_cast<uint32_t>(t->getCurrentState());
                r.mipLevels = t->getMipLevels();
                r.arrayLayers = t->getArrayLayers();
                return r;
            };

            colorIn = wrapD3D12(static_cast<TextureD3D12*>(inputTex));
            colorOut = wrapD3D12(static_cast<TextureD3D12*>(outputTex));
            depthRes = wrapD3D12(static_cast<TextureD3D12*>(depthTex));
            mvecRes = wrapD3D12(static_cast<TextureD3D12*>(mvecTex));

            tags.push_back(sl::ResourceTag(&colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent));
            tags.push_back(sl::ResourceTag(&colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &outputExtent));
            tags.push_back(sl::ResourceTag(&depthRes, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &depthExtent));
            tags.push_back(sl::ResourceTag(&mvecRes, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate, &mvecExtent));

            if (useRayReconstruction && textures.albedo && textures.normal &&
                (desc.normalRoughnessPacked || textures.roughness))
            {
                albedoRes = wrapD3D12(static_cast<TextureD3D12*>(textures.albedo));
                specAlbedoRes = wrapD3D12(static_cast<TextureD3D12*>(textures.specularAlbedo ? textures.specularAlbedo : textures.albedo));
                normalRes = wrapD3D12(static_cast<TextureD3D12*>(textures.normal));
                tags.push_back(sl::ResourceTag(&albedoRes, sl::kBufferTypeAlbedo, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent));
                tags.push_back(sl::ResourceTag(&specAlbedoRes, sl::kBufferTypeSpecularAlbedo, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent));
                tags.push_back(sl::ResourceTag(&normalRes,
                    desc.normalRoughnessPacked ? sl::kBufferTypeNormalRoughness : sl::kBufferTypeNormals,
                    sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent));
                if (!desc.normalRoughnessPacked && textures.roughness)
                {
                    roughRes = wrapD3D12(static_cast<TextureD3D12*>(textures.roughness));
                    tags.push_back(sl::ResourceTag(&roughRes, sl::kBufferTypeRoughness, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent));
                }
                if (textures.specularHitDistance)
                {
                    hitDistRes = wrapD3D12(static_cast<TextureD3D12*>(textures.specularHitDistance));
                    tags.push_back(sl::ResourceTag(&hitDistRes, sl::kBufferTypeSpecularHitDistance, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent));
                }
                else if (textures.specularMotionVectors)
                {
                    specMvecRes = wrapD3D12(static_cast<TextureD3D12*>(textures.specularMotionVectors));
                    tags.push_back(sl::ResourceTag(&specMvecRes, sl::kBufferTypeSpecularMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent));
                }
                if (textures.colorBeforeTransparency)
                {
                    preTransRes = wrapD3D12(static_cast<TextureD3D12*>(textures.colorBeforeTransparency));
                    tags.push_back(sl::ResourceTag(&preTransRes, sl::kBufferTypeColorBeforeTransparency, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent));
                }
            }
#endif
        }

        if (!slCmd)
            return false;

        const sl::Result tagRes = slSetTagForFrame(*frameToken, viewport, tags.data(), static_cast<uint32_t>(tags.size()), slCmd);
        if (tagRes != sl::Result::eOk)
        {
            NRI_LOG_WARN("[Streamline] slSetTagForFrame failed: {}", static_cast<int>(tagRes));
            return false;
        }

        const sl::BaseStructure* inputs[] = { &viewport };
        const sl::Feature featureToEval = useRayReconstruction ? sl::kFeatureDLSS_RR : sl::kFeatureDLSS;
        const sl::Result evalRes = slEvaluateFeature(featureToEval, *frameToken, inputs, static_cast<uint32_t>(std::size(inputs)), slCmd);
        // Streamline runs with eUseManualHooking and eDisableCLStateTracking: it restores no command-list state after its dispatches.
        cmd.restoreBindingsAfterExternalWork();
        if (evalRes != sl::Result::eOk)
        {
            NRI_LOG_WARN("[Streamline] slEvaluateFeature failed: {}", static_cast<int>(evalRes));
            return false;
        }

        m_impl->dlssContextEvaluated = true;
        (useRayReconstruction ? m_impl->dlssRREvaluated : m_impl->dlssEvaluated) = true;
        cmd.executionBarrier();
        return true;
#else
        (void)device;
        (void)cmd;
        (void)type;
        (void)desc;
        return false;
#endif
    }
}
