#include "Device.h"
#include <stdexcept>

#ifdef NRI_ENABLE_VULKAN
#include "Vulkan/DeviceVK.h"
#endif
#if defined(_WIN32) && defined(NRI_ENABLE_D3D12)
#include "D3D12/DeviceD3D12.h"
#endif

namespace NRI
{
    std::unique_ptr<Device> Device::create(const DeviceInitDesc& desc)
    {
        switch (desc.api)
        {
        case GraphicsAPI::Vulkan:
#ifdef NRI_ENABLE_VULKAN
            return std::make_unique<DeviceVK>(desc);
#else
            throw std::runtime_error("Vulkan backend is disabled in this NoxRHI build.");
#endif
#ifdef _WIN32
        case GraphicsAPI::D3D12:
#ifdef NRI_ENABLE_D3D12
            return std::make_unique<DeviceD3D12>(desc);
#else
            throw std::runtime_error("D3D12 backend is disabled in this NoxRHI build.");
#endif
#endif
        case GraphicsAPI::Metal:
            throw std::runtime_error("Metal backend not implemented yet!");
        }
        return nullptr;
    }
}
