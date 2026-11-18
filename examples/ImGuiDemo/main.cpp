#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <NRIHPP/NRI.hpp>
#include <NRIHPP/NRIExtensions.hpp>
#include <NRIRenderGraph/RenderGraph.h>

static_assert(sizeof(ImDrawVert) == sizeof(NriImguiVertex), "ImDrawVert must match NriImguiVertex size");
static_assert(offsetof(ImDrawVert, pos) == offsetof(NriImguiVertex, pos), "ImDrawVert::pos mismatch");
static_assert(offsetof(ImDrawVert, uv) == offsetof(NriImguiVertex, uv), "ImDrawVert::uv mismatch");
static_assert(offsetof(ImDrawVert, col) == offsetof(NriImguiVertex, col), "ImDrawVert::col mismatch");

static int g_validationErrors = 0;

static void NRI_CALL onNriLog(NriLogLevel level, const char* message, void* userData)
{
    (void)userData;
    const char* tag = level == NRI_LOG_ERROR ? "ERROR" : level == NRI_LOG_WARN ? "WARN" : "INFO";
    std::fprintf(stderr, "[ImGuiDemo][%s] %s\n", tag, message);
    if (level == NRI_LOG_ERROR)
        ++g_validationErrors;
}

static void onRgLog(nri::RGLogLevel level, std::string_view message, void* userData)
{
    (void)userData;
    const char* tag = level == nri::RGLogLevel::Error ? "ERROR" : level == nri::RGLogLevel::Warn ? "WARN" : "INFO";
    std::fprintf(stderr, "[ImGuiDemo][RG][%s] %.*s\n", tag, static_cast<int>(message.size()), message.data());
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

namespace
{
    constexpr uint32_t kFramesInFlight = 2;
    constexpr size_t kMetricWindow = 60;

    struct PassTimingRecord
    {
        std::string name;
        uint32_t queryId = NRI_GPU_PROFILER_INVALID_QUERY_ID;
    };

    struct FrameTimingSlot
    {
        uint32_t frameBeginQuery = NRI_GPU_PROFILER_INVALID_QUERY_ID;
        uint32_t frameEndQuery = NRI_GPU_PROFILER_INVALID_QUERY_ID;
        std::vector<PassTimingRecord> passes;
        std::vector<size_t> openPassIndices;
    };

    struct ProfilerState
    {
        nri::GpuProfiler* profiler = nullptr;
        uint32_t activeSlot = 0;
        std::array<FrameTimingSlot, kFramesInFlight> slots{};

        double lastGpuFrameMs = 0.0;
        std::vector<std::pair<std::string, double>> lastPassTimesMs;
    };

    void profilerBeginFrame(uint32_t frameSlot, void* userData)
    {
        auto* ps = static_cast<ProfilerState*>(userData);
        if (!ps || !ps->profiler || !ps->profiler->isSupported())
            return;

        ps->activeSlot = frameSlot % kFramesInFlight;
        ps->profiler->beginFrame(ps->activeSlot);

        const auto timestamps = ps->profiler->getReadbackTimestamps();
        const float periodNs = ps->profiler->getTimestampPeriod();
        const uint32_t validBits = ps->profiler->getTimestampValidBits();
        const uint64_t validMask = (validBits >= 64 || validBits == 0) ? ~0ull : ((1ull << validBits) - 1ull);

        auto lookupTicks = [&](uint32_t queryId, uint64_t& outTicks) -> bool
        {
            if (queryId == NRI_GPU_PROFILER_INVALID_QUERY_ID)
                return false;
            for (const auto& ts : timestamps)
            {
                if (ts.queryId == queryId)
                {
                    outTicks = ts.ticks;
                    return true;
                }
            }
            return false;
        };

        auto ticksToMs = [periodNs, validMask](uint64_t beginTicks, uint64_t endTicks) -> double
        {
            const uint64_t delta = (endTicks - beginTicks) & validMask;
            return (static_cast<double>(delta) * static_cast<double>(periodNs)) / 1'000'000.0;
        };

        FrameTimingSlot& completed = ps->slots[ps->activeSlot];
        uint64_t frameStart = 0;
        uint64_t frameEnd = 0;
        if (lookupTicks(completed.frameBeginQuery, frameStart) && lookupTicks(completed.frameEndQuery, frameEnd))
            ps->lastGpuFrameMs = ticksToMs(frameStart, frameEnd);

        ps->lastPassTimesMs.clear();
        for (const auto& p : completed.passes)
        {
            uint64_t s = 0;
            uint64_t e = 0;
            if (p.queryId != NRI_GPU_PROFILER_INVALID_QUERY_ID && lookupTicks(p.queryId, s) && lookupTicks(p.queryId + 1, e))
                ps->lastPassTimesMs.emplace_back(p.name, ticksToMs(s, e));
        }

        completed.frameBeginQuery = NRI_GPU_PROFILER_INVALID_QUERY_ID;
        completed.frameEndQuery = NRI_GPU_PROFILER_INVALID_QUERY_ID;
        completed.passes.clear();
        completed.openPassIndices.clear();
    }

    void profilerRecordFrameBegin(nri::CommandBuffer& cmd, void* userData)
    {
        auto* ps = static_cast<ProfilerState*>(userData);
        if (!ps || !ps->profiler || !ps->profiler->isSupported())
            return;
        ps->profiler->resetQueries(cmd);
        const uint32_t pairId = ps->profiler->allocateQueryPair();
        if (pairId != NRI_GPU_PROFILER_INVALID_QUERY_ID)
        {
            ps->slots[ps->activeSlot].frameBeginQuery = pairId;
            ps->slots[ps->activeSlot].frameEndQuery = pairId + 1;
            ps->profiler->writeTimestamp(cmd, pairId);
        }
    }

    void profilerRecordFrameEnd(nri::CommandBuffer& cmd, void* userData)
    {
        auto* ps = static_cast<ProfilerState*>(userData);
        if (!ps || !ps->profiler || !ps->profiler->isSupported())
            return;
        const uint32_t endId = ps->slots[ps->activeSlot].frameEndQuery;
        if (endId != NRI_GPU_PROFILER_INVALID_QUERY_ID)
            ps->profiler->writeTimestamp(cmd, endId);
    }

    void profilerBeginGpuZone(void*, nri::CommandBuffer& cmd, uint32_t, const char* name, void* userData)
    {
        auto* ps = static_cast<ProfilerState*>(userData);
        if (!ps || !ps->profiler || !ps->profiler->isSupported())
            return;
        auto& slot = ps->slots[ps->activeSlot];
        const uint32_t pairId = ps->profiler->allocateQueryPair();
        slot.openPassIndices.push_back(slot.passes.size());
        slot.passes.push_back({ name ? name : "Pass", pairId });
        if (pairId != NRI_GPU_PROFILER_INVALID_QUERY_ID)
            ps->profiler->writeTimestamp(cmd, pairId);
    }

    void profilerEndGpuZone(void*, nri::CommandBuffer& cmd, void* userData)
    {
        auto* ps = static_cast<ProfilerState*>(userData);
        if (!ps || !ps->profiler || !ps->profiler->isSupported())
            return;
        auto& slot = ps->slots[ps->activeSlot];
        if (slot.openPassIndices.empty())
            return;
        const size_t idx = slot.openPassIndices.back();
        slot.openPassIndices.pop_back();
        if (slot.passes[idx].queryId != NRI_GPU_PROFILER_INVALID_QUERY_ID)
            ps->profiler->writeTimestamp(cmd, slot.passes[idx].queryId + 1);
    }

    struct NriDrawDataScratch
    {
        std::vector<NriImguiDrawList> drawLists;
        std::vector<NriImguiDrawCmd> drawCmds;
    };

    void syncImguiTextures(nri::Imgui& imgui, ImDrawData* drawData)
    {
        if (!drawData || !drawData->Textures)
            return;

        for (ImTextureData* tex : *drawData->Textures)
        {
            if (tex->Status == ImTextureStatus_WantCreate)
            {
                const uint64_t texId = imgui.createTexture(static_cast<uint32_t>(tex->Width),
                                                           static_cast<uint32_t>(tex->Height),
                                                           tex->GetPixels());
                tex->SetTexID(static_cast<ImTextureID>(texId));
                tex->SetStatus(ImTextureStatus_OK);
            }
            else if (tex->Status == ImTextureStatus_WantUpdates)
            {
                imgui.updateTexture(static_cast<uint64_t>(tex->GetTexID()),
                                     static_cast<uint32_t>(tex->UpdateRect.x),
                                     static_cast<uint32_t>(tex->UpdateRect.y),
                                     static_cast<uint32_t>(tex->UpdateRect.w),
                                     static_cast<uint32_t>(tex->UpdateRect.h),
                                     tex->GetPixels(),
                                     static_cast<uint32_t>(tex->Width),
                                     static_cast<uint32_t>(tex->Height));
                tex->SetStatus(ImTextureStatus_OK);
            }
            else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0)
            {
                imgui.destroyTexture(static_cast<uint64_t>(tex->GetTexID()));
                tex->SetTexID(ImTextureID_Invalid);
                tex->SetStatus(ImTextureStatus_Destroyed);
            }
        }
    }

    NriImguiDrawDataDesc translateImDrawData(ImDrawData* drawData, uint32_t frameSlot, uint32_t viewportSlot, NriDrawDataScratch& scratch)
    {
        scratch.drawLists.clear();
        scratch.drawCmds.clear();

        if (!drawData || drawData->CmdListsCount == 0)
            return {};

        size_t totalCmds = 0;
        for (const ImDrawList* list : drawData->CmdLists)
            totalCmds += static_cast<size_t>(list->CmdBuffer.Size);

        scratch.drawLists.reserve(static_cast<size_t>(drawData->CmdListsCount));
        scratch.drawCmds.reserve(totalCmds);

        for (const ImDrawList* list : drawData->CmdLists)
        {
            const size_t cmdStart = scratch.drawCmds.size();
            for (const ImDrawCmd& cmd : list->CmdBuffer)
            {
                if (cmd.UserCallback)
                    continue;
                scratch.drawCmds.push_back(NriImguiDrawCmd{
                    .clipRect = { cmd.ClipRect.x, cmd.ClipRect.y, cmd.ClipRect.z, cmd.ClipRect.w },
                    .elemCount = cmd.ElemCount,
                    .idxOffset = cmd.IdxOffset,
                    .vtxOffset = static_cast<int32_t>(cmd.VtxOffset),
                    .textureId = static_cast<uint64_t>(cmd.GetTexID())
                });
            }

            scratch.drawLists.push_back(NriImguiDrawList{
                .vertices = reinterpret_cast<const NriImguiVertex*>(list->VtxBuffer.Data),
                .vertexCount = static_cast<uint32_t>(list->VtxBuffer.Size),
                .indices = list->IdxBuffer.Data,
                .indexCount = static_cast<uint32_t>(list->IdxBuffer.Size),
                .indexSize = sizeof(ImDrawIdx),
                .cmds = scratch.drawCmds.data() + cmdStart,
                .cmdCount = static_cast<uint32_t>(scratch.drawCmds.size() - cmdStart)
            });
        }

        return NriImguiDrawDataDesc{
            .drawLists = scratch.drawLists.data(),
            .drawListCount = static_cast<uint32_t>(scratch.drawLists.size()),
            .displayPos = { drawData->DisplayPos.x, drawData->DisplayPos.y },
            .displaySize = { drawData->DisplaySize.x, drawData->DisplaySize.y },
            .framebufferScale = { drawData->FramebufferScale.x, drawData->FramebufferScale.y },
            .frameIndex = frameSlot,
            .viewportSlot = viewportSlot
        };
    }

    struct AppState
    {
        SDL_Window* window = nullptr;
        NriGraphicsAPI api = NRI_GRAPHICS_API_NONE;
        int width = 1280;
        int height = 720;
        int maxFrames = -1;
        bool resizeTest = false;
        bool viewportTest = false;
        bool needsResize = false;
        uint32_t frameNumber = 0;
        bool failed = false;
        bool imguiInitialized = false;

        bool showDemoWindow = true;
        bool enableViewports = true;
        std::array<float, 3> clearColor{ 0.08f, 0.10f, 0.14f };

        std::array<double, kMetricWindow> cpuHistoryMs{};
        std::array<double, kMetricWindow> gpuHistoryMs{};
        size_t metricIndex = 0;
        size_t metricCount = 0;
        std::chrono::steady_clock::time_point lastFrameTime = std::chrono::steady_clock::now();

        nri::Device device;
        nri::Swapchain swapchain;
        nri::DescriptorHeap resourceHeap;
        nri::DescriptorHeap samplerHeap;
        nri::Imgui imgui;
        nri::GpuProfiler gpuProfiler;
        nri::RenderGraph renderGraph;
        ProfilerState profilerState{};
        NriDrawDataScratch mainScratch{};
        NriDrawDataScratch viewportScratch{};
    };

    void rendererCreateWindow(ImGuiViewport* viewport)
    {
        auto* state = static_cast<AppState*>(ImGui::GetIO().BackendRendererUserData);
        if (!state)
            return;

        NriNativeWindowHandle handle{};
#ifdef _WIN32
        handle.type = NRI_WINDOW_SYSTEM_WIN32;
        handle.hwnd = viewport->PlatformHandleRaw;
#endif
        const uint64_t vpHandle = state->imgui.createViewport(NriImguiViewportDesc{
            .window = handle,
            .width = static_cast<uint32_t>(std::max(1.0f, viewport->Size.x)),
            .height = static_cast<uint32_t>(std::max(1.0f, viewport->Size.y)),
            .vsync = false
        });
        viewport->RendererUserData = reinterpret_cast<void*>(static_cast<uintptr_t>(vpHandle));
    }

    void rendererDestroyWindow(ImGuiViewport* viewport)
    {
        auto* state = static_cast<AppState*>(ImGui::GetIO().BackendRendererUserData);
        const uint64_t vpHandle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(viewport->RendererUserData));
        if (state && vpHandle != 0)
            state->imgui.destroyViewport(vpHandle);
        viewport->RendererUserData = nullptr;
    }

    void rendererSetWindowSize(ImGuiViewport* viewport, ImVec2 size)
    {
        auto* state = static_cast<AppState*>(ImGui::GetIO().BackendRendererUserData);
        const uint64_t vpHandle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(viewport->RendererUserData));
        if (state && vpHandle != 0)
            state->imgui.resizeViewport(vpHandle, static_cast<uint32_t>(std::max(0.0f, size.x)), static_cast<uint32_t>(std::max(0.0f, size.y)));
    }

    void rendererRenderWindow(ImGuiViewport* viewport, void*)
    {
        auto* state = static_cast<AppState*>(ImGui::GetIO().BackendRendererUserData);
        const uint64_t vpHandle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(viewport->RendererUserData));
        if (!state || vpHandle == 0)
            return;

        syncImguiTextures(state->imgui, viewport->DrawData);
        const uint32_t vpSlot = state->imgui.getViewportSlot(vpHandle);
        const NriImguiDrawDataDesc dd = translateImDrawData(viewport->DrawData, 0, vpSlot, state->viewportScratch);
        const bool noBg = (viewport->Flags & ImGuiViewportFlags_NoRendererClear) != 0;
        state->imgui.renderViewport(vpHandle, dd, noBg ? nri::ClearColor{ 0.0f, 0.0f, 0.0f, 0.0f } : nri::ClearColor{ 0.0f, 0.0f, 0.0f, 1.0f });
    }

    void rendererSwapBuffers(ImGuiViewport* viewport, void*)
    {
        auto* state = static_cast<AppState*>(ImGui::GetIO().BackendRendererUserData);
        const uint64_t vpHandle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(viewport->RendererUserData));
        if (state && vpHandle != 0)
            state->imgui.presentViewport(vpHandle);
    }
}

