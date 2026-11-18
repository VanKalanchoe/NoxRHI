#include <array>
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
    std::fprintf(stderr, "[StreamlineNRDDemo][%s] %s\n", tag, message);
    if (level == NRI_LOG_ERROR)
        ++g_validationErrors;
}

static void makeIdentity(float m[16])
{
    std::memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void makePerspectiveReverseZ(float fovRad, float aspect, float nearPlane, float out[16])
{
    std::memset(out, 0, sizeof(float) * 16);
    const float f = 1.0f / std::tan(fovRad * 0.5f);
    out[0] = f / aspect;
    out[5] = f;
    out[11] = -1.0f;
    out[14] = nearPlane;
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    NriGraphicsAPI api = NRI_GRAPHICS_API_NONE;
    uint32_t frameCount = 4;
    bool enableValidation = true;
    bool enableStreamline = true;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--vk") == 0 || std::strcmp(argv[i], "--vulkan") == 0)
            api = NRI_GRAPHICS_API_VULKAN;
        else if (std::strcmp(argv[i], "--dx12") == 0 || std::strcmp(argv[i], "--d3d12") == 0)
            api = NRI_GRAPHICS_API_D3D12;
        else if (std::strcmp(argv[i], "--no-validation") == 0)
            enableValidation = false;
        else if (std::strcmp(argv[i], "--no-streamline") == 0)
            enableStreamline = false;
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            frameCount = static_cast<uint32_t>(std::atoi(argv[++i]));
    }

    try
    {
        nri::DeviceDesc devDesc{};
        devDesc.api = api;
        devDesc.window.type = NRI_WINDOW_SYSTEM_HEADLESS;
        devDesc.enableValidation = enableValidation;
        devDesc.enableStreamline = enableStreamline;
        devDesc.shaderCacheDir = "shader_cache_streamline_nrd";
        devDesc.logCallback = onNriLog;

        nri::Device device = nri::createDevice(devDesc);

        const bool slInit = nri::isStreamlineInitialized(device);
        const bool dlssSR = nri::isUpscalerSupported(device, NRI_UPSCALER_TYPE_DLSS_SR);
        const bool dlssRR = nri::isUpscalerSupported(device, NRI_UPSCALER_TYPE_DLSS_RR);
        const bool nrdAvail = nri::isDenoiserAvailable(device);

        const char* apiName = (api == NRI_GRAPHICS_API_D3D12) ? "D3D12"
                            : (api == NRI_GRAPHICS_API_VULKAN) ? "Vulkan"
                            : "None";
        std::printf("[StreamlineNRDDemo] API=%s | Streamline=%s | DLSS-SR=%s | DLSS-RR=%s | NRD=%s\n",
                    apiName,
                    slInit ? "on" : "off",
                    dlssSR ? "supported" : "unavailable",
                    dlssRR ? "supported" : "unavailable",
                    nrdAvail ? "available" : "unavailable");

        constexpr uint32_t kOutW = 1280;
        constexpr uint32_t kOutH = 720;
        nri::Upscaler upscaler;
        uint32_t kRenderW = kOutW;
        uint32_t kRenderH = kOutH;
        if (slInit)
        {
            upscaler = nri::createUpscaler(device, NRI_UPSCALER_TYPE_DLSS_SR);
            const nri::UpscalerOptimalSettings opt = upscaler.getOptimalSettings(NRI_UPSCALER_MODE_QUALITY, { kOutW, kOutH });
            kRenderW = opt.renderSize.width > 0 ? opt.renderSize.width : kOutW;
            kRenderH = opt.renderSize.height > 0 ? opt.renderSize.height : kOutH;
        }
        else
        {
            kRenderW = 853;
            kRenderH = 480;
        }
        std::printf("[StreamlineNRDDemo] Output %ux%u -> DLSS Quality render extent %ux%u\n",
                    kOutW, kOutH, kRenderW, kRenderH);

        auto makeStorageTex = [&](uint32_t w, uint32_t h, NriImageFormat fmt)
        {
            return device.createTexture({
                .width = w,
                .height = h,
                .arrayLayers = 1,
                .mipLevels = 1,
                .sampleCount = 1,
                .usage = NRI_TEXTURE_USAGE_STORAGE,
                .format = fmt
            });
        };

        nri::Texture colorIn = makeStorageTex(kRenderW, kRenderH, NRI_FORMAT_R16G16B16A16_SFLOAT);
        nri::Texture depth = makeStorageTex(kRenderW, kRenderH, NRI_FORMAT_R32_SFLOAT);
        nri::Texture motionVectors = makeStorageTex(kRenderW, kRenderH, NRI_FORMAT_R16G16_SFLOAT);
        nri::Texture normalRoughness = makeStorageTex(kRenderW, kRenderH, NRI_FORMAT_R10G10B10A2_UNORM);
        nri::Texture viewZ = makeStorageTex(kRenderW, kRenderH, NRI_FORMAT_R16_SFLOAT);
        nri::Texture rawSpec = makeStorageTex(kRenderW, kRenderH, NRI_FORMAT_R16G16B16A16_SFLOAT);
        nri::Texture denoisedSpec = makeStorageTex(kRenderW, kRenderH, NRI_FORMAT_R16G16B16A16_SFLOAT);
        nri::Texture rawPenumbra = makeStorageTex(kRenderW, kRenderH, NRI_FORMAT_R16G16_SFLOAT);
        nri::Texture denoisedShadow = makeStorageTex(kRenderW, kRenderH, NRI_FORMAT_R16G16_SFLOAT);
        nri::Texture upscaledOut = makeStorageTex(kOutW, kOutH, NRI_FORMAT_R16G16B16A16_SFLOAT);

        nri::Denoiser denoiser;
        if (nrdAvail)
        {
            denoiser = nri::createDenoiser(device, {
                .width = kRenderW,
                .height = kRenderH,
                .instances = {
                    { 0u, NRI_DENOISER_SIGMA_SHADOW },
                    { 1u, NRI_DENOISER_REBLUR_SPECULAR }
                },
                .queuedFrameNum = 2
            });
        }

        nri::CommandAllocator alloc = device.createCommandAllocator(NRI_COMMAND_BUFFER_RESET_PER_COMMAND_BUFFER, NRI_QUEUE_GRAPHICS);
        nri::CommandBuffer cmd = alloc.allocateCommandBuffer(1);

        float view[16]{};
        float proj[16]{};
        makeIdentity(view);
        makePerspectiveReverseZ(0.785398f, static_cast<float>(kOutW) / static_cast<float>(kOutH), 0.1f, proj);

        for (uint32_t frame = 0; frame < frameCount; ++frame)
        {
            device.beginFrame(frame);
            cmd.begin(0, true);

            if (denoiser)
            {
                nri::DenoiserCommonSettings common{};
                std::memcpy(common.view, view, sizeof(view));
                std::memcpy(common.proj, proj, sizeof(proj));
                std::memcpy(common.prevView, view, sizeof(view));
                std::memcpy(common.prevProj, proj, sizeof(proj));
                common.frameIndex = frame;
                common.resetHistory = (frame == 0);
                denoiser.beginFrame(common);

                nri::DenoiserSigmaSettings sigma{};
                sigma.lightDirection[1] = -1.0f;
                denoiser.setSigmaSettings(0u, sigma);

                nri::DenoiserReblurSettings reblur{};
                denoiser.setReblurSettings(1u, reblur);

                nri::DenoiserResourcesDesc sigmaRes{};
                sigmaRes.inMotionVectors = motionVectors.handle();
                sigmaRes.inNormalRoughness = normalRoughness.handle();
                sigmaRes.inViewZ = viewZ.handle();
                sigmaRes.inPenumbra = rawPenumbra.handle();
                sigmaRes.outShadowTranslucent = denoisedShadow.handle();
                sigmaRes.restoreInitialState = true;
                denoiser.denoise(cmd, 0u, sigmaRes);

                nri::DenoiserResourcesDesc specRes{};
                specRes.inMotionVectors = motionVectors.handle();
                specRes.inNormalRoughness = normalRoughness.handle();
                specRes.inViewZ = viewZ.handle();
                specRes.inSpecRadianceHitDist = rawSpec.handle();
                specRes.outSpecRadianceHitDist = denoisedSpec.handle();
                specRes.restoreInitialState = true;
                denoiser.denoise(cmd, 1u, specRes);
            }

            if (dlssSR)
            {
                nri::UpscalerEvaluateDesc evalDesc{};
                evalDesc.inputColor = colorIn.handle();
                evalDesc.outputColor = upscaledOut.handle();
                evalDesc.depth = depth.handle();
                evalDesc.motionVectors = motionVectors.handle();
                std::memcpy(evalDesc.nonJitteredProj, proj, sizeof(proj));
                std::memcpy(evalDesc.view, view, sizeof(view));
                std::memcpy(evalDesc.prevNonJitteredProj, proj, sizeof(proj));
                std::memcpy(evalDesc.prevView, view, sizeof(view));
                evalDesc.cameraUp[1] = 1.0f;
                evalDesc.cameraRight[0] = 1.0f;
                evalDesc.cameraFwd[2] = -1.0f;
                evalDesc.cameraNear = 0.1f;
                evalDesc.cameraFar = 10000.0f;
                evalDesc.cameraFovRad = 0.785398f;
                evalDesc.reset = (frame == 0);
                evalDesc.mode = NRI_UPSCALER_MODE_QUALITY;
                if (!upscaler.evaluate(cmd, evalDesc))
                {
                    std::fprintf(stderr, "[StreamlineNRDDemo] Upscaler evaluation failed on frame %u\n", frame);
                    return 1;
                }
            }

            cmd.end(0);
            device.submitAndWait(cmd, 0);
        }

        device.waitIdle();
        denoiser.reset();
        upscaler.reset();
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "[StreamlineNRDDemo] Exception: %s\n", e.what());
        return 1;
    }

    if (g_validationErrors > 0)
    {
        std::fprintf(stderr, "[StreamlineNRDDemo] Failed with %d validation errors\n", g_validationErrors);
        return 1;
    }

    std::printf("[StreamlineNRDDemo] Clean exit.\n");
    return 0;
}
