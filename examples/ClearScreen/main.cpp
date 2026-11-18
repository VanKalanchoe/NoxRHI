#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include <NRIHPP/NRI.hpp>
#include <NRIHPP/NRIExtensions.hpp>

static int g_validationErrors = 0;

static void NRI_CALL onNriLog(NriLogLevel level, const char* message, void* userData)
{
    (void)userData;
    const char* tag = level == NRI_LOG_ERROR ? "ERROR" : level == NRI_LOG_WARN ? "WARN" : "INFO";
    std::fprintf(stderr, "[ClearScreen][%s] %s\n", tag, message);
    if (level == NRI_LOG_ERROR)
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

namespace
{
    constexpr uint32_t kFramesInFlight = 2;

    struct AppState
    {
        SDL_Window* window = nullptr;
        int width = 800;
        int height = 600;
        int maxFrames = -1;
        bool vsync = false;
        bool resizeTest = false;
        bool needsResize = false;
        uint32_t frameNumber = 0;
        bool failed = false;

        nri::Device device;
        nri::Swapchain swapchain;
        std::vector<nri::CommandAllocator> allocators;
        std::vector<nri::CommandBuffer> cmdBuffers;
    };
}

SDL_AppResult SDL_AppInit(void** appstate, int argc, char** argv)
{
    auto state = std::make_unique<AppState>();
    NriGraphicsAPI api = NRI_GRAPHICS_API_NONE;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            state->maxFrames = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--resize-test") == 0)
            state->resizeTest = true;
        else if (std::strcmp(argv[i], "--vsync") == 0)
            state->vsync = true;
        else if (std::strcmp(argv[i], "--vk") == 0 || std::strcmp(argv[i], "--vulkan") == 0)
            api = NRI_GRAPHICS_API_VULKAN;
        else if (std::strcmp(argv[i], "--dx12") == 0 || std::strcmp(argv[i], "--d3d12") == 0)
            api = NRI_GRAPHICS_API_D3D12;
    }

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    state->window = SDL_CreateWindow("NoxRHI ClearScreen (NRIHPP)", state->width, state->height, SDL_WINDOW_RESIZABLE);
    if (!state->window)
    {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    try
    {
#ifndef NDEBUG
        constexpr bool kEnableValidation = true;
#else
        constexpr bool kEnableValidation = false;
#endif
        nri::DeviceDesc devDesc{};
        devDesc.api = api;
        devDesc.window = getWindowHandle(state->window);
        devDesc.enableValidation = kEnableValidation;
        devDesc.logCallback = onNriLog;

        state->device = nri::createDevice(devDesc);
        state->swapchain = nri::createSwapchain(state->device, {
            static_cast<uint32_t>(state->width),
            static_cast<uint32_t>(state->height),
            state->vsync
        });

        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            state->allocators.push_back(
                state->device.createCommandAllocator(NRI_COMMAND_BUFFER_RESET_WITH_ALLOCATOR, NRI_QUEUE_GRAPHICS));
            state->cmdBuffers.push_back(state->allocators.back().allocateCommandBuffer(1));
        }
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "[ClearScreen] Init Exception: %s\n", e.what());
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
    if (event->type == SDL_EVENT_WINDOW_RESIZED || event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || event->type == SDL_EVENT_WINDOW_RESTORED)
    {
        SDL_GetWindowSizeInPixels(state->window, &state->width, &state->height);
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
            SDL_SetWindowSize(state->window, 1024, 720);
            state->width = 1024;
            state->height = 720;
            state->needsResize = true;
        }

        const bool minimized = (SDL_GetWindowFlags(state->window) & SDL_WINDOW_MINIMIZED) != 0;
        if (minimized || state->width <= 0 || state->height <= 0)
        {
            SDL_Delay(16);
            return SDL_APP_CONTINUE;
        }

        if (state->needsResize)
        {
            state->device.waitIdle();
            state->swapchain.reset();
            state->swapchain = nri::createSwapchain(state->device, {
                static_cast<uint32_t>(state->width),
                static_cast<uint32_t>(state->height),
                state->vsync
            });
            state->needsResize = false;
        }

        state->device.beginFrame(state->frameNumber);

        const uint32_t frameSlot = state->frameNumber % kFramesInFlight;
        uint32_t imageIndex = 0;
        if (state->swapchain.acquireNextImage(frameSlot, imageIndex) == NRI_FRAME_RESULT_RESIZE_REQUIRED)
        {
            state->needsResize = true;
            return SDL_APP_CONTINUE;
        }

        state->allocators[frameSlot].resetPool();
        nri::CommandBuffer& cmd = state->cmdBuffers[frameSlot];
        cmd.begin(0, true);
        cmd.transitionSwapchainLayout(state->swapchain.handle(), imageIndex, NRI_TEXTURE_LAYOUT_UNDEFINED, NRI_TEXTURE_LAYOUT_COLOR_ATTACHMENT);

        const float t = static_cast<float>(state->frameNumber) * 0.05f;
        nri::RenderDesc renderDesc{};
        renderDesc.renderArea = state->swapchain.getExtent();
        renderDesc.colorAttachments.push_back(nri::RenderAttachDesc{
            .attachmentSwapchain = state->swapchain.handle(),
            .resolveImageIndex = imageIndex,
            .loadOp = NRI_LOAD_OP_CLEAR,
            .storeOp = NRI_STORE_OP_STORE,
            .clearColor = { 0.1f + 0.1f * std::sin(t), 0.15f + 0.1f * std::cos(t), 0.35f, 1.0f }
        });
        cmd.beginRendering(renderDesc);
        cmd.endRendering();

        cmd.transitionSwapchainLayout(state->swapchain.handle(), imageIndex, NRI_TEXTURE_LAYOUT_COLOR_ATTACHMENT, NRI_TEXTURE_LAYOUT_PRESENT);
        cmd.end(0);

        const NriCommandBuffer submitCmds[] = { cmd.handle() };
        state->device.submitCommandBuffers(submitCmds, state->swapchain.handle(), frameSlot, imageIndex);

        if (state->swapchain.present(frameSlot, imageIndex) == NRI_FRAME_RESULT_RESIZE_REQUIRED)
            state->needsResize = true;

        ++state->frameNumber;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "[ClearScreen] Exception: %s\n", e.what());
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
            state->device.waitIdle();
        state->cmdBuffers.clear();
        state->allocators.clear();
        state->swapchain.reset();
        state->device.reset();
        if (state->window)
            SDL_DestroyWindow(state->window);

        const bool hadFailure = state->failed || result == SDL_APP_FAILURE || g_validationErrors > 0;
        delete state;
        if (hadFailure)
        {
            std::fprintf(stderr, "[ClearScreen] Failed with %d validation/NRI errors\n", g_validationErrors);
            return;
        }
    }
    std::printf("[ClearScreen] Clean exit.\n");
}
