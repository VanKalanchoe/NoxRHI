#pragma once
// ExampleApp.h -- what every cross-API example shares: an SDL window, the API picked on the command line, the device, swapchain,
// descriptor heaps, shader compiler and render graph, the frame loop, and the host side of nox_gpu.slang's buffer references.
//
//   <Example>.exe --vk | --dx12 [--frames N] [--vsync] [--tearing] [--verbose]
//
// Without --vsync the frame rate is uncapped and the newest finished frame is shown without tearing (Vulkan MAILBOX, D3D12 Present(0, 0));
// --tearing allows tearing instead (Vulkan IMMEDIATE, D3D12 DXGI_PRESENT_ALLOW_TEARING), as NVIDIA's NRI with SwapChainBits::ALLOW_TEARING.
//
// The window title names the API, so two runs side by side show the same Slang source on both. --frames N renders N frames and exits
// (non-zero when NoxRHI or the render graph reported an error), for smoke tests.
//
// An example derives from nox::Example and ends its main.cpp with NOX_EXAMPLE_MAIN(TheExample).

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <NRIHPP/NRI.hpp>
#include <NRIHPP/NRIExtensions.hpp>
#include <NRIRenderGraph/RenderGraph.h>

#include "ExampleMath.h"

namespace nox
{
    inline int g_Errors = 0;
    inline bool g_Verbose = false; // (--verbose: NoxRHI's information lines too, e.g. the swapchain's present mode)

    inline void NRI_CALL OnNriLog(NriLogLevel level, const char* message, void*)
    {
        if (level == NRI_LOG_INFO)
        {
            if (g_Verbose)
                std::fprintf(stderr, "[NoxRHI][INFO] %s\n", message);
            return;
        }
        std::fprintf(stderr, "[NoxRHI][%s] %s\n", level == NRI_LOG_ERROR ? "ERROR" : "WARN", message);
        if (level == NRI_LOG_ERROR)
            ++g_Errors;
    }

    inline void OnGraphLog(nri::RGLogLevel level, std::string_view message, void*)
    {
        if (level == nri::RGLogLevel::Info)
            return;
        std::fprintf(stderr, "[RenderGraph][%s] %.*s\n", level == nri::RGLogLevel::Error ? "ERROR" : "WARN", static_cast<int>(message.size()), message.data());
        if (level == nri::RGLogLevel::Error)
            ++g_Errors;
    }

    constexpr uint32_t kFramesInFlight = 2;

    // Sampler heap slots every example can use.
    enum SamplerSlot : uint32_t
    {
        kSamplerLinearRepeat = 0,
        kSamplerLinearClamp = 1,
        kSamplerNearestClamp = 2,
    };

    // A buffer as nox_gpu.slang's BufferRef<T> (read) and RWBufferRef<T> (write) see it: the device address on Vulkan (both the same),
    // descriptor slots on D3D12 (an SRV for reading, a UAV for writing -- a pass that reads a buffer has it in a shader-resource state,
    // where D3D12 allows only the SRV). `write` is 0 for buffers that cannot be written (Uniform: CPU-written, upload heap on D3D12).
    struct BufferRefs
    {
        uint64_t read = 0;
        uint64_t write = 0;
    };

    // Fixed-function state set before a draw (everything is dynamic state in NoxRHI; D3D12 picks a matching pipeline variant). Set it in
    // every raster pass: the render graph resets the baseline once per command buffer, and passes in one command buffer inherit each
    // other's state (BindlessScene's present triangle was culled by the scene pass's back-face culling).
    struct RasterState
    {
        bool depthTest = false;
        bool depthWrite = false;
        nri::CompareOp depthCompare = NRI_COMPARE_GREATER; // reverse-Z
        nri::CullMode cull = NRI_CULL_MODE_NONE;
        enum class Blend { Off, Alpha, Additive } blend = Blend::Off;
    };

    struct FrameInfo
    {
        uint64_t frameNumber = 0;
        uint32_t frameSlot = 0;  // 0 .. kFramesInFlight - 1: which per-frame resources the CPU may write now
        uint32_t imageIndex = 0; // the swapchain image to draw into
        nri::Extent2D extent{};
        float time = 0.0f;       // seconds since start
        float deltaTime = 0.0f;  // seconds since the last frame (clamped)
        float Aspect() const { return extent.height ? float(extent.width) / float(extent.height) : 1.0f; }
    };

    class Host;

    class Example
    {
    public:
        virtual ~Example() = default;
        virtual const char* Name() const = 0;
        virtual void Init(Host& host) = 0;
        // Adds this frame's passes; the last one draws into the swapchain image (RGBuilder::SwapchainTarget).
        virtual void Render(Host& host, nri::RenderGraph& graph, const FrameInfo& frame) = 0;
        virtual void Shutdown(Host&) {}
        virtual void OnKey(SDL_Keycode) {}
        // Command-line arguments the host does not know (before Init); return how many were used (0: not this example's).
        virtual int OnArgument(int /*argc*/, char** /*argv*/, int /*index*/) { return 0; }
        // Before the device is created (after the arguments): e.g. desc.enableStreamline for DLSS.
        virtual void ConfigureDevice(nri::DeviceDesc& /*desc*/) {}
    };

    class Host
    {
    public:
        NriGraphicsAPI api = NRI_GRAPHICS_API_VULKAN;
        nri::Device device;
        nri::Swapchain swapchain;
        nri::DescriptorHeap resources;
        nri::DescriptorHeap samplers;
        nri::ShaderCompiler compiler;
        nri::RenderGraph graph;
        bool vsync = false;
        bool tearing = false;

