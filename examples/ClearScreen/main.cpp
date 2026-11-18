#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
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

int main(int argc, char** argv)
{
    int maxFrames = -1; // -1 = run until closed
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
    SDL_Window* window = SDL_CreateWindow("NoxRHI ClearScreen (NRIHPP)", width, height, SDL_WINDOW_RESIZABLE);
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
        devDesc.logCallback = onNriLog;

        nri::Device device = nri::createDevice(devDesc);
        nri::Swapchain swapchain = nri::createSwapchain(device, { static_cast<uint32_t>(width), static_cast<uint32_t>(height), true });

        constexpr uint32_t framesInFlight = 2;
        std::vector<nri::CommandAllocator> allocators;
        std::vector<nri::CommandBuffer> cmdBuffers;
        for (uint32_t i = 0; i < framesInFlight; ++i)
        {
            allocators.push_back(device.createCommandAllocator(NRI_COMMAND_BUFFER_RESET_WITH_ALLOCATOR, NRI_QUEUE_GRAPHICS));
            cmdBuffers.push_back(allocators.back().allocateCommandBuffer(1));
        }

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

            allocators[frameSlot].resetPool();
            nri::CommandBuffer& cmd = cmdBuffers[frameSlot];
            cmd.begin(0, true);
            cmd.transitionSwapchainLayout(swapchain.handle(), imageIndex, NRI_TEXTURE_LAYOUT_UNDEFINED, NRI_TEXTURE_LAYOUT_COLOR_ATTACHMENT);

            const float t = static_cast<float>(frameNumber) * 0.05f;
            nri::RenderDesc renderDesc{};
            renderDesc.renderArea = swapchain.getExtent();
            renderDesc.colorAttachments.push_back(nri::RenderAttachDesc{
                .attachmentSwapchain = swapchain.handle(),
                .resolveImageIndex = imageIndex,
                .loadOp = NRI_LOAD_OP_CLEAR,
                .storeOp = NRI_STORE_OP_STORE,
                .clearColor = { 0.1f + 0.1f * std::sin(t), 0.15f + 0.1f * std::cos(t), 0.35f, 1.0f }
            });
            cmd.beginRendering(renderDesc);
            cmd.endRendering();

            cmd.transitionSwapchainLayout(swapchain.handle(), imageIndex, NRI_TEXTURE_LAYOUT_COLOR_ATTACHMENT, NRI_TEXTURE_LAYOUT_PRESENT);
            cmd.end(0);

            const NriCommandBuffer submitCmds[] = { cmd.handle() };
            device.submitCommandBuffers(submitCmds, swapchain.handle(), frameSlot, imageIndex);

            if (swapchain.present(frameSlot, imageIndex) == NRI_FRAME_RESULT_RESIZE_REQUIRED)
                needsResize = true;

            ++frameNumber;
        }

        device.waitIdle();
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "[ClearScreen] Exception: %s\n", e.what());
        exitCode = 1;
    }

    SDL_DestroyWindow(window);
    SDL_Quit();

    if (g_validationErrors > 0)
    {
        std::fprintf(stderr, "[ClearScreen] Failed with %d validation/NRI errors\n", g_validationErrors);
        return 1;
    }
    std::printf("[ClearScreen] Clean exit.\n");
    return exitCode;
}
