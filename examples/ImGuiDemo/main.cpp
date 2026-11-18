#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <imgui.h>
#include <imgui_impl_sdl3.h>

#if defined(NRI_ENABLE_VULKAN)
    #include <vulkan/vulkan.h>
    #include <imgui_impl_vulkan.h>
#endif

#if defined(NRI_ENABLE_D3D12) && defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <d3d12.h>
    #include <dxgi1_6.h>
    #include <wrl/client.h>
    #include <imgui_impl_dx12.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
                    outTicks = ts.ticks & validMask;
                    return true;
                }
            }
            return false;
        };

        auto& completed = ps->slots[ps->activeSlot];
        uint64_t frameStart = 0, frameEnd = 0;
        if (lookupTicks(completed.frameBeginQuery, frameStart) && lookupTicks(completed.frameEndQuery, frameEnd) && frameEnd >= frameStart)
        {
            ps->lastGpuFrameMs = static_cast<double>(frameEnd - frameStart) * static_cast<double>(periodNs) * 1e-6;
        }

        ps->lastPassTimesMs.clear();
        for (const auto& p : completed.passes)
        {
            uint64_t s = 0, e = 0;
            if (lookupTicks(p.queryId, s) && lookupTicks(p.queryId + 1, e) && e >= s)
            {
                double ms = static_cast<double>(e - s) * static_cast<double>(periodNs) * 1e-6;
                ps->lastPassTimesMs.emplace_back(p.name, ms);
            }
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

#if defined(NRI_ENABLE_D3D12) && defined(_WIN32)
    struct D3D12ImGuiState
    {
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
        uint32_t srvDescSize = 0;
        std::vector<uint32_t> freeSrvSlots;
        uint32_t nextSrvSlot = 0;
        static constexpr uint32_t kMaxSrvDescriptors = 128;

        static void allocSrv(ImGui_ImplDX12_InitInfo* info,
                             D3D12_CPU_DESCRIPTOR_HANDLE* outCpu,
                             D3D12_GPU_DESCRIPTOR_HANDLE* outGpu)
        {
            auto* self = static_cast<D3D12ImGuiState*>(info->UserData);
            uint32_t slot = 0;
            if (!self->freeSrvSlots.empty())
            {
                slot = self->freeSrvSlots.back();
                self->freeSrvSlots.pop_back();
            }
            else
            {
                slot = self->nextSrvSlot++;
            }
            auto cpu = self->srvHeap->GetCPUDescriptorHandleForHeapStart();
            auto gpu = self->srvHeap->GetGPUDescriptorHandleForHeapStart();
            cpu.ptr += static_cast<SIZE_T>(slot) * self->srvDescSize;
            gpu.ptr += static_cast<UINT64>(slot) * self->srvDescSize;
            *outCpu = cpu;
            *outGpu = gpu;
        }

        static void freeSrv(ImGui_ImplDX12_InitInfo* info,
                            D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                            D3D12_GPU_DESCRIPTOR_HANDLE)
        {
            auto* self = static_cast<D3D12ImGuiState*>(info->UserData);
            const auto start = self->srvHeap->GetCPUDescriptorHandleForHeapStart().ptr;
            if (cpu.ptr >= start && self->srvDescSize > 0)
            {
                const uint32_t slot = static_cast<uint32_t>((cpu.ptr - start) / self->srvDescSize);
                self->freeSrvSlots.push_back(slot);
            }
        }
    };
#endif

    struct AppState
    {
        SDL_Window* window = nullptr;
        NriGraphicsAPI api = NRI_GRAPHICS_API_VULKAN;
        int width = 1280;
        int height = 720;
        int maxFrames = -1;
        bool resizeTest = false;
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
        nri::GpuProfiler gpuProfiler;
        nri::RenderGraph renderGraph;
        ProfilerState profilerState{};

#if defined(NRI_ENABLE_D3D12) && defined(_WIN32)
        D3D12ImGuiState d3d12ImGui{};
#endif
    };
}

