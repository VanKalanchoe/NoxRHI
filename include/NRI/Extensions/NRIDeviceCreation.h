#pragma once

#include "../NRI.h"

NRI_EXTERN_C_BEGIN

typedef enum NriWindowSystemType
{
    NRI_WINDOW_SYSTEM_WIN32 = 0,
    NRI_WINDOW_SYSTEM_X11 = 1,
    NRI_WINDOW_SYSTEM_WAYLAND = 2,
    NRI_WINDOW_SYSTEM_METAL = 3,
    NRI_WINDOW_SYSTEM_HEADLESS = 4
} NriWindowSystemType;

typedef struct NriNativeWindowHandle
{
    NriWindowSystemType type;
    void* hwnd;         // Win32 HWND (or X11 Window* / Wayland wl_surface*)
    void* hinstance;    // Win32 HINSTANCE (or X11 Display* / Wayland wl_display*)
    uint64_t vkSurface; // Optional caller-supplied VkSurfaceKHR (non-owning)
} NriNativeWindowHandle;

typedef struct NriDeviceDesc
{
    NriGraphicsAPI api;
    NriNativeWindowHandle window;
    bool enableValidation;
    const char* shaderCacheDir; // Optional; NULL defaults to "shader_cache"
    const char* const* extraInstanceExtensions;
    uint32_t extraInstanceExtensionCount;
    const char* const* extraDeviceExtensions;
    uint32_t extraDeviceExtensionCount;
    NriLogCallback logCallback;
    void* logUserData;
    bool enableStreamline;          // Initialize NVIDIA Streamline (manual hooking before device creation)
    uint32_t streamlineAppId;       // Optional Streamline App ID (0 uses default)
    const char* streamlineProjectId;// Optional Streamline Project GUID (NULL uses default)
} NriDeviceDesc;

NRI_API NriResult NRI_CALL nriCreateDevice(const NriDeviceDesc* desc, NriDevice* outDevice);

NRI_EXTERN_C_END
