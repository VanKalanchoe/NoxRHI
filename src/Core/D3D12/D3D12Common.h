#pragma once

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <stdexcept>
#include <string>

#include "../Log.h"

namespace NRI
{
    using Microsoft::WRL::ComPtr;

    // Set by DeviceD3D12 while DRED is on: logs why the device was removed (reason, breadcrumbs, page fault) before ThrowIfFailed throws.
    inline void (*g_OnD3D12DeviceRemoved)() = nullptr;

    inline void ThrowIfFailed(HRESULT hr, const char* message = nullptr)
    {
        if (FAILED(hr))
        {
            if ((hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DEVICE_RESET) && g_OnD3D12DeviceRemoved)
                g_OnD3D12DeviceRemoved();
            char errorMsg[512];
            snprintf(errorMsg, sizeof(errorMsg), "D3D12 Error (HRESULT 0x%08X)%s%s", 
                     static_cast<unsigned int>(hr), message ? ": " : "", message ? message : "");
            NRI_LOG_ERROR("{}", errorMsg);
            throw std::runtime_error(errorMsg);
        }
    }
}

#endif // _WIN32
