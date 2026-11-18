#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <NRIHPP/NRI.hpp>
#include <NRIHPP/NRIExtensions.hpp>
#include <NRIRenderGraph/RenderGraph.h>

static int g_validationErrors = 0;

static void NRI_CALL onNriLog(NriLogLevel level, const char* message, void* userData)
{
    (void)userData;
    const char* tag = level == NRI_LOG_ERROR ? "ERROR" : level == NRI_LOG_WARN ? "WARN" : "INFO";
    std::fprintf(stderr, "[RenderGraphTriangle][%s] %s\n", tag, message);
    if (level == NRI_LOG_ERROR)
        ++g_validationErrors;
}

static void onRgLog(nri::RGLogLevel level, std::string_view message, void* userData)
{
    (void)userData;
    const char* tag = level == nri::RGLogLevel::Error ? "ERROR" : level == nri::RGLogLevel::Warn ? "WARN" : "INFO";
    std::fprintf(stderr, "[RenderGraphTriangle][RG][%s] %.*s\n", tag, static_cast<int>(message.size()), message.data());
    if (level == nri::RGLogLevel::Error)
        ++g_validationErrors;
}

static nri::NativeWindowHandle getWindowHandle(SDL_Window* window)
{
    nri::NativeWindowHandle handle{};
#ifdef _WIN32
    handle.type = NRI_WINDOW_SYSTEM_WIN32;
    handle.hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#endif
    return handle;
}

