# NoxRHI

A standalone C++20 Rendering Hardware Interface (**Vulkan** & **Direct3D 12**) and **RenderGraph** library with a pure C ABI boundary.

Public headers expose **zero** backend SDK headers (`<vulkan/...>`, `<d3d12.h>`), engine headers, or windowing libraries. Consumers can link statically or consume the C ABI DLL across compiler toolchains (e.g., an MSVC application linking against a ClangCL-built `NoxRHI.dll`).

---

## Architecture (4 Layers)

1. **`include/NRI/` (`NoxRHI::Core`)** — Pure C11 ABI (`extern "C"`, `NRI_API`, opaque handles, POD structs) backed by private Vulkan (`vk::raii` + VMA) and D3D12 (`ComPtr` + D3D12MA) implementations inside `src/Core/`.
2. **`include/NRI/Extensions/` (`NoxRHI::Core`)** — Modular C ABI extensions:
   - `NRIDeviceCreation.h`, `NRISwapchain.h`, `NRIRayTracing.h`, `NRIMeshShader.h`
   - `NRIGpuProfiler.h`, `NRIShaderCompiler.h`, `NRIImgui.h`
   - `NRIUpscaler.h` (NVIDIA Streamline: DLSS Super Resolution & Ray Reconstruction)
   - `NRIDenoiser.h` (NVIDIA Real-Time Denoisers: SIGMA Shadow, REBLUR, RELAX)
   - `NRIWrapperVK.h`, `NRIWrapperD3D12.h` (opt-in native interop using opaque `void*` / `uint64_t` handles)
3. **`include/NRIHPP/` (`NoxRHI::HPP`)** — Header-only C++20 RAII wrappers (`namespace nri`, `NRI.hpp`, `NRIExtensions.hpp`) with move-only handles, `std::span`, and `std::string_view`.
4. **`include/NRIRenderGraph/` + `src/RenderGraph/` (`NoxRHI::RenderGraph`)** — Standalone RenderGraph library built on `NoxRHI::HPP` with transient resource pooling, barrier planning, validation, and optional profiler/task hooks.

> **Two Headless Modes:**
> - **No-GPU Null Backend (`NRI_GRAPHICS_API_NONE`):** Null/CPU-backed RHI (`NRI_ENABLE_NONE=ON`) for builds running with **zero** GPU drivers, Vulkan/D3D12 devices, or windows (`HeadlessNone` example). CPU-backed `map`/`copyBuffer`/`fillBuffer` and full `RenderGraph` ticks succeed cleanly while GPU extensions (`Streamline`, `NRD`) safely report unavailable.
> - **Windowless GPU Rendering (`NRI_WINDOW_SYSTEM_HEADLESS`):** Real Vulkan (`VK_KHR_swapchain` skipped cleanly) and Direct3D 12 compute/offscreen rendering without any window or swapchain, with CPU readback via `NRI_BUFFER_USAGE_READBACK` (`HeadlessRender` and `StreamlineNRDDemo` examples).

---

## Requirements & Dependencies

### System Prerequisites (Windows)
- **Visual Studio 2026** (v18) with **ClangCL** (default in scripts) and/or **MSVC** C++20 toolset, plus **Windows 10/11 SDK** (`d3d12`, `dxgi`, `dxguid`).
- **CMake >= 3.25**
- **Git**, **curl**, and **tar** (built into Windows 10/11)
- **Vulkan SDK** (tested with **1.4.350.0**, `VULKAN_SDK` set in environment)

### Vendored Dependencies (Downloaded into `vendors/` by `scripts\install_dependencies.bat`)

