#include "SwapchainVK.h"
#include "DeviceVK.h"
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
        createSwapChain(desc);
        createImageViews();
        createSyncObjects();
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
            m_ownedSurface = m_deviceVK.getInstance().createWin32SurfaceKHR(sci);
            surface = *m_ownedSurface;
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
        vk::PresentModeKHR presentMode = chooseSwapPresentMode(availablePresentModes, desc.VSync);

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

        m_swapChain = vk::raii::SwapchainKHR(device, swapChainCreateInfo);
        m_swapChainImages = m_swapChain.getImages();
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

    vk::PresentModeKHR SwapchainVK::chooseSwapPresentMode(std::vector<vk::PresentModeKHR> const& availablePresentModes, bool vSync)
    {
        assert(std::ranges::any_of(availablePresentModes, [](auto presentMode) { return presentMode == vk::PresentModeKHR::eFifo; }));
        
        if (vSync)
        {
            return vk::PresentModeKHR::eFifo; 
        }
        
        if (std::ranges::any_of(availablePresentModes, [](const vk::PresentModeKHR mode) { return vk::PresentModeKHR::eImmediate == mode; }))
        {
            return vk::PresentModeKHR::eImmediate;
        }
        
        return std::ranges::any_of(availablePresentModes,
                                   [](const vk::PresentModeKHR value) { return vk::PresentModeKHR::eMailbox == value; })
                   ? vk::PresentModeKHR::eMailbox
                   : vk::PresentModeKHR::eFifo;
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

        auto [result, imageIndex] = m_swapChain.acquireNextImage(UINT64_MAX, *m_presentCompleteSemaphores[frameIndex], nullptr);

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

        const vk::PresentInfoKHR presentInfoKHR
        {
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &*m_renderFinishedSemaphores[imageIndex],
            .swapchainCount = 1,
            .pSwapchains = &*m_swapChain,
            .pImageIndices = &imageIndex
        };
        
        const vk::Result result = queue.presentKHR(presentInfoKHR);
        
        if ((result == vk::Result::eSuboptimalKHR) || (result == vk::Result::eErrorOutOfDateKHR))
        {
            return FrameResult::ResizeRequired;
        }

        assert(result == vk::Result::eSuccess);
        return FrameResult::Success;
    }
}