static void initImGuiBackend(AppState& state)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    if (state.enableViewports)
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

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

    if (state.api == NRI_GRAPHICS_API_VULKAN)
    {
#if defined(NRI_ENABLE_VULKAN)
        const NriDeviceNativeVK nativeVK = nri::getDeviceNativeVK(state.device);
        ImGui_ImplSDL3_InitForVulkan(state.window);

        static VkFormat mainFormat = VK_FORMAT_B8G8R8A8_SRGB;
        mainFormat = static_cast<VkFormat>(nativeVK.surfaceVkFormat);
        static VkFormat viewportFormat = VK_FORMAT_B8G8R8A8_UNORM;
        viewportFormat = static_cast<VkFormat>(nativeVK.surfaceVkFormat);

        ImGui_ImplVulkan_InitInfo initInfo{};
        initInfo.ApiVersion = VK_API_VERSION_1_4;
        initInfo.Instance = static_cast<VkInstance>(nativeVK.vkInstance);
        initInfo.PhysicalDevice = static_cast<VkPhysicalDevice>(nativeVK.vkPhysicalDevice);
        initInfo.Device = static_cast<VkDevice>(nativeVK.vkDevice);
        initInfo.QueueFamily = nativeVK.graphicsQueueFamilyIndex;
        initInfo.Queue = static_cast<VkQueue>(nativeVK.vkGraphicsQueue);
        initInfo.DescriptorPoolSize = 1024;
        initInfo.MinImageCount = 2;
        initInfo.ImageCount = 3;
        initInfo.UseDynamicRendering = true;

        initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
        initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &mainFormat;

        initInfo.PipelineInfoForViewports.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        initInfo.PipelineInfoForViewports.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
        initInfo.PipelineInfoForViewports.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        initInfo.PipelineInfoForViewports.PipelineRenderingCreateInfo.pColorAttachmentFormats = &viewportFormat;
        initInfo.PipelineInfoForViewports.SwapChainImageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        if (!ImGui_ImplVulkan_Init(&initInfo))
            throw std::runtime_error("ImGui_ImplVulkan_Init failed");
        state.imguiInitialized = true;
#endif
    }
    else if (state.api == NRI_GRAPHICS_API_D3D12)
    {
#if defined(NRI_ENABLE_D3D12) && defined(_WIN32)
        const NriDeviceNativeD3D12 nativeD3D12 = nri::getDeviceNativeD3D12(state.device);
        auto* dev = static_cast<ID3D12Device*>(nativeD3D12.device);
        auto* queue = static_cast<ID3D12CommandQueue*>(nativeD3D12.graphicsQueue);

        D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
        srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvDesc.NumDescriptors = D3D12ImGuiState::kMaxSrvDescriptors;
        srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(dev->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&state.d3d12ImGui.srvHeap))))
            throw std::runtime_error("CreateDescriptorHeap (ImGui SRV) failed");
        state.d3d12ImGui.srvDescSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDesc.NumDescriptors = 1;
        if (FAILED(dev->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&state.d3d12ImGui.rtvHeap))))
            throw std::runtime_error("CreateDescriptorHeap (ImGui UNORM RTV) failed");

        ImGui_ImplSDL3_InitForD3D(state.window);

        ImGui_ImplDX12_InitInfo initInfo{};
        initInfo.Device = dev;
        initInfo.CommandQueue = queue;
        initInfo.NumFramesInFlight = static_cast<int>(kFramesInFlight);
        // DXGI FLIP_DISCARD secondary viewports require UNORM swapchain format.
        initInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
        initInfo.UserData = &state.d3d12ImGui;
        initInfo.SrvDescriptorHeap = state.d3d12ImGui.srvHeap.Get();
        initInfo.SrvDescriptorAllocFn = &D3D12ImGuiState::allocSrv;
        initInfo.SrvDescriptorFreeFn = &D3D12ImGuiState::freeSrv;

        if (!ImGui_ImplDX12_Init(&initInfo))
            throw std::runtime_error("ImGui_ImplDX12_Init failed");
        state.imguiInitialized = true;
#endif
    }
}

