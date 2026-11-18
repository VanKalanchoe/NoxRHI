@echo off
setlocal EnableExtensions

for %%I in ("%~dp0..") do set "ROOT_DIR=%%~fI"
set "VENDORS_DIR=%ROOT_DIR%\vendors"

if not exist "%VENDORS_DIR%" mkdir "%VENDORS_DIR%"

echo ============================================================================
echo Installing NoxRHI dependencies into:
echo   %VENDORS_DIR%
echo ============================================================================

:: ----------------------------------------------------------------------------
:: 1. VulkanMemoryAllocator-Hpp (v3.4.0+3, includes VulkanMemoryAllocator submodule)
:: ----------------------------------------------------------------------------
set "VMA_HPP_VERSION=v3.4.0+3"
set "VMA_HPP_DIR=%VENDORS_DIR%\VulkanMemoryAllocator-Hpp"
if not exist "%VMA_HPP_DIR%\include\vk_mem_alloc.hpp" (
    echo [1/7] Cloning VulkanMemoryAllocator-Hpp ^(%VMA_HPP_VERSION%^)...
    if exist "%VMA_HPP_DIR%" rmdir /s /q "%VMA_HPP_DIR%"
    git clone --recursive --depth 1 --branch "%VMA_HPP_VERSION%" https://github.com/YaaZ/VulkanMemoryAllocator-Hpp.git "%VMA_HPP_DIR%"
    if errorlevel 1 (
        echo ERROR: Failed to clone VulkanMemoryAllocator-Hpp.
        goto :fail
    )
) else (
    echo [1/7] VulkanMemoryAllocator-Hpp already present, skipping.
)

:: ----------------------------------------------------------------------------
:: 2. D3D12MemoryAllocator (v3.1.0)
:: ----------------------------------------------------------------------------
set "D3D12MA_VERSION=v3.1.0"
set "D3D12MA_DIR=%VENDORS_DIR%\D3D12MemoryAllocator"
if not exist "%D3D12MA_DIR%\src\D3D12MemAlloc.cpp" (
    echo [2/7] Cloning D3D12MemoryAllocator ^(%D3D12MA_VERSION%^)...
    if exist "%D3D12MA_DIR%" rmdir /s /q "%D3D12MA_DIR%"
    git clone --depth 1 --branch "%D3D12MA_VERSION%" https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator.git "%D3D12MA_DIR%"
    if errorlevel 1 (
        echo ERROR: Failed to clone D3D12MemoryAllocator.
        goto :fail
    )
) else (
    echo [2/7] D3D12MemoryAllocator already present, skipping.
)

:: ----------------------------------------------------------------------------
:: 3. Slang (v2026.14.1 Windows x86_64 prebuilt binaries + headers)
:: ----------------------------------------------------------------------------
set "SLANG_VERSION=2026.14.1"
set "SLANG_URL=https://github.com/shader-slang/slang/releases/download/v%SLANG_VERSION%/slang-%SLANG_VERSION%-windows-x86_64.zip"
set "SLANG_DIR=%VENDORS_DIR%\slang"
set "SLANG_ZIP=%TEMP%\noxrhi-slang-%SLANG_VERSION%.zip"
if not exist "%SLANG_DIR%\lib\slang.lib" (
    echo [3/7] Downloading Slang %SLANG_VERSION%...
    curl -fSL "%SLANG_URL%" -o "%SLANG_ZIP%"
    if errorlevel 1 (
        echo ERROR: Failed to download Slang from %SLANG_URL%.
        goto :fail
    )
    if exist "%SLANG_DIR%" rmdir /s /q "%SLANG_DIR%"
    mkdir "%SLANG_DIR%"
    echo Extracting Slang to "%SLANG_DIR%"...
    tar -xf "%SLANG_ZIP%" -C "%SLANG_DIR%"
    if errorlevel 1 (
        if exist "%SLANG_ZIP%" del /q "%SLANG_ZIP%"
        if exist "%SLANG_DIR%" rmdir /s /q "%SLANG_DIR%"
        echo ERROR: Failed to extract Slang archive.
        goto :fail
    )
    if exist "%SLANG_ZIP%" del /q "%SLANG_ZIP%"
) else (
    echo [3/7] Slang already present, skipping.
)

:: ----------------------------------------------------------------------------
:: 4. Dear ImGui (docking branch - standalone ImGuiDemo example only)
:: ----------------------------------------------------------------------------
set "IMGUI_DIR=%VENDORS_DIR%\imgui"
if not exist "%IMGUI_DIR%\imgui.h" (
    echo [4/7] Cloning Dear ImGui ^(docking branch^)...
    if exist "%IMGUI_DIR%" rmdir /s /q "%IMGUI_DIR%"
    git clone --depth 1 --branch docking https://github.com/ocornut/imgui.git "%IMGUI_DIR%"
    if errorlevel 1 (
        echo ERROR: Failed to clone Dear ImGui.
        goto :fail
    )
) else (
    echo [4/7] Dear ImGui already present, skipping.
)

:: ----------------------------------------------------------------------------
:: 5. vcpkg + SDL3 + stb (standalone examples only)
:: ----------------------------------------------------------------------------
set "VCPKG_ROOT=%VENDORS_DIR%\vcpkg"
if not exist "%VCPKG_ROOT%\vcpkg.exe" (
    if not exist "%VCPKG_ROOT%\.git" (
        echo [5/7] Cloning vcpkg into "%VCPKG_ROOT%"...
        if exist "%VCPKG_ROOT%" rmdir /s /q "%VCPKG_ROOT%"
        git clone https://github.com/Microsoft/vcpkg.git "%VCPKG_ROOT%"
        if errorlevel 1 (
            echo ERROR: Failed to clone vcpkg.
            goto :fail
        )
    ) else (
        echo [5/7] vcpkg repository already cloned, bootstrapping...
    )
    pushd "%VCPKG_ROOT%"
    call bootstrap-vcpkg.bat -disableMetrics
    if errorlevel 1 (
        popd
        echo ERROR: Failed to bootstrap vcpkg.
        goto :fail
    )
    popd
) else (
    echo [5/7] vcpkg executable already present.
)