        bool IsD3D12() const { return api == NRI_GRAPHICS_API_D3D12; }
        // Text shown after the API and fps in the window title (and printed when it changes, at most once a second).
        void SetStatus(std::string status) { m_Status = std::move(status); }
        const char* ApiName() const { return IsD3D12() ? "Direct3D 12" : "Vulkan"; }

        // --- shaders and pipelines -------------------------------------------------------------------------------------------------

        std::string ShaderPath(const char* file) const { return (m_BaseDir / file).string(); }

        nri::Pipeline Compute(const char* file, const char* entry)
        {
            nri::Pipeline p = device.createPipeline({ .type = NRI_PIPELINE_COMPUTE, .shaders = { { NRI_SHADER_STAGE_COMPUTE, entry, ShaderPath(file) } },
                                                      .buildNow = true }, compiler.handle());
            device.finishPipelineBuilds();
            return p;
        }

        // Vertex + fragment.
        nri::Pipeline Graphics(const char* file, const char* vertex, const char* fragment, std::vector<nri::ImageFormat> colorFormats = { NRI_FORMAT_SURFACE })
        {
            nri::Pipeline p = device.createPipeline({ .type = NRI_PIPELINE_GRAPHICS,
                                                      .shaders = { { NRI_SHADER_STAGE_VERTEX, vertex, ShaderPath(file) }, { NRI_SHADER_STAGE_FRAGMENT, fragment, ShaderPath(file) } },
                                                      .colorFormats = std::move(colorFormats), .buildNow = true }, compiler.handle());
            device.finishPipelineBuilds();
            return p;
        }

        // Task (optional: nullptr) + mesh + fragment.
        nri::Pipeline Mesh(const char* file, const char* task, const char* mesh, const char* fragment, std::vector<nri::ImageFormat> colorFormats = { NRI_FORMAT_SURFACE })
        {
            nri::PipelineDesc desc{ .type = NRI_PIPELINE_GRAPHICS, .colorFormats = std::move(colorFormats), .buildNow = true };
            if (task)
                desc.shaders.push_back({ NRI_SHADER_STAGE_TASK, task, ShaderPath(file) });
            desc.shaders.push_back({ NRI_SHADER_STAGE_MESH, mesh, ShaderPath(file) });
            desc.shaders.push_back({ NRI_SHADER_STAGE_FRAGMENT, fragment, ShaderPath(file) });
            nri::Pipeline p = device.createPipeline(desc, compiler.handle());
            device.finishPipelineBuilds();
            return p;
        }

        static void SetRasterState(nri::CommandBuffer& cmd, const RasterState& s)
        {
            cmd.setDepthTestEnable(s.depthTest);
            cmd.setDepthWriteEnable(s.depthWrite);
            cmd.setDepthCompareOp(s.depthTest ? s.depthCompare : NRI_COMPARE_ALWAYS);
            cmd.setCullMode(s.cull);
            cmd.setColorBlendEnable(0, s.blend != RasterState::Blend::Off);
            if (s.blend == RasterState::Blend::Alpha)
                cmd.setColorBlendEquation(0, { NRI_BLEND_FACTOR_SRC_ALPHA, NRI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, NRI_BLEND_OP_ADD,
                                               NRI_BLEND_FACTOR_ONE, NRI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, NRI_BLEND_OP_ADD });
            else if (s.blend == RasterState::Blend::Additive)
                cmd.setColorBlendEquation(0, { NRI_BLEND_FACTOR_ONE, NRI_BLEND_FACTOR_ONE, NRI_BLEND_OP_ADD,
                                               NRI_BLEND_FACTOR_ONE, NRI_BLEND_FACTOR_ONE, NRI_BLEND_OP_ADD });
        }

        // --- buffers -------------------------------------------------------------------------------------------------------------

        // A buffer, filled with `data` when given (through a staging buffer for GPU-local usages, mapped for CPU-visible ones).
        nri::Buffer CreateBuffer(uint64_t size, nri::BufferUsage usage, const void* data = nullptr)
        {
            nri::Buffer buffer = device.createBuffer({ .size = size, .usage = usage });
            if (data)
            {
                const bool cpuVisible = usage == NRI_BUFFER_USAGE_UNIFORM || usage == NRI_BUFFER_USAGE_STAGING || usage == NRI_BUFFER_USAGE_ACCELERATION_STRUCTURE_INSTANCE;
                if (cpuVisible)
                {
                    std::memcpy(buffer.map(0, size), data, size);
                    buffer.unmap();
                }
                else
                {
                    nri::Buffer staging = device.createBuffer({ .size = size, .usage = NRI_BUFFER_USAGE_STAGING });
                    std::memcpy(staging.map(0, size), data, size);
                    staging.unmap();
                    Submit([&](nri::CommandBuffer& cmd) {
                        cmd.copyBuffer(staging, buffer, nri::BufferCopyRegion{ 0, 0, size });
                        const nri::BufferBarrierDesc barrier{ buffer.handle(), { NRI_ACCESS_TRANSFER_WRITE, NRI_STAGE_TRANSFER },
                                                              { NRI_ACCESS_SHADER_READ, NRI_STAGE_COMPUTE | NRI_STAGE_FRAGMENT | NRI_STAGE_VERTEX | NRI_STAGE_TASK | NRI_STAGE_MESH } };
                        cmd.resourceBarriers({}, { &barrier, 1 });
                    });
                }
            }
            return buffer;
        }

