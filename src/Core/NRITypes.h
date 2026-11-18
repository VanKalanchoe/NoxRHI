#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace NRI
{
    enum class GraphicsAPI : uint8_t
    {
        Vulkan = 0,
        Metal = 1,
        D3D12 = 2
    };

    enum class WindowSystemType : uint8_t
    {
        Win32 = 0,
        X11 = 1,
        Wayland = 2,
        Metal = 3,
        Headless = 4
    };

    struct NativeWindowHandle
    {
        WindowSystemType type = WindowSystemType::Win32;
        void* hwnd = nullptr;
        void* hinstance = nullptr;
        uint64_t vkSurface = 0; // Optional caller-provided VkSurfaceKHR (non-owning)
    };

    struct DeviceInitDesc
    {
        GraphicsAPI api = GraphicsAPI::Vulkan;
        NativeWindowHandle window{};
        bool enableValidation =
#ifdef NDEBUG
            false;
#else
            true;
#endif
        std::string shaderCacheDir = "shader_cache";
        std::vector<std::string> extraInstanceExtensions;
        std::vector<std::string> extraDeviceExtensions;
    };

    struct Extent2D
    {
        uint32_t width = 0;
        uint32_t height = 0;
    };

    struct ViewportBounds
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
    };

    // One physical memory heap as reported by the allocator (VK_EXT_memory_budget when available,
    // otherwise the allocator's own estimate).
    struct MemoryHeapStats
    {
        bool deviceLocal = false;
        uint64_t usage = 0;           // bytes the process uses in this heap (all allocations, not only ours)
        uint64_t budget = 0;          // bytes the OS allows this process to use in this heap
        uint64_t blockBytes = 0;      // bytes allocated from the driver by the allocator
        uint64_t allocationBytes = 0; // bytes handed out to resources inside those blocks
        uint32_t allocationCount = 0;
    };

    enum class CompareOp : uint8_t
    {
        Never = 0,
        Less,
        Equal,
        LessOrEqual,
        Greater,
        NotEqual,
        GreaterOrEqual,
        Always
    };
}