if not exist "%TEMP%\vcpkg-cache" mkdir "%TEMP%\vcpkg-cache"
set "VCPKG_BINARY_SOURCES=clear;files,%TEMP%\vcpkg-cache,readwrite"

echo Ensuring SDL3 and stb ^(x64-windows^) are installed via vcpkg...
"%VCPKG_ROOT%\vcpkg.exe" install sdl3 stb --triplet=x64-windows
if errorlevel 1 (
    echo ERROR: Failed to install sdl3/stb via vcpkg.
    goto :fail
)

:: ----------------------------------------------------------------------------
:: 6. NVIDIA Streamline SDK (DLSS Super Resolution / DLSS-RR)
:: ----------------------------------------------------------------------------
set "STREAMLINE_VERSION=2.14.1"
set "STREAMLINE_URL=https://github.com/NVIDIA-RTX/Streamline/releases/download/v%STREAMLINE_VERSION%/streamline-sdk-v%STREAMLINE_VERSION%.zip"
set "STREAMLINE_DIR=%VENDORS_DIR%\Streamline"
set "STREAMLINE_ZIP=%TEMP%\noxrhi-streamline-%STREAMLINE_VERSION%.zip"
if not exist "%STREAMLINE_DIR%\include\sl.h" (
    echo [6/7] Downloading NVIDIA Streamline SDK %STREAMLINE_VERSION%...
    curl -fSL "%STREAMLINE_URL%" -o "%STREAMLINE_ZIP%"
    if errorlevel 1 (
        echo ERROR: Failed to download Streamline SDK from %STREAMLINE_URL%.
        goto :fail
    )
    if exist "%STREAMLINE_DIR%" rmdir /s /q "%STREAMLINE_DIR%"
    mkdir "%STREAMLINE_DIR%"
    echo Extracting Streamline SDK to "%STREAMLINE_DIR%"...
    tar -xf "%STREAMLINE_ZIP%" -C "%STREAMLINE_DIR%"
    if errorlevel 1 (
        if exist "%STREAMLINE_ZIP%" del /q "%STREAMLINE_ZIP%"
        if exist "%STREAMLINE_DIR%" rmdir /s /q "%STREAMLINE_DIR%"
        echo ERROR: Failed to extract Streamline SDK archive.
        goto :fail
    )
    if exist "%STREAMLINE_ZIP%" del /q "%STREAMLINE_ZIP%"
) else (
    echo [6/7] Streamline SDK already present, skipping.
)

:: ----------------------------------------------------------------------------
:: 7. NVIDIA Real-Time Denoisers (NRD v4.17.3 + NRI SDK for NRDIntegration)
:: ----------------------------------------------------------------------------
set "NRD_VERSION=v4.17.3"
set "NRD_DIR=%VENDORS_DIR%\NRD"
set "NRD_BUILD_DIR=%TEMP%\noxrhi-NRD-build"
if not exist "%NRD_DIR%\Include\NRD.h" (
    echo [7/7] Building and packaging NVIDIA NRD ^(%NRD_VERSION%^)...
    if exist "%NRD_BUILD_DIR%" rmdir /s /q "%NRD_BUILD_DIR%"
    git clone --depth 1 --branch "%NRD_VERSION%" --recursive https://github.com/NVIDIA-RTX/NRD.git "%NRD_BUILD_DIR%"
    if errorlevel 1 (
        echo ERROR: Failed to clone NRD.
        goto :fail
    )
    pushd "%NRD_BUILD_DIR%"
    call 1-Deploy.bat -DNRI_ENABLE_AGILITY_SDK_SUPPORT=OFF
    if errorlevel 1 (
        popd
        echo ERROR: Failed to deploy NRD.
        goto :fail
    )
    call 2-Build.bat
    if errorlevel 1 (
        popd
        echo ERROR: Failed to build NRD.
        goto :fail
    )
    call 3-PrepareSDK.bat
    if errorlevel 1 (
        popd
        echo ERROR: Failed to prepare NRD SDK.
        goto :fail
    )
    popd

    if exist "%NRD_DIR%" rmdir /s /q "%NRD_DIR%"
    mkdir "%NRD_DIR%"
    xcopy /E /I /Y "%NRD_BUILD_DIR%\_NRD_SDK\*" "%NRD_DIR%\"
    if exist "%NRD_BUILD_DIR%\_NRI_SDK" (
        xcopy /E /I /Y "%NRD_BUILD_DIR%\_NRI_SDK\*" "%NRD_DIR%\"
    )
    if exist "%NRD_BUILD_DIR%\_Build\_deps\nri-src\_NRI_SDK" (
        xcopy /E /I /Y "%NRD_BUILD_DIR%\_Build\_deps\nri-src\_NRI_SDK\*" "%NRD_DIR%\"
    )
    rmdir /s /q "%NRD_BUILD_DIR%"
) else (
    echo [7/7] NRD SDK already present, skipping.
)

echo.
echo All NoxRHI dependencies are ready in "%VENDORS_DIR%".
if /i not "%NRI_NO_PAUSE%"=="1" pause
exit /b 0

:fail
echo.
echo Dependency installation failed.
if /i not "%NRI_NO_PAUSE%"=="1" pause
exit /b 1