        // The references a shader needs for `buffer` (see BufferRefs). Call once per buffer: on D3D12 it registers descriptors.
        BufferRefs Refs(const nri::Buffer& buffer, uint64_t size, bool writable)
        {
            if (!IsD3D12())
            {
                const uint64_t address = buffer.getDeviceAddress();
                return { address, writable ? address : 0 };
            }
            BufferRefs refs;
            refs.read = resources.registerBuffer(buffer, size);
            if (writable)
                refs.write = resources.registerStorageBuffer(buffer, size);
            return refs;
        }

        // The TLAS reference for nox_gpu.slang's LoadTlas: its address on Vulkan, a descriptor slot on D3D12.
        uint64_t TlasRef(const nri::AccelerationStructure& tlas)
        {
            return IsD3D12() ? nriRegisterAccelerationStructure(resources.handle(), tlas.handle(), ~0u) : tlas.getDeviceAddress();
        }

        // --- textures ------------------------------------------------------------------------------------------------------------

        // A sampled RGBA8 (sRGB) texture from pixels; returns the slot a shader passes to Tex2D (image heap offset included).
        uint32_t CreateTexture(uint32_t width, uint32_t height, const uint32_t* rgba, std::vector<nri::Texture>& keep)
        {
            nri::Texture texture = device.createTexture({ .width = width, .height = height, .usage = NRI_TEXTURE_USAGE_SHADER_RESOURCE, .format = NRI_FORMAT_SRGBA8 });
            const uint64_t rowPitch = (uint64_t(width) * 4 + 255) & ~255ull; // (256-byte rows: what D3D12 needs, honoured by Vulkan too)
            nri::Buffer staging = device.createBuffer({ .size = rowPitch * height, .usage = NRI_BUFFER_USAGE_STAGING });
            auto* mapped = static_cast<uint8_t*>(staging.map(0, rowPitch * height));
            for (uint32_t y = 0; y < height; ++y)
                std::memcpy(mapped + y * rowPitch, rgba + y * width, width * 4);
            staging.unmap();
            Submit([&](nri::CommandBuffer& cmd) {
                const NriTextureRegionUpload region{ .stagingOffset = 0, .x = 0, .y = 0, .width = width, .height = height, .rowPitch = static_cast<uint32_t>(rowPitch) };
                nriTextureRecordRegionUploads(texture.handle(), cmd.handle(), staging.handle(), &region, 1);
                const nri::TextureBarrierDesc barrier{ texture.handle(), { NRI_ACCESS_TRANSFER_WRITE, NRI_STAGE_TRANSFER }, { NRI_ACCESS_SHADER_READ, NRI_STAGE_FRAGMENT | NRI_STAGE_COMPUTE } };
                cmd.resourceBarriers({ &barrier, 1 }, {});
            });
            resources.registerTexture(texture);
            const uint32_t slot = texture.getDescriptorSlot() + resources.getImageHeapIndexOffset();
            keep.push_back(std::move(texture));
            return slot;
        }

        uint32_t ImageSlot(uint32_t textureSlot) const { return textureSlot + resources.getImageHeapIndexOffset(); }

        // Records `record` into a one-off command buffer and waits for the GPU to finish it.
        template <class F>
        void Submit(F&& record)
        {
            nri::CommandAllocator allocator = device.createCommandAllocator(NRI_COMMAND_BUFFER_RESET_WITH_ALLOCATOR, NRI_QUEUE_GRAPHICS);
            nri::CommandBuffer cmd = allocator.allocateCommandBuffer(1);
            cmd.begin(0, true);
            cmd.bindDescriptorHeaps(&resources, &samplers);
            record(cmd);
            cmd.end(0);
            device.submitAndWait(cmd, 0);
        }

        // --- probes: what a render-graph texture holds ------------------------------------------------------------------------------

        // With --probe: copies the centre 64 x 64 pixels of `texture` (RGBA8 / sRGB: 4 bytes, RGBA16F: 8 bytes per pixel) to the CPU and
        // prints their average colour and range two frames later -- to see which pass of a frame produces what. Nothing without --probe.
        void Probe(nri::RenderGraph& graph, nri::RGTexture texture, const char* name, uint32_t bytesPerPixel, const FrameInfo& frame)
        {
            if (!m_ProbesOn || frame.extent.width < 64 || frame.extent.height < 64)
                return;
            ProbeSlot& probe = m_Probes[name];
            const uint64_t bytes = 64ull * 64 * bytesPerPixel;
            if (probe.pending[frame.frameSlot])
                PrintProbe(name, probe, frame.frameSlot);
            if (!probe.readback[frame.frameSlot])
                probe.readback[frame.frameSlot] = device.createBuffer({ .size = bytes, .usage = NRI_BUFFER_USAGE_READBACK });
            probe.bytesPerPixel = bytesPerPixel;
            probe.pending[frame.frameSlot] = true;
            const nri::RGBuffer target = graph.ImportBuffer((std::string("Probe ") + name).c_str(), probe.readback[frame.frameSlot]);
            const uint32_t x = frame.extent.width / 2 - 32, y = frame.extent.height / 2 - 32;
            graph.AddPass((std::string("Probe ") + name).c_str(), nri::RGPassFlags::None,
                [&](nri::RGBuilder& b) {
                    b.Read(texture, nri::RGTextureAccess::CopySource);
                    b.Write(target, nri::RGBufferAccess::CopyDestination);
                },
                [texture, target, x, y](nri::RGPassContext& ctx) {
                    nriTextureCopyToBuffer(ctx.Texture(texture), ctx.Cmd().handle(), ctx.Buffer(target), x, y, 64, 64);
                });
        }