| Dependency | Version | Purpose | Scope | Source |
|---|---|---|---|---|
| **VulkanMemoryAllocator-Hpp** (incl. VMA) | `v3.4.0+3` | Vulkan GPU memory allocation | Private (`NoxRHI::Core`, Vulkan) | [YaaZ/VulkanMemoryAllocator-Hpp](https://github.com/YaaZ/VulkanMemoryAllocator-Hpp) |
| **D3D12MemoryAllocator** | `v3.1.0` | D3D12 GPU memory allocation | Private (`NoxRHI::Core`, D3D12) | [GPUOpen/D3D12MemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator) |
| **Slang** | `v2026.14.1` | Runtime shader compiler (`NRI_ENABLE_SLANG=ON`) | Private (`NoxRHI::Core`, delay-loaded `slang.dll`) | [shader-slang/slang](https://github.com/shader-slang/slang) |
| **vcpkg** + **SDL3** + **stb** | `x64-windows` | Window/event loop & `stb_image` texture loader for examples | `examples/` only (`NRI_BUILD_EXAMPLES=ON`) | [microsoft/vcpkg](https://github.com/Microsoft/vcpkg) |
| **Dear ImGui** | `docking` branch | Docking + multi-viewport UI demo (`ImGuiDemo` via Layer 2 native wrappers) | `examples/ImGuiDemo/` only | [ocornut/imgui](https://github.com/ocornut/imgui) |
| **NVIDIA Streamline SDK** | `v2.14.1` | DLSS Super Resolution & Ray Reconstruction (`NRI_ENABLE_STREAMLINE=ON`) | Private (`NoxRHI::Core`, delay-loaded `sl.interposer.dll`) | [NVIDIA-RTX/Streamline](https://github.com/NVIDIA-RTX/Streamline) |
| **NVIDIA NRD** (+ **NRI**) | `v4.17.3` (`NRI v179`) | Real-time denoising: SIGMA / REBLUR / RELAX (`NRI_ENABLE_NRD=ON`) | Private (`NoxRHI::Core`, delay-loaded `NRD.dll` + `NRI.dll`) | [NVIDIA-RTX/NRD](https://github.com/NVIDIA-RTX/NRD) |

> **D3D12 Shader Compilation Note:** Compiling Slang shaders to DXIL (`--d3d12`) at runtime uses `dxcompiler.dll` / `dxil.dll` (from the Windows SDK or Vulkan SDK on `PATH` / beside the executable).

---

## Quick Start (Fresh Clone)

### 1. Install Dependencies into `vendors/`
```bat
scripts\install_dependencies.bat
```
This downloads/bootstraps `VulkanMemoryAllocator-Hpp`, `D3D12MemoryAllocator`, `Slang`, `Dear ImGui (docking)`, `vcpkg` + `sdl3` + `stb`, `NVIDIA Streamline SDK (2.14.1)`, and builds `NVIDIA NRD (v4.17.3)` into `vendors/` (gitignored). `vcpkg` caches built packages in `%TEMP%\vcpkg-cache` so subsequent runs restore quickly.

### 2. Generate the Solution (`.slnx`) for Rider / Visual Studio
```bat
:: Configure with Visual Studio 2026 + ClangCL (default), generating build\NoxRHI.slnx:
scripts\setup_build.bat

:: To configure with default MSVC instead of ClangCL:
scripts\setup_build.bat --msvc

:: To pass custom CMake flags (e.g. static library):
scripts\setup_build.bat -DNRI_STATIC_LIBRARY=ON
```

Open **`build\NoxRHI.slnx`** in **JetBrains Rider** (or Visual Studio 2026) and build/run any target (`HeadlessNone`, `HeadlessRender`, `ClearScreen`, `RenderGraphTriangle`, `TexturedQuads`, `ImGuiDemo`, `StreamlineNRDDemo`).

### 3. Example CLI Flags
All examples default to the no-GPU backend (`NRI_GRAPHICS_API_NONE`); pass `--vk` (Vulkan) or `--dx12` / `--d3d12` (Direct3D 12) to run on a GPU backend. Windowed examples (`ClearScreen`, `RenderGraphTriangle`, `TexturedQuads`, `ImGuiDemo`) use SDL3 main callbacks (`SDL_MAIN_USE_CALLBACKS`), `HeadlessNone` runs solely on `NRI_GRAPHICS_API_NONE`, and `HeadlessRender` / `StreamlineNRDDemo` run windowless (`NRI_WINDOW_SYSTEM_HEADLESS`):
```bat
build\Debug\HeadlessNone.exe
build\Debug\HeadlessRender.exe --vk
build\Debug\StreamlineNRDDemo.exe --vk --frames 5
build\Debug\ClearScreen.exe --vk --frames 10 --resize-test
build\Debug\RenderGraphTriangle.exe --vk --frames 10 --resize-test
build\Debug\TexturedQuads.exe --vk --frames 10 --resize-test
build\Debug\ImGuiDemo.exe --vk --frames 10 --resize-test
:: Use --dx12 (or --d3d12) to run on Direct3D 12:
build\Debug\StreamlineNRDDemo.exe --dx12 --frames 5
build\Debug\ImGuiDemo.exe --dx12 --frames 10 --resize-test
```

---

## CMake Options

| Option | Default | Description |
|---|---|---|
| `NRI_STATIC_LIBRARY` | `OFF` | Build `NoxRHI::Core` as a static `.lib` instead of a shared `.dll` |
| `NRI_BUILD_HPP` | `ON` | Provide header-only C++20 `NoxRHI::HPP` interface target |
| `NRI_BUILD_RENDERGRAPH` | `ON` | Build static `NoxRHI::RenderGraph` library (requires `NRI_BUILD_HPP=ON`) |
| `NRI_ENABLE_VULKAN` | `ON` | Enable Vulkan 1.3/1.4 backend |
| `NRI_ENABLE_D3D12` | `ON` | Enable Direct3D 12 backend (Windows) |
| `NRI_ENABLE_NONE` | `ON` | Enable No-GPU (`NRI_GRAPHICS_API_NONE`) backend |
| `NRI_ENABLE_SLANG` | `ON` | Enable Slang shader compiler extension |
| `NRI_ENABLE_STREAMLINE` | `ON` | Enable NVIDIA Streamline (DLSS-SR / DLSS-RR) extension |
| `NRI_ENABLE_NRD` | `ON` | Enable NVIDIA NRD (SIGMA / REBLUR / RELAX) denoiser extension |
| `NRI_ENABLE_PCH` | `ON` | Use private precompiled headers inside `NoxRHI::Core` for faster builds |
| `NRI_BUILD_EXAMPLES` | `ON` | Build `HeadlessNone`, `HeadlessRender`, `StreamlineNRDDemo`, `ClearScreen`, `RenderGraphTriangle`, `TexturedQuads`, and `ImGuiDemo` |

---

## Known Limitations

### Vulkan Validation Layers: Synchronization Validation (`vkconfig` Sync Mode) + Sparse Resources
When sparse buffer residency/binding (`sparseBinding` + `sparseResidencyBuffer` / `vkQueueBindSparse`) is enabled on Vulkan and **Synchronization Validation** (`SYNC-*`, e.g., via the `vkconfig` *Synchronization* preset or `VK_VALIDATION_VALIDATE_SYNC=1`) is turned on, `VK_LAYER_KHRONOS_validation` may report `SYNC-*` false-positive errors on sparse buffers.

This is a documented limitation of Khronos `Vulkan-ValidationLayers` rather than an application bug — their official documentation (`docs/syncval_usage.md`, *"Known Limitations"*) explicitly states:
> *"No dedicated support for sparse resources"*

**Why NoxRHI does not suppress these messages:**
Following the same approach as upstream RHI layers (such as NVIDIA NRI), `NoxRHI` intentionally leaves native Vulkan validation callbacks unfiltered so genuine synchronization hazards—and future validation coverage once Khronos adds sparse SyncVal support—are never silently masked. When validating sparse workloads on Vulkan, use standard **Core Validation** (disable Synchronization Validation) or filter sparse-specific `SYNC-*` IDs locally inside `vkconfig`.
