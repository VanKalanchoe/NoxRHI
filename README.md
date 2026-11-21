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

Five windowed examples, each drawn from **one** Slang file with no `#ifdef` of its own. Run each twice and compare: the window title names
the API (and shows the fps and any live numbers).

```bat
build\Debug\GpuParticles.exe --vk          build\Debug\GpuParticles.exe --dx12
build\Debug\BindlessScene.exe --vk         build\Debug\BindlessScene.exe --dx12
build\Debug\MeshletCulling.exe --vk        build\Debug\MeshletCulling.exe --dx12
build\Debug\RayQueryShadows.exe --vk       build\Debug\RayQueryShadows.exe --dx12
build\Debug\PathTracer.exe --vk            build\Debug\PathTracer.exe --dx12         (--dlss-rr: Streamline on, DLSS-RR mode)
:: --frames N: render N frames and exit (non-zero if NoxRHI, the D3D12 debug layer / Vulkan validation or the render graph reported an error)
```

| Example | What it shows (both APIs, same shader) | Keys |
|---|---|---|
| `GpuParticles` | a compute shader moves 131072 particles in a GPU buffer (`RWBufferRef`); a vertex shader pulls them from it (`BufferRef`, no vertex buffers) as additive billboards; the render graph orders the compute write before the vertex read | Space: pause |
| `BindlessScene` | 144 instanced cubes, each with its own texture from a bindless table (a non-uniform index: `SampleNonUniform`); per-frame constants from a CPU-written buffer; HDR target + reverse-Z depth; compute tonemap into a storage texture; fullscreen present | Space: pause |
| `MeshletCulling` | task + mesh shaders: the task shader culls 576 spheres x 38 meshlets (frustum + normal cone), the mesh shader reads **byte-packed** triangle indices with `LoadByte`; the drawn count is read back into the title | F: freeze culling, C: colours, Space |
| `RayQueryShadows` | a rasterized scene whose fragment shader casts shadow and ambient-occlusion rays with `RayQuery` against a TLAS rebuilt every frame (one per frame in flight) | S: shadows, A: AO rays, Space |
| `PathTracer` | a 1-sample-per-pixel path tracer (ray queries in a compute shader: sun + sky, a diffuse and a specular bounce) denoised by **NRD** (REBLUR) or **DLSS Ray Reconstruction** (Streamline), next to an accumulated reference; shows each denoiser's input guides | 1 reference, 2 noisy, 3 NRD, 4 DLSS-RR, G guides, Space, +/- |

`examples/Common` holds what they share: `ExampleApp.h` (window, API choice, device, swapchain, descriptor heaps, render graph, frame loop,
uploads, buffer references), `ExampleMath.h` (column-major matrices, reverse-Z projection) and **`nox_gpu.slang`**, the one header that hides
what differs between the two APIs' shaders. CI runs all five on D3D12 (WARP, debug layer on) for 30 frames on every push, plus
`MeshletCulling --indirect`, `MeshletCulling --shader-checks` and PathTracer's noisy mode and guides (CI builds without NRD / Streamline).

Options every cross-API example takes: `--vk` / `--dx12`, `--frames N` (quit after N frames: the exit code says whether anything failed),
`--vsync`, `--probe` (print what a render-graph texture holds) and `--shader-checks` (below). `MeshletCulling` also takes `--no-cull`
(every meshlet drawn) and `--indirect` (the group count read by the GPU from an argument buffer -- `drawMeshTasksIndirect`, as the Nox
engine draws -- instead of a direct `drawMeshTasks`).

### PathTracer: NRD and DLSS Ray Reconstruction on both APIs

`PathTracer` writes, every frame, exactly what each denoiser's headers ask for (NRD 4.17 `NRD.hlsli` / `NRDSettings.h`, Streamline
2.14 `sl_dlss_d.h` as NoxRHI's Streamline context feeds it):