        // --- the app -------------------------------------------------------------------------------------------------------------

        bool Start(Example& example, int argc, char** argv)
        {
            m_Example = &example;
            for (int i = 1; i < argc; ++i)
            {
                if (!std::strcmp(argv[i], "--vk") || !std::strcmp(argv[i], "--vulkan"))
                    api = NRI_GRAPHICS_API_VULKAN;
                else if (!std::strcmp(argv[i], "--dx12") || !std::strcmp(argv[i], "--d3d12"))
                    api = NRI_GRAPHICS_API_D3D12;
                else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc)
                    m_MaxFrames = std::atoi(argv[++i]);
                else if (!std::strcmp(argv[i], "--vsync"))
                    vsync = true;
                else if (!std::strcmp(argv[i], "--tearing"))
                    tearing = true;
                else if (!std::strcmp(argv[i], "--verbose"))
                    g_Verbose = true;
                else if (!std::strcmp(argv[i], "--probe"))
                    m_ProbesOn = true;
                else if (!std::strcmp(argv[i], "--shader-checks"))
                    m_ChecksOn = true;
                else if (const int used = example.OnArgument(argc, argv, i); used > 0)
                    i += used - 1;
                else
                    std::fprintf(stderr, "[%s] unknown argument '%s'\n", example.Name(), argv[i]);
            }
            if (const char* base = SDL_GetBasePath())
                m_BaseDir = base;

            if (!SDL_Init(SDL_INIT_VIDEO))
            {
                std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
                return false;
            }
            m_Window = SDL_CreateWindow(Title(0.0f).c_str(), m_Width, m_Height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
            if (!m_Window)
            {
                std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
                return false;
            }
            SDL_GetWindowSizeInPixels(m_Window, &m_Width, &m_Height);

            try
            {
                nri::DeviceDesc desc{};
                desc.api = api;
#ifdef _WIN32
                desc.window.type = NRI_WINDOW_SYSTEM_WIN32;
                desc.window.hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(m_Window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#endif
#ifndef NDEBUG
                desc.enableValidation = true;
#endif
                const std::string cache = std::string("shader_cache_") + example.Name() + (IsD3D12() ? "_dx12" : "_vk");
                desc.shaderCacheDir = cache.c_str();
                desc.logCallback = OnNriLog;
                example.ConfigureDevice(desc);
                device = nri::createDevice(desc);
                swapchain = nri::createSwapchain(device, { .width = uint32_t(m_Width), .height = uint32_t(m_Height), .vsync = vsync, .allowTearing = tearing });

                resources = device.createDescriptorHeap({ .type = NRI_DESCRIPTOR_HEAP_RESOURCE, .maxBufferDescriptors = 256, .maxImageDescriptors = 256 });
                nri::SamplerDesc linearRepeat{};
                linearRepeat.magFilter = linearRepeat.minFilter = NRI_FILTER_LINEAR;
                linearRepeat.mipmapMode = NRI_SAMPLER_MIPMAP_LINEAR;
                linearRepeat.addressModeU = linearRepeat.addressModeV = linearRepeat.addressModeW = NRI_SAMPLER_ADDRESS_REPEAT;
                linearRepeat.maxLod = 1000.0f;
                nri::SamplerDesc linearClamp = linearRepeat;
                linearClamp.addressModeU = linearClamp.addressModeV = linearClamp.addressModeW = NRI_SAMPLER_ADDRESS_CLAMP_TO_EDGE;
                nri::SamplerDesc nearestClamp = linearClamp;
                nearestClamp.magFilter = nearestClamp.minFilter = NRI_FILTER_NEAREST;
                nearestClamp.mipmapMode = NRI_SAMPLER_MIPMAP_NEAREST;
                samplers = device.createDescriptorHeap({ .type = NRI_DESCRIPTOR_HEAP_SAMPLER, .maxSamplerDescriptors = 8, .samplers = { linearRepeat, linearClamp, nearestClamp } });

                compiler = nri::createSlangCompiler();
                compiler.setTargetAPI(api);
                compiler.addSearchPath(m_BaseDir.string()); // (nox_gpu.slang sits beside the executable)
                if (m_ChecksOn)
                    EnableShaderChecks(); // (before anything else takes an image slot, and before any shader compiles)

                graph.SetLogCallback(OnGraphLog);
#ifndef NDEBUG
                graph.SetValidationEnabled(true);
#endif
                graph.Initialize(device, resources, kFramesInFlight);
                graph.SetCommandBufferSetup([this](nri::CommandBuffer& cmd, nri::QueueType queue) {
                    cmd.bindDescriptorHeaps(&resources, &samplers);
                    if (queue == NRI_QUEUE_GRAPHICS)
                        BaselineState(cmd);
                });

                std::printf("[%s] %s, present: %s\n", example.Name(), ApiName(),
                            vsync ? (IsD3D12() ? "vsync (Present(1, 0))" : "vsync (FIFO)")
                            : tearing ? (IsD3D12() ? "uncapped, tearing (Present(0, ALLOW_TEARING))" : "uncapped, tearing (IMMEDIATE, else FIFO)")
                                      : (IsD3D12() ? "uncapped, no tearing (Present(0, 0))" : "uncapped, no tearing (MAILBOX, else FIFO)"));
                example.Init(*this);
            }
            catch (const std::exception& e)
            {
                std::fprintf(stderr, "[%s] start-up failed on %s: %s\n", example.Name(), ApiName(), e.what());
                return false;
            }
            m_Start = m_LastFrame = std::chrono::steady_clock::now();
            return true;
        }

        SDL_AppResult Event(const SDL_Event& event)
        {
            if (event.type == SDL_EVENT_QUIT)
                return SDL_APP_SUCCESS;
            if (event.type == SDL_EVENT_KEY_DOWN)
            {
                if (event.key.key == SDLK_ESCAPE)
                    return SDL_APP_SUCCESS;
                m_Example->OnKey(event.key.key);
            }
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || event.type == SDL_EVENT_WINDOW_RESIZED || event.type == SDL_EVENT_WINDOW_RESTORED)
            {
                SDL_GetWindowSizeInPixels(m_Window, &m_Width, &m_Height);
                m_NeedsResize = true;
            }
            return SDL_APP_CONTINUE;
        }

