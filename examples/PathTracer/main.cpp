// PathTracer -- a 1-sample-per-pixel path tracer on Vulkan and Direct3D 12 (one Slang source, ray queries in a compute shader), with
// its noise removed by NVIDIA NRD (REBLUR) or DLSS Ray Reconstruction (Streamline), next to an accumulated reference.
//
//   PathTracer.exe --vk      PathTracer.exe --dx12      (add --dlss-rr to load Streamline for DLSS-RR)
//
// Keys:   1 reference (accumulates while the camera stands still)   2 noisy (1 sample per pixel)   3 NRD   4 DLSS-RR
//         G next guide (normal, roughness, view Z, motion, diffuse albedo, specular albedo, specular hit distance, depth; then the image)
//         Space pause the camera        + / - exposure
// Options: --mode reference|noisy|nrd|dlss-rr   --dlss-rr (Streamline on, and DLSS-RR mode)   --guide N   --exposure X
//
// NRD runs where the NRD SDK was found when NoxRHI was built (vendors/NRD: scripts\install_dependencies.bat), DLSS-RR where the Streamline
// SDK was found and the GPU supports it (an NVIDIA RTX GPU). Without them those modes fall back to the noisy image and say so.

#include "../Common/ExampleApp.h"

#include <filesystem>

namespace
{
    constexpr uint32_t kMaxObjects = 16;

    struct Vertex
    {
        float position[4];
        float normal[4];
    };

    struct Object
    {
        nox::Mat4 model;
        nox::Mat4 normalMatrix;
        nox::Float4 baseColorRoughness;
        nox::Float4 material; // x metalness
    };

    struct Targets
    {
        uint32_t viewZ, motion, normalRoughness, diffuse, specular;
        uint32_t color, depth, diffuseAlbedo, specularAlbedo, rrNormalRoughness, specularHitDistance;
        uint32_t nrdDiffuse, nrdSpecular, rrOutput;
        uint32_t display;
        uint32_t padding;
    };

    struct Frame
    {
        nox::Mat4 worldToView, viewToClip, worldToClipPrev;
        nox::Float4 cameraPosition, cameraForward, cameraRight, cameraUp, sunDirection, sunIrradiance, hitDistanceParams;
        float jitter[2];
        uint32_t width, height, frameIndex, accumulatedFrames, mode, guide;
        float exposure;
        uint32_t objectCount;
        uint32_t padding[2];
        Targets storage; // (csPathTrace writes through these, csComposite writes display)
        Targets sampled; // (csComposite reads through these)
    };

    struct Push
    {
        uint64_t frame, objects, vertices, tlas, accumulation;
    };

    struct PresentPush
    {
        uint32_t imageSlot;
    };

    enum Mode : uint32_t { kReference = 0, kNoisy = 1, kNrd = 2, kDlssRR = 3 };
    const char* kModeNames[] = { "reference (accumulated)", "noisy (1 sample per pixel)", "NRD (REBLUR diffuse + specular)", "DLSS-RR" };
    constexpr uint32_t kGuideCount = 8;
    const char* kGuideNames[] = { "image", "world normal", "linear roughness", "view Z (bands of 10)", "motion vectors (x50)", "diffuse albedo",
                                  "specular albedo", "specular hit distance (/20)", "reverse-Z depth (x10)" };

    // REBLUR's hit distance normalization: the values NoxRHI's NRD host gives REBLUR by default (NriDenoiserReblurSettings), sent to the
    // shader as well -- REBLUR_FrontEnd_GetNormHitDist must use the same ones.
    constexpr float kHitDistanceA = 3.0f, kHitDistanceB = 0.1f, kHitDistanceC = 20.0f;

    std::vector<Vertex> MakeCube()
    {
        std::vector<Vertex> vertices;
        const float faces[6][3][3] = {
            { { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 } }, { { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },
            { { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } }, { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },
            { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } },  { { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 } } };
        const float corners[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
        for (const auto& f : faces)
            for (const auto& c : corners)
            {
                Vertex v{};
                for (int i = 0; i < 3; ++i)
                {
                    v.position[i] = f[0][i] + f[1][i] * c[0] + f[2][i] * c[1];
                    v.normal[i] = f[0][i];
                }
                vertices.push_back(v);
            }
        return vertices;
    }

    nri::AccelerationStructure CreateAccelerationStructure(nox::Host& host, NriAccelerationStructureType type, uint64_t size, nri::Buffer& storage)
    {
        storage = host.device.createBuffer({ .size = size, .usage = NRI_BUFFER_USAGE_ACCELERATION_STRUCTURE });
        const NriAccelerationStructureDesc desc{ type, storage.handle(), 0, size };
        NriAccelerationStructure handle = nullptr;
        if (nriCreateAccelerationStructure(host.device.handle(), &desc, &handle) != NRI_SUCCESS || !handle)
            throw std::runtime_error("nriCreateAccelerationStructure failed");
        return nri::AccelerationStructure(handle);
    }

    void Copy(float out[16], const nox::Mat4& m) { std::memcpy(out, &m, sizeof(float) * 16); } // (column-major: what NRD and the upscaler take)
}

class PathTracer final : public nox::Example
{
public:
    const char* Name() const override { return "PathTracer"; }

