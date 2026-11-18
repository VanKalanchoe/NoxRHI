# NoxRHI

A standalone C++20 Rendering Hardware Interface (**Vulkan** & **Direct3D 12**) and **RenderGraph** library with a pure C ABI boundary.

Public headers expose **zero** backend SDK headers (`<vulkan/...>`, `<d3d12.h>`), engine headers, or windowing libraries. Consumers can link statically or consume the C ABI DLL across compiler toolchains (e.g., an MSVC application linking against a ClangCL-built `NoxRHI.dll`).

---

## Architecture (4 Layers)

1. **`include/NRI/` (`NoxRHI::Core`)** — Pure C11 ABI (`extern "C"`, `NRI_API`, opaque handles, POD structs) backed by private Vulkan (`vk::raii` + VMA) and D3D12 (`ComPtr` + D3D12MA) implementations inside `src/Core/`.
2. **`include/NRI/Extensions/` (`NoxRHI::Core`)** — Modular C ABI extensions:
   - `NRIDeviceCreation.h`, `NRISwapchain.h`, `NRIRayTracing.h`, `NRIMeshShader.h`
   - `NRIGpuProfiler.h`, `NRIShaderCompiler.h`
   - `NRIWrapperVK.h`, `NRIWrapperD3D12.h` (opt-in native interop using opaque `void*` / `uint64_t` handles)
3. **`include/NRIHPP/` (`NoxRHI::HPP`)** — Header-only C++20 RAII wrappers (`namespace nri`) with move-only handles, `std::span`, and `std::string_view`.
4. **`include/NRIRenderGraph/` + `src/RenderGraph/` (`NoxRHI::RenderGraph`)** — Standalone RenderGraph library built on `NoxRHI::HPP` with transient resource pooling, barrier planning, validation, and optional profiler/task hooks.

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
| **vcpkg** + **SDL3** | `x64-windows` | Window/event loop for standalone examples only | `examples/` only (`NRI_BUILD_EXAMPLES=ON`) | [microsoft/vcpkg](https://github.com/Microsoft/vcpkg) |

> **D3D12 Shader Compilation Note:** The prebuilt Slang release provides SPIR-V generation out of the box. Compiling Slang shaders to DXIL (`--d3d12` in `RenderGraphTriangle`) at runtime requires DirectX Shader Compiler (`dxcompiler.dll` and `dxil.dll`) on `PATH` or next to the executable.

---

## Quick Start (Fresh Clone)

### 1. Install Dependencies into `vendors/`
```bat
scripts\install_dependencies.bat
```
This downloads/bootstraps `VulkanMemoryAllocator-Hpp`, `D3D12MemoryAllocator`, `Slang`, and `vcpkg` + `sdl3` into `vendors/` (gitignored). `vcpkg` caches built packages in `%TEMP%\vcpkg-cache` so subsequent runs restore quickly.

### 2. Configure & Build
```bat
:: Configure with Visual Studio 2026 + ClangCL (default) and build Debug:
scripts\build.bat

:: Or configure explicitly first:
scripts\setup_build.bat
scripts\build.bat Debug

:: To configure with default MSVC instead of ClangCL:
scripts\setup_build.bat --msvc

:: To pass custom CMake flags (e.g. static library):
scripts\setup_build.bat -DNRI_STATIC_LIBRARY=ON
```

### 3. Run Standalone Examples
Built binaries and runtime DLLs are placed under `build\Debug\` (or `build\Release\`):
```bat
build\Debug\ClearScreen.exe --frames 10 --resize-test
build\Debug\ClearScreen.exe --d3d12 --frames 10 --resize-test
build\Debug\RenderGraphTriangle.exe --frames 10 --resize-test
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
| `NRI_ENABLE_SLANG` | `ON` | Enable Slang shader compiler extension |
| `NRI_BUILD_EXAMPLES` | `ON` | Build `ClearScreen` and `RenderGraphTriangle` (`SDL3`) |

---

## Known Limitations

### Vulkan Validation Layers: Synchronization Validation (`vkconfig` Sync Mode) + Sparse Resources
When sparse buffer residency/binding (`sparseBinding` + `sparseResidencyBuffer` / `vkQueueBindSparse`) is enabled on Vulkan and **Synchronization Validation** (`SYNC-*`, e.g., via the `vkconfig` *Synchronization* preset or `VK_VALIDATION_VALIDATE_SYNC=1`) is turned on, `VK_LAYER_KHRONOS_validation` may report `SYNC-*` false-positive errors on sparse buffers.

This is a documented limitation of Khronos `Vulkan-ValidationLayers` rather than an application bug — their official documentation (`docs/syncval_usage.md`, *"Known Limitations"*) explicitly states:
> *"No dedicated support for sparse resources"*

**Why NoxRHI does not suppress these messages:**
Following the same approach as upstream RHI layers (such as NVIDIA NRI), `NoxRHI` intentionally leaves native Vulkan validation callbacks unfiltered so genuine synchronization hazards—and future validation coverage once Khronos adds sparse SyncVal support—are never silently masked. When validating sparse workloads on Vulkan, use standard **Core Validation** (disable Synchronization Validation) or filter sparse-specific `SYNC-*` IDs locally inside `vkconfig`.