        SDL_AppResult Iterate()
        {
            if (m_MaxFrames >= 0 && int(m_FrameNumber) >= m_MaxFrames)
                return g_Errors ? SDL_APP_FAILURE : SDL_APP_SUCCESS;
            try
            {
                if ((SDL_GetWindowFlags(m_Window) & SDL_WINDOW_MINIMIZED) || m_Width <= 0 || m_Height <= 0)
                {
                    SDL_Delay(16);
                    return SDL_APP_CONTINUE;
                }
                if (m_NeedsResize)
                {
                    device.waitIdle();
                    graph.ReleaseResources();
                    swapchain.reset();
                    swapchain = nri::createSwapchain(device, { .width = uint32_t(m_Width), .height = uint32_t(m_Height), .vsync = vsync, .allowTearing = tearing });
                    m_NeedsResize = false;
                }

                device.beginFrame(uint32_t(m_FrameNumber));
                FrameInfo frame;
                frame.frameNumber = m_FrameNumber;
                frame.frameSlot = uint32_t(m_FrameNumber % kFramesInFlight);
                const auto acquireStart = std::chrono::steady_clock::now();
                const nri::FrameResult acquired = swapchain.acquireNextImage(frame.frameSlot, frame.imageIndex);
                const auto acquireEnd = std::chrono::steady_clock::now();
                if (acquired == NRI_FRAME_RESULT_RESIZE_REQUIRED)
                {
                    m_NeedsResize = true;
                    return SDL_APP_CONTINUE;
                }
                frame.extent = swapchain.getExtent();
                if (!frame.extent.width || !frame.extent.height)
                {
                    m_NeedsResize = true;
                    return SDL_APP_CONTINUE;
                }
                const auto now = std::chrono::steady_clock::now();
                frame.time = std::chrono::duration<float>(now - m_Start).count();
                frame.deltaTime = std::min(std::chrono::duration<float>(now - m_LastFrame).count(), 0.1f);
                m_LastFrame = now;

                graph.Reset({ m_FrameNumber, frame.extent, frame.extent });
                if (m_ChecksOn && m_ChecksPending[frame.frameSlot])
                    ReadShaderChecks(frame.frameSlot); // (the frame that last used this slot has finished)
                m_Example->Render(*this, graph, frame);
                if (m_ChecksOn)
                    AddShaderChecksPass(graph, frame.frameSlot);
                graph.Compile();
                const auto recordEnd = std::chrono::steady_clock::now();
                const auto submissions = graph.Execute(frame.frameSlot);
                for (size_t s = 0; s < submissions.size(); ++s)
                {
                    const auto& sub = submissions[s];
                    if (s + 1 == submissions.size())
                        device.submitCommandBuffers(sub.CommandBuffers, swapchain.handle(), frame.frameSlot, frame.imageIndex, sub.Waits);
                    else
                        device.submit(sub.Queue, sub.CommandBuffers, sub.Waits, sub.Signals);
                }
                const auto submitEnd = std::chrono::steady_clock::now();
                if (swapchain.present(frame.frameSlot, frame.imageIndex) == NRI_FRAME_RESULT_RESIZE_REQUIRED)
                    m_NeedsResize = true;
                ReportStall(acquireStart, acquireEnd, recordEnd, submitEnd, std::chrono::steady_clock::now());

                ++m_FrameNumber;
                ++m_FramesThisSecond;
                const float sinceTitle = std::chrono::duration<float>(now - m_TitleTime).count();
                if (sinceTitle >= 1.0f)
                {
                    SDL_SetWindowTitle(m_Window, Title(float(m_FramesThisSecond) / sinceTitle).c_str());
                    if (!m_Status.empty() && m_Status != m_PrintedStatus)
                    {
                        std::printf("[%s] %s: %s\n", m_Example->Name(), ApiName(), m_Status.c_str());
                        m_PrintedStatus = m_Status;
                    }
                    m_TitleTime = now;
                    m_FramesThisSecond = 0;
                }
            }
            catch (const std::exception& e)
            {
                std::fprintf(stderr, "[%s] frame %llu on %s: %s\n", m_Example->Name(), static_cast<unsigned long long>(m_FrameNumber), ApiName(), e.what());
                return SDL_APP_FAILURE;
            }
            return SDL_APP_CONTINUE;
        }