    int OnArgument(int argc, char** argv, int index) override
    {
        if (!std::strcmp(argv[index], "--share-command-buffer"))
        {
            m_DenoiserPassFlags = nri::RGPassFlags::None; // (to compare: NRD / DLSS-RR recorded with the passes around them)
            return 1;
        }
        if (!std::strcmp(argv[index], "--dlss-rr"))
        {
            m_StreamlineWanted = true;
            m_Mode = kDlssRR;
            return 1;
        }
        if (index + 1 >= argc)
            return 0;
        if (!std::strcmp(argv[index], "--mode"))
        {
            const char* mode = argv[index + 1];
            m_Mode = !std::strcmp(mode, "noisy") ? kNoisy : !std::strcmp(mode, "nrd") ? kNrd : !std::strcmp(mode, "dlss-rr") ? kDlssRR : kReference;
            m_StreamlineWanted |= m_Mode == kDlssRR;
            return 2;
        }
        if (!std::strcmp(argv[index], "--guide"))
        {
            m_Guide = uint32_t(std::strtoul(argv[index + 1], nullptr, 10)) % (kGuideCount + 1);
            return 2;
        }
        if (!std::strcmp(argv[index], "--exposure"))
        {
            m_Exposure = float(std::atof(argv[index + 1]));
            return 2;
        }
        return 0;
    }

    void ConfigureDevice(nri::DeviceDesc& desc) override
    {
        // Streamline must be up before the device is created (it hooks the device's creation); only when DLSS-RR was asked for.
        desc.enableStreamline = m_StreamlineWanted;
    }

    void Init(nox::Host& host) override
    {
        CreateScene(host);

        // NRD: the library (NoxRHI's denoiser extension) and its shader header for the packing functions (NRD.hlsli, NRDConfig.hlsli).
        m_NrdAvailable = nri::isDenoiserAvailable(host.device);
#ifdef NOXRHI_NRD_SHADER_DIR
        if (m_NrdAvailable && std::filesystem::exists(std::filesystem::path(NOXRHI_NRD_SHADER_DIR) / "NRD.hlsli"))
        {
            host.compiler.addSearchPath(NOXRHI_NRD_SHADER_DIR);
            host.compiler.setDefine("NOX_HAS_NRD", "1");
            m_NrdShaders = true;
        }
#endif
        if (m_NrdAvailable && !m_NrdShaders)
            std::printf("[PathTracer] NRD is available but NRD.hlsli was not found (vendors/NRD/Shaders): NRD mode is off\n");
        // DLSS-RR: Streamline (enabled at device creation with --dlss-rr) and a GPU that supports Ray Reconstruction.
        if (m_StreamlineWanted && nri::isUpscalerSupported(host.device, NRI_UPSCALER_TYPE_DLSS_RR))
            m_Upscaler = nri::createUpscaler(host.device, NRI_UPSCALER_TYPE_DLSS_RR);
        std::printf("[PathTracer] NRD: %s, DLSS-RR: %s\n", m_NrdAvailable && m_NrdShaders ? "available" : "not available",
                    m_Upscaler ? "available" : m_StreamlineWanted ? "not supported (Streamline / GPU)" : "off (start with --dlss-rr)");

        m_Frame.Create(host);
        m_PathTrace = host.Compute("path_tracer.slang", "csPathTrace");
        m_Composite = host.Compute("path_tracer.slang", "csComposite");
        m_Present = host.Graphics("path_tracer.slang", "vsFullscreen", "psPresent");
        PrintMode();
    }