static void applyBaselineDynamicState(nri::CommandBuffer& cmd)
{
    cmd.setVertexInput();
    cmd.setPrimitiveTopology(NRI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    cmd.setPrimitiveRestartEnable(false);
    cmd.setRasterizerDiscardEnable(false);
    cmd.setPolygonMode(NRI_POLYGON_MODE_FILL);
    cmd.setCullMode(NRI_CULL_MODE_NONE);
    cmd.setFrontFace(NRI_FRONT_FACE_COUNTER_CLOCKWISE);
    cmd.setDepthBiasEnable(false);
    cmd.setDepthClampEnable(false);
    cmd.setLineWidth(1.0f);

    cmd.setRasterizationSamples(1);
    cmd.setSampleMask(1, 0xFFFFFFFFu);
    cmd.setAlphaToCoverageEnable(false);
    cmd.setAlphaToOneEnable(false);

    cmd.setDepthTestEnable(false);
    cmd.setDepthWriteEnable(false);
    cmd.setDepthCompareOp(NRI_COMPARE_ALWAYS);
    cmd.setDepthBoundsTestEnable(false);
    cmd.setStencilTestEnable(false);

    const nri::ColorBlendEquation blendEq{
        .srcColorBlendFactor = NRI_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = NRI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = NRI_BLEND_OP_ADD,
        .srcAlphaBlendFactor = NRI_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = NRI_BLEND_FACTOR_ZERO,
        .alphaBlendOp = NRI_BLEND_OP_ADD
    };
    cmd.setColorBlendEnable(0, false);
    cmd.setColorBlendEquation(0, blendEq);
    cmd.setColorWriteMask(0, nri::ColorComponent::All);
    cmd.setLogicOpEnable(false);
}

int main(int argc, char** argv)
{
    int maxFrames = -1;
    bool resizeTest = false;
    NriGraphicsAPI api = NRI_GRAPHICS_API_VULKAN;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            maxFrames = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--resize-test") == 0)
            resizeTest = true;
        else if (std::strcmp(argv[i], "--d3d12") == 0)
            api = NRI_GRAPHICS_API_D3D12;
    }

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    int width = 800;
    int height = 600;
    SDL_Window* window = SDL_CreateWindow("NoxRHI RenderGraphTriangle (NRIRenderGraph + NRIHPP)", width, height, SDL_WINDOW_RESIZABLE);
    if (!window)
    {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    int exitCode = 0;
    try
    {
        nri::DeviceDesc devDesc{};
        devDesc.api = api;
        devDesc.window = getWindowHandle(window);
        devDesc.enableValidation = true;
        devDesc.shaderCacheDir = "shader_cache_triangle";
        devDesc.logCallback = onNriLog;

        nri::Device device = nri::createDevice(devDesc);
        nri::Swapchain swapchain = nri::createSwapchain(device, { static_cast<uint32_t>(width), static_cast<uint32_t>(height), true });

        nri::DescriptorHeap resourceHeap = device.createDescriptorHeap({
            .type = NRI_DESCRIPTOR_HEAP_RESOURCE,
            .maxBufferDescriptors = 64,
            .maxImageDescriptors = 64
        });

        nri::SamplerDesc defaultSampler{};
        defaultSampler.magFilter = NRI_FILTER_LINEAR;
        defaultSampler.minFilter = NRI_FILTER_LINEAR;
        defaultSampler.maxLod = 1000.0f;
        nri::DescriptorHeap samplerHeap = device.createDescriptorHeap({
            .type = NRI_DESCRIPTOR_HEAP_SAMPLER,
            .maxSamplerDescriptors = 8,
            .samplers = { defaultSampler }
        });

        nri::ShaderCompiler compiler = nri::createSlangCompiler();
        compiler.setTargetAPI(api);

        std::string shaderPath = "triangle.slang";
        if (!std::filesystem::exists(shaderPath))
        {
            const char* basePath = SDL_GetBasePath();
            if (basePath && std::filesystem::exists(std::filesystem::path(basePath) / "triangle.slang"))
                shaderPath = (std::filesystem::path(basePath) / "triangle.slang").string();
        }

        nri::Pipeline trianglePipeline = device.createPipeline({
            .type = NRI_PIPELINE_GRAPHICS,
            .shaders = {
                { NRI_SHADER_STAGE_VERTEX, "vertMain", shaderPath },
                { NRI_SHADER_STAGE_FRAGMENT, "fragMain", shaderPath }
            },
            .colorFormats = { NRI_FORMAT_SURFACE },
            .buildNow = true
        }, compiler.handle());
        device.finishPipelineBuilds();

        constexpr uint32_t framesInFlight = 2;
        nri::RenderGraph renderGraph;
        renderGraph.SetLogCallback(onRgLog);
        renderGraph.SetValidationEnabled(true);
        renderGraph.Initialize(device, resourceHeap, framesInFlight);
        renderGraph.SetCommandBufferSetup([&](nri::CommandBuffer& cmd, nri::QueueType queue)
        {
            cmd.bindDescriptorHeaps(&resourceHeap, &samplerHeap);
            if (queue == NRI_QUEUE_GRAPHICS)
                applyBaselineDynamicState(cmd);
        });

        uint32_t frameNumber = 0;
        bool running = true;
        bool needsResize = false;

        while (running && (maxFrames < 0 || static_cast<int>(frameNumber) < maxFrames))
        {
            SDL_Event ev;
            while (SDL_PollEvent(&ev))
            {
                if (ev.type == SDL_EVENT_QUIT)
                    running = false;
                else if (ev.type == SDL_EVENT_WINDOW_RESIZED || ev.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                {
                    width = ev.window.data1;
                    height = ev.window.data2;
                    needsResize = true;
                }
            }

            if (resizeTest && frameNumber == 4)
            {
                SDL_SetWindowSize(window, 1024, 720);
                width = 1024;
                height = 720;
                needsResize = true;
            }

            if (needsResize && width > 0 && height > 0)
            {
                device.waitIdle();
                renderGraph.ReleaseResources();
                swapchain.reset();
                swapchain = nri::createSwapchain(device, { static_cast<uint32_t>(width), static_cast<uint32_t>(height), true });
                needsResize = false;
            }

            if (width == 0 || height == 0)
            {
                SDL_Delay(16);
                continue;
            }

            device.beginFrame(frameNumber);

            const uint32_t frameSlot = frameNumber % framesInFlight;
            uint32_t imageIndex = 0;
            if (swapchain.acquireNextImage(frameSlot, imageIndex) == NRI_FRAME_RESULT_RESIZE_REQUIRED)
            {
                needsResize = true;
                continue;
            }

            const nri::Extent2D extent = swapchain.getExtent();
            renderGraph.Reset({ frameNumber, extent, extent });

            // Pass 1: Draw triangle into a RenderGraph transient color texture
            nri::RGTexture offscreenColor = renderGraph.CreateTexture("TriangleColor", {
                .Size = nri::RGSize::RenderResolution,
                .Format = NRI_FORMAT_SURFACE,
                .Usage = NRI_TEXTURE_USAGE_COLOR_ATTACHMENT
            });

            const float timeSec = static_cast<float>(frameNumber) * 0.03f;
            renderGraph.AddPass("DrawTriangle", nri::RGPassFlags::Raster,
                [&](nri::RGBuilder& b)
                {
                    b.ColorTarget(offscreenColor, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE, { 0.08f, 0.10f, 0.14f, 1.0f });
                    b.SetRenderArea(extent, nri::RGViewport::FlippedY);
                },
                [&](nri::RGPassContext& ctx)
                {
                    nri::CommandBuffer& cmd = ctx.Cmd();
                    cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, trianglePipeline);
                    struct { float t; } pc{ timeSec };
                    cmd.pushData(&pc, sizeof(pc));
                    cmd.draw(3, 1, 0, 0);
                });

            // Pass 2: Copy/resolve transient into the swapchain backbuffer (or draw/copy)
            // Note: Swapchain images have eColorAttachment usage; copy or render directly.
            // Here we also exercise a direct swapchain raster pass so both transient + swapchain paths run.
            renderGraph.AddPass("PresentTriangle", nri::RGPassFlags::Raster,
                [&](nri::RGBuilder& b)
                {
                    b.Read(offscreenColor, nri::RGTextureAccess::Sampled);
                    b.SwapchainTarget(swapchain, imageIndex, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE, { 0.08f, 0.10f, 0.14f, 1.0f });
                    b.SetRenderArea(extent, nri::RGViewport::FlippedY);
                },
                [&](nri::RGPassContext& ctx)
                {
                    nri::CommandBuffer& cmd = ctx.Cmd();
                    cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, trianglePipeline);
                    struct { float t; } pc{ timeSec };
                    cmd.pushData(&pc, sizeof(pc));
                    cmd.draw(3, 1, 0, 0);
                });

            renderGraph.Compile();
            const auto submissions = renderGraph.Execute(frameSlot);
            for (size_t s = 0; s < submissions.size(); ++s)
            {
                const auto& sub = submissions[s];
                if (s + 1 == submissions.size())
                    device.submitCommandBuffers(sub.CommandBuffers, swapchain.handle(), frameSlot, imageIndex, sub.Waits);
                else
                    device.submit(sub.Queue, sub.CommandBuffers, sub.Waits, sub.Signals);
            }

            if (swapchain.present(frameSlot, imageIndex) == NRI_FRAME_RESULT_RESIZE_REQUIRED)
                needsResize = true;

            ++frameNumber;
        }

        device.waitIdle();
        renderGraph.ReleaseResources();
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "[RenderGraphTriangle] Exception: %s\n", e.what());
        exitCode = 1;
    }

    SDL_DestroyWindow(window);
    SDL_Quit();

    if (g_validationErrors > 0)
    {
        std::fprintf(stderr, "[RenderGraphTriangle] Failed with %d validation/NRI errors\n", g_validationErrors);
        return 1;
    }
    std::printf("[RenderGraphTriangle] Clean exit.\n");
    return exitCode;
}