        // A frame whose host side took over 150 ms is printed with where the time went: the acquire (waiting for the frame slot's fence --
        // the GPU finishing the frame that last used it -- and for a swapchain image), recording the passes, submitting, and the present.
        // A freeze then names the call it is stuck in; one where nothing is printed while the window title's frame rate keeps counting is
        // not the host waiting: the frames run, only the image does not change.
        using Clock = std::chrono::steady_clock;
        void ReportStall(Clock::time_point acquireStart, Clock::time_point acquireEnd, Clock::time_point recordEnd, Clock::time_point submitEnd,
                         Clock::time_point presentEnd)
        {
            const auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
            if (ms(acquireStart, presentEnd) <= 150.0)
                return;
            std::printf("[%s] %s: frame %llu stalled %.0f ms -- acquire (frame fence + swapchain image) %.0f ms, record %.0f ms, submit %.0f ms, "
                        "present %.0f ms\n",
                        m_Example->Name(), ApiName(), static_cast<unsigned long long>(m_FrameNumber), ms(acquireStart, presentEnd),
                        ms(acquireStart, acquireEnd), ms(acquireEnd, recordEnd), ms(recordEnd, submitEnd), ms(submitEnd, presentEnd));
        }

        void Quit(SDL_AppResult result)
        {
            if (device)
            {
                device.waitIdle();
                for (uint32_t slot = 0; m_ChecksOn && slot < kFramesInFlight; ++slot)
                    if (m_ChecksPending[slot])
                        ReadShaderChecks(slot); // (the last frames' reports)
                m_ChecksTexture.reset();
                for (nri::Buffer& readback : m_ChecksReadback)
                    readback.reset();
                m_Probes.clear();
                m_Example->Shutdown(*this);
                graph.ReleaseResources();
            }
            const bool failed = result == SDL_APP_FAILURE || g_Errors > 0;
            if (m_Example)
                std::printf("[%s] %s: %llu frames, %d error(s)%s\n", m_Example->Name(), ApiName(), static_cast<unsigned long long>(m_FrameNumber), g_Errors,
                            failed ? "" : " -- clean exit");
        }

        // Resets the state every raster pass starts from (the RenderGraph calls it for each graphics command buffer).
        static void BaselineState(nri::CommandBuffer& cmd)
        {
            cmd.setVertexInput();
            cmd.setPrimitiveTopology(NRI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
            cmd.setPrimitiveRestartEnable(false);
            cmd.setRasterizerDiscardEnable(false);
            cmd.setPolygonMode(NRI_POLYGON_MODE_FILL);
            cmd.setFrontFace(NRI_FRONT_FACE_COUNTER_CLOCKWISE);
            cmd.setDepthBiasEnable(false);
            cmd.setDepthClampEnable(false);
            cmd.setLineWidth(1.0f);
            cmd.setRasterizationSamples(1);
            cmd.setSampleMask(1, 0xFFFFFFFFu);
            cmd.setAlphaToCoverageEnable(false);
            cmd.setAlphaToOneEnable(false);
            cmd.setDepthBoundsTestEnable(false);
            cmd.setStencilTestEnable(false);
            cmd.setColorWriteMask(0, nri::ColorComponent::All);
            cmd.setLogicOpEnable(false);
            SetRasterState(cmd, {});
        }

        ~Host()
        {
            graph.ReleaseResources();
            compiler.reset();
            samplers.reset();
            resources.reset();
            swapchain.reset();
            device.reset();
            if (m_Window)
                SDL_DestroyWindow(m_Window);
        }

        // --- shader checks (--shader-checks) ---------------------------------------------------------------------------------------
        // nox_gpu.slang's NOX_CHECK / NoxIndex report into a small R32_SINT storage texture: [0] failures, [1] the first failure's line,
        // [2] / [3] its values. A storage texture, not a buffer: storage images live in the resource heap's image range on both APIs (the
        // path every example's storage textures use), so any shader reaches it by slot, without push data. It takes the first image slot
        // and the compiler gets NOX_SHADER_CHECKS=1 and NOX_CHECKS_SLOT; the host copies it back every frame and prints a failure once.
        bool ShaderChecksOn() const { return m_ChecksOn; }

    private:
        static constexpr uint32_t kChecksWidth = 64; // (64 texels x 4 bytes: one 256-byte row, what D3D12's texture copies need)

        void EnableShaderChecks()
        {
            m_ChecksTexture = device.createTexture({ .width = kChecksWidth, .height = 1, .usage = NRI_TEXTURE_USAGE_STORAGE, .format = NRI_FORMAT_R32_SINT });
            ClearShaderChecks();
            const uint32_t slot = ImageSlot(nriRegisterStorageTextureMip(resources.handle(), m_ChecksTexture.handle(), 0));
            for (nri::Buffer& readback : m_ChecksReadback)
                readback = device.createBuffer({ .size = kChecksWidth * 4, .usage = NRI_BUFFER_USAGE_READBACK });
            compiler.setDefine("NOX_SHADER_CHECKS", "1");
            compiler.setDefine("NOX_CHECKS_SLOT", std::to_string(slot).c_str());

            // Self-test: a shader that fails a check on purpose must leave exactly one failure with its values in the report -- otherwise
            // the checks cannot report on this GPU / API, and their silence would mean nothing.
            nri::Pipeline selfTest = Compute("nox_checks_selftest.slang", "csNoxCheckSelfTest");
            Submit([&](nri::CommandBuffer& cmd) {
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_COMPUTE, selfTest);
                cmd.dispatch(1, 1, 1);
                const nri::TextureBarrierDesc toCopy{ m_ChecksTexture.handle(), { NRI_ACCESS_SHADER_READ | NRI_ACCESS_SHADER_WRITE, kAllShaderStages },
                                                      { NRI_ACCESS_TRANSFER_READ, NRI_STAGE_TRANSFER } };
                cmd.resourceBarriers({ &toCopy, 1 }, {});
                nriTextureCopyToBuffer(m_ChecksTexture.handle(), cmd.handle(), m_ChecksReadback[0].handle(), 0, 0, kChecksWidth, 1);
            });
            int32_t report[4] = {};
            std::memcpy(report, m_ChecksReadback[0].map(0, sizeof(report)), sizeof(report));
            m_ChecksReadback[0].unmap();
            if (report[0] == 1 && report[1] > 0 && report[2] == 12345 && report[3] == 678)
                std::printf("[%s] shader checks on (report: image slot %u; self-test passed)\n", m_Example->Name(), slot);
            else
            {
                std::printf("[%s] SHADER CHECKS DO NOT WORK on %s: the self-test read %d failure(s), line %d, values %d, %d (expected 1, 12345, 678)\n",
                            m_Example->Name(), ApiName(), report[0], report[1], report[2], report[3]);
                ++g_Errors;
            }
            ClearShaderChecks();
        }