    void Render(nox::Host& host, nri::RenderGraph& graph, const nox::FrameInfo& frame) override
    {
        ReportSlowFrame();
        if (frame.extent.width != m_Width || frame.extent.height != m_Height)
            CreateTargets(host, frame.extent.width, frame.extent.height);

        // Camera: an orbit around the scene (Space pauses it).
        if (!m_Paused)
            m_AnimTime += frame.deltaTime;
        const float orbit = 0.6f + m_AnimTime * 0.15f;
        const nox::Float3 eye{ std::sin(orbit) * 10.0f, 3.0f + 0.8f * std::sin(m_AnimTime * 0.3f), std::cos(orbit) * 10.0f };
        const nox::Float3 target{ 0.0f, 1.0f, 0.0f }, worldUp{ 0.0f, 1.0f, 0.0f };
        const float fovY = 0.9f, aspect = float(m_Width) / float(m_Height), nearPlane = 0.1f;
        const nox::Mat4 view = nox::LookAt(eye, target, worldUp);
        const nox::Mat4 proj = nox::PerspectiveReverseZ(fovY, aspect, nearPlane);
        const nox::Mat4 viewProj = proj * view;
        const bool cameraMoved = std::memcmp(&viewProj, &m_PrevViewProj, sizeof(viewProj)) != 0;
        if (m_FirstFrame)
        {
            m_PrevView = view;
            m_PrevProj = proj;
            m_PrevViewProj = viewProj;
        }
        m_Accumulated = cameraMoved || m_FirstFrame ? 0 : m_Accumulated + 1;
        const nox::Float3 forward = nox::Normalize(target - eye), right = nox::Normalize(nox::Cross(forward, worldUp)), up = nox::Cross(right, forward);

        const uint32_t mode = EffectiveMode();
        Frame data{};
        data.worldToView = view;
        data.viewToClip = proj;
        data.worldToClipPrev = m_PrevViewProj;
        data.cameraPosition = { eye.x, eye.y, eye.z, 1 };
        data.cameraForward = { forward.x, forward.y, forward.z, std::tan(fovY * 0.5f) };
        data.cameraRight = { right.x, right.y, right.z, aspect };
        data.cameraUp = { up.x, up.y, up.z, 0 };
        const nox::Float3 sun = nox::Normalize({ 0.45f, 0.8f, 0.35f });
        data.sunDirection = { sun.x, sun.y, sun.z, std::tan(0.0093f * 2.0f) }; // (a sun twice the real one's angular size: softer shadows)
        data.sunIrradiance = { 3.2f, 3.0f, 2.7f, 0 };
        data.hitDistanceParams = { kHitDistanceA, kHitDistanceB, kHitDistanceC, 0 };
        if (mode == kReference)
        {
            // (the reference accumulates jittered samples: anti-aliased; the denoised modes trace pixel centres, as their guides are)
            data.jitter[0] = Halton(m_FrameIndex + 1, 2) - 0.5f;
            data.jitter[1] = Halton(m_FrameIndex + 1, 3) - 0.5f;
        }
        data.width = m_Width;
        data.height = m_Height;
        data.frameIndex = m_FrameIndex;
        data.accumulatedFrames = m_Accumulated;
        data.mode = mode;
        data.guide = m_Guide;
        data.exposure = m_Exposure;
        data.objectCount = m_ObjectCount;
        data.storage = m_StorageTargets;
        data.sampled = m_SampledTargets;
        const Push push{ m_Frame.Write(frame.frameSlot, data), m_ObjectsRef, m_VerticesRef, m_TlasRef, m_AccumulationRef };

        // --- the frame's passes ---
        auto import = [&](const char* name, nri::Texture& texture) { return graph.ImportTexture(name, texture); };
        const nri::RGTexture viewZ = import("ViewZ", m_ViewZ), motion = import("Motion", m_Motion), normalRoughness = import("NormalRoughness", m_NormalRoughness);
        const nri::RGTexture diffuse = import("Diffuse", m_Diffuse), specular = import("Specular", m_Specular), color = import("Color", m_Color);
        const nri::RGTexture depth = import("Depth", m_Depth), diffuseAlbedo = import("DiffuseAlbedo", m_DiffuseAlbedo);
        const nri::RGTexture specularAlbedo = import("SpecularAlbedo", m_SpecularAlbedo), rrNormalRoughness = import("RRNormalRoughness", m_RRNormalRoughness);
        const nri::RGTexture specularHitDistance = import("SpecularHitDistance", m_SpecularHitDistance);
        const nri::RGTexture nrdDiffuse = import("NrdDiffuse", m_NrdDiffuse), nrdSpecular = import("NrdSpecular", m_NrdSpecular);
        const nri::RGTexture rrOutput = import("RROutput", m_RROutput), display = import("Display", m_Display);
        const nri::RGBuffer accumulation = graph.ImportBuffer("Accumulation", m_Accumulation);
        const nri::RGTexture written[] = { viewZ, motion, normalRoughness, diffuse, specular, color, depth, diffuseAlbedo, specularAlbedo, rrNormalRoughness, specularHitDistance };

        const uint32_t groupsX = (m_Width + 7) / 8, groupsY = (m_Height + 7) / 8;
        graph.AddPass("PathTrace", nri::RGPassFlags::None,
            [&](nri::RGBuilder& b) {
                for (const nri::RGTexture& t : written)
                    b.Write(t, nri::RGTextureAccess::StorageWrite);
                b.Write(accumulation, nri::RGBufferAccess::Write);
            },
            [this, push, groupsX, groupsY](nri::RGPassContext& ctx) {
                ctx.Cmd().bindPipeline(NRI_PIPELINE_BIND_POINT_COMPUTE, m_PathTrace);
                ctx.Cmd().pushData(&push, sizeof(push));
                ctx.Cmd().dispatch(groupsX, groupsY, 1);
            });

        if (mode == kNrd)
        {
            nri::DenoiserCommonSettings common{};
            Copy(common.view, view);
            Copy(common.proj, proj);
            Copy(common.prevView, m_PrevView);
            Copy(common.prevProj, m_PrevProj);
            common.motionVectorScale[0] = common.motionVectorScale[1] = 1.0f; // (2D motion in uv units: prevUv = uv + mv)
            common.frameIndex = m_FrameIndex;
            common.denoisingRange = 1000.0f; // (the sky's view Z is beyond it)
            common.resetHistory = m_ResetNrd;
            m_ResetNrd = false;
            m_Denoiser.beginFrame(common);
            nri::DenoiserReblurSettings reblur{};
            reblur.hitDistanceA = kHitDistanceA;
            reblur.hitDistanceB = kHitDistanceB;
            reblur.hitDistanceC = kHitDistanceC;
            m_Denoiser.setReblurSettings(0, reblur);
            graph.AddPass("NRD", m_DenoiserPassFlags,
                [&](nri::RGBuilder& b) {
                    for (const nri::RGTexture& t : { viewZ, motion, normalRoughness, diffuse, specular })
                        b.Read(t, nri::RGTextureAccess::StorageRead);
                    b.Write(nrdDiffuse, nri::RGTextureAccess::StorageWrite);
                    b.Write(nrdSpecular, nri::RGTextureAccess::StorageWrite);
                },
                [this](nri::RGPassContext& ctx) {
                    nri::DenoiserResourcesDesc resources{};
                    resources.inMotionVectors = m_Motion.handle();
                    resources.inNormalRoughness = m_NormalRoughness.handle();
                    resources.inViewZ = m_ViewZ.handle();
                    resources.inDiffRadianceHitDist = m_Diffuse.handle();
                    resources.outDiffRadianceHitDist = m_NrdDiffuse.handle();
                    resources.inSpecRadianceHitDist = m_Specular.handle();
                    resources.outSpecRadianceHitDist = m_NrdSpecular.handle();
                    resources.restoreInitialState = true;
                    if (!m_Denoiser.denoise(ctx.Cmd(), 0, resources))
                        ReportOnce(m_NrdFailed, "NRD's denoise call failed");
                });
        }
        else if (mode == kDlssRR)
        {
            nri::UpscalerEvaluateDesc rr{};
            rr.inputColor = m_Color.handle();
            rr.outputColor = m_RROutput.handle();
            rr.depth = m_Depth.handle();
            rr.motionVectors = m_Motion.handle();
            rr.albedo = m_DiffuseAlbedo.handle();
            rr.specularAlbedo = m_SpecularAlbedo.handle();
            rr.normal = m_RRNormalRoughness.handle();
            rr.normalRoughnessPacked = true;      // (xyz world normal, w linear roughness)
            rr.specularHitDistance = m_SpecularHitDistance.handle();
            Copy(rr.nonJitteredProj, proj);
            Copy(rr.view, view);
            Copy(rr.prevNonJitteredProj, m_PrevProj);
            Copy(rr.prevView, m_PrevView);
            rr.motionVectorScale[0] = rr.motionVectorScale[1] = 1.0f; // (uv units, prevUv = uv + mv)
            rr.cameraPos[0] = eye.x; rr.cameraPos[1] = eye.y; rr.cameraPos[2] = eye.z;
            rr.cameraUp[0] = up.x; rr.cameraUp[1] = up.y; rr.cameraUp[2] = up.z;
            rr.cameraRight[0] = right.x; rr.cameraRight[1] = right.y; rr.cameraRight[2] = right.z;
            rr.cameraFwd[0] = forward.x; rr.cameraFwd[1] = forward.y; rr.cameraFwd[2] = forward.z;
            rr.cameraNear = nearPlane;
            rr.cameraFovRad = fovY;
            rr.reset = m_ResetRR;
            m_ResetRR = false;
            rr.mode = NRI_UPSCALER_MODE_DLAA;     // (render size = output size; no jitter: the guides are pixel centres)
            graph.AddPass("DLSS-RR", m_DenoiserPassFlags,
                [&](nri::RGBuilder& b) {
                    for (const nri::RGTexture& t : { color, depth, motion, diffuseAlbedo, specularAlbedo, rrNormalRoughness, specularHitDistance })
                        b.Read(t, nri::RGTextureAccess::StorageRead);
                    b.Write(rrOutput, nri::RGTextureAccess::StorageWrite);
                },
                [this, rr](nri::RGPassContext& ctx) {
                    if (!m_Upscaler.evaluate(ctx.Cmd(), rr))
                        ReportOnce(m_RRFailed, "DLSS-RR's evaluate call failed (see the [Streamline] lines above)");
                });
        }

        graph.AddPass("Composite", nri::RGPassFlags::None,
            [&](nri::RGBuilder& b) {
                for (const nri::RGTexture& t : written)
                    b.Read(t, nri::RGTextureAccess::Sampled);
                if (mode == kNrd)
                {
                    b.Read(nrdDiffuse, nri::RGTextureAccess::Sampled);
                    b.Read(nrdSpecular, nri::RGTextureAccess::Sampled);
                }
                if (mode == kDlssRR)
                    b.Read(rrOutput, nri::RGTextureAccess::Sampled);
                b.Write(accumulation, nri::RGBufferAccess::Write); // (read through RWBufferRef -- a UAV on D3D12, so its state stays UAV)
                b.Write(display, nri::RGTextureAccess::StorageWrite);
            },
            [this, push, groupsX, groupsY](nri::RGPassContext& ctx) {
                ctx.Cmd().bindPipeline(NRI_PIPELINE_BIND_POINT_COMPUTE, m_Composite);
                ctx.Cmd().pushData(&push, sizeof(push));
                ctx.Cmd().dispatch(groupsX, groupsY, 1);
            });

        // --watch: how long each of these kept the same pixels (the final image; in NRD mode also NRD's diffuse input and output).
        host.Watch(graph, display, "Display", 8, frame);
        if (mode == kNrd)
        {
            host.Watch(graph, diffuse, "NRD diffuse input", 8, frame);
            host.Watch(graph, nrdDiffuse, "NRD diffuse output", 8, frame);
        }
        host.Probe(graph, display, "Display", 8, frame); // (--probe: the composited image's average, e.g. to see in CI that it is not black)

        graph.AddPass("Present", nri::RGPassFlags::Raster,
            [&](nri::RGBuilder& b) {
                b.Read(display, nri::RGTextureAccess::Sampled);
                b.SwapchainTarget(host.swapchain, frame.imageIndex, NRI_LOAD_OP_DONT_CARE, NRI_STORE_OP_STORE, { 0, 0, 0, 1 });
                b.SetRenderArea(frame.extent);
            },
            [this](nri::RGPassContext& ctx) {
                nri::CommandBuffer& cmd = ctx.Cmd();
                nox::Host::SetRasterState(cmd, {});
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, m_Present);
                const PresentPush present{ m_SampledTargets.display };
                cmd.pushData(&present, sizeof(present));
                cmd.draw(3, 1, 0, 0);
            });

