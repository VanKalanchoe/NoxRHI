# NoxRHI

[![CI](https://github.com/VanKalanchoe/NoxRHI/actions/workflows/ci.yml/badge.svg)](https://github.com/VanKalanchoe/NoxRHI/actions/workflows/ci.yml)

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
3. **`include/NRIHPP/` (`NoxRHI::HPP`)** — Header-only C++20 RAII wrappers (`namespace nri`, `NRI.hpp`, `NRIExtensions.hpp`) with move-only handles, `std::span`, and `std::string_view`, plus an optional Dear ImGui renderer backend (`imgui_impl_noxrhi.h` with docking + multi-viewport support).
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
| **Slang** | `v2026.22.1` | Runtime shader compiler (`NRI_ENABLE_SLANG=ON`) | Private (`NoxRHI::Core`, delay-loaded `slang.dll`) | [shader-slang/slang](https://github.com/shader-slang/slang) |
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

Open **`build\NoxRHI.slnx`** in **JetBrains Rider** (or Visual Studio 2026) and build/run any target (`HeadlessNone`, `HeadlessRender`, `ClearScreen`, `RenderGraphTriangle`, `TexturedQuads`, `ImGuiDemo`, `StreamlineNRDDemo`, `ShaderCompat`, and the cross-API examples `GpuParticles`, `BindlessScene`, `MeshletCulling`, `RayQueryShadows`).

### 3. Example CLI Flags
All examples default to the no-GPU backend (`NRI_GRAPHICS_API_NONE`); pass `--vk` (Vulkan) or `--dx12` / `--d3d12` (Direct3D 12) to run on a GPU backend. Windowed examples (`ClearScreen`, `RenderGraphTriangle`, `TexturedQuads`, `ImGuiDemo`) use SDL3 main callbacks (`SDL_MAIN_USE_CALLBACKS`), `HeadlessNone` runs solely on `NRI_GRAPHICS_API_NONE`, and `HeadlessRender` / `StreamlineNRDDemo` run windowless (`NRI_WINDOW_SYSTEM_HEADLESS`):
```bat
build\Debug\ShaderCompat.exe --all
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

## Cross-API Shader Compatibility (`ShaderCompat`)

`ShaderCompat` runs one Slang source (`examples/ShaderCompat/shader_compat.slang`) on Vulkan and on Direct3D 12, checks every result
against what the CPU computes, and with `--all` compares the two APIs with each other (integer results must match bit for bit). It is
headless (no window, no swapchain) and exits non-zero on any failure:

```bat
build\Debug\ShaderCompat.exe --all                 :: Vulkan, then D3D12, then "Vulkan vs D3D12"
build\Debug\ShaderCompat.exe --dx12 --only csWave  :: one API, one test
build\Debug\ShaderCompat.exe --vk --verbose        :: also print each step and NoxRHI's info / warnings
set NOXRHI_D3D12_WARP=1 & build\Debug\ShaderCompat.exe --dx12   :: D3D12 on Microsoft's software adapter (WARP), as CI runs it
```

| Test | Checks |
|---|---|
| `csPushLayout` | push data (D3D12 root constants) has the same layout on both APIs |
| `csBufferCopy` | read one buffer, write another |
| `csGroupShared` | `groupshared` memory and `GroupMemoryBarrierWithGroupSync` (a tree sum) |
| `csAtomics` | `InterlockedAdd` / `InterlockedMax` on a buffer |
| `csWave` | `WaveGetLaneCount`, `WavePrefixSum`, `WaveActiveSum` (the wave size may differ between APIs: reported, allowed) |
| `csIntOps` | `countbits`, `firstbithigh` / `firstbitlow` (also of 0), `reversebits`, signed division / modulo, arithmetic shifts, wrapping |
| `csFloatMath` | `sin`, `cos`, `exp2`, `log2`, `sqrt`, `rsqrt`, `pow`, `frac`, `mad` within tolerances; NaN and infinity |
| `csHalf` | 16-bit float math (SKIP when the device has no 16-bit shader ops) |
| `csInt64` | 64-bit integer multiply / add / shift |
| `csMatrix` | `float4x4` conventions: `mul(M, v)`, `mul(v, M)`, `m[row][col]` |
| `csTextureSample` | a bindless `Texture2D` with a nearest and a linear sampler (exact texels, then averages) |
| `csStorageTexture` | `RWTexture2D` writes, copied back to the CPU |
| `csRayQuery` | inline ray tracing (`RayQuery`) against a one-triangle TLAS |
| `gfxOrientation` | a raster triangle in the top-left corner lands at row 0: NDC +Y is up, the image origin is the top-left (NoxRHI flips the viewport the same way on both) |
| `gfxCulling` | back-face culling: the counter-clockwise triangle is drawn, the clockwise one is culled |
| `gfxDepth` | D32 depth: `LESS` test and depth writes with three overlapping quads drawn out of order |
| `gfxBlend` | alpha blending (`SRC_ALPHA`, `ONE_MINUS_SRC_ALPHA`) over a cleared target |
| `gfxInterpolation` | smooth colour interpolation against barycentrics, and a `nointerpolation` value taken from the first vertex |
| `gfxMsaa` | 4x MSAA with the standard sample positions, resolved: every edge pixel's coverage is checked |
| `gfxMeshShader` | a task + mesh shader pipeline draws the orientation triangle (SKIP without mesh shaders) |

**One source, both APIs.** NoxRHI's Slang compiler defines `NOX_VULKAN` for SPIR-V and `NOX_DX12` for DXIL. Textures and samplers use
bindless handles on both (`Texture2D.Handle`, `RWTexture2D.Handle`, `SamplerState.Handle`). Buffers are reached by device address on
Vulkan (`(uint*)push.address`) and by descriptor on D3D12 (`ByteAddressBuffer.Handle(slot)` from `registerBuffer`,
`RWByteAddressBuffer.Handle(slot)` from `registerStorageBuffer`), and a TLAS by address on Vulkan and by descriptor on D3D12
(`registerAccelerationStructure`) -- the same convention NoxUI's shader and the Nox engine use.

The graphics tests render 64 x 64 headless (raster: `vsGfx` / `psGfx`; mesh: `tsGfx` / `msGfx` / `psGfx`) and read the image back.
If the program dies, it says what it was doing (a crash with its exception code, `std::terminate`, `abort`, or a library calling `exit`).

**Found by it and fixed:** D3D12 samplers ignored their description (every sampler was linear with wrap addressing); D3D12 had no
writable buffer descriptor (`registerStorageBuffer` adds a raw UAV); D3D12 pipelines were always single-sampled
(`setRasterizationSamples` was empty) and `resolveImage` used the wrong states; D3D12 depth textures had no usable format;
`recordRegionUploads` on D3D12 did not move the texture to `COPY_DEST`; the D3D12 acceleration-structure size query got no geometry
and builds ignored their flags (no updates, no compaction); D3D12 render targets were used before the Discard / Clear / Copy that placed
memory needs; only `slang.dll` was copied beside the programs, which since Slang 2026.x forwards everything to `slang-compiler.dll`.
CI runs every test on D3D12 (WARP, debug layer on) on each push: all pass. `nriSupportsMeshShaders` says whether `gfxMeshShader` can run. **Known D3D12 gaps still open:** `fillBuffer` records nothing
(clear a buffer with a copy from a zeroed staging buffer instead), and `bindVertexBuffer` uses a fixed 36-byte stride (pull vertices from a
buffer in the shader instead).

## Cross-API Examples (one Slang source, `--vk` or `--dx12`)

Four windowed examples, each drawn from **one** Slang file with no `#ifdef` of its own. Run each twice and compare: the window title names
the API (and shows the fps and any live numbers).

```bat
build\Debug\GpuParticles.exe --vk          build\Debug\GpuParticles.exe --dx12
build\Debug\BindlessScene.exe --vk         build\Debug\BindlessScene.exe --dx12
build\Debug\MeshletCulling.exe --vk        build\Debug\MeshletCulling.exe --dx12
build\Debug\RayQueryShadows.exe --vk       build\Debug\RayQueryShadows.exe --dx12
:: --frames N: render N frames and exit (non-zero if NoxRHI, the D3D12 debug layer / Vulkan validation or the render graph reported an error)
```

| Example | What it shows (both APIs, same shader) | Keys |
|---|---|---|
| `GpuParticles` | a compute shader moves 131072 particles in a GPU buffer (`RWBufferRef`); a vertex shader pulls them from it (`BufferRef`, no vertex buffers) as additive billboards; the render graph orders the compute write before the vertex read | Space: pause |
| `BindlessScene` | 144 instanced cubes, each with its own texture from a bindless table (a non-uniform index: `SampleNonUniform`); per-frame constants from a CPU-written buffer; HDR target + reverse-Z depth; compute tonemap into a storage texture; fullscreen present | Space: pause |
| `MeshletCulling` | task + mesh shaders: the task shader culls 576 spheres x 38 meshlets (frustum + normal cone), the mesh shader reads **byte-packed** triangle indices with `LoadByte`; the drawn count is read back into the title | F: freeze culling, C: colours, Space |
| `RayQueryShadows` | a rasterized scene whose fragment shader casts shadow and ambient-occlusion rays with `RayQuery` against a TLAS rebuilt every frame (one per frame in flight) | S: shadows, A: AO rays, Space |

`examples/Common` holds what they share: `ExampleApp.h` (window, API choice, device, swapchain, descriptor heaps, render graph, frame loop,
uploads, buffer references), `ExampleMath.h` (column-major matrices, reverse-Z projection) and **`nox_gpu.slang`**, the one header that hides
what differs between the two APIs' shaders. CI runs all four on D3D12 (WARP, debug layer on) for 30 frames on every push, and
MeshletCulling's other draw paths (`--indirect`, `--task-engine`, `--task-single`).

`MeshletCulling` start-up options, to narrow down a driver problem: `--no-cull`, `--no-stats` (no counter atomic), `--mesh-only` (no task
shader), `--split-files` (task and mesh shaders compiled from separate files), `--no-readback`, `--no-depth`, `--groups N`,
`--task-single` (a one-thread task shader), `--task-engine` (the Nox engine's task shader design: 64 meshlets per group, bitmask + prefix
compaction, payload = base index + offsets) and `--indirect` (draw with `drawMeshTasksIndirect` from a GPU argument buffer, as the engine
does). The first line it prints names the path it took.

### Porting Vulkan-only Slang (such as the Nox engine's shaders) to D3D12

What has to change, found by going through the engine's shaders. Textures and samplers (`Texture2D.Handle(...)`, `SamplerState.Handle(...)`)
already work on both and need nothing.

0. **Push data layout (the one that bites silently).** On D3D12, push data is a constant buffer (root constants) with HLSL's packing
   rules: a **struct member starts a new 16-byte row** (and so does the member after it), and a vector may not cross a 16-byte boundary.
   Vulkan (scalar layout) packs tightly. So `struct Push { BufferRef<A> a; BufferRef<B> b; }` sits at 0 / 8 on Vulkan but 0 / 16 on
   D3D12: the shader reads the wrong bytes, with no error. Keep structs out of push data: store references as plain `uint64_t` and
   expose the typed reference with a Slang `property` (no space):
   `uint64_t aRef; property BufferRef<A> a { get { return BufferRef<A>(aRef); } }`. All four examples do this; their push layouts were
   checked in the DXIL against the C++ structs. (Structs *inside buffers*, read through `BufferRef`, use the C-like layout on both.)
   **NoxRHI reports it:** every D3D12 compile compares the entry point's push data with Vulkan's scalar layout and logs an error naming
   the first field that moved (`Push data of 'csFoo' (file.slang) differs between the APIs: 'pc.b' is at byte 16 on D3D12 but byte 8 on
   Vulkan ...`) -- `src/Extensions/ShaderCompiler/PushLayoutCheck.h`.
1. **Buffer pointers.** D3D12 shaders have no pointers. Every `(T*)push.someReference` (the engine reaches its UBO, lights, instances, meshlets
   and particles this way) becomes a `BufferRef<T>` (read) or `RWBufferRef<T>` (write) from `nox_gpu.slang`: a 64-bit value that is the device
   address on Vulkan and a descriptor slot on D3D12 (`ByteAddressBuffer` / `RWByteAddressBuffer` handles). The push data layout does not change
   (still one `uint64` per buffer), and structs that hold references to other buffers keep working. `ref[i]`, `ref.Load(i)` and `ref.Store(i, v)`
   replace pointer indexing; pointer arithmetic and casts between types become index math on the right element type.
2. **Read and write references differ on D3D12.** A buffer that a pass reads is in a shader-resource state, where only an SRV may read it; a
   pass that writes it uses a UAV. On the host, `Host::Refs(buffer, size, writable)` registers both (`registerBuffer` /
   `registerStorageBuffer`) on D3D12 and returns the address twice on Vulkan. The engine registers no buffers today; on D3D12 it must.
3. **No 8-bit types in DXIL.** `uint8_t` locals, arrays, payload members and `uint8_t*` streams (the engine's `ClusterIndices`,
   `VisibilityBuffer`, `Material_PBR_MeshTask` and its `MeshletPayload::meshletIndices`) do not compile for D3D12. Read bytes out of 32-bit
   words with `LoadByte` (identical on Vulkan), and keep payload indices as 32-bit values or pack four per `uint`. 16-bit types are fine.
4. **TLAS by address.** `RaytracingAccelerationStructure(address)` is Vulkan-only; use `LoadTlas(ref)` (D3D12: a descriptor from
   `nriRegisterAccelerationStructure`).
5. **Non-uniform texture indices.** An index that differs inside a wave (per-instance or per-pixel materials) needs `NonUniformResourceIndex`.
   Slang carries it into DXIL, but for Vulkan's descriptor heap it emits no `NonUniform` decoration (checked with Slang 2026.14.1 and 2026.22.1),
   which this Vulkan path needs. `SampleNonUniform` scalarizes on Vulkan until Slang emits it; this applies to the engine's Vulkan path today too.
6. **Matrices in buffers.** Use four `float4` columns (`Mat4` in `nox_gpu.slang`, multiplied with explicit columns) rather than relying on a
   matrix layout setting meaning the same for every kind of buffer on both APIs.
7. **Small differences to know:** `SV_InstanceID` does not include the draw's first instance on D3D12 (Vulkan's does); wave sizes differ (WARP 4,
   GPUs 16 / 32 / 64), so a group that relies on one wave per group must use groupshared memory instead; push data is at most 128 bytes on D3D12
   (32 root constants).
8. **NoxRHI on D3D12 today:** `fillBuffer` records nothing (copy from a zeroed buffer) and `bindVertexBuffer` has a fixed 36-byte stride (pull
   vertices from a buffer, as all four examples do). Indirect mesh draws (`drawMeshTasksIndirect`, `drawMeshTasksIndirectCount`, which the
   engine's meshlet path uses) run as `ExecuteIndirect` with a DispatchMesh command signature; the argument records are the same 12 bytes as
   Vulkan's `VkDrawMeshTasksIndirectCommandEXT`. The by-device-address form has no D3D12 equivalent (it logs an error).
9. **Draw index in indirect draws.** The engine's task shader reads `SV_DrawIndex` (Vulkan's `DrawIndex`). D3D12 has no draw index (dxc rejects `SV_DrawIndex`): the usual
   way is a root constant per indirect record, set by the command signature next to the dispatch arguments. NoxRHI's signature holds only the
   dispatch arguments today, so a shader that reads `SV_DrawIndex` needs that added before it runs on D3D12.
10. **Slang: one payload type per file for task shaders.** In a file with several task shaders whose payloads are of *different* types,
    Slang (2026.22.1) gives only the first type's payload the task payload storage class; the others become plain groupshared memory, which
    is invalid SPIR-V (each entry point compiled alone is valid). NoxRHI compiles a whole file into one SPIR-V module on Vulkan, so keep
    such task shaders in separate files (MeshletCulling's `meshlet_task_engine.slang`). Task shaders sharing one payload type, and mesh
    shaders reading different payload types, compile correctly.

Struct layout through `BufferRef` is the same on both APIs (C-like / scalar: no `float3` padding, 64-bit members 8-byte aligned);
ShaderCompat's `csStructLayout` checks it at runtime.

### Building on Linux (Vulkan only)
The Vulkan backend, the RenderGraph and the examples build on Linux with GCC or Clang. Put the Linux Slang release
(`slang-<version>-linux-x86_64.tar.gz`) in `vendors/slang`, VulkanMemoryAllocator-Hpp in `vendors/VulkanMemoryAllocator-Hpp`, and point
CMake at Vulkan headers matching the tested SDK (1.4.350):

```sh
cmake -S . -B build_linux -G Ninja -DNRI_ENABLE_STREAMLINE=OFF -DNRI_ENABLE_NRD=OFF \
      -DVulkan_INCLUDE_DIR=<Vulkan-Headers>/include -DSDL3_DIR=<SDL3>/lib/cmake/SDL3
cmake --build build_linux
```

Running needs a driver with `VK_EXT_descriptor_heap`, `VK_KHR_unified_image_layouts` and `VK_KHR_shader_untyped_pointers` (Mesa 25.2's
lavapipe has none of them). The windowed examples create a Win32 surface, so on Linux only the headless examples (`ShaderCompat`,
`HeadlessRender`, `HeadlessNone`) run.

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
| `NRI_BUILD_EXAMPLES` | `ON` | Build `ShaderCompat`, `HeadlessNone`, `HeadlessRender`, `StreamlineNRDDemo`, `ClearScreen`, `RenderGraphTriangle`, `TexturedQuads`, `ImGuiDemo`, `GpuParticles`, `BindlessScene`, `MeshletCulling` and `RayQueryShadows` |

---

## Known Limitations

### Vulkan Validation Layers: Synchronization Validation (`vkconfig` Sync Mode) + Sparse Resources
When sparse buffer residency/binding (`sparseBinding` + `sparseResidencyBuffer` / `vkQueueBindSparse`) is enabled on Vulkan and **Synchronization Validation** (`SYNC-*`, e.g., via the `vkconfig` *Synchronization* preset or `VK_VALIDATION_VALIDATE_SYNC=1`) is turned on, `VK_LAYER_KHRONOS_validation` may report `SYNC-*` false-positive errors on sparse buffers.

This is a documented limitation of Khronos `Vulkan-ValidationLayers` rather than an application bug — their official documentation (`docs/syncval_usage.md`, *"Known Limitations"*) explicitly states:
> *"No dedicated support for sparse resources"*

**Why NoxRHI does not suppress these messages:**
Following the same approach as upstream RHI layers (such as NVIDIA NRI), `NoxRHI` intentionally leaves native Vulkan validation callbacks unfiltered so genuine synchronization hazards—and future validation coverage once Khronos adds sparse SyncVal support—are never silently masked. When validating sparse workloads on Vulkan, use standard **Core Validation** (disable Synchronization Validation) or filter sparse-specific `SYNC-*` IDs locally inside `vkconfig`.