        // Zeroes the report and leaves it ready for shader access.
        void ClearShaderChecks()
        {
            const uint32_t rowBytes = kChecksWidth * 4;
            nri::Buffer staging = device.createBuffer({ .size = rowBytes, .usage = NRI_BUFFER_USAGE_STAGING });
            std::memset(staging.map(0, rowBytes), 0, rowBytes);
            staging.unmap();
            Submit([&](nri::CommandBuffer& cmd) {
                const NriTextureRegionUpload region{ .stagingOffset = 0, .x = 0, .y = 0, .width = kChecksWidth, .height = 1, .rowPitch = rowBytes };
                nriTextureRecordRegionUploads(m_ChecksTexture.handle(), cmd.handle(), staging.handle(), &region, 1);
                const nri::TextureBarrierDesc barrier{ m_ChecksTexture.handle(), { NRI_ACCESS_TRANSFER_WRITE, NRI_STAGE_TRANSFER },
                                                       { NRI_ACCESS_SHADER_READ | NRI_ACCESS_SHADER_WRITE, kAllShaderStages } };
                cmd.resourceBarriers({ &barrier, 1 }, {});
            });
        }

        // Copies the report to this frame slot's readback buffer, last in the frame: shaders wrote it without declaring it, so the
        // barriers are explicit -- every shader stage's writes before the copy, and the texture back to shader access after it.
        void AddShaderChecksPass(nri::RenderGraph& graph, uint32_t frameSlot)
        {
            const nri::RGBuffer target = graph.ImportBuffer("Shader checks readback", m_ChecksReadback[frameSlot]);
            graph.AddPass("Shader checks", nri::RGPassFlags::None,
                [&](nri::RGBuilder& b) { b.Write(target, nri::RGBufferAccess::CopyDestination); },
                [this, target](nri::RGPassContext& ctx) {
                    nri::CommandBuffer& cmd = ctx.Cmd();
                    const nri::TextureBarrierDesc toCopy{ m_ChecksTexture.handle(), { NRI_ACCESS_SHADER_READ | NRI_ACCESS_SHADER_WRITE, kAllShaderStages },
                                                          { NRI_ACCESS_TRANSFER_READ, NRI_STAGE_TRANSFER } };
                    cmd.resourceBarriers({ &toCopy, 1 }, {});
                    nriTextureCopyToBuffer(m_ChecksTexture.handle(), cmd.handle(), ctx.Buffer(target), 0, 0, kChecksWidth, 1);
                    const nri::TextureBarrierDesc toShaders{ m_ChecksTexture.handle(), { NRI_ACCESS_TRANSFER_READ, NRI_STAGE_TRANSFER },
                                                             { NRI_ACCESS_SHADER_READ | NRI_ACCESS_SHADER_WRITE, kAllShaderStages } };
                    cmd.resourceBarriers({ &toShaders, 1 }, {});
                });
            m_ChecksPending[frameSlot] = true;
        }

        void ReadShaderChecks(uint32_t frameSlot)
        {
            m_ChecksPending[frameSlot] = false;
            int32_t report[4] = {};
            std::memcpy(report, m_ChecksReadback[frameSlot].map(0, sizeof(report)), sizeof(report));
            m_ChecksReadback[frameSlot].unmap();
            const uint32_t failures = uint32_t(report[0]);
            if (failures == m_ChecksPrinted)
                return;
            if (m_ChecksPrinted == 0)
            {
                std::printf("[%s] SHADER CHECK FAILED: NOX_CHECK / NoxIndex at line %d of its .slang file -- values %d, %d (%u failure(s) so far)\n",
                            m_Example->Name(), report[1], report[2], report[3], failures);
                ++g_Errors;
            }
            else
                std::printf("[%s] shader checks: %u failure(s) so far (the first: line %d)\n", m_Example->Name(), failures, report[1]);
            std::fflush(stdout);
            m_ChecksPrinted = failures;
        }

        static constexpr uint32_t kAllShaderStages = NRI_STAGE_VERTEX | NRI_STAGE_TASK | NRI_STAGE_MESH | NRI_STAGE_FRAGMENT | NRI_STAGE_COMPUTE | NRI_STAGE_RAY_TRACING;
        bool m_ChecksOn = false;
        nri::Texture m_ChecksTexture;
        nri::Buffer m_ChecksReadback[kFramesInFlight];
        bool m_ChecksPending[kFramesInFlight]{};
        uint32_t m_ChecksPrinted = 0;

        struct ProbeSlot
        {
            nri::Buffer readback[kFramesInFlight];
            bool pending[kFramesInFlight]{};
            uint32_t bytesPerPixel = 4;
            uint32_t printed = 0;
        };