        char status[160];
        std::snprintf(status, sizeof(status), "%s%s%s", m_Guide ? "guide: " : kModeNames[mode], m_Guide ? kGuideNames[m_Guide] : "",
                      mode == kReference && !m_Guide ? (m_Paused ? " [paused: accumulating]" : " [Space: pause to accumulate]") : "");
        host.SetStatus(status);

        m_PrevView = view;
        m_PrevProj = proj;
        m_PrevViewProj = viewProj;
        m_FirstFrame = false;
        ++m_FrameIndex;
    }

    void OnKey(SDL_Keycode key) override
    {
        if (key >= SDLK_1 && key <= SDLK_4)
        {
            m_Mode = uint32_t(key - SDLK_1);
            m_ResetNrd = m_ResetRR = true;
            m_FramesSinceSwitch = 0;
            m_Guide = 0;
            PrintMode();
        }
        else if (key == SDLK_G)
        {
            m_Guide = (m_Guide + 1) % (kGuideCount + 1);
            std::printf("[PathTracer] showing: %s\n", kGuideNames[m_Guide]);
        }
        else if (key == SDLK_SPACE)
            m_Paused = !m_Paused;
        else if (key == SDLK_EQUALS || key == SDLK_KP_PLUS)
            m_Exposure *= 1.25f;
        else if (key == SDLK_MINUS || key == SDLK_KP_MINUS)
            m_Exposure /= 1.25f;
    }