static void initImGui(AppState& state)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    if (state.enableViewports)
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.BackendRendererName = "NoxRHI_NRIImgui";
    io.BackendRendererUserData = &state;

    ImGui::StyleColorsDark();
    const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    ImGuiStyle& style = ImGui::GetStyle();
    if (scale > 0.0f)
        style.ScaleAllSizes(scale);
    io.ConfigDpiScaleFonts = true;
    io.ConfigDpiScaleViewports = true;

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
    platformIO.Renderer_CreateWindow = rendererCreateWindow;
    platformIO.Renderer_DestroyWindow = rendererDestroyWindow;
    platformIO.Renderer_SetWindowSize = rendererSetWindowSize;
    platformIO.Renderer_RenderWindow = rendererRenderWindow;
    platformIO.Renderer_SwapBuffers = rendererSwapBuffers;

    ImGui_ImplSDL3_InitForOther(state.window);
    state.imguiInitialized = true;
}

static void shutdownImGui(AppState& state)
{
    if (!state.imguiInitialized)
        return;

    ImGui::DestroyPlatformWindows();
    ImGui_ImplSDL3_Shutdown();
    ImGui::GetIO().BackendRendererUserData = nullptr;
    ImGui::DestroyContext();
    state.imguiInitialized = false;
}

SDL_AppResult SDL_AppInit(void** appstate, int argc, char** argv)
{
    auto state = std::make_unique<AppState>();
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            state->maxFrames = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--resize-test") == 0)
            state->resizeTest = true;
        else if (std::strcmp(argv[i], "--viewport-test") == 0)
            state->viewportTest = true;
        else if (std::strcmp(argv[i], "--vk") == 0 || std::strcmp(argv[i], "--vulkan") == 0)
            state->api = NRI_GRAPHICS_API_VULKAN;
        else if (std::strcmp(argv[i], "--dx12") == 0 || std::strcmp(argv[i], "--d3d12") == 0)
            state->api = NRI_GRAPHICS_API_D3D12;
        else if (std::strcmp(argv[i], "--no-viewports") == 0)
            state->enableViewports = false;
    }

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    state->window = SDL_CreateWindow("NoxRHI ImGuiDemo (NRIImgui Extension)", state->width, state->height, SDL_WINDOW_RESIZABLE);
    if (!state->window)
    {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    try
    {
        nri::DeviceDesc devDesc{};
        devDesc.api = state->api;
        devDesc.window = getWindowHandle(state->window);
        devDesc.enableValidation = true;
        devDesc.shaderCacheDir = "shader_cache_imgui";
        devDesc.logCallback = onNriLog;

        state->device = nri::createDevice(devDesc);
        state->swapchain = nri::createSwapchain(state->device, {
            static_cast<uint32_t>(state->width),
            static_cast<uint32_t>(state->height),
            true
        });

        state->resourceHeap = state->device.createDescriptorHeap({
            .type = NRI_DESCRIPTOR_HEAP_RESOURCE,
            .maxBufferDescriptors = 128,
            .maxImageDescriptors = 128
        });

        nri::SamplerDesc defaultSampler{};
        defaultSampler.magFilter = NRI_FILTER_LINEAR;
        defaultSampler.minFilter = NRI_FILTER_LINEAR;
        defaultSampler.addressModeU = NRI_SAMPLER_ADDRESS_CLAMP_TO_EDGE;
        defaultSampler.addressModeV = NRI_SAMPLER_ADDRESS_CLAMP_TO_EDGE;
        defaultSampler.addressModeW = NRI_SAMPLER_ADDRESS_CLAMP_TO_EDGE;
        defaultSampler.maxLod = 1000.0f;
        state->samplerHeap = state->device.createDescriptorHeap({
            .type = NRI_DESCRIPTOR_HEAP_SAMPLER,
            .maxSamplerDescriptors = 8,
            .samplers = { defaultSampler }
        });

        state->imgui = nri::createImgui(state->device, {
            .resourceHeap = &state->resourceHeap,
            .samplerHeap = &state->samplerHeap,
            .samplerSlot = 0,
            .colorFormat = NRI_FORMAT_SURFACE,
            .framesInFlight = kFramesInFlight,
            .linearColorOutput = true
        });

        state->gpuProfiler = nri::createGpuProfiler(state->device, kFramesInFlight);
        state->profilerState.profiler = &state->gpuProfiler;

        nri::RGProfilerHooks hooks{};
        hooks.UserData = &state->profilerState;
        hooks.BeginGpuFrame = &profilerBeginFrame;
        hooks.RecordGpuFrameBegin = &profilerRecordFrameBegin;
        hooks.RecordGpuFrameEnd = &profilerRecordFrameEnd;
        hooks.BeginGpuZone = &profilerBeginGpuZone;
        hooks.EndGpuZone = &profilerEndGpuZone;

        state->renderGraph.SetLogCallback(onRgLog);
        state->renderGraph.SetValidationEnabled(true);
        state->renderGraph.SetProfilerHooks(hooks);
        state->renderGraph.Initialize(state->device, state->resourceHeap, kFramesInFlight);

        initImGui(*state);
        state->lastFrameTime = std::chrono::steady_clock::now();
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "[ImGuiDemo] Init Exception: %s\n", e.what());
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
    if (state->imguiInitialized)
        ImGui_ImplSDL3_ProcessEvent(event);

    if (event->type == SDL_EVENT_QUIT)
        return SDL_APP_SUCCESS;
    if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
        event->window.windowID == SDL_GetWindowID(state->window))
        return SDL_APP_SUCCESS;
    if ((event->type == SDL_EVENT_WINDOW_RESIZED || event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) &&
        event->window.windowID == SDL_GetWindowID(state->window))
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
        const auto now = std::chrono::steady_clock::now();
        const double dtMs = std::chrono::duration<double, std::milli>(now - state->lastFrameTime).count();
        state->lastFrameTime = now;
        if (state->frameNumber > 0)
        {
            state->cpuHistoryMs[state->metricIndex] = dtMs;
            state->gpuHistoryMs[state->metricIndex] = state->profilerState.lastGpuFrameMs;
            state->metricIndex = (state->metricIndex + 1) % kMetricWindow;
            state->metricCount = std::min(state->metricCount + 1, kMetricWindow);
        }

        double cpuAvgMs = 0.0;
        double gpuAvgMs = 0.0;
        if (state->metricCount > 0)
        {
            for (size_t i = 0; i < state->metricCount; ++i)
            {
                cpuAvgMs += state->cpuHistoryMs[i];
                gpuAvgMs += state->gpuHistoryMs[i];
            }
            cpuAvgMs /= static_cast<double>(state->metricCount);
            gpuAvgMs /= static_cast<double>(state->metricCount);
        }
        const double fps = cpuAvgMs > 0.001 ? 1000.0 / cpuAvgMs : 0.0;

        if (state->resizeTest && state->frameNumber == 4)
        {
            SDL_SetWindowSize(state->window, 1024, 680);
            state->width = 1024;
            state->height = 680;
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

        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

        if (state->showDemoWindow)
            ImGui::ShowDemoWindow(&state->showDemoWindow);

        if (state->viewportTest)
        {
            const ImGuiViewport* mainVp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(ImVec2(mainVp->Pos.x + mainVp->Size.x + 24.0f, mainVp->Pos.y + 40.0f), ImGuiCond_Once);
            ImGui::SetNextWindowSize(ImVec2(320.0f, 180.0f), ImGuiCond_Once);
            if (ImGui::Begin("Detached Multi-Viewport Test"))
                ImGui::Text("Secondary OS window backed by NRIImgui.");
            ImGui::End();
        }

        if (ImGui::Begin("NoxRHI Telemetry"))
        {
            const char* apiLabel = (state->api == NRI_GRAPHICS_API_D3D12) ? "Direct3D 12"
                                 : (state->api == NRI_GRAPHICS_API_VULKAN) ? "Vulkan"
                                 : "None (No-GPU)";
            ImGui::Text("Backend API: %s", apiLabel);
            ImGui::Text("Renderer: NRIImgui Extension (Zero native API code in app)");
            ImGui::Separator();
            ImGui::Text("FPS (avg %zu): %.1f", state->metricCount, fps);
            ImGui::Text("CPU Avg: %.3f ms", cpuAvgMs);
            ImGui::Text("GPU Avg: %.3f ms (instant: %.3f ms)", gpuAvgMs, state->profilerState.lastGpuFrameMs);

            if (!state->profilerState.lastPassTimesMs.empty())
            {
                ImGui::Separator();
                ImGui::Text("RenderGraph Passes:");
                for (const auto& [passName, passMs] : state->profilerState.lastPassTimesMs)
                    ImGui::BulletText("%s: %.3f ms", passName.c_str(), passMs);
            }

            ImGui::Separator();
            ImGui::Checkbox("Show ImGui Demo Window", &state->showDemoWindow);
            ImGui::ColorEdit3("Backdrop Clear", state->clearColor.data());
            ImGui::Text("Multi-Viewport: %s", (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) ? "Enabled (drag windows outside)" : "Disabled");
        }
        ImGui::End();

        ImGui::Render();
        ImDrawData* drawData = ImGui::GetDrawData();
        syncImguiTextures(state->imgui, drawData);

        const NriImguiDrawDataDesc mainDrawData = translateImDrawData(drawData, frameSlot, 0, state->mainScratch);

        const nri::Extent2D extent = state->swapchain.getExtent();
        state->renderGraph.Reset({ state->frameNumber, extent, extent });

        state->renderGraph.AddPass("ImGuiUpload", nri::RGPassFlags::NeverCull,
            [](nri::RGBuilder&) {},
            [state, mainDrawData](nri::RGPassContext& ctx)
            {
                state->imgui.copyData(ctx.Cmd(), mainDrawData);
            });

        state->renderGraph.AddPass("ImGuiOverlay", nri::RGPassFlags::Raster,
            [&](nri::RGBuilder& b)
            {
                b.SwapchainTarget(state->swapchain, imageIndex, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE,
                                  { state->clearColor[0], state->clearColor[1], state->clearColor[2], 1.0f });
                b.SetRenderArea(extent, nri::RGViewport::None);
            },
            [state, mainDrawData](nri::RGPassContext& ctx)
            {
                state->imgui.draw(ctx.Cmd(), mainDrawData);
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

        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }

        if (state->swapchain.present(frameSlot, imageIndex) == NRI_FRAME_RESULT_RESIZE_REQUIRED)
            state->needsResize = true;

        ++state->frameNumber;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "[ImGuiDemo] Exception: %s\n", e.what());
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
        shutdownImGui(*state);
        state->imgui.reset();
        state->gpuProfiler.reset();
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
            std::fprintf(stderr, "[ImGuiDemo] Failed with %d validation/NRI errors\n", g_validationErrors);
            return;
        }
    }
    std::printf("[ImGuiDemo] Clean exit.\n");
}