        static float HalfToFloat(uint16_t h)
        {
            const uint32_t sign = (h & 0x8000u) << 16, exponent = (h >> 10) & 0x1Fu, mantissa = h & 0x3FFu;
            uint32_t bits;
            if (exponent == 0)
                return (sign ? -1.0f : 1.0f) * std::ldexp(float(mantissa), -24);
            bits = exponent == 31 ? (sign | 0x7F800000u | (mantissa << 13)) : (sign | ((exponent + 112) << 23) | (mantissa << 13));
            float f;
            std::memcpy(&f, &bits, 4);
            return f;
        }

        void PrintProbe(const char* name, ProbeSlot& probe, uint32_t slot)
        {
            probe.pending[slot] = false;
            if (probe.printed++ % 60 != 0) // (the first one, then once every 60)
                return;
            const uint64_t bytes = 64ull * 64 * probe.bytesPerPixel;
            const auto* data = static_cast<const uint8_t*>(probe.readback[slot].map(0, bytes));
            float sum[4]{}, lo[4] = { 1e30f, 1e30f, 1e30f, 1e30f }, hi[4] = { -1e30f, -1e30f, -1e30f, -1e30f };
            for (uint32_t i = 0; i < 64 * 64; ++i)
                for (int c = 0; c < 4; ++c)
                {
                    float v;
                    if (probe.bytesPerPixel == 8)
                    {
                        uint16_t h;
                        std::memcpy(&h, data + i * 8 + c * 2, 2);
                        v = HalfToFloat(h);
                    }
                    else
                        v = data[i * 4 + c] / 255.0f;
                    sum[c] += v;
                    lo[c] = std::min(lo[c], v);
                    hi[c] = std::max(hi[c], v);
                }
            probe.readback[slot].unmap();
            std::printf("[%s] %s: probe '%s' (centre 64 x 64, channels in memory order): mean (%.3f %.3f %.3f %.3f) min (%.3f %.3f %.3f) max (%.3f %.3f %.3f)\n",
                        m_Example->Name(), ApiName(), name, sum[0] / 4096, sum[1] / 4096, sum[2] / 4096, sum[3] / 4096, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
        }

        std::map<std::string, ProbeSlot> m_Probes;
        bool m_ProbesOn = false;

        std::string Title(float fps) const
        {
            char text[160];
            std::snprintf(text, sizeof(text), "NoxRHI %s -- %s", m_Example ? m_Example->Name() : "", ApiName());
            std::string title = text;
            if (fps > 0)
            {
                std::snprintf(text, sizeof(text), " -- %.0f fps", fps);
                title += text;
            }
            if (!m_Status.empty())
                title += " -- " + m_Status;
            return title;
        }

        Example* m_Example = nullptr;
        std::string m_Status, m_PrintedStatus;
        SDL_Window* m_Window = nullptr;
        std::filesystem::path m_BaseDir = ".";
        int m_Width = 1280, m_Height = 720;
        int m_MaxFrames = -1;
        bool m_NeedsResize = false;
        uint64_t m_FrameNumber = 0;
        uint32_t m_FramesThisSecond = 0;
        std::chrono::steady_clock::time_point m_Start, m_LastFrame, m_TitleTime;
    };

    // Per-frame constants a shader reads through BufferRef<T>: one CPU-written buffer per frame in flight, so the CPU never
    // overwrites what the GPU is still reading.
    template <class T>
    class FrameConstants
    {
    public:
        void Create(Host& host)
        {
            for (uint32_t i = 0; i < kFramesInFlight; ++i)
            {
                m_Buffers[i] = host.device.createBuffer({ .size = sizeof(T), .usage = NRI_BUFFER_USAGE_UNIFORM });
                m_Refs[i] = host.Refs(m_Buffers[i], sizeof(T), false).read;
            }
        }
        // Writes this frame's values and returns the reference to pass to the shader.
        uint64_t Write(uint32_t frameSlot, const T& value)
        {
            std::memcpy(m_Buffers[frameSlot].map(0, sizeof(T)), &value, sizeof(T));
            m_Buffers[frameSlot].unmap();
            return m_Refs[frameSlot];
        }
        void Reset()
        {
            for (auto& b : m_Buffers)
                b.reset();
        }

    private:
        nri::Buffer m_Buffers[kFramesInFlight];
        uint64_t m_Refs[kFramesInFlight]{};
    };
}

// The SDL entry points for one example class.
#define NOX_EXAMPLE_MAIN(ExampleType)                                                                                                       \
    struct NoxExampleState                                                                                                                  \
    {                                                                                                                                       \
        nox::Host host;       /* (declared first: destroyed last, after the example's buffers and pipelines) */                           \
        ExampleType example;                                                                                                                \
    };                                                                                                                                      \
    SDL_AppResult SDL_AppInit(void** appstate, int argc, char** argv)                                                                       \
    {                                                                                                                                       \
        auto* state = new NoxExampleState();                                                                                                \
        *appstate = state;                                                                                                                  \
        return state->host.Start(state->example, argc, argv) ? SDL_APP_CONTINUE : SDL_APP_FAILURE;                                          \
    }                                                                                                                                       \
    SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event) { return static_cast<NoxExampleState*>(appstate)->host.Event(*event); }    \
    SDL_AppResult SDL_AppIterate(void* appstate) { return static_cast<NoxExampleState*>(appstate)->host.Iterate(); }                        \
    void SDL_AppQuit(void* appstate, SDL_AppResult result)                                                                                  \
    {                                                                                                                                       \
        if (auto* state = static_cast<NoxExampleState*>(appstate))                                                                          \
        {                                                                                                                                   \
            state->host.Quit(result);                                                                                                       \
            delete state;                                                                                                                   \
        }                                                                                                                                   \
    }