    void Shutdown(nox::Host&) override
    {
        m_Denoiser.reset();
        m_Upscaler.reset();
        DestroyTargets();
        m_PathTrace.reset();
        m_Composite.reset();
        m_Present.reset();
        m_Frame.Reset();
        for (nri::Buffer* b : { &m_Vertices, &m_Indices, &m_Objects, &m_BlasStorage, &m_BlasScratch, &m_TlasStorage, &m_TlasScratch, &m_Instances })
            b->reset();
        m_Blas.reset();
        m_Tlas.reset();
    }

private:
    uint32_t EffectiveMode() const
    {
        if (m_Mode == kNrd && !(m_Denoiser && m_NrdShaders))
            return kNoisy;
        if (m_Mode == kDlssRR && !m_Upscaler)
            return kNoisy;
        return m_Mode;
    }

    void PrintMode()
    {
        std::printf("[PathTracer] mode: %s\n", kModeNames[m_Mode]);
        if (m_Mode == kNrd && !(m_NrdAvailable && m_NrdShaders))
            std::printf("[PathTracer]   NRD is not available in this build (vendors/NRD): showing the noisy image\n");
        if (m_Mode == kDlssRR && !m_Upscaler)
            std::printf("[PathTracer]   DLSS-RR is not available (%s): showing the noisy image\n",
                        m_StreamlineWanted ? "Streamline did not load DLSS-RR, or the GPU cannot run it" : "start with --dlss-rr");
    }

    // A frame that took over 150 ms (CPU, from one Render to the next -- it includes waiting for the GPU) is reported with the mode and
    // how many frames after the last mode switch it came: to tell a one-off cost of a switch (pipelines or resources created on first use)
    // from a steady one.
    void ReportSlowFrame()
    {
        const auto now = std::chrono::steady_clock::now();
        if (m_LastRender.time_since_epoch().count() != 0)
        {
            const double ms = std::chrono::duration<double, std::milli>(now - m_LastRender).count();
            if (ms > 150.0)
                std::printf("[PathTracer] slow frame: %.0f ms (mode %s, %u frames after the last switch)\n", ms, kModeNames[EffectiveMode()], m_FramesSinceSwitch);
        }
        m_LastRender = now;
        ++m_FramesSinceSwitch;
    }

    static void ReportOnce(bool& reported, const char* message)
    {
        if (!reported)
            std::printf("[PathTracer] %s\n", message);
        reported = true;
    }

    static float Halton(uint32_t index, uint32_t base)
    {
        float result = 0.0f, f = 1.0f;
        for (uint32_t i = index; i > 0; i /= base)
        {
            f /= float(base);
            result += f * float(i % base);
        }
        return result;
    }

