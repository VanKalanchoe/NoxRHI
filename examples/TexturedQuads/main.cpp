#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <NRIHPP/NRI.hpp>
#include <NRIHPP/NRIExtensions.hpp>
#include <NRIRenderGraph/RenderGraph.h>

static int g_validationErrors = 0;

static void NRI_CALL onNriLog(NriLogLevel level, const char* message, void* userData)
{
    (void)userData;
    const char* tag = level == NRI_LOG_ERROR ? "ERROR" : level == NRI_LOG_WARN ? "WARN" : "INFO";
    std::fprintf(stderr, "[TexturedQuads][%s] %s\n", tag, message);
    if (level == NRI_LOG_ERROR)
        ++g_validationErrors;
}

static void onRgLog(nri::RGLogLevel level, std::string_view message, void* userData)
{
    (void)userData;
    const char* tag = level == nri::RGLogLevel::Error ? "ERROR" : level == nri::RGLogLevel::Warn ? "WARN" : "INFO";
    std::fprintf(stderr, "[TexturedQuads][RG][%s] %.*s\n", tag, static_cast<int>(message.size()), message.data());
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

static std::filesystem::path resolveAssetPath(const std::filesystem::path& relPath)
{
    if (std::filesystem::exists(relPath))
        return relPath;
    if (const char* basePath = SDL_GetBasePath())
    {
        auto candidate = std::filesystem::path(basePath) / relPath;
        if (std::filesystem::exists(candidate))
            return candidate;
    }
    return relPath;
}

namespace
{
    constexpr uint32_t kFramesInFlight = 2;

    struct alignas(16) PushConstants
    {
        uint32_t texIndices[4]{};
        uint32_t imageHeapOffset = 0;
        float time = 0.0f;
        float aspectRatio = 1.0f;
        uint32_t pad = 0;
    };

    struct AppState
    {
        SDL_Window* window = nullptr;
        int width = 1024;
        int height = 768;
        int maxFrames = -1;
        bool resizeTest = false;
        bool needsResize = false;
        uint32_t frameNumber = 0;
        bool failed = false;

        nri::Device device;
        nri::Swapchain swapchain;
        nri::DescriptorHeap resourceHeap;
        nri::DescriptorHeap samplerHeap;
        nri::ShaderCompiler compiler;
        nri::Pipeline quadPipeline;
        std::vector<nri::Texture> textures;
        std::array<uint32_t, 4> textureSlots{};
        nri::RenderGraph renderGraph;
    };
}

static void loadAndUploadTextures(AppState& state)
{
    static constexpr const char* kTextureFiles[4] = {
        "assets/textures/Checkerboard.png",
        "assets/textures/ChernoLogo.png",
        "assets/textures/Pacmanface.png",
        "assets/textures/viking_room.png"
    };

    struct LoadedImage
    {
        int w = 0;
        int h = 0;
        stbi_uc* pixels = nullptr;
        size_t bytes = 0;
        uint64_t stagingOffset = 0;
    };

    std::array<LoadedImage, 4> images{};
    uint64_t totalStagingBytes = 0;

    for (size_t i = 0; i < 4; ++i)
    {
        const std::filesystem::path imgPath = resolveAssetPath(kTextureFiles[i]);
        int channels = 0;
        images[i].pixels = stbi_load(imgPath.string().c_str(), &images[i].w, &images[i].h, &channels, 4);
        if (!images[i].pixels)
            throw std::runtime_error("Failed to load texture via stbi_load: " + imgPath.string());

        images[i].bytes = static_cast<size_t>(images[i].w) * static_cast<size_t>(images[i].h) * 4u;
        // Align staging offsets to 512 bytes for D3D12 placement compatibility.
        totalStagingBytes = (totalStagingBytes + 511ull) & ~511ull;
        images[i].stagingOffset = totalStagingBytes;
        totalStagingBytes += images[i].bytes;
    }

    nri::Buffer staging = state.device.createBuffer({
        .size = totalStagingBytes,
        .usage = NRI_BUFFER_USAGE_STAGING
    });
    auto* mapped = static_cast<uint8_t*>(staging.map(0, totalStagingBytes));
    for (const auto& img : images)
        std::memcpy(mapped + img.stagingOffset, img.pixels, img.bytes);
    staging.unmap();

    for (auto& img : images)
    {
        stbi_image_free(img.pixels);
        img.pixels = nullptr;
    }

    nri::CommandAllocator uploadAlloc = state.device.createCommandAllocator(
        NRI_COMMAND_BUFFER_RESET_WITH_ALLOCATOR, NRI_QUEUE_GRAPHICS);
    nri::CommandBuffer uploadCmd = uploadAlloc.allocateCommandBuffer(1);
    uploadCmd.begin(0, true);

    state.textures.reserve(4);
    for (size_t i = 0; i < 4; ++i)
    {
        nri::Texture tex = state.device.createTexture({
            .width = static_cast<uint32_t>(images[i].w),
            .height = static_cast<uint32_t>(images[i].h),
            .arrayLayers = 1,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI_TEXTURE_USAGE_SHADER_RESOURCE,
            .format = NRI_FORMAT_SRGBA8,
            .sharedAcrossQueues = true
        });
        const size_t mipOffset = 0;
        tex.recordUpload(uploadCmd.handle(), staging.handle(), images[i].stagingOffset, { &mipOffset, 1 });
        state.resourceHeap.registerTexture(tex);
        state.textureSlots[i] = tex.getDescriptorSlot();
        std::printf("[TexturedQuads] Loaded %s (%dx%d) -> descriptor slot %u\n",
                    kTextureFiles[i], images[i].w, images[i].h, state.textureSlots[i]);
        state.textures.push_back(std::move(tex));
    }

    uploadCmd.end(0);
    state.device.submitAndWait(uploadCmd, 0);
}

SDL_AppResult SDL_AppInit(void** appstate, int argc, char** argv)
{
    auto state = std::make_unique<AppState>();
    NriGraphicsAPI api = NRI_GRAPHICS_API_VULKAN;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            state->maxFrames = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--resize-test") == 0)
            state->resizeTest = true;
        else if (std::strcmp(argv[i], "--d3d12") == 0)
            api = NRI_GRAPHICS_API_D3D12;
    }

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    state->window = SDL_CreateWindow("NoxRHI TexturedQuads (DescriptorHeap + Bindless Textures)", state->width, state->height, SDL_WINDOW_RESIZABLE);
    if (!state->window)
    {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    try
    {
        nri::DeviceDesc devDesc{};
        devDesc.api = api;
        devDesc.window = getWindowHandle(state->window);
        devDesc.enableValidation = true;
        devDesc.shaderCacheDir = "shader_cache_textured_quads";
        devDesc.logCallback = onNriLog;

        state->device = nri::createDevice(devDesc);
        state->swapchain = nri::createSwapchain(state->device, {
            static_cast<uint32_t>(state->width),
            static_cast<uint32_t>(state->height),
            true
        });

        state->resourceHeap = state->device.createDescriptorHeap({
            .type = NRI_DESCRIPTOR_HEAP_RESOURCE,
            .maxBufferDescriptors = 64,
            .maxImageDescriptors = 64
        });

        nri::SamplerDesc defaultSampler{};
        defaultSampler.magFilter = NRI_FILTER_LINEAR;
        defaultSampler.minFilter = NRI_FILTER_LINEAR;
        defaultSampler.maxLod = 1000.0f;
        state->samplerHeap = state->device.createDescriptorHeap({
            .type = NRI_DESCRIPTOR_HEAP_SAMPLER,
            .maxSamplerDescriptors = 8,
            .samplers = { defaultSampler }
        });

        loadAndUploadTextures(*state);

        state->compiler = nri::createSlangCompiler();
        state->compiler.setTargetAPI(api);

        const std::string shaderPath = resolveAssetPath("textured_quads.slang").string();
        state->quadPipeline = state->device.createPipeline({
            .type = NRI_PIPELINE_GRAPHICS,
            .shaders = {
                { NRI_SHADER_STAGE_VERTEX, "vertMain", shaderPath },
                { NRI_SHADER_STAGE_FRAGMENT, "fragMain", shaderPath }
            },
            .colorFormats = { NRI_FORMAT_SURFACE },
            .buildNow = true
        }, state->compiler.handle());
        state->device.finishPipelineBuilds();

        state->renderGraph.SetLogCallback(onRgLog);
        state->renderGraph.SetValidationEnabled(true);
        state->renderGraph.Initialize(state->device, state->resourceHeap, kFramesInFlight);

        AppState* rawState = state.get();
        state->renderGraph.SetCommandBufferSetup([rawState](nri::CommandBuffer& cmd, nri::QueueType queue)
        {
            cmd.bindDescriptorHeaps(&rawState->resourceHeap, &rawState->samplerHeap);
            if (queue == NRI_QUEUE_GRAPHICS)
                applyBaselineDynamicState(cmd);
        });
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "[TexturedQuads] Init Exception: %s\n", e.what());
        state->failed = true;
        *appstate = state.release();
        return SDL_APP_FAILURE;
    }

    *appstate = state.release();
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event)
{
    auto* state = static_cast<AppState*>(appstate);
    if (event->type == SDL_EVENT_QUIT)
        return SDL_APP_SUCCESS;
    if (event->type == SDL_EVENT_WINDOW_RESIZED || event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
    {
        state->width = event->window.data1;
        state->height = event->window.data2;
        state->needsResize = true;
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate)
{
    auto* state = static_cast<AppState*>(appstate);
    if (state->maxFrames >= 0 && static_cast<int>(state->frameNumber) >= state->maxFrames)
        return g_validationErrors > 0 ? SDL_APP_FAILURE : SDL_APP_SUCCESS;

    try
    {
        if (state->resizeTest && state->frameNumber == 4)
        {
            SDL_SetWindowSize(state->window, 1280, 800);
            state->width = 1280;
            state->height = 800;
            state->needsResize = true;
        }

        if (state->needsResize && state->width > 0 && state->height > 0)
        {
            state->device.waitIdle();
            state->renderGraph.ReleaseResources();
            state->swapchain.reset();
            state->swapchain = nri::createSwapchain(state->device, {
                static_cast<uint32_t>(state->width),
                static_cast<uint32_t>(state->height),
                true
            });
            state->needsResize = false;
        }

        if (state->width <= 0 || state->height <= 0)
        {
            SDL_Delay(16);
            return SDL_APP_CONTINUE;
        }

        state->device.beginFrame(state->frameNumber);

        const uint32_t frameSlot = state->frameNumber % kFramesInFlight;
        uint32_t imageIndex = 0;
        if (state->swapchain.acquireNextImage(frameSlot, imageIndex) == NRI_FRAME_RESULT_RESIZE_REQUIRED)
        {
            state->needsResize = true;
            return SDL_APP_CONTINUE;
        }

        const nri::Extent2D extent = state->swapchain.getExtent();
        state->renderGraph.Reset({ state->frameNumber, extent, extent });

        const float timeSec = static_cast<float>(state->frameNumber) * 0.03f;
        const float aspect = extent.height > 0 ? static_cast<float>(extent.width) / static_cast<float>(extent.height) : 1.0f;

        state->renderGraph.AddPass("DrawTexturedQuads", nri::RGPassFlags::Raster,
            [&](nri::RGBuilder& b)
            {
                b.SwapchainTarget(state->swapchain, imageIndex, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE, { 0.06f, 0.07f, 0.10f, 1.0f });
                b.SetRenderArea(extent, nri::RGViewport::FlippedY);
            },
            [state, timeSec, aspect](nri::RGPassContext& ctx)
            {
                nri::CommandBuffer& cmd = ctx.Cmd();
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, state->quadPipeline);
                PushConstants pc{};
                for (size_t i = 0; i < 4; ++i)
                    pc.texIndices[i] = state->textureSlots[i];
                pc.imageHeapOffset = state->resourceHeap.getImageHeapIndexOffset();
                pc.time = timeSec;
                pc.aspectRatio = aspect;
                cmd.pushData(&pc, sizeof(pc));
                cmd.draw(24, 1, 0, 0);
            });

        state->renderGraph.Compile();
        const auto submissions = state->renderGraph.Execute(frameSlot);
        for (size_t s = 0; s < submissions.size(); ++s)
        {
            const auto& sub = submissions[s];
            if (s + 1 == submissions.size())
                state->device.submitCommandBuffers(sub.CommandBuffers, state->swapchain.handle(), frameSlot, imageIndex, sub.Waits);
            else
                state->device.submit(sub.Queue, sub.CommandBuffers, sub.Waits, sub.Signals);
        }

        if (state->swapchain.present(frameSlot, imageIndex) == NRI_FRAME_RESULT_RESIZE_REQUIRED)
            state->needsResize = true;

        ++state->frameNumber;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "[TexturedQuads] Exception: %s\n", e.what());
        state->failed = true;
        return SDL_APP_FAILURE;
    }

    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult result)
{
    auto* state = static_cast<AppState*>(appstate);
    if (state)
    {
        if (state->device.handle())
        {
            state->device.waitIdle();
            state->renderGraph.ReleaseResources();
        }
        state->textures.clear();
        state->quadPipeline.reset();
        state->compiler.reset();
        state->samplerHeap.reset();
        state->resourceHeap.reset();
        state->swapchain.reset();
        state->device.reset();
        if (state->window)
            SDL_DestroyWindow(state->window);

        const bool hadFailure = state->failed || result == SDL_APP_FAILURE || g_validationErrors > 0;
        delete state;
        if (hadFailure)
        {
            std::fprintf(stderr, "[TexturedQuads] Failed with %d validation/NRI errors\n", g_validationErrors);
            return;
        }
    }
    std::printf("[TexturedQuads] Clean exit.\n");
}
