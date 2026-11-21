#include "SwapchainVK.h"
#include "DeviceVK.h"
#include "../Log.h"
#include "../../Extensions/Upscaler/StreamlineContext.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace NRI
{
    SwapchainVK::SwapchainVK(DeviceVK& device, const SwapchainDesc& desc) : m_deviceVK(device)
    {
        m_hooks = m_deviceVK.getStreamlineVulkanHooks();
        createSwapChain(desc);
        createImageViews();
        createSyncObjects();
    }

    SwapchainVK::~SwapchainVK()
    {
        if (!m_hooks)
            return; // (the raii members destroy the swapchain, then the surface)
        // Through Streamline's proxies: the image views first, then the swapchain, then the surface.
        m_swapChainImageViews.clear();
        const VkDevice device = static_cast<VkDevice>(*m_deviceVK.getDevice());
        if (m_hookedSwapchain)
            reinterpret_cast<PFN_vkDestroySwapchainKHR>(m_hooks->vkDestroySwapchainKHR)(device, m_hookedSwapchain, nullptr);
        if (m_hookedSurface)
            reinterpret_cast<PFN_vkDestroySurfaceKHR>(m_hooks->vkDestroySurfaceKHR)(static_cast<VkInstance>(*m_deviceVK.getInstance()), m_hookedSurface, nullptr);
    }
    
    void SwapchainVK::createSwapChain(const SwapchainDesc& desc)
    {
        auto& physicalDevice = m_deviceVK.getPhysicalDevice();
        auto& device = m_deviceVK.getDevice();
        vk::SurfaceKHR surface = m_deviceVK.getActiveSurface();
        if (desc.Window.vkSurface != 0)
        {
            surface = reinterpret_cast<VkSurfaceKHR>(desc.Window.vkSurface);
        }
        else if (desc.Window.hwnd != nullptr)
        {
#ifdef _WIN32
            HINSTANCE hinst = desc.Window.hinstance
                ? static_cast<HINSTANCE>(desc.Window.hinstance)
                : GetModuleHandleW(nullptr);
            vk::Win32SurfaceCreateInfoKHR sci{
                .hinstance = hinst,
                .hwnd = static_cast<HWND>(desc.Window.hwnd)
            };
            if (m_hooks)
            {
                const VkResult result = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(m_hooks->vkCreateWin32SurfaceKHR)(
                    static_cast<VkInstance>(*m_deviceVK.getInstance()), reinterpret_cast<const VkWin32SurfaceCreateInfoKHR*>(&sci), nullptr, &m_hookedSurface);
                if (result != VK_SUCCESS)
                    throw std::runtime_error("SwapchainVK: vkCreateWin32SurfaceKHR (Streamline proxy) failed");
                surface = m_hookedSurface;
            }
            else
            {
                m_ownedSurface = m_deviceVK.getInstance().createWin32SurfaceKHR(sci);
                surface = *m_ownedSurface;
            }
#else
            throw std::runtime_error("Win32 surface creation is only supported on Windows!");
#endif
        }
        if (!surface)
            throw std::runtime_error("SwapchainVK: no Vulkan surface available!");

        vk::SurfaceCapabilitiesKHR surfaceCapabilities = physicalDevice.getSurfaceCapabilitiesKHR(surface);
        m_swapChainExtent = chooseSwapExtent(surfaceCapabilities, desc.Width, desc.Height);
        uint32_t minImageCount = chooseSwapMinImageCount(surfaceCapabilities);

        m_swapChainSurfaceFormat = m_deviceVK.getSurfaceFormat();

        std::vector<vk::PresentModeKHR> availablePresentModes = physicalDevice.getSurfacePresentModesKHR(surface);
        vk::PresentModeKHR presentMode = chooseSwapPresentMode(availablePresentModes, desc.VSync, desc.AllowTearing);
        NRI_LOG_INFO("Swapchain present mode: {}", vk::to_string(presentMode));

        vk::SwapchainCreateInfoKHR swapChainCreateInfo{
            .surface = surface,
            .minImageCount = minImageCount,
            .imageFormat = m_swapChainSurfaceFormat.format,
            .imageColorSpace = m_swapChainSurfaceFormat.colorSpace,
            .imageExtent = m_swapChainExtent,
            .imageArrayLayers = 1,
            .imageUsage = vk::ImageUsageFlagBits::eColorAttachment,
            .imageSharingMode = vk::SharingMode::eExclusive,
            .preTransform = surfaceCapabilities.currentTransform,
            .compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque,
            .presentMode = presentMode,
            .clipped = true
        };

        if (m_hooks)
        {
            const VkDevice vkDevice = static_cast<VkDevice>(*device);
            if (reinterpret_cast<PFN_vkCreateSwapchainKHR>(m_hooks->vkCreateSwapchainKHR)(
                    vkDevice, reinterpret_cast<const VkSwapchainCreateInfoKHR*>(&swapChainCreateInfo), nullptr, &m_hookedSwapchain) != VK_SUCCESS)
                throw std::runtime_error("SwapchainVK: vkCreateSwapchainKHR (Streamline proxy) failed");
            const auto getImages = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(m_hooks->vkGetSwapchainImagesKHR);
            uint32_t count = 0;
            getImages(vkDevice, m_hookedSwapchain, &count, nullptr);
            std::vector<VkImage> images(count);
            getImages(vkDevice, m_hookedSwapchain, &count, images.data());
            m_swapChainImages.assign(images.begin(), images.end());
        }
        else
        {
            m_swapChain = vk::raii::SwapchainKHR(device, swapChainCreateInfo);
            m_swapChainImages = m_swapChain.getImages();
        }
    }

    void SwapchainVK::createImageViews()
    {
        assert(m_swapChainImageViews.empty());
        
        m_swapChainImageViews.clear();
        m_swapChainImageViews.reserve(m_swapChainImages.size());
        for (auto& image : m_swapChainImages)
        {
            m_swapChainImageViews.emplace_back(m_deviceVK.createImageView(image, m_swapChainSurfaceFormat.format, vk::ImageAspectFlagBits::eColor, 1));
        }
    }

    // As NVIDIA's NRI (Source/VK/SwapChainVK.hpp): vsync -> FIFO; no vsync -> MAILBOX (uncapped, no tearing: the newest finished image is
    // shown at vblank), or IMMEDIATE when tearing is allowed; FIFO is the fallback (the one mode every driver has). IMMEDIATE in a window
    // on an NVIDIA GPU froze the picture now and then for about a second while the frames kept running (PathTracer with NRD).
    vk::PresentModeKHR SwapchainVK::chooseSwapPresentMode(std::vector<vk::PresentModeKHR> const& availablePresentModes, bool vSync, bool allowTearing)
    {
        assert(std::ranges::any_of(availablePresentModes, [](auto presentMode) { return presentMode == vk::PresentModeKHR::eFifo; }));
        const auto available = [&](vk::PresentModeKHR mode) { return std::ranges::find(availablePresentModes, mode) != availablePresentModes.end(); };

        if (vSync)
            return vk::PresentModeKHR::eFifo;
        const vk::PresentModeKHR uncapped = allowTearing ? vk::PresentModeKHR::eImmediate : vk::PresentModeKHR::eMailbox;
        return available(uncapped) ? uncapped : vk::PresentModeKHR::eFifo;
    }

    vk::SurfaceFormatKHR SwapchainVK::chooseSwapSurfaceFormat(const std::vector<vk::SurfaceFormatKHR>& availableFormats)
    {
        const auto formatIt = std::ranges::find_if(
            availableFormats,
            [](const auto& format) { return format.format == vk::Format::eB8G8R8A8Srgb && format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear; });
        return formatIt != availableFormats.end() ? *formatIt : availableFormats[0];
    }

    uint32_t SwapchainVK::chooseSwapMinImageCount(vk::SurfaceCapabilitiesKHR const& surfaceCapabilities)
    {
        auto minImageCount = std::max(3u, surfaceCapabilities.minImageCount);
        if ((0 < surfaceCapabilities.maxImageCount) && (surfaceCapabilities.maxImageCount < minImageCount))
        {
            minImageCount = surfaceCapabilities.maxImageCount;
        }
        return minImageCount;
    }

    vk::Extent2D SwapchainVK::chooseSwapExtent(vk::SurfaceCapabilitiesKHR const& capabilities, uint32_t windowWidth, uint32_t windowHeight)
    {
        if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max())
        {
            return capabilities.currentExtent;
        }
        
        return {
            std::clamp<uint32_t>(windowWidth, capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
            std::clamp<uint32_t>(windowHeight, capabilities.minImageExtent.height, capabilities.maxImageExtent.height)
        };
    }

    void SwapchainVK::createSyncObjects()
    {
        auto& device = m_deviceVK.getDevice();

        assert(m_presentCompleteSemaphores.empty() && m_renderFinishedSemaphores.empty() && m_inFlightFences.empty());

        for (size_t i = 0; i < m_swapChainImages.size(); i++)
        {
            m_renderFinishedSemaphores.emplace_back(device, vk::SemaphoreCreateInfo());
        }

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            m_presentCompleteSemaphores.emplace_back(device, vk::SemaphoreCreateInfo());
            m_inFlightFences.emplace_back(device, vk::FenceCreateInfo{.flags = vk::FenceCreateFlagBits::eSignaled});
        }
    }

    FrameResult SwapchainVK::acquireNextImage(uint32_t frameIndex, uint32_t& outImageIndex)
    {
        auto& device = m_deviceVK.getDevice();

        auto fenceResult = device.waitForFences(*m_inFlightFences[frameIndex], vk::True, UINT64_MAX);
        if (fenceResult != vk::Result::eSuccess)
        {
            throw std::runtime_error("failed to wait for fence!");
        }

        vk::Result result = vk::Result::eSuccess;
        uint32_t imageIndex = 0;
        if (m_hooks)
            result = static_cast<vk::Result>(reinterpret_cast<PFN_vkAcquireNextImageKHR>(m_hooks->vkAcquireNextImageKHR)(
                static_cast<VkDevice>(*device), m_hookedSwapchain, UINT64_MAX, static_cast<VkSemaphore>(*m_presentCompleteSemaphores[frameIndex]),
                VK_NULL_HANDLE, &imageIndex));
        else
            std::tie(result, imageIndex) = m_swapChain.acquireNextImage(UINT64_MAX, *m_presentCompleteSemaphores[frameIndex], nullptr);

        if (result == vk::Result::eErrorOutOfDateKHR)
        {
            return FrameResult::ResizeRequired;
        }
        if (result != vk::Result::eSuccess && result != vk::Result::eSuboptimalKHR)
        {
            assert(result == vk::Result::eTimeout || result == vk::Result::eNotReady);
            throw std::runtime_error("failed to acquire swap chain image!");
        }

        outImageIndex = imageIndex;

        device.resetFences(*m_inFlightFences[frameIndex]);
        return FrameResult::Success;
    }

    FrameResult SwapchainVK::present(uint32_t frameIndex, uint32_t imageIndex)
    {
        auto& queue = m_deviceVK.getQueue();

        const vk::SwapchainKHR swapchain = getNativeSwapchain();
        const vk::PresentInfoKHR presentInfoKHR
        {
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &*m_renderFinishedSemaphores[imageIndex],
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &imageIndex
        };

        vk::Result result = vk::Result::eSuccess;
        if (m_hooks) // (Streamline's present: runs its presentCommon())
            result = static_cast<vk::Result>(reinterpret_cast<PFN_vkQueuePresentKHR>(m_hooks->vkQueuePresentKHR)(
                static_cast<VkQueue>(*queue), reinterpret_cast<const VkPresentInfoKHR*>(&presentInfoKHR)));
        else
            result = queue.presentKHR(presentInfoKHR);
        
        if ((result == vk::Result::eSuboptimalKHR) || (result == vk::Result::eErrorOutOfDateKHR))
        {
            return FrameResult::ResizeRequired;
        }

        assert(result == vk::Result::eSuccess);
        return FrameResult::Success;
    }
}
