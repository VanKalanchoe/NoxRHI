#include "DeviceVK.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "../Log.h"
#include "../../Extensions/ShaderCompiler/ShaderCompiler.h"
#include "SwapchainVK.h"
#include "PipelineVK.h"
#include "CommandAllocatorVK.h"
#include "CommandBufferVK.h"
#include "TextureVK.h"
#include "BufferVK.h"
#include "DescriptorHeapVK.h"
#include "AccelerationStructureVK.h"
#include "GpuProfilerVK.h"
#include "MemoryAllocatorVK.h"
#include "TimelineSemaphoreVK.h"
#include "QueryPoolVK.h"

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace NRI
{
    static const std::vector<const char*> kRequiredDeviceExtensions =
    {
        vk::KHRSwapchainExtensionName,
        vk::KHRUnifiedImageLayoutsExtensionName,
        vk::KHRBufferDeviceAddressExtensionName,

        // Descriptorheap + untyped Pointer
        vk::EXTDescriptorHeapExtensionName,
        vk::KHRMaintenance5ExtensionName,
        vk::KHRShaderUntypedPointersExtensionName,
        vk::KHRShaderNonSemanticInfoExtensionName,

        // Shader Objects
        vk::EXTShaderObjectExtensionName,
        vk::EXTExtendedDynamicState3ExtensionName,
        vk::EXTVertexInputDynamicStateExtensionName,

        // Task + Mesh Shader
        vk::EXTMeshShaderExtensionName,

        // Hardware Ray Tracing
        vk::KHRAccelerationStructureExtensionName,
        vk::KHRRayQueryExtensionName,
        vk::KHRDeferredHostOperationsExtensionName,
        vk::KHRRayTracingPipelineExtensionName,
    };

    static const std::vector<const char*> kOptionalDeviceExtensions =
    {
        vk::KHRPushDescriptorExtensionName,
        vk::KHRComputeShaderDerivativesExtensionName,
        "VK_NVX_binary_import",
        "VK_NVX_image_view_handle",
    };

    // Factory
    std::unique_ptr<Swapchain> DeviceVK::createSwapchain(const SwapchainDesc& desc)
    {
        return std::make_unique<SwapchainVK>(*this, desc);
    }

    void DeviceVK::finishPipelineBuilds()
    {
        const auto started = std::chrono::steady_clock::now();
        const PipelineVK::BuildStats stats = PipelineVK::FinishPendingBuilds();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (stats.Parallel + stats.Compiled + stats.Failed > 0)
            NRI_LOG_INFO("Pipelines: {} built in parallel from the cache, {} compiled in parallel, {} failed (waited {:.0f} ms for them)",
                         stats.Parallel, stats.Compiled, stats.Failed, ms);
    }

    std::unique_ptr<Pipeline> DeviceVK::createPipeline(const PipelineDesc& desc, ShaderCompiler& compiler)
    {
        auto pipeline = std::make_unique<PipelineVK>(*this, desc, compiler);
        if (ShaderProgress* progress = GetShaderProgress(); progress && progress->OnPipelineCreated && !pipeline->isBuiltOnPool())
            progress->OnPipelineCreated();
        return pipeline;
    }

    void DeviceVK::precompileShaderModules(const std::vector<std::string>& sources, ShaderCompiler& compiler)
    {
        if (isShaderObjectExtensionEnabled())
            PipelineVK::PrecompileModules(sources, compiler, m_shaderCacheDir);
    }

    bool DeviceVK::compileShaderModules(const std::vector<std::string>& sources, ShaderCompiler& compiler)
    {
        return PipelineVK::CompileModulesNow(sources, compiler, m_shaderCacheDir);
    }

    std::unique_ptr<CommandAllocator> DeviceVK::createCommandAllocator(CommandBufferReset resetMode, QueueType queue)
    {
        return std::make_unique<CommandAllocatorVK>(*this, resetMode, getQueueFamily(queue));
    }

    std::unique_ptr<TimelineSemaphore> DeviceVK::createTimelineSemaphore(uint64_t initialValue)
    {
        return std::make_unique<TimelineSemaphoreVK>(*this, initialValue);
    }

    std::unique_ptr<Texture2D> DeviceVK::createTexture(const TextureDesc& desc)
    {
        return std::make_unique<TextureVK>(*this, desc);
    }

    std::unique_ptr<Buffer> DeviceVK::createBuffer(const BufferDesc& desc)
    {
        return std::make_unique<BufferVK>(*this, desc);
    }

    std::unique_ptr<DescriptorHeap> DeviceVK::createDescriptorHeap(const DescriptorHeapDesc& desc)
    {
        return std::make_unique<DescriptorHeapVK>(*this, desc);
    }

    AccelerationStructureBuildSizes DeviceVK::getAccelerationStructureBuildSizes(const AccelerationStructureBuildDesc& desc)
    {
        const vk::BuildAccelerationStructureFlagsKHR vkFlags = toVkBuildFlags(desc.flags);

        if (desc.type == AccelerationStructureType::BottomLevel)
        {
            std::vector<vk::AccelerationStructureGeometryKHR> geometries;
            geometries.reserve(desc.triangles.size());
            std::vector<uint32_t> maxPrimitiveCounts;
            maxPrimitiveCounts.reserve(desc.triangles.size());

            for (const auto& tri : desc.triangles)
            {
                vk::AccelerationStructureGeometryTrianglesDataKHR trianglesData{
                    .vertexFormat = vk::Format::eR32G32B32Sfloat,
                    .vertexData = tri.vertexBufferAddress,
                    .vertexStride = tri.vertexStride,
                    .maxVertex = tri.maxVertex,
                    .indexType = vk::IndexType::eUint32,
                    .indexData = tri.indexBufferAddress
                };

                vk::AccelerationStructureGeometryKHR geometry{
                    .geometryType = vk::GeometryTypeKHR::eTriangles,
                    .geometry = trianglesData,
                    .flags = tri.isOpaque ? vk::GeometryFlagBitsKHR::eOpaque : vk::GeometryFlagsKHR{}
                };

                geometries.push_back(geometry);
                maxPrimitiveCounts.push_back(tri.primitiveCount);
            }

            vk::AccelerationStructureBuildGeometryInfoKHR buildInfo{
                .type = vk::AccelerationStructureTypeKHR::eBottomLevel,
                .flags = vkFlags,
                .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
                .geometryCount = static_cast<uint32_t>(geometries.size()),
                .pGeometries = geometries.data()
            };

            vk::AccelerationStructureBuildSizesInfoKHR vkSizes = m_device.getAccelerationStructureBuildSizesKHR(
                vk::AccelerationStructureBuildTypeKHR::eDevice,
                buildInfo,
                maxPrimitiveCounts
            );

            return AccelerationStructureBuildSizes{
                .accelerationStructureSize = vkSizes.accelerationStructureSize,
                .buildScratchSize = vkSizes.buildScratchSize,
                .updateScratchSize = vkSizes.updateScratchSize
            };
        }
        else
        {
            vk::AccelerationStructureGeometryInstancesDataKHR instancesData{
                .arrayOfPointers = vk::False,
                .data = desc.instances.instanceBufferAddress
            };

            vk::AccelerationStructureGeometryKHR geometry{
                .geometryType = vk::GeometryTypeKHR::eInstances,
                .geometry = instancesData
            };

            vk::AccelerationStructureBuildGeometryInfoKHR buildInfo{
                .type = vk::AccelerationStructureTypeKHR::eTopLevel,
                .flags = vkFlags,
                .mode = vk::BuildAccelerationStructureModeKHR::eBuild,
                .geometryCount = 1,
                .pGeometries = &geometry
            };

            vk::AccelerationStructureBuildSizesInfoKHR vkSizes = m_device.getAccelerationStructureBuildSizesKHR(
                vk::AccelerationStructureBuildTypeKHR::eDevice,
                buildInfo,
                { desc.instances.instanceCount }
            );

            return AccelerationStructureBuildSizes{
                .accelerationStructureSize = vkSizes.accelerationStructureSize,
                .buildScratchSize = vkSizes.buildScratchSize,
                .updateScratchSize = vkSizes.updateScratchSize
            };
        }
    }

    std::unique_ptr<AccelerationStructure> DeviceVK::createAccelerationStructure(const AccelerationStructureDesc& desc)
    {
        return std::make_unique<AccelerationStructureVK>(*this, desc);
    }

    std::unique_ptr<QueryPool> DeviceVK::createQueryPool(const QueryPoolDesc& desc)
    {
        return std::make_unique<QueryPoolVK>(*this, desc);
    }

    std::unique_ptr<GpuProfiler> DeviceVK::createGpuProfiler(uint32_t framesInFlight)
    {
        return std::make_unique<GpuProfilerVK>(*this, framesInFlight);
    }

    bool DeviceVK::updateSparseBuffer(Buffer& buffer, std::span<const SparsePageRange> commits, std::span<const SparsePageRange> releases)
    {
        if (!m_sparseQueue)
            return false;
        freeRetiredSparseMemory();
        m_sparseBindScratch.clear();
        std::vector<vma::raii::Allocation> retired;
        auto& vkBuffer = static_cast<BufferVK&>(buffer);
        if (!vkBuffer.prepareSparseBinds(commits, releases, m_sparseBindScratch, retired))
            return false;
        if (m_sparseBindScratch.empty())
            return true;

        const vk::Semaphore semaphore = *static_cast<TimelineSemaphoreVK*>(m_sparseTimeline.get())->getNativeSemaphore();
        const uint64_t waitValue = m_sparseValue;
        const uint64_t signalValue = m_sparseValue + 1;
        const vk::TimelineSemaphoreSubmitInfo timeline{
            .waitSemaphoreValueCount = 1,
            .pWaitSemaphoreValues = &waitValue,
            .signalSemaphoreValueCount = 1,
            .pSignalSemaphoreValues = &signalValue
        };
        const vk::SparseBufferMemoryBindInfo bufferBinds{
            .buffer = *vkBuffer.getNativeBuffer(),
            .bindCount = static_cast<uint32_t>(m_sparseBindScratch.size()),
            .pBinds = m_sparseBindScratch.data()
        };
        const vk::BindSparseInfo bindInfo{
            .pNext = &timeline,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &semaphore,
            .bufferBindCount = 1,
            .pBufferBinds = &bufferBinds,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &semaphore
        };
        m_sparseQueue->bindSparse(bindInfo, nullptr);
        m_sparseValue = signalValue;
        if (!retired.empty())
            m_sparseRetired.emplace_back(std::move(retired), signalValue);
        return true;
    }

    void DeviceVK::freeRetiredSparseMemory()
    {
        if (m_sparseRetired.empty())
            return;
        const uint64_t completed = m_sparseTimeline->getValue();
        std::erase_if(m_sparseRetired, [completed](const std::pair<std::vector<vma::raii::Allocation>, uint64_t>& retired)
        {
            return retired.second <= completed;
        });
    }

    void DeviceVK::beginFrame(uint32_t frameNumber)
    {
        m_allocator->setCurrentFrameIndex(frameNumber);
        freeRetiredSparseMemory();
        collectDeferredDeletions();
    }

    void DeviceVK::getMemoryStats(std::vector<MemoryHeapStats>& outHeaps) const
    {
        m_allocator->getHeapStats(outHeaps);
    }

    QueueTimelineValues DeviceVK::getSubmittedTimelineValues() const
    {
        return {
            m_submittedValues[static_cast<size_t>(QueueType::Graphics)].load(std::memory_order_relaxed),
            m_submittedValues[static_cast<size_t>(QueueType::Transfer)].load(std::memory_order_relaxed),
            m_submittedValues[static_cast<size_t>(QueueType::Compute)].load(std::memory_order_relaxed)
        };
    }

    QueueTimelineValues DeviceVK::getCompletedTimelineValues() const
    {
        QueueTimelineValues completed{};
        if (*m_retirementSemaphores[0])
            completed.graphics = m_retirementSemaphores[0].getCounterValue();
        if (*m_retirementSemaphores[1])
            completed.transfer = m_retirementSemaphores[1].getCounterValue();
        if (*m_retirementSemaphores[2])
            completed.compute = m_retirementSemaphores[2].getCounterValue();
        return completed;
    }

    DeviceVK::DeviceVK(const DeviceInitDesc& desc)
        : m_validationEnabled(desc.enableValidation),
          m_shaderCacheDir(desc.shaderCacheDir.empty() ? "shader_cache" : desc.shaderCacheDir)
    {
        initVulkan(desc);
        m_allocator = std::make_unique<MemoryAllocatorVK>(*this);
        m_deviceInitialized = true;
    }

    DeviceVK::~DeviceVK()
    {
        shutdown();
        m_sparseRetired.clear();
        m_allocator.reset();
    }

    void DeviceVK::shutdown()
    {
        if (m_shuttingDown)
            return;
        if (*m_device)
        {
            try { m_device.waitIdle(); } catch (...) {}
        }
        m_shuttingDown = true;
        drainDeferredDeletions();
        m_sparseRetired.clear();
        m_deviceInitialized = false;
    }

    void DeviceVK::initVulkan(const DeviceInitDesc& desc)
    {
        createInstance(desc);
        setupDebugMessenger();
        createSurface(desc.window);
        pickPhysicalDevice();
        m_msaaSamples = getMaxUsableSampleCount();
        createLogicalDevice(desc);
        initDeviceCapabilities();
    }

    void DeviceVK::createInstance(const DeviceInitDesc& desc)
    {
        constexpr vk::ApplicationInfo appInfo{
            .pApplicationName = "NoxRHI Application",
            .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
            .pEngineName = "NoxRHI",
            .engineVersion = VK_MAKE_VERSION(1, 0, 0),
            .apiVersion = vk::ApiVersion14
        };

        std::vector<char const*> requiredLayers;
        if (m_validationEnabled)
        {
            requiredLayers.assign(validationLayers.begin(), validationLayers.end());
        }

        auto layerProperties = m_context.enumerateInstanceLayerProperties();
        auto unsupportedLayerIt = std::ranges::find_if(requiredLayers,
                                                       [&layerProperties](auto const& requiredLayer)
                                                       {
                                                           return std::ranges::none_of(layerProperties,
                                                                                       [requiredLayer](auto const& layerProperty) { return strcmp(layerProperty.layerName, requiredLayer) == 0; });
                                                       });
        if (unsupportedLayerIt != requiredLayers.end())
        {
            NRI_LOG_WARN("Requested Vulkan validation layer '{}' is not available; continuing without it.", *unsupportedLayerIt);
            requiredLayers.clear();
            m_validationEnabled = false;
        }

        auto requiredExtensions = getRequiredInstanceExtensions(desc);
        auto extensionProperties = m_context.enumerateInstanceExtensionProperties();
        auto unsupportedPropertyIt =
            std::ranges::find_if(requiredExtensions,
                                 [&extensionProperties](auto const& requiredExtension)
                                 {
                                     return std::ranges::none_of(extensionProperties,
                                                                 [requiredExtension](auto const& extensionProperty) { return strcmp(extensionProperty.extensionName, requiredExtension) == 0; });
                                 });
        if (unsupportedPropertyIt != requiredExtensions.end())
        {
            throw std::runtime_error("Required Vulkan instance extension not supported: " + std::string(*unsupportedPropertyIt));
        }

        m_debugUtilsEnabled = m_validationEnabled;
        if (!m_debugUtilsEnabled &&
            std::ranges::any_of(extensionProperties, [](const auto& property) { return strcmp(property.extensionName, vk::EXTDebugUtilsExtensionName) == 0; }))
        {
            requiredExtensions.push_back(vk::EXTDebugUtilsExtensionName);
            m_debugUtilsEnabled = true;
        }

        vk::InstanceCreateInfo createInfo
        {
            .pApplicationInfo = &appInfo,
            .enabledLayerCount = static_cast<uint32_t>(requiredLayers.size()),
            .ppEnabledLayerNames = requiredLayers.data(),
            .enabledExtensionCount = static_cast<uint32_t>(requiredExtensions.size()),
            .ppEnabledExtensionNames = requiredExtensions.data()
        };
        m_instance = vk::raii::Instance(m_context, createInfo);
        VULKAN_HPP_DEFAULT_DISPATCHER.init(*m_instance, vkGetInstanceProcAddr);
    }

    void DeviceVK::setupDebugMessenger()
    {
        if (!m_validationEnabled)
            return;

        vk::DebugUtilsMessageSeverityFlagsEXT severityFlags(vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
            vk::DebugUtilsMessageSeverityFlagBitsEXT::eError);
        vk::DebugUtilsMessageTypeFlagsEXT messageTypeFlags(
            vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral | vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance | vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation);
        vk::DebugUtilsMessengerCreateInfoEXT debugUtilsMessengerCreateInfoEXT{
            .messageSeverity = severityFlags,
            .messageType = messageTypeFlags,
            .pfnUserCallback = &debugCallback
        };
        m_debugMessenger = m_instance.createDebugUtilsMessengerEXT(debugUtilsMessengerCreateInfoEXT);
    }

    void DeviceVK::createSurface(const NativeWindowHandle& window)
    {
        if (window.vkSurface != 0)
        {
            m_externalSurface = reinterpret_cast<VkSurfaceKHR>(window.vkSurface);
            return;
        }
        if (window.type == WindowSystemType::Headless || window.hwnd == nullptr)
        {
            return;
        }
#ifdef _WIN32
        HINSTANCE hinst = window.hinstance
            ? static_cast<HINSTANCE>(window.hinstance)
            : GetModuleHandleW(nullptr);
        vk::Win32SurfaceCreateInfoKHR createInfo{
            .hinstance = hinst,
            .hwnd = static_cast<HWND>(window.hwnd)
        };
        m_surface = m_instance.createWin32SurfaceKHR(createInfo);
#else
        throw std::runtime_error("Native window surface creation requires vkSurface on non-Windows platforms.");
#endif
    }

    bool DeviceVK::isDeviceSuitable(vk::raii::PhysicalDevice const& physicalDevice)
    {
        bool supportsVulkan1_3 = physicalDevice.getProperties().apiVersion >= vk::ApiVersion13;

        auto queueFamilies = physicalDevice.getQueueFamilyProperties();
        bool supportsGraphics = std::ranges::any_of(queueFamilies, [](auto const& qfp) { return !!(qfp.queueFlags & vk::QueueFlagBits::eGraphics); });

        auto availableDeviceExtensions = physicalDevice.enumerateDeviceExtensionProperties();
        bool supportsAllRequiredExtensions =
            std::ranges::all_of(kRequiredDeviceExtensions,
                                [&availableDeviceExtensions](auto const& requiredDeviceExtension)
                                {
                                    return std::ranges::any_of(availableDeviceExtensions,
                                                               [requiredDeviceExtension](auto const& availableDeviceExtension)
                                                               {
                                                                   return strcmp(availableDeviceExtension.extensionName, requiredDeviceExtension) == 0;
                                                               });
                                });

        auto features = physicalDevice.template getFeatures2<vk::PhysicalDeviceFeatures2,
                                                             vk::PhysicalDeviceVulkan11Features,
                                                             vk::PhysicalDeviceVulkan13Features,
                                                             vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT,
                                                             vk::PhysicalDeviceMeshShaderFeaturesEXT,
                                                             vk::PhysicalDeviceAccelerationStructureFeaturesKHR,
                                                             vk::PhysicalDeviceRayQueryFeaturesKHR,
                                                             vk::PhysicalDeviceRayTracingPipelineFeaturesKHR>();
        bool supportsRequiredFeatures = features.template get<vk::PhysicalDeviceFeatures2>().features.samplerAnisotropy &&
            features.template get<vk::PhysicalDeviceFeatures2>().features.geometryShader &&
            features.template get<vk::PhysicalDeviceVulkan11Features>().shaderDrawParameters &&
            features.template get<vk::PhysicalDeviceVulkan13Features>().dynamicRendering &&
            features.template get<vk::PhysicalDeviceVulkan13Features>().synchronization2 &&
            features.template get<vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT>().extendedDynamicState &&
            features.template get<vk::PhysicalDeviceMeshShaderFeaturesEXT>().meshShader &&
            features.template get<vk::PhysicalDeviceAccelerationStructureFeaturesKHR>().accelerationStructure &&
            features.template get<vk::PhysicalDeviceRayQueryFeaturesKHR>().rayQuery &&
            features.template get<vk::PhysicalDeviceRayTracingPipelineFeaturesKHR>().rayTracingPipeline;

        return supportsVulkan1_3 && supportsGraphics && supportsAllRequiredExtensions && supportsRequiredFeatures;
    }

    void DeviceVK::pickPhysicalDevice()
    {
        std::vector<vk::raii::PhysicalDevice> physicalDevices = m_instance.enumeratePhysicalDevices();
        auto const devIter = std::ranges::find_if(physicalDevices, [&](auto const& physicalDevice) { return isDeviceSuitable(physicalDevice); });
        if (devIter == physicalDevices.end())
        {
            throw std::runtime_error("failed to find a suitable GPU!");
        }
        m_physicalDevice = *devIter;
    }

    void DeviceVK::createLogicalDevice(const DeviceInitDesc& desc)
    {
        std::vector<vk::QueueFamilyProperties> queueFamilyProperties = m_physicalDevice.getQueueFamilyProperties();
        const vk::SurfaceKHR activeSurface = getActiveSurface();

        for (uint32_t qfpIndex = 0; qfpIndex < queueFamilyProperties.size(); qfpIndex++)
        {
            const bool graphicsCompute = (queueFamilyProperties[qfpIndex].queueFlags & vk::QueueFlagBits::eGraphics) &&
                                         (queueFamilyProperties[qfpIndex].queueFlags & vk::QueueFlagBits::eCompute);
            const bool presentOk = !activeSurface || m_physicalDevice.getSurfaceSupportKHR(qfpIndex, activeSurface);
            if (graphicsCompute && presentOk)
            {
                m_queueIndex = qfpIndex;
                break;
            }
        }
        if (m_queueIndex == ~0u)
        {
            throw std::runtime_error("Could not find a queue for graphics and present -> terminating");
        }

        m_transferQueueIndex = m_queueIndex;
        for (uint32_t qfpIndex = 0; qfpIndex < queueFamilyProperties.size(); qfpIndex++)
        {
            const vk::QueueFlags flags = queueFamilyProperties[qfpIndex].queueFlags;
            if ((flags & vk::QueueFlagBits::eTransfer) && !(flags & vk::QueueFlagBits::eGraphics) && !(flags & vk::QueueFlagBits::eCompute))
            {
                m_transferQueueIndex = qfpIndex;
                break;
            }
        }

        for (uint32_t qfpIndex = 0; qfpIndex < queueFamilyProperties.size(); qfpIndex++)
        {
            const vk::QueueFlags flags = queueFamilyProperties[qfpIndex].queueFlags;
            if ((flags & vk::QueueFlagBits::eCompute) && !(flags & vk::QueueFlagBits::eGraphics) && qfpIndex != m_transferQueueIndex)
            {
                m_computeQueueIndex = qfpIndex;
                break;
            }
        }

        m_sharedQueueFamilyCount = 0;
        for (uint32_t family : { m_queueIndex, m_transferQueueIndex, m_computeQueueIndex })
        {
            if (family != ~0u && std::find(m_sharedQueueFamilies.begin(), m_sharedQueueFamilies.begin() + m_sharedQueueFamilyCount, family) ==
                                     m_sharedQueueFamilies.begin() + m_sharedQueueFamilyCount)
                m_sharedQueueFamilies[m_sharedQueueFamilyCount++] = family;
        }

        {
            const vk::PhysicalDeviceFeatures supported = m_physicalDevice.getFeatures();
            const bool transferSparse = !!(queueFamilyProperties[m_transferQueueIndex].queueFlags & vk::QueueFlagBits::eSparseBinding);
            const bool graphicsSparse = !!(queueFamilyProperties[m_queueIndex].queueFlags & vk::QueueFlagBits::eSparseBinding);
            m_sparseFeatures = supported.sparseBinding && supported.sparseResidencyBuffer && (transferSparse || graphicsSparse);
        }

        auto features = m_physicalDevice.template getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceShaderObjectFeaturesEXT, vk::PhysicalDeviceMeshShaderFeaturesEXT>();
        m_shaderObjectsEnabled = features.template get<vk::PhysicalDeviceShaderObjectFeaturesEXT>().shaderObject;
        m_pipelineStatisticsEnabled = features.template get<vk::PhysicalDeviceFeatures2>().features.pipelineStatisticsQuery &&
                                      features.template get<vk::PhysicalDeviceMeshShaderFeaturesEXT>().meshShaderQueries;

        std::vector<const char*> enabledDeviceExtensions = kRequiredDeviceExtensions;
        const std::vector<vk::ExtensionProperties> availableDeviceExtensions = m_physicalDevice.enumerateDeviceExtensionProperties();
        auto isExtensionAvailable = [&availableDeviceExtensions](const char* name)
        {
            return std::ranges::any_of(availableDeviceExtensions, [name](const vk::ExtensionProperties& extension)
            {
                return strcmp(extension.extensionName, name) == 0;
            });
        };

        auto addIfNotPresent = [&enabledDeviceExtensions](const char* name)
        {
            if (std::ranges::none_of(enabledDeviceExtensions, [name](const char* existing) { return strcmp(existing, name) == 0; }))
                enabledDeviceExtensions.push_back(name);
        };

        for (const char* optExt : kOptionalDeviceExtensions)
        {
            if (isExtensionAvailable(optExt))
                addIfNotPresent(optExt);
        }
        for (const std::string& extraExt : desc.extraDeviceExtensions)
        {
            if (isExtensionAvailable(extraExt.c_str()))
                addIfNotPresent(extraExt.c_str());
            else
                NRI_LOG_WARN("Requested extra Vulkan device extension '{}' is not supported by physical device.", extraExt);
        }

        const bool computeDerivativesEnabled = isExtensionAvailable(vk::KHRComputeShaderDerivativesExtensionName);

        m_memoryBudgetEnabled = isExtensionAvailable(vk::EXTMemoryBudgetExtensionName);
        if (m_memoryBudgetEnabled)
            addIfNotPresent(vk::EXTMemoryBudgetExtensionName);

#ifdef _WIN32
        constexpr vk::TimeDomainKHR hostTimeDomain = vk::TimeDomainKHR::eQueryPerformanceCounter;
#else
        constexpr vk::TimeDomainKHR hostTimeDomain = vk::TimeDomainKHR::eClockMonotonic;
#endif
        const bool calibratedKHR = isExtensionAvailable(vk::KHRCalibratedTimestampsExtensionName);
        if (calibratedKHR || isExtensionAvailable(vk::EXTCalibratedTimestampsExtensionName))
        {
            const std::vector<vk::TimeDomainKHR> domains = calibratedKHR ? m_physicalDevice.getCalibrateableTimeDomainsKHR()
                                                                          : m_physicalDevice.getCalibrateableTimeDomainsEXT();
            if (std::ranges::find(domains, vk::TimeDomainKHR::eDevice) != domains.end() &&
                std::ranges::find(domains, hostTimeDomain) != domains.end())
            {
                m_calibratedTimestamps = calibratedKHR ? CalibratedTimestamps::KHR : CalibratedTimestamps::EXT;
                m_hostTimeDomain = hostTimeDomain;
                addIfNotPresent(calibratedKHR ? vk::KHRCalibratedTimestampsExtensionName : vk::EXTCalibratedTimestampsExtensionName);
            }
        }

        vk::StructureChain<vk::PhysicalDeviceFeatures2,
                           vk::PhysicalDeviceVulkan11Features,
                           vk::PhysicalDeviceVulkan12Features,
                           vk::PhysicalDeviceVulkan13Features,
                           vk::PhysicalDeviceShaderObjectFeaturesEXT,
                           vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT,
                           vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT,
                           vk::PhysicalDeviceVertexInputDynamicStateFeaturesEXT,
                           vk::PhysicalDeviceDescriptorHeapFeaturesEXT,
                           vk::PhysicalDeviceShaderUntypedPointersFeaturesKHR,
                           vk::PhysicalDeviceMaintenance5FeaturesKHR,
                           vk::PhysicalDeviceMeshShaderFeaturesEXT,
                           vk::PhysicalDeviceUnifiedImageLayoutsFeaturesKHR,
                           vk::PhysicalDeviceAccelerationStructureFeaturesKHR,
                           vk::PhysicalDeviceRayQueryFeaturesKHR,
                           vk::PhysicalDeviceRayTracingPipelineFeaturesKHR,
                           vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR
            >
            featureChain = {
                {
                    .features = {
                        .geometryShader = true,
                        .tessellationShader = true,
                        .sampleRateShading = true,
                        .multiDrawIndirect = true,
                        .depthClamp = true,
                        .depthBiasClamp = true,
                        .fillModeNonSolid = true,
                        .wideLines = true,
                        .samplerAnisotropy = true,
                        .pipelineStatisticsQuery = m_pipelineStatisticsEnabled,
                        .shaderInt64 = true,
                        .shaderInt16 = true,
                        .sparseBinding = m_sparseFeatures,
                        .sparseResidencyBuffer = m_sparseFeatures,
                    }
                },
                { .shaderDrawParameters = true },
                {
                    .drawIndirectCount = true,
                    .storageBuffer8BitAccess = true,
                    .shaderFloat16 = true,
                    .shaderInt8 = true,
                    .shaderSampledImageArrayNonUniformIndexing = true,
                    .shaderStorageBufferArrayNonUniformIndexing = true,
                    .descriptorBindingUniformBufferUpdateAfterBind = true,
                    .descriptorBindingSampledImageUpdateAfterBind = true,
                    .descriptorBindingStorageImageUpdateAfterBind = true,
                    .descriptorBindingStorageBufferUpdateAfterBind = true,
                    .descriptorBindingUpdateUnusedWhilePending = true,
                    .descriptorBindingPartiallyBound = true,
                    .descriptorBindingVariableDescriptorCount = true,
                    .runtimeDescriptorArray = true,
                    .scalarBlockLayout = true,
                    .hostQueryReset = true,
                    .timelineSemaphore = true,
                    .bufferDeviceAddress = true
                },
                {
                    .privateData = true,
                    .shaderDemoteToHelperInvocation = true,
                    .synchronization2 = true,
                    .dynamicRendering = true
                },
                { .shaderObject = m_shaderObjectsEnabled },
                { .extendedDynamicState = true },
                {
                    .extendedDynamicState3DepthClampEnable = vk::True,
                    .extendedDynamicState3PolygonMode = vk::True,
                    .extendedDynamicState3RasterizationSamples = vk::True,
                    .extendedDynamicState3SampleMask = vk::True,
                    .extendedDynamicState3AlphaToCoverageEnable = vk::True,
                    .extendedDynamicState3AlphaToOneEnable = vk::True,
                    .extendedDynamicState3LogicOpEnable = vk::True,
                    .extendedDynamicState3ColorBlendEnable = vk::True,
                    .extendedDynamicState3ColorBlendEquation = vk::True,
                    .extendedDynamicState3ColorWriteMask = vk::True
                },
                { .vertexInputDynamicState = true },
                { .descriptorHeap = true },
                { .shaderUntypedPointers = true },
                { .maintenance5 = true },
                { .taskShader = true, .meshShader = true, .meshShaderQueries = m_pipelineStatisticsEnabled },
                { .unifiedImageLayouts = true },
                { .accelerationStructure = true },
                { .rayQuery = true },
                { .rayTracingPipeline = true },
                { .computeDerivativeGroupQuads = computeDerivativesEnabled, .computeDerivativeGroupLinear = computeDerivativesEnabled }
            };

        if (!computeDerivativesEnabled)
            featureChain.unlink<vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR>();

        float queuePriority = 0.5f;
        std::array<vk::DeviceQueueCreateInfo, 3> deviceQueueCreateInfos{};
        for (uint32_t index = 0; index < m_sharedQueueFamilyCount; ++index)
            deviceQueueCreateInfos[index] = vk::DeviceQueueCreateInfo{
                .queueFamilyIndex = m_sharedQueueFamilies[index],
                .queueCount = 1,
                .pQueuePriorities = &queuePriority
            };

        vk::DeviceCreateInfo deviceCreateInfo{
            .pNext = &featureChain.get<vk::PhysicalDeviceFeatures2>(),
            .queueCreateInfoCount = m_sharedQueueFamilyCount,
            .pQueueCreateInfos = deviceQueueCreateInfos.data(),
            .enabledExtensionCount = static_cast<uint32_t>(enabledDeviceExtensions.size()),
            .ppEnabledExtensionNames = enabledDeviceExtensions.data()
        };

        m_device = vk::raii::Device(m_physicalDevice, deviceCreateInfo);
        m_queue = vk::raii::Queue(m_device, m_queueIndex, 0);
        m_transferQueue = vk::raii::Queue(m_device, m_transferQueueIndex, 0);
        if (hasAsyncComputeQueue())
            m_computeQueue = vk::raii::Queue(m_device, m_computeQueueIndex, 0);
        m_submitFence = vk::raii::Fence(m_device, vk::FenceCreateInfo{});

        // Create internal retirement timeline semaphores for deferred deletion
        for (auto& sem : m_retirementSemaphores)
        {
            vk::SemaphoreTypeCreateInfo typeInfo{
                .semaphoreType = vk::SemaphoreType::eTimeline,
                .initialValue = 0
            };
            vk::SemaphoreCreateInfo semInfo{ .pNext = &typeInfo };
            sem = vk::raii::Semaphore(m_device, semInfo);
        }

        if (m_sparseFeatures)
            m_sparseQueue = (queueFamilyProperties[m_transferQueueIndex].queueFlags & vk::QueueFlagBits::eSparseBinding) ? &m_transferQueue : &m_queue;

        NRI_LOG_INFO("Transfer queue: {}", hasDedicatedTransferQueue() ? "dedicated family" : "graphics queue");
        NRI_LOG_INFO("Async compute queue: {}", hasAsyncComputeQueue() ? "compute-only family " + std::to_string(m_computeQueueIndex)
                                                                       : std::string("none (compute runs on the graphics queue)"));

        VULKAN_HPP_DEFAULT_DISPATCHER.init(*m_instance, vkGetInstanceProcAddr, *m_device);
    }

    void DeviceVK::initDeviceCapabilities()
    {
        m_depthFormat = findDepthFormat();
        m_surfaceFormat = chooseSurfaceFormat();

        auto rtProps = m_physicalDevice.template getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceRayTracingPipelinePropertiesKHR,
                                                                vk::PhysicalDeviceMaintenance4Properties>();
        m_rtPipelineProperties = rtProps.template get<vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>();
        if (m_sparseQueue)
        {
            const uint64_t addressSpace = rtProps.template get<vk::PhysicalDeviceProperties2>().properties.limits.sparseAddressSpaceSize;
            const uint64_t maxBuffer = rtProps.template get<vk::PhysicalDeviceMaintenance4Properties>().maxBufferSize;
            m_sparseReserveLimit = std::min(maxBuffer, addressSpace / 16);
            m_sparseTimeline = createTimelineSemaphore(0);
            NRI_LOG_INFO("Sparse buffers: on ({} queue), {} GB reservable per buffer (sparse address space {} GB, largest buffer {} GB)",
                         m_sparseQueue == &m_transferQueue && hasDedicatedTransferQueue() ? "transfer" : "graphics", m_sparseReserveLimit >> 30,
                         addressSpace >> 30, maxBuffer >> 30);
        }
        else
        {
            NRI_LOG_INFO("Sparse buffers: off (no sparseBinding + sparseResidencyBuffer): geometry streams grow by copying");
        }
        NRI_LOG_INFO("RT Pipeline Properties: handleSize={}, baseAlignment={}, handleAlignment={}, maxStride={}",
                     m_rtPipelineProperties.shaderGroupHandleSize,
                     m_rtPipelineProperties.shaderGroupBaseAlignment,
                     m_rtPipelineProperties.shaderGroupHandleAlignment,
                     m_rtPipelineProperties.maxShaderGroupStride);
    }

    vk::SampleCountFlagBits DeviceVK::getMaxUsableSampleCount()
    {
        vk::PhysicalDeviceProperties physicalDeviceProperties = m_physicalDevice.getProperties();
        vk::SampleCountFlags counts = physicalDeviceProperties.limits.framebufferColorSampleCounts & physicalDeviceProperties.limits.framebufferDepthSampleCounts;
        if (counts & vk::SampleCountFlagBits::e64) return vk::SampleCountFlagBits::e64;
        if (counts & vk::SampleCountFlagBits::e32) return vk::SampleCountFlagBits::e32;
        if (counts & vk::SampleCountFlagBits::e16) return vk::SampleCountFlagBits::e16;
        if (counts & vk::SampleCountFlagBits::e8)  return vk::SampleCountFlagBits::e8;
        if (counts & vk::SampleCountFlagBits::e4)  return vk::SampleCountFlagBits::e4;
        if (counts & vk::SampleCountFlagBits::e2)  return vk::SampleCountFlagBits::e2;
        return vk::SampleCountFlagBits::e1;
    }

    std::vector<const char*> DeviceVK::getRequiredInstanceExtensions(const DeviceInitDesc& desc)
    {
        std::vector<const char*> extensions;
        if (desc.window.type != WindowSystemType::Headless)
        {
            extensions.push_back(vk::KHRSurfaceExtensionName);
#ifdef _WIN32
            if (desc.window.type == WindowSystemType::Win32)
                extensions.push_back(vk::KHRWin32SurfaceExtensionName);
#endif
        }
        if (m_validationEnabled)
        {
            extensions.push_back(vk::EXTDebugUtilsExtensionName);
        }
        extensions.push_back(vk::KHRGetPhysicalDeviceProperties2ExtensionName);
        extensions.push_back(vk::KHRExternalMemoryCapabilitiesExtensionName);
        extensions.push_back(vk::KHRExternalSemaphoreCapabilitiesExtensionName);

        for (const std::string& ext : desc.extraInstanceExtensions)
        {
            if (std::ranges::none_of(extensions, [&ext](const char* existing) { return ext == existing; }))
                extensions.push_back(ext.c_str());
        }
        return extensions;
    }

    VKAPI_ATTR vk::Bool32 VKAPI_CALL DeviceVK::debugCallback(vk::DebugUtilsMessageSeverityFlagBitsEXT severity,
                                                             vk::DebugUtilsMessageTypeFlagsEXT type,
                                                             const vk::DebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                                             void* pUserData)
    {
        if (severity >= vk::DebugUtilsMessageSeverityFlagBitsEXT::eError)
            NRI_LOG_ERROR("Vulkan validation ({}): {}", to_string(type), pCallbackData->pMessage);
        else if (severity >= vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning)
            NRI_LOG_WARN("Vulkan validation ({}): {}", to_string(type), pCallbackData->pMessage);
        else
            NRI_LOG_INFO("Vulkan validation ({}): {}", to_string(type), pCallbackData->pMessage);

        return vk::False;
    }

    vk::raii::ImageView DeviceVK::createImageView(vk::Image const& image, vk::Format format, vk::ImageAspectFlags aspectFlags, uint32_t mipLevels)
    {
        vk::ImageViewCreateInfo viewInfo{
            .image = image,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = { .aspectMask = aspectFlags, .baseMipLevel = 0, .levelCount = mipLevels, .baseArrayLayer = 0, .layerCount = 1 }
        };
        return vk::raii::ImageView(m_device, viewInfo);
    }

    vk::Format DeviceVK::findSupportedFormat(const std::vector<vk::Format>& candidates, vk::ImageTiling tiling, vk::FormatFeatureFlags features)
    {
        for (const auto format : candidates)
        {
            vk::FormatProperties props = m_physicalDevice.getFormatProperties(format);
            if (((tiling == vk::ImageTiling::eLinear) && ((props.linearTilingFeatures & features) == features)) ||
                ((tiling == vk::ImageTiling::eOptimal) && ((props.optimalTilingFeatures & features) == features)))
            {
                return format;
            }
        }
        throw std::runtime_error("failed to find supported format!");
    }

    vk::Format DeviceVK::findDepthFormat()
    {
        return findSupportedFormat({ vk::Format::eD32Sfloat, vk::Format::eD32SfloatS8Uint, vk::Format::eD24UnormS8Uint },
                                   vk::ImageTiling::eOptimal,
                                   vk::FormatFeatureFlagBits::eDepthStencilAttachment);
    }

    vk::SurfaceFormatKHR DeviceVK::chooseSurfaceFormat()
    {
        const vk::SurfaceKHR activeSurface = getActiveSurface();
        if (!activeSurface)
            return { vk::Format::eB8G8R8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear };
        std::vector<vk::SurfaceFormatKHR> availableFormats = m_physicalDevice.getSurfaceFormatsKHR(activeSurface);
        return chooseSwapSurfaceFormat(availableFormats);
    }

    vk::SurfaceFormatKHR DeviceVK::chooseSwapSurfaceFormat(const std::vector<vk::SurfaceFormatKHR>& availableFormats)
    {
        const auto formatIt = std::ranges::find_if(
            availableFormats,
            [](const auto& format) { return format.format == vk::Format::eB8G8R8A8Srgb && format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear; });
        return formatIt != availableFormats.end() ? *formatIt : availableFormats[0];
    }

    uint32_t DeviceVK::findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties)
    {
        vk::PhysicalDeviceMemoryProperties memProperties = m_physicalDevice.getMemoryProperties();
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
        {
            if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
                return i;
        }
        throw std::runtime_error("failed to find suitable memory type!");
    }

    void DeviceVK::submitAndWait(CommandBuffer& cmdBuffer, uint32_t slotIndex)
    {
        auto* cmdBufferVK = static_cast<CommandBufferVK*>(&cmdBuffer);
        vk::raii::CommandBuffer& nativeCB = cmdBufferVK->getNativeBuffer(slotIndex);

        const uint64_t nextVal = m_submittedValues[0].fetch_add(1, std::memory_order_relaxed) + 1;
        const vk::CommandBufferSubmitInfo cmdInfo{ .commandBuffer = *nativeCB };
        const vk::SemaphoreSubmitInfo retireSignal{
            .semaphore = *m_retirementSemaphores[0],
            .value = nextVal,
            .stageMask = vk::PipelineStageFlagBits2::eAllCommands
        };
        const vk::SubmitInfo2 submitInfo{
            .commandBufferInfoCount = 1,
            .pCommandBufferInfos = &cmdInfo,
            .signalSemaphoreInfoCount = 1,
            .pSignalSemaphoreInfos = &retireSignal
        };

        m_device.resetFences(*m_submitFence);
        m_queue.submit2(submitInfo, *m_submitFence);
        (void)m_device.waitForFences(*m_submitFence, vk::True, UINT64_MAX);
        collectDeferredDeletions();
    }

    void DeviceVK::submitCommandBuffers(std::span<CommandBuffer* const> cmdBuffers, Swapchain& swapchain, uint32_t frameIndex,
                                        uint32_t imageIndex, std::span<const TimelinePoint> timelineWaits)
    {
        auto* vkSwap = static_cast<SwapchainVK*>(&swapchain);

        m_submitScratch.clear();
        for (CommandBuffer* cmdBuffer : cmdBuffers)
            m_submitScratch.push_back(vk::CommandBufferSubmitInfo{ .commandBuffer = *static_cast<CommandBufferVK*>(cmdBuffer)->getNativeBuffer(0) });

        m_waitScratch.clear();
        m_waitScratch.push_back(vk::SemaphoreSubmitInfo{
            .semaphore = *vkSwap->getPresentCompleteSemaphore(frameIndex),
            .stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput
        });
        for (const TimelinePoint& point : timelineWaits)
        {
            m_waitScratch.push_back(vk::SemaphoreSubmitInfo{
                .semaphore = *static_cast<TimelineSemaphoreVK*>(point.semaphore)->getNativeSemaphore(),
                .value = point.value,
                .stageMask = vk::PipelineStageFlagBits2::eAllCommands
            });
        }

        const uint64_t nextVal = m_submittedValues[0].fetch_add(1, std::memory_order_relaxed) + 1;
        const std::array<vk::SemaphoreSubmitInfo, 2> signals = {
            vk::SemaphoreSubmitInfo{
                .semaphore = *vkSwap->getRenderFinishedSemaphore(imageIndex),
                .stageMask = vk::PipelineStageFlagBits2::eAllCommands
            },
            vk::SemaphoreSubmitInfo{
                .semaphore = *m_retirementSemaphores[0],
                .value = nextVal,
                .stageMask = vk::PipelineStageFlagBits2::eAllCommands
            }
        };

        const vk::SubmitInfo2 submitInfo{
            .waitSemaphoreInfoCount = static_cast<uint32_t>(m_waitScratch.size()),
            .pWaitSemaphoreInfos = m_waitScratch.data(),
            .commandBufferInfoCount = static_cast<uint32_t>(m_submitScratch.size()),
            .pCommandBufferInfos = m_submitScratch.data(),
            .signalSemaphoreInfoCount = static_cast<uint32_t>(signals.size()),
            .pSignalSemaphoreInfos = signals.data()
        };
        m_queue.submit2(submitInfo, *vkSwap->getInFlightFence(frameIndex));
    }

    void DeviceVK::submit(QueueType queue, std::span<CommandBuffer* const> cmdBuffers, std::span<const TimelinePoint> waits,
                          std::span<const TimelinePoint> signals)
    {
        m_submitScratch.clear();
        for (CommandBuffer* cmdBuffer : cmdBuffers)
            m_submitScratch.push_back(vk::CommandBufferSubmitInfo{ .commandBuffer = *static_cast<CommandBufferVK*>(cmdBuffer)->getNativeBuffer(0) });

        auto toSubmitInfo = [](const TimelinePoint& point)
        {
            return vk::SemaphoreSubmitInfo{
                .semaphore = *static_cast<TimelineSemaphoreVK*>(point.semaphore)->getNativeSemaphore(),
                .value = point.value,
                .stageMask = vk::PipelineStageFlagBits2::eAllCommands
            };
        };
        m_waitScratch.clear();
        for (const TimelinePoint& point : waits)
            m_waitScratch.push_back(toSubmitInfo(point));

        QueueType effectiveQueue = QueueType::Graphics;
        vk::raii::Queue* target = &m_queue;
        if (queue == QueueType::Transfer && hasDedicatedTransferQueue())
        {
            effectiveQueue = QueueType::Transfer;
            target = &m_transferQueue;
        }
        else if (queue == QueueType::Compute && hasAsyncComputeQueue())
        {
            effectiveQueue = QueueType::Compute;
            target = &m_computeQueue;
        }

        const size_t queueIdx = static_cast<size_t>(effectiveQueue);
        const uint64_t nextVal = m_submittedValues[queueIdx].fetch_add(1, std::memory_order_relaxed) + 1;

        m_signalScratch.clear();
        for (const TimelinePoint& point : signals)
            m_signalScratch.push_back(toSubmitInfo(point));
        m_signalScratch.push_back(vk::SemaphoreSubmitInfo{
            .semaphore = *m_retirementSemaphores[queueIdx],
            .value = nextVal,
            .stageMask = vk::PipelineStageFlagBits2::eAllCommands
        });

        const vk::SubmitInfo2 submitInfo{
            .waitSemaphoreInfoCount = static_cast<uint32_t>(m_waitScratch.size()),
            .pWaitSemaphoreInfos = m_waitScratch.data(),
            .commandBufferInfoCount = static_cast<uint32_t>(m_submitScratch.size()),
            .pCommandBufferInfos = m_submitScratch.data(),
            .signalSemaphoreInfoCount = static_cast<uint32_t>(m_signalScratch.size()),
            .pSignalSemaphoreInfos = m_signalScratch.data()
        };
        target->submit2(submitInfo);
    }

    void DeviceVK::waitIdle()
    {
        if (*m_device)
            m_device.waitIdle();
    }
}