static void shutdownImGuiBackend(AppState& state)
{
    if (!state.imguiInitialized)
        return;

    if (state.api == NRI_GRAPHICS_API_VULKAN)
    {
#if defined(NRI_ENABLE_VULKAN)
        ImGui_ImplVulkan_Shutdown();
#endif
    }
    else if (state.api == NRI_GRAPHICS_API_D3D12)
    {
#if defined(NRI_ENABLE_D3D12) && defined(_WIN32)
        ImGui_ImplDX12_Shutdown();
        state.d3d12ImGui.rtvHeap.Reset();
        state.d3d12ImGui.srvHeap.Reset();
#endif
    }
    ImGui_ImplSDL3_Shutdown();
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
        else if (std::strcmp(argv[i], "--d3d12") == 0)
            state->api = NRI_GRAPHICS_API_D3D12;
        else if (std::strcmp(argv[i], "--no-viewports") == 0)
            state->enableViewports = false;
    }

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    state->window = SDL_CreateWindow("NoxRHI ImGuiDemo (Docking + Multi-Viewport + Telemetry)", state->width, state->height, SDL_WINDOW_RESIZABLE);
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

        initImGuiBackend(*state);
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

        if (state->api == NRI_GRAPHICS_API_VULKAN)
        {
#if defined(NRI_ENABLE_VULKAN)
            ImGui_ImplVulkan_NewFrame();
#endif
        }
        else if (state->api == NRI_GRAPHICS_API_D3D12)
        {
#if defined(NRI_ENABLE_D3D12) && defined(_WIN32)
            ImGui_ImplDX12_NewFrame();
#endif
        }
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

        if (state->showDemoWindow)
            ImGui::ShowDemoWindow(&state->showDemoWindow);

        if (ImGui::Begin("NoxRHI Telemetry"))
        {
            ImGui::Text("Backend API: %s", state->api == NRI_GRAPHICS_API_D3D12 ? "Direct3D 12" : "Vulkan");
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

        const nri::Extent2D extent = state->swapchain.getExtent();
        state->renderGraph.Reset({ state->frameNumber, extent, extent });

        state->renderGraph.AddPass("ImGuiOverlay", nri::RGPassFlags::Raster,
            [&](nri::RGBuilder& b)
            {
                b.SwapchainTarget(state->swapchain, imageIndex, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE,
                                  { state->clearColor[0], state->clearColor[1], state->clearColor[2], 1.0f });
                b.SetRenderArea(extent, nri::RGViewport::None);
            },
            [state, drawData, imageIndex](nri::RGPassContext& ctx)
            {
                nri::CommandBuffer& cmd = ctx.Cmd();
                if (state->api == NRI_GRAPHICS_API_VULKAN)
                {
#if defined(NRI_ENABLE_VULKAN)
                    auto vkCmd = static_cast<VkCommandBuffer>(nriGetCommandBufferNativeVK(cmd.handle(), 0));
                    ImGui_ImplVulkan_RenderDrawData(drawData, vkCmd);
#endif
                }
                else if (state->api == NRI_GRAPHICS_API_D3D12)
                {
#if defined(NRI_ENABLE_D3D12) && defined(_WIN32)
                    auto* d3dCmd = static_cast<ID3D12GraphicsCommandList*>(nriGetCommandBufferNativeD3D12(cmd.handle()));
                    const NriDeviceNativeD3D12 nativeD3D12 = nri::getDeviceNativeD3D12(state->device);
                    auto* dev = static_cast<ID3D12Device*>(nativeD3D12.device);
                    auto* dxgiSwap = static_cast<IDXGISwapChain4*>(nriGetSwapchainNativeD3D12(state->swapchain.handle()));

                    Microsoft::WRL::ComPtr<ID3D12Resource> backBuffer;
                    if (dxgiSwap && SUCCEEDED(dxgiSwap->GetBuffer(imageIndex, IID_PPV_ARGS(&backBuffer))))
                    {
                        D3D12_RENDER_TARGET_VIEW_DESC unormRtv{};
                        unormRtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                        unormRtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                        const D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = state->d3d12ImGui.rtvHeap->GetCPUDescriptorHandleForHeapStart();
                        dev->CreateRenderTargetView(backBuffer.Get(), &unormRtv, rtvHandle);
                        d3dCmd->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
                    }

                    ID3D12DescriptorHeap* heaps[] = { state->d3d12ImGui.srvHeap.Get() };
                    d3dCmd->SetDescriptorHeaps(1, heaps);
                    ImGui_ImplDX12_RenderDrawData(drawData, d3dCmd);
#endif
                }
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
        shutdownImGuiBackend(*state);
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