| Input | NRD (REBLUR_DIFFUSE_SPECULAR) | DLSS-RR |
|---|---|---|
| depth | view-space Z, `R32_SFLOAT` (either sign; the sky beyond `denoisingRange`) | reverse-Z hardware depth (`depthInverted` is set), `R32_SFLOAT` |
| motion | 2D, uv units, `prevUv = uv + mv`, `RG16F` (`motionVectorScale` 1, 1, 0) | the same texture (`mvecScale` 1, 1) |
| normal + roughness | `NRD_FrontEnd_PackNormalAndRoughness` (encoding 2: `R10G10B10A2_UNORM`), linear roughness | world normal + linear roughness, `RGBA16F` (packed mode) |
| signal | demodulated diffuse / specular radiance (material colour divided out) + the bounce's hit distance normalized by `REBLUR_FrontEnd_GetNormHitDist` (A, B, C = 3, 0.1, 20, the values REBLUR is given) | the noisy colour (`RGBA16F`), diffuse + specular albedo, the specular hit distance in world units (`R16F`) |
| camera | column-major view / projection, current and previous (no jitter) | the same (NoxRHI transposes for Streamline); DLAA, no jitter |

Run it, press **G** to step through the guides (normal, roughness, view Z, motion, albedos, hit distance, depth) and check them by eye,
then compare **3** (NRD) and **4** (DLSS-RR) with **1** (the reference: press Space to stop the camera and let it accumulate). NRD needs
the NRD SDK (`vendors/NRD`, from `scripts\install_dependencies.bat`) when NoxRHI is built -- its shader header is compiled into the path
tracer (`NOX_HAS_NRD`). DLSS-RR needs the Streamline SDK, `--dlss-rr` (Streamline must be loaded before the device is created) and an
NVIDIA RTX GPU. Without them those modes show the noisy image and say why. Tested: the path tracer, its guides and the reference compile
and validate for both APIs (with and without NRD's header) and run in CI on D3D12; NRD and DLSS-RR run only on a machine with the SDKs
and an NVIDIA GPU -- not tested here.

**Streamline with `VK_EXT_descriptor_heap` (Streamline issues #109 and #122).** On Vulkan with descriptor heaps, DLSS reads none of the
tagged images (a black result) unless a classic descriptor set is bound on the command buffer right before `slEvaluateFeature`. NVIDIA
acknowledged it (#109, labels `bug` + `ack`, nvbug 6436675) and has not fixed it yet; the empty descriptor set is the known workaround,
and NoxRHI's Streamline context binds one before every evaluate (`StreamlineContext.cpp`) -- it was not a mistake in the Nox engine. It
cannot help DLSS frame generation (DLSS-G has no evaluate call on the application side). NVIDIA's own samples avoid Streamline here: NRI
calls NGX directly for DLSS-SR / DLSS-RR (`NRIUpscaler.h`) and supports `VK_EXT_descriptor_heap`; an NGX path in NoxRHI would do the same.

**After NRD or DLSS, NoxRHI binds its state again.** Both record into NoxRHI's command buffer with their own pipelines and descriptor
sets (NRD on D3D12 also its own descriptor heaps and root signature), and Streamline runs with manual hooking and no command-list state
tracking, so it restores nothing (NVIDIA's NRD sample calls `RestoreBindings` for the same reason). The extensions now call
`restoreBindingsAfterExternalWork` after their dispatches: the descriptor heaps (and on D3D12 the root signature) are bound again and the
cached pipeline is forgotten -- on D3D12 a pass after NRD in the same command list would otherwise read through NRD's heap, or run NRD's
last shader when it bound the same pipeline as before. Code that records its own native commands calls
`nriCmdRestoreBindingsAfterExternalWork` (or `CommandBuffer::restoreBindingsAfterExternalWork`) the same way.

### What works the same on both APIs, with nothing to set

- **Matrices:** plain `float4x4` and `mul(M, v)` everywhere (porting note 6) -- column-major on the C++ side, as glm and NVIDIA's MathLib.
- **Buffers:** `BufferRef` / `RWBufferRef` (an address on Vulkan, a descriptor on D3D12); structs inside buffers have one layout.
- **Push data:** `uint64_t` references + a `property` (D3D12's constant-buffer packing); NoxRHI reports a push struct that differs.
- **Shaders:** each Vulkan shader gets the SPIR-V of its own entry point (porting note 10), as D3D12 does.
- **External libraries:** NoxRHI's bindings come back after NRD / Streamline (above).
- **Bugs that only crash Vulkan:** `--shader-checks` (below) reports the out-of-range index D3D12's bounds checks hide.

### Shader checks (`--shader-checks`): out-of-range indices reported instead of a lost device

On Vulkan a buffer reference is a raw device address (BDA), so an out-of-range index faults the GPU: `ErrorDeviceLost`, often a frozen
PC, and no message. D3D12 reads the same buffer through a bounds-checked descriptor and gets 0 -- the bug runs "fine" there. (This is how
MeshletCulling's Vulkan crash hid; porting note 10.) `nox_gpu.slang` has two checks for any shader:

```slang
NOX_CHECK(meshlet.vertexCount <= kMaxVertices, meshlet.vertexCount, item); // false: a failure is counted; the first one's line and values are reported
const uint i = NoxIndex(index, count); // index when below count, else 0 (reported with index and count): the read stays in range
```

Without the define `NOX_SHADER_CHECKS` both compile to nothing (`NoxIndex(i, n)` is `i`), so they can stay in shipping shaders. With
`--shader-checks` the example host turns them on and prints the first failure, then counts:

```
[MeshletCulling] shader checks on (report: image slot 0; self-test passed)
[MeshletCulling] SHADER CHECK FAILED: NOX_CHECK / NoxIndex at line <line> of its .slang file -- values <a>, <b> (<n> failure(s) so far)
```

A failure counts as an error (the exit code fails, so CI catches it). How it works, to do the same in another app (the Nox engine):
1. Create a small `NRI_FORMAT_R32_SINT` storage texture (64 x 1: one 256-byte row, what D3D12's texture copies need), zero it, and
   register it **first** in the resource heap (`nriRegisterStorageTextureMip`): storage images sit in the heap's image range on both
   APIs, so every shader reaches it by slot, with no push data. (Not a buffer: on Vulkan NoxRHI's buffer descriptors sit after the
   images, and shaders reach buffers by address.)
2. Before any shader compiles: `nriShaderCompilerSetDefine(compiler, "NOX_SHADER_CHECKS", "1")` and `"NOX_CHECKS_SLOT"` = that slot
   (+ `nriGetDescriptorHeapImageIndexOffset`). Defines are part of the shader cache key, so checked and unchecked binaries never mix.
3. Each frame, after the passes: a barrier from shader writes (all shader stages) to transfer read, `nriTextureCopyToBuffer` into a
   readback buffer, and a barrier back to shader access; read it once the frame's fence has passed. Texels: [0] failures, [1] the
   first failure's line, [2] / [3] its values.
4. Self-test it once: `examples/Common/nox_checks_selftest.slang` fails a check on purpose; expect 1 failure with 12345 and 678. The
   example host does all of this (`ExampleApp.h`, `EnableShaderChecks`).

The line is that of the `.slang` file holding the check (which file is not recorded: give checks distinct values to tell them apart).
The checks cost atomics only when they fail, plus the compare; a frame with checks on runs slower and is for finding bugs. The Vulkan
validation layers' GPU-assisted validation is the other tool, but it did not report MeshletCulling's bad index before the device was lost.

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
6. **Matrices: plain `float4x4` and `mul(M, v)`, nothing to set.** NoxRHI compiles every shader with Slang's column-major matrix layout --
   HLSL's own default, and what NVIDIA's NRD sample relies on (its constant buffers declare `float4x4` with no `row_major` /
   `column_major` keyword, and the C++ side copies its column-major MathLib matrices straight in). So a `float4x4` is four columns in
   memory on both APIs, whether it comes from push data, a buffer reference or a struct inside a buffer: Vulkan loads it through a
   `ColMajor` pointer, D3D12 assembles it from `ByteAddressBuffer` loads with the same column order. Write column-major matrices on the
   C++ side (glm, MathLib, `ExampleMath.h`'s `Mat4`) and multiply `mul(M, v)`. ShaderCompat's `csMatrix` (a `float4x4` read straight from
   a buffer) and `csStructLayout` (a `float4x4` member) check it on both APIs. (The examples used a hand-written four-column `Mat4` before
   this was verified; it is gone.)
7. **Small differences to know:** `SV_InstanceID` does not include the draw's first instance on D3D12 (Vulkan's does); wave sizes differ (WARP 4,
   GPUs 16 / 32 / 64), so a group that relies on one wave per group must use groupshared memory instead; push data is at most 128 bytes on D3D12
   (32 root constants).
8. **NoxRHI on D3D12 today:** `fillBuffer` records nothing (copy from a zeroed buffer) and `bindVertexBuffer` has a fixed 36-byte stride (pull
   vertices from a buffer, as all four examples do). Indirect mesh draws (`drawMeshTasksIndirect`, `drawMeshTasksIndirectCount`, which the
   engine's meshlet path uses) run as `ExecuteIndirect` with a DispatchMesh command signature; the argument records are the same 12 bytes as
   Vulkan's `VkDrawMeshTasksIndirectCommandEXT`. The by-device-address form has no D3D12 equivalent (it logs an error).
   **Buffers the GPU writes with atomics must be `STORAGE_STATIC`** (VRAM). `STORAGE` is host-visible, mapped memory on Vulkan (possibly
   system RAM) but VRAM on D3D12, so a counter there works on D3D12 and can hang the device on Vulkan: MeshletCulling's task-shader counter
   did, on an NVIDIA GPU, until it moved to `STORAGE_STATIC` (the engine's cluster statistics already use it).
9. **Draw index in indirect draws.** The engine's task shader reads `SV_DrawIndex` (Vulkan's `DrawIndex`). D3D12 has no draw index (dxc rejects `SV_DrawIndex`): the usual
   way is a root constant per indirect record, set by the command signature next to the dispatch arguments. NoxRHI's signature holds only the
   dispatch arguments today, so a shader that reads `SV_DrawIndex` needs that added before it runs on D3D12.
10. **One entry point per SPIR-V module (the MeshletCulling device loss).** NoxRHI used to create every Vulkan shader from the *whole
    file's* SPIR-V (all entry points, picked by name). With two task shaders in one file, an NVIDIA driver handed MeshletCulling's mesh
    shaders garbage instead of the task payload: a check of every index (now `--shader-checks`) caught ~7,300 groups a frame reading the float 19.94 (a vertex's clip-space
    `w`) as a meshlet index, and without the check the GPU faulted (device lost). D3D12 compiles each entry point by itself and was fine;
    so was the engine-design task shader, alone in its file. NoxRHI now loads and links a file once and gives each Vulkan shader the SPIR-V
    of its own entry point only (`ShaderCompiler::compileEntryPoints`, Slang's `getEntryPointCode`); the shader cache key changed with it.
    This also avoids a Slang bug: in one module, task shaders whose payloads have different types get the task payload storage class
    only for the first type (invalid SPIR-V). **The Nox engine's NRI still compiles whole modules** -- safe today only because each of its
    files has one task shader; port this change before a file holds several.

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
| `NRI_BUILD_EXAMPLES` | `ON` | Build `ShaderCompat`, `HeadlessNone`, `HeadlessRender`, `StreamlineNRDDemo`, `ClearScreen`, `RenderGraphTriangle`, `TexturedQuads`, `ImGuiDemo`, `GpuParticles`, `BindlessScene`, `MeshletCulling`, `RayQueryShadows` and `PathTracer` |

---

## Known Limitations

### Vulkan Validation Layers: Synchronization Validation (`vkconfig` Sync Mode) + Sparse Resources
When sparse buffer residency/binding (`sparseBinding` + `sparseResidencyBuffer` / `vkQueueBindSparse`) is enabled on Vulkan and **Synchronization Validation** (`SYNC-*`, e.g., via the `vkconfig` *Synchronization* preset or `VK_VALIDATION_VALIDATE_SYNC=1`) is turned on, `VK_LAYER_KHRONOS_validation` may report `SYNC-*` false-positive errors on sparse buffers.

This is a documented limitation of Khronos `Vulkan-ValidationLayers` rather than an application bug — their official documentation (`docs/syncval_usage.md`, *"Known Limitations"*) explicitly states:
> *"No dedicated support for sparse resources"*

**Why NoxRHI does not suppress these messages:**
Following the same approach as upstream RHI layers (such as NVIDIA NRI), `NoxRHI` intentionally leaves native Vulkan validation callbacks unfiltered so genuine synchronization hazards—and future validation coverage once Khronos adds sparse SyncVal support—are never silently masked. When validating sparse workloads on Vulkan, use standard **Core Validation** (disable Synchronization Validation) or filter sparse-specific `SYNC-*` IDs locally inside `vkconfig`.