    void CreateScene(nox::Host& host)
    {
        const std::vector<Vertex> cube = MakeCube();
        std::vector<uint32_t> indices(cube.size());
        for (uint32_t i = 0; i < indices.size(); ++i)
            indices[i] = i;
        m_Vertices = host.CreateBuffer(cube.size() * sizeof(Vertex), NRI_BUFFER_USAGE_STORAGE_STATIC, cube.data());
        m_VerticesRef = host.Refs(m_Vertices, cube.size() * sizeof(Vertex), false).read;
        m_Indices = host.CreateBuffer(indices.size() * 4, NRI_BUFFER_USAGE_STORAGE_STATIC, indices.data());

        // The objects: boxes (the unit cube scaled, rotated and moved) with a base colour, roughness and metalness each.
        std::vector<Object> objects;
        const auto add = [&](nox::Float3 position, nox::Float3 scale, float yaw, nox::Float3 baseColor, float roughness, float metalness) {
            const nox::Mat4 rotation = nox::RotateY(yaw);
            objects.push_back({ nox::Translate(position) * rotation * nox::Scale(scale), rotation * nox::Scale({ 1 / scale.x, 1 / scale.y, 1 / scale.z }),
                                { baseColor.x, baseColor.y, baseColor.z, roughness }, { metalness, 0, 0, 0 } });
        };
        add({ 0, -0.5f, 0 }, { 30, 0.5f, 30 }, 0, { 0.62f, 0.62f, 0.6f }, 0.85f, 0);           // ground, top at y = 0
        add({ 0, 2.0f, -4.5f }, { 7, 2.0f, 0.25f }, 0, { 0.8f, 0.78f, 0.74f }, 0.9f, 0);       // wall behind
        add({ -3.0f, 1.0f, -1.0f }, { 1, 1, 1 }, 0.4f, { 1.0f, 0.78f, 0.34f }, 0.25f, 1);      // gold
        add({ 0.0f, 0.75f, 0.5f }, { 0.75f, 0.75f, 0.75f }, -0.3f, { 0.8f, 0.1f, 0.08f }, 0.15f, 0); // glossy red plastic
        add({ 3.0f, 1.5f, -1.5f }, { 0.6f, 1.5f, 0.6f }, 0.8f, { 0.95f, 0.95f, 0.95f }, 0.04f, 1); // chrome pillar (a near mirror)
        add({ 2.2f, 0.4f, 2.0f }, { 0.4f, 0.4f, 0.4f }, 0.2f, { 0.15f, 0.35f, 0.8f }, 0.5f, 0); // rough blue
        add({ -1.6f, 0.3f, 2.4f }, { 0.3f, 0.3f, 0.3f }, 0.9f, { 0.9f, 0.9f, 0.9f }, 0.9f, 0);  // matte white
        m_ObjectCount = uint32_t(objects.size());
        m_Objects = host.CreateBuffer(objects.size() * sizeof(Object), NRI_BUFFER_USAGE_STORAGE_STATIC, objects.data());
        m_ObjectsRef = host.Refs(m_Objects, objects.size() * sizeof(Object), false).read;

        // BLAS: the unit cube. TLAS: one instance per object, built once (the scene does not move).
        NriAccelerationStructureTrianglesDesc triangles{};
        triangles.vertexBufferAddress = m_Vertices.getDeviceAddress();
        triangles.vertexStride = sizeof(Vertex);
        triangles.maxVertex = uint32_t(cube.size() - 1);
        triangles.indexBufferAddress = m_Indices.getDeviceAddress();
        triangles.primitiveCount = uint32_t(indices.size() / 3);
        triangles.isOpaque = true;
        NriAccelerationStructureBuildDesc blasBuild{};
        blasBuild.type = NRI_ACCELERATION_STRUCTURE_BOTTOM_LEVEL;
        blasBuild.flags = NRI_AS_BUILD_PREFER_FAST_TRACE;
        blasBuild.triangles = &triangles;
        blasBuild.triangleCount = 1;
        const NriAccelerationStructureBuildSizes blasSizes = nriGetAccelerationStructureBuildSizes(host.device.handle(), &blasBuild);
        if (blasSizes.accelerationStructureSize == 0)
            throw std::runtime_error("this device has no ray tracing");
        m_Blas = CreateAccelerationStructure(host, NRI_ACCELERATION_STRUCTURE_BOTTOM_LEVEL, blasSizes.accelerationStructureSize, m_BlasStorage);
        m_BlasScratch = host.device.createBuffer({ .size = blasSizes.buildScratchSize, .usage = NRI_BUFFER_USAGE_ACCELERATION_STRUCTURE_SCRATCH });

        NriAccelerationStructureBuildDesc tlasBuild{};
        tlasBuild.type = NRI_ACCELERATION_STRUCTURE_TOP_LEVEL;
        tlasBuild.flags = NRI_AS_BUILD_PREFER_FAST_TRACE;
        tlasBuild.instances = { 0, kMaxObjects };
        const NriAccelerationStructureBuildSizes tlasSizes = nriGetAccelerationStructureBuildSizes(host.device.handle(), &tlasBuild);
        m_Tlas = CreateAccelerationStructure(host, NRI_ACCELERATION_STRUCTURE_TOP_LEVEL, tlasSizes.accelerationStructureSize, m_TlasStorage);
        m_TlasScratch = host.device.createBuffer({ .size = tlasSizes.buildScratchSize, .usage = NRI_BUFFER_USAGE_ACCELERATION_STRUCTURE_SCRATCH });
        m_Instances = host.device.createBuffer({ .size = sizeof(NriAccelerationStructureInstance) * kMaxObjects, .usage = NRI_BUFFER_USAGE_ACCELERATION_STRUCTURE_INSTANCE });
        std::vector<NriAccelerationStructureInstance> instances(objects.size());
        for (size_t i = 0; i < objects.size(); ++i)
        {
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c)
                    instances[i].transform.matrix[r][c] = objects[i].model.At(r, c); // (3 x 4, row-major: both APIs)
            instances[i].instanceCustomIndex = uint32_t(i); // (the shader's CommittedInstanceID: the object's index)
            instances[i].mask = 0xFF;
            instances[i].accelerationStructureReference = m_Blas.getDeviceAddress();
        }
        std::memcpy(m_Instances.map(0, instances.size() * sizeof(instances[0])), instances.data(), instances.size() * sizeof(instances[0]));
        m_Instances.unmap();

