#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <NRIHPP/NRI.hpp>
#include <NRIHPP/NRIExtensions.hpp>

static int g_validationErrors = 0;

static void NRI_CALL onNriLog(NriLogLevel level, const char* message, void* userData)
{
    (void)userData;
    const char* tag = level == NRI_LOG_ERROR ? "ERROR" : level == NRI_LOG_WARN ? "WARN" : "INFO";
    std::fprintf(stderr, "[HeadlessRender][%s] %s\n", tag, message);
    if (level == NRI_LOG_ERROR)
        ++g_validationErrors;
}

int main(int argc, char** argv)
{
    NriGraphicsAPI api = NRI_GRAPHICS_API_VULKAN;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--d3d12") == 0)
            api = NRI_GRAPHICS_API_D3D12;
    }

    try
    {
        nri::DeviceDesc devDesc{};
        devDesc.api = api;
        devDesc.window.type = NRI_WINDOW_SYSTEM_HEADLESS;
        devDesc.enableValidation = true;
        devDesc.shaderCacheDir = "shader_cache_headless";
        devDesc.logCallback = onNriLog;

        nri::Device device = nri::createDevice(devDesc);

        constexpr uint32_t kSize = 64; // 64 * 4 = 256 bytes/row (matches D3D12 256B copy pitch alignment)
        nri::Texture target = device.createTexture({
            .width = kSize,
            .height = kSize,
            .arrayLayers = 1,
            .mipLevels = 1,
            .sampleCount = 1,
            .usage = NRI_TEXTURE_USAGE_COLOR_ATTACHMENT,
            .format = NRI_FORMAT_RGBA8
        });

        const uint64_t readbackBytes = static_cast<uint64_t>(kSize) * kSize * 4ull;
        nri::Buffer readback = device.createBuffer({
            .size = readbackBytes,
            .usage = NRI_BUFFER_USAGE_READBACK
        });

        nri::CommandAllocator alloc = device.createCommandAllocator(NRI_COMMAND_BUFFER_RESET_PER_COMMAND_BUFFER, NRI_QUEUE_GRAPHICS);
        nri::CommandBuffer cmd = alloc.allocateCommandBuffer(1);

        cmd.begin(0, true);

        const nri::RenderAttachDesc colorAttach{
            .attachment = target.handle(),
            .loadOp = NRI_LOAD_OP_CLEAR,
            .storeOp = NRI_STORE_OP_STORE,
            .clearColor = { 0.25f, 0.50f, 0.75f, 1.0f }
        };
        const nri::RenderDesc renderDesc{
            .renderArea = { kSize, kSize },
            .colorAttachments = { colorAttach },
            .isSwapchainPass = false
        };
        cmd.beginRendering(renderDesc);
        cmd.endRendering();

        cmd.executionBarrier();
        target.copyToBuffer(cmd.handle(), readback.handle(), 0, 0, kSize, kSize);
        cmd.end(0);

        device.submitAndWait(cmd, 0);

        const auto* pixels = static_cast<const uint8_t*>(readback.map(0, readbackBytes));
        const size_t centerOffset = (static_cast<size_t>(kSize / 2) * kSize + (kSize / 2)) * 4;
        const uint8_t r = pixels[centerOffset + 0];
        const uint8_t g = pixels[centerOffset + 1];
        const uint8_t b = pixels[centerOffset + 2];
        const uint8_t a = pixels[centerOffset + 3];
        readback.unmap();

        std::printf("[HeadlessRender] Center pixel RGBA = (%u, %u, %u, %u)\n", r, g, b, a);
        const bool pixelOk = (std::abs(static_cast<int>(r) - 64) <= 2) &&
                             (std::abs(static_cast<int>(g) - 128) <= 2) &&
                             (std::abs(static_cast<int>(b) - 191) <= 2) &&
                             (a == 255);
        if (!pixelOk)
        {
            std::fprintf(stderr, "[HeadlessRender] Unexpected pixel readback!\n");
            device.waitIdle();
            return 1;
        }

        device.waitIdle();
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "[HeadlessRender] Exception: %s\n", e.what());
        return 1;
    }

    if (g_validationErrors > 0)
    {
        std::fprintf(stderr, "[HeadlessRender] Failed with %d validation errors\n", g_validationErrors);
        return 1;
    }

    std::printf("[HeadlessRender] Clean exit.\n");
    return 0;
}
