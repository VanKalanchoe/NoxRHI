#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <NRIHPP/NRI.hpp>
#include <NRIHPP/NRIExtensions.hpp>
#include <NRIRenderGraph/RenderGraph.h>

static int g_validationErrors = 0;

static void NRI_CALL onNriLog(NriLogLevel level, const char* message, void* userData)
{
    (void)userData;
    const char* tag = level == NRI_LOG_ERROR ? "ERROR" : level == NRI_LOG_WARN ? "WARN" : "INFO";
    std::fprintf(stderr, "[HeadlessNone][%s] %s\n", tag, message);
    if (level == NRI_LOG_ERROR)
        ++g_validationErrors;
}

static void onRgLog(nri::RGLogLevel level, std::string_view message, void* userData)
{
    (void)userData;
    const char* tag = level == nri::RGLogLevel::Error ? "ERROR" : level == nri::RGLogLevel::Warn ? "WARN" : "INFO";
    std::fprintf(stderr, "[HeadlessNone][RG][%s] %.*s\n", tag, static_cast<int>(message.size()), message.data());
    if (level == nri::RGLogLevel::Error)
        ++g_validationErrors;
}

int main()
{
    try
    {
#ifndef NDEBUG
        constexpr bool kEnableValidation = true;
#else
        constexpr bool kEnableValidation = false;
#endif
        nri::DeviceDesc devDesc{};
        devDesc.api = NRI_GRAPHICS_API_NONE;
        devDesc.window.type = NRI_WINDOW_SYSTEM_HEADLESS;
        devDesc.enableValidation = kEnableValidation;
        devDesc.logCallback = onNriLog;

        nri::Device device = nri::createDevice(devDesc);
        if (device.getAPI() != NRI_GRAPHICS_API_NONE)
        {
            std::fprintf(stderr, "[HeadlessNone] Expected NRI_GRAPHICS_API_NONE.\n");
            return 1;
        }
        if (nri::isStreamlineInitialized(device) || nri::isDenoiserAvailable(device))
        {
            std::fprintf(stderr, "[HeadlessNone] GPU extensions should report unavailable on None API.\n");
            return 1;
        }

        nri::DescriptorHeap resourceHeap = device.createDescriptorHeap({
            .type = NRI_DESCRIPTOR_HEAP_RESOURCE,
            .maxBufferDescriptors = 32,
            .maxImageDescriptors = 32
        });

        nri::Pipeline dummyPipeline = device.createPipeline({
            .type = NRI_PIPELINE_COMPUTE,
            .buildNow = true
        }, nullptr);

        // Verify CPU-backed upload/copy/readback still works without any GPU driver.
        constexpr std::array<uint32_t, 4> kPayload = { 0x11223344u, 0x55667788u, 0xAABBCCDDu, 0xCAFEBABEu };
        constexpr uint64_t kPayloadBytes = sizeof(kPayload);

        nri::Buffer srcBuffer = device.createBuffer({
            .size = kPayloadBytes,
            .usage = NRI_BUFFER_USAGE_STAGING
        });
        nri::Buffer readbackBuffer = device.createBuffer({
            .size = kPayloadBytes,
            .usage = NRI_BUFFER_USAGE_READBACK
        });

        void* mappedSrc = srcBuffer.map(0, kPayloadBytes);
        std::memcpy(mappedSrc, kPayload.data(), kPayloadBytes);
        srcBuffer.unmap();

        constexpr uint32_t kFramesInFlight = 2;
        nri::CommandAllocator cmdAlloc = device.createCommandAllocator(NRI_COMMAND_BUFFER_RESET_PER_COMMAND_BUFFER, NRI_QUEUE_GRAPHICS);
        nri::CommandBuffer cmd = cmdAlloc.allocateCommandBuffer(1);
        cmd.begin(0, true);
        cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_COMPUTE, dummyPipeline);
        cmd.copyBuffer(srcBuffer, readbackBuffer, { 0, 0, kPayloadBytes });
        cmd.end(0);
        device.submitAndWait(cmd, 0);

        std::array<uint32_t, 4> readbackData{};
        const void* mappedDst = readbackBuffer.map(0, kPayloadBytes);
        std::memcpy(readbackData.data(), mappedDst, kPayloadBytes);
        readbackBuffer.unmap();

        if (readbackData != kPayload)
        {
            std::fprintf(stderr, "[HeadlessNone] CPU-backed buffer copy mismatch.\n");
            return 1;
        }

        // Run a headless no-GPU RenderGraph tick loop without any GPU driver or window.
        nri::RenderGraph renderGraph;
        renderGraph.SetLogCallback(onRgLog);
        renderGraph.SetValidationEnabled(kEnableValidation);
        renderGraph.Initialize(device, resourceHeap, kFramesInFlight);

        constexpr nri::Extent2D kVirtualExtent{ 1280, 720 };
        constexpr uint64_t kTickCount = 5;
        for (uint64_t tick = 0; tick < kTickCount; ++tick)
        {
            device.beginFrame(tick);
            const uint32_t frameSlot = static_cast<uint32_t>(tick % kFramesInFlight);

            renderGraph.Reset({ tick, kVirtualExtent, kVirtualExtent });

            nri::RGTexture gbuffer = renderGraph.CreateTexture("NullDummyTarget", {
                .Size = nri::RGSize::RenderResolution,
                .Format = NRI_FORMAT_RGBA8,
                .Usage = NRI_TEXTURE_USAGE_COLOR_ATTACHMENT
            });

            renderGraph.AddPass("NullNoOpPass", nri::RGPassFlags::Raster,
                [&](nri::RGBuilder& b)
                {
                    b.ColorTarget(gbuffer, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE, { 0.0f, 0.0f, 0.0f, 1.0f });
                    b.SetRenderArea(kVirtualExtent, nri::RGViewport::FlippedY);
                },
                [](nri::RGPassContext& ctx)
                {
                    (void)ctx.Cmd();
                });

            renderGraph.Compile();
            const auto submissions = renderGraph.Execute(frameSlot);
            for (const auto& sub : submissions)
                device.submit(sub.Queue, sub.CommandBuffers, sub.Waits, sub.Signals);
        }

        // RGPassFlags::OwnCommandBuffer: with one chunk planned (no parallel recording) the flagged pass still gets a command buffer of its
        // own, and the passes before and after it others -- A | B(own) | C D.
        {
            renderGraph.SetParallelRecording(false);
            device.beginFrame(kTickCount);
            const uint32_t frameSlot = static_cast<uint32_t>(kTickCount % kFramesInFlight);
            renderGraph.Reset({ kTickCount, kVirtualExtent, kVirtualExtent });
            std::array<NriCommandBuffer, 4> recordedIn{};
            const char* names[4] = { "IsolateA", "IsolateB (own command buffer)", "IsolateC", "IsolateD" };
            for (uint32_t i = 0; i < 4; ++i)
            {
                const nri::RGPassFlags flags = i == 1 ? nri::RGPassFlags::NeverCull | nri::RGPassFlags::OwnCommandBuffer : nri::RGPassFlags::NeverCull;
                renderGraph.AddPass(names[i], flags, [](nri::RGBuilder&) {},
                    [&recordedIn, i](nri::RGPassContext& ctx) { recordedIn[i] = ctx.Cmd().handle(); });
            }
            renderGraph.Compile();
            const auto submissions = renderGraph.Execute(frameSlot);
            size_t commandBufferCount = 0;
            for (const auto& sub : submissions)
            {
                commandBufferCount += sub.CommandBuffers.size();
                device.submit(sub.Queue, sub.CommandBuffers, sub.Waits, sub.Signals);
            }
            const bool isolated = recordedIn[0] && recordedIn[1] && recordedIn[2] && recordedIn[3] && recordedIn[0] != recordedIn[1] &&
                                  recordedIn[1] != recordedIn[2] && recordedIn[0] != recordedIn[2] && recordedIn[2] == recordedIn[3] &&
                                  commandBufferCount == 3;
            if (!isolated)
            {
                std::fprintf(stderr, "[HeadlessNone] OwnCommandBuffer: expected A | B | C D in 3 command buffers, got %zu command buffer(s).\n",
                             commandBufferCount);
                return 1;
            }
            std::printf("[HeadlessNone] OwnCommandBuffer: the flagged pass recorded alone (3 command buffers: A | B | C D).\n");
            renderGraph.SetParallelRecording(true);
        }

        renderGraph.ReleaseResources();
        device.waitIdle();

        if (g_validationErrors > 0)
        {
            std::fprintf(stderr, "[HeadlessNone] Validation errors encountered: %d\n", g_validationErrors);
            return 1;
        }

        std::printf("[HeadlessNone] Verified NRI_GRAPHICS_API_NONE across %llu headless frames.\n",
                    static_cast<unsigned long long>(kTickCount));
        std::printf("[HeadlessNone] Clean exit.\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "[HeadlessNone] Exception: %s\n", e.what());
        return 1;
    }
}