        host.Submit([&](nri::CommandBuffer& cmd) {
            const nri::ResourceState shaderRead{ NRI_ACCESS_SHADER_READ, NRI_STAGE_VERTEX | NRI_STAGE_FRAGMENT | NRI_STAGE_COMPUTE };
            const nri::ResourceState buildInput{ NRI_ACCESS_ACCELERATION_STRUCTURE_READ, NRI_STAGE_ACCELERATION_STRUCTURE_BUILD };
            const nri::ResourceState scratch{ NRI_ACCESS_ACCELERATION_STRUCTURE_READ | NRI_ACCESS_ACCELERATION_STRUCTURE_WRITE, NRI_STAGE_ACCELERATION_STRUCTURE_BUILD };
            const nri::BufferBarrierDesc before[] = { { m_Vertices.handle(), shaderRead, buildInput }, { m_Indices.handle(), shaderRead, buildInput },
                                                      { m_BlasScratch.handle(), {}, scratch }, { m_TlasScratch.handle(), {}, scratch } };
            cmd.resourceBarriers({}, before);
            nriCmdBuildAccelerationStructure(cmd.handle(), &blasBuild, m_BlasScratch.getDeviceAddress(), m_Blas.handle());
            nriCmdAccelerationStructureBarrier(cmd.handle(), NRI_AS_BARRIER_BUILD_TO_BUILD);
            NriAccelerationStructureBuildDesc build = tlasBuild;
            build.instances = { m_Instances.getDeviceAddress(), uint32_t(objects.size()) };
            nriCmdBuildAccelerationStructure(cmd.handle(), &build, m_TlasScratch.getDeviceAddress(), m_Tlas.handle());
            nriCmdAccelerationStructureBarrier(cmd.handle(), NRI_AS_BARRIER_BUILD_TO_SHADER_READ);
            const nri::BufferBarrierDesc after[] = { { m_Vertices.handle(), buildInput, shaderRead }, { m_Indices.handle(), buildInput, shaderRead } };
            cmd.resourceBarriers({}, after);
        });
        m_TlasRef = host.TlasRef(m_Tlas); // (D3D12: the descriptor is created after the build)
    }

    // The frame's textures at the window size, each registered twice: as a storage image (the path tracer writes it; NRD and DLSS-RR
    // read and write the textures directly) and as a sampled image (the composite pass reads it). Plus the reference's accumulation
    // buffer. Recreated when the window is resized.
    void CreateTargets(nox::Host& host, uint32_t width, uint32_t height)
    {
        host.device.waitIdle();
        DestroyTargets();
        m_Heap = &host.resources;
        m_Width = width;
        m_Height = height;
        const auto create = [&](nri::Texture& texture, NriImageFormat format, uint32_t& storageSlot, uint32_t& sampledSlot) {
            texture = host.device.createTexture({ .width = width, .height = height, .usage = NRI_TEXTURE_USAGE_STORAGE, .format = format });
            const uint32_t storage = host.resources.registerStorageTextureMip(texture, 0);
            host.resources.registerTexture(texture, NRI_TEXTURE_USAGE_SHADER_RESOURCE);
            const uint32_t sampled = texture.getDescriptorSlot();
            m_Slots.push_back(storage);
            m_Slots.push_back(sampled);
            storageSlot = host.ImageSlot(storage);
            sampledSlot = host.ImageSlot(sampled);
        };
        Targets storage{}, sampled{};
        create(m_ViewZ, NRI_FORMAT_R32_SFLOAT, storage.viewZ, sampled.viewZ);
        create(m_Motion, NRI_FORMAT_R16G16_SFLOAT, storage.motion, sampled.motion);
        create(m_NormalRoughness, NRI_FORMAT_R10G10B10A2_UNORM, storage.normalRoughness, sampled.normalRoughness); // (NRD's encoding 2)
        create(m_Diffuse, NRI_FORMAT_R16G16B16A16_SFLOAT, storage.diffuse, sampled.diffuse);
        create(m_Specular, NRI_FORMAT_R16G16B16A16_SFLOAT, storage.specular, sampled.specular);
        create(m_Color, NRI_FORMAT_R16G16B16A16_SFLOAT, storage.color, sampled.color);
        create(m_Depth, NRI_FORMAT_R32_SFLOAT, storage.depth, sampled.depth);
        create(m_DiffuseAlbedo, NRI_FORMAT_R16G16B16A16_SFLOAT, storage.diffuseAlbedo, sampled.diffuseAlbedo);
        create(m_SpecularAlbedo, NRI_FORMAT_R16G16B16A16_SFLOAT, storage.specularAlbedo, sampled.specularAlbedo);
        create(m_RRNormalRoughness, NRI_FORMAT_R16G16B16A16_SFLOAT, storage.rrNormalRoughness, sampled.rrNormalRoughness);
        create(m_SpecularHitDistance, NRI_FORMAT_R16_SFLOAT, storage.specularHitDistance, sampled.specularHitDistance);
        create(m_NrdDiffuse, NRI_FORMAT_R16G16B16A16_SFLOAT, storage.nrdDiffuse, sampled.nrdDiffuse);
        create(m_NrdSpecular, NRI_FORMAT_R16G16B16A16_SFLOAT, storage.nrdSpecular, sampled.nrdSpecular);
        create(m_RROutput, NRI_FORMAT_R16G16B16A16_SFLOAT, storage.rrOutput, sampled.rrOutput);
        create(m_Display, NRI_FORMAT_R16G16B16A16_SFLOAT, storage.display, sampled.display);
        m_StorageTargets = storage;
        m_SampledTargets = sampled;

        const uint64_t accumulationBytes = uint64_t(width) * height * 16;
        m_Accumulation = host.CreateBuffer(accumulationBytes, NRI_BUFFER_USAGE_STORAGE_STATIC);
        m_AccumulationRef = host.Refs(m_Accumulation, accumulationBytes, true).write;
        m_Accumulated = 0;
        m_FirstFrame = true;

        if (m_NrdAvailable && m_NrdShaders)
        {
            if (m_Denoiser)
                m_Denoiser.resize(width, height);
            else
                m_Denoiser = nri::createDenoiser(host.device, { .width = width, .height = height,
                                                                .instances = { { 0u, NRI_DENOISER_REBLUR_DIFFUSE_SPECULAR } } });
        }
        m_ResetNrd = m_ResetRR = true;
    }

    void DestroyTargets()
    {
        // Descriptors first: a storage registration is never freed by the texture, and on D3D12 the sampled one is not either (Vulkan's
        // texture frees its sampled slot itself; unregistering it here first is harmless there -- the free list ignores a slot twice).
        if (m_Heap)
            for (uint32_t slot : m_Slots)
                m_Heap->unregisterTexture(slot);
        m_Slots.clear();
        for (nri::Texture* t : { &m_ViewZ, &m_Motion, &m_NormalRoughness, &m_Diffuse, &m_Specular, &m_Color, &m_Depth, &m_DiffuseAlbedo,
                                 &m_SpecularAlbedo, &m_RRNormalRoughness, &m_SpecularHitDistance, &m_NrdDiffuse, &m_NrdSpecular, &m_RROutput, &m_Display })
            t->reset();
        m_Accumulation.reset();
    }

    nri::Buffer m_Vertices, m_Indices, m_Objects, m_BlasStorage, m_BlasScratch, m_TlasStorage, m_TlasScratch, m_Instances, m_Accumulation;
    nri::AccelerationStructure m_Blas, m_Tlas;
    uint64_t m_VerticesRef = 0, m_ObjectsRef = 0, m_TlasRef = 0, m_AccumulationRef = 0;
    uint32_t m_ObjectCount = 0;
    nri::Texture m_ViewZ, m_Motion, m_NormalRoughness, m_Diffuse, m_Specular, m_Color, m_Depth, m_DiffuseAlbedo, m_SpecularAlbedo,
        m_RRNormalRoughness, m_SpecularHitDistance, m_NrdDiffuse, m_NrdSpecular, m_RROutput, m_Display;
    std::vector<uint32_t> m_Slots; // (registered storage and sampled slots, unregistered by DestroyTargets)
    nri::DescriptorHeap* m_Heap = nullptr;
    Targets m_StorageTargets{}, m_SampledTargets{};
    uint32_t m_Width = 0, m_Height = 0;
    nox::FrameConstants<Frame> m_Frame;
    nri::Pipeline m_PathTrace, m_Composite, m_Present;
    nri::Denoiser m_Denoiser;
    // NRD and DLSS-RR record their own native commands: each in a command buffer of its own (RGPassFlags::OwnCommandBuffer). On Vulkan,
    // NoxRHI's composite after NRD in the same command buffer sometimes had no effect for 50-90 frames (the picture "froze"; --watch
    // showed NRD's output changing and the final image not). --share-command-buffer turns it off, to compare.
    nri::RGPassFlags m_DenoiserPassFlags = nri::RGPassFlags::OwnCommandBuffer;
    nri::Upscaler m_Upscaler;
    bool m_StreamlineWanted = false, m_NrdAvailable = false, m_NrdShaders = false, m_NrdFailed = false, m_RRFailed = false;
    bool m_ResetNrd = true, m_ResetRR = true, m_Paused = false, m_FirstFrame = true;
    uint32_t m_Mode = kReference, m_Guide = 0, m_FrameIndex = 0, m_Accumulated = 0;
    float m_AnimTime = 0.0f, m_Exposure = 1.0f;
    std::chrono::steady_clock::time_point m_LastRender{};
    uint32_t m_FramesSinceSwitch = 0;
    nox::Mat4 m_PrevView, m_PrevProj, m_PrevViewProj;
};

NOX_EXAMPLE_MAIN(PathTracer)
