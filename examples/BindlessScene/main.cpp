// BindlessScene -- one Slang source (bindless_scene.slang) on Vulkan and Direct3D 12.
//
//   BindlessScene.exe --vk      BindlessScene.exe --dx12
//
// 144 spinning cubes, each with its own texture from a bindless table, lit in an HDR target with a reverse-Z depth buffer
// ("Scene" pass), tonemapped by a compute shader into a storage texture ("Tonemap"), then drawn to the swapchain ("Present").
// Per-frame constants come from a CPU-written buffer, the cube and instances from GPU buffers. Inspired by the Nox engine's
// deferred / PBR path and its tonemapping pass. Space pauses the animation.

#include "../Common/ExampleApp.h"

namespace
{
    struct Scene
    {
        nox::Mat4 viewProj;
        nox::Float4 cameraPosition;
        nox::Float4 lightDirection;
        nox::Float4 lightColor;
        nox::Float4 ambient;
        float time;
        uint32_t textureCount;
        uint32_t pad0, pad1;
    };

    struct Instance
    {
        float positionScale[4];
        float tint[4];
        uint32_t texture;
        uint32_t pad0, pad1, pad2;
    };

    struct Vertex
    {
        float position[4];
        float normal[4];
        float uv[4];
    };

    struct ScenePush
    {
        uint64_t scene, instances, vertices, textures;
    };

    struct PostPush
    {
        uint32_t hdrSlot, outputSlot, width, height;
        float exposure;
    };

    struct PresentPush
    {
        uint32_t imageSlot;
    };

    constexpr uint32_t kGrid = 12;
    constexpr uint32_t kInstanceCount = kGrid * kGrid;
    constexpr uint32_t kTextureSize = 128;

    // A unit cube (-1..1), 6 faces x 2 counter-clockwise triangles, outward normals.
    std::vector<Vertex> MakeCube()
    {
        std::vector<Vertex> vertices;
        const float faces[6][3][3] = { // normal, u axis, v axis
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
                v.uv[0] = c[0] * 0.5f + 0.5f;
                v.uv[1] = 0.5f - c[1] * 0.5f;
                vertices.push_back(v);
            }
        return vertices;
    }

    uint32_t Rgba(float r, float g, float b)
    {
        const auto c = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        return c(r) | (c(g) << 8) | (c(b) << 16) | (255u << 24);
    }

    // Six procedural patterns, so every texture is recognisable on screen.
    std::vector<uint32_t> MakePattern(int kind)
    {
        std::vector<uint32_t> pixels(kTextureSize * kTextureSize);
        for (uint32_t y = 0; y < kTextureSize; ++y)
            for (uint32_t x = 0; x < kTextureSize; ++x)
            {
                const float u = (x + 0.5f) / kTextureSize, v = (y + 0.5f) / kTextureSize;
                float r = 1, g = 1, b = 1;
                switch (kind)
                {
                case 0: { const bool on = ((x / 16) + (y / 16)) & 1; r = g = b = on ? 0.95f : 0.25f; break; }                       // checker
                case 1: { const uint32_t row = y / 16; const bool mortar = (y % 16) < 2 || ((x + (row & 1) * 16) % 32) < 2;           // bricks
                          r = mortar ? 0.8f : 0.7f; g = mortar ? 0.8f : 0.3f; b = mortar ? 0.75f : 0.2f; break; }
                case 2: { const bool on = ((x + y) / 12) & 1; r = on ? 0.95f : 0.15f; g = on ? 0.8f : 0.15f; b = 0.1f; break; }       // stripes
                case 3: { const float dx = std::fmod(u * 6, 1.0f) - 0.5f, dy = std::fmod(v * 6, 1.0f) - 0.5f;                       // dots
                          const bool dot = dx * dx + dy * dy < 0.09f; r = dot ? 0.1f : 0.9f; g = dot ? 0.6f : 0.9f; b = dot ? 0.9f : 0.9f; break; }
                case 4: { const bool line = (x % 32) < 2 || (y % 32) < 2; r = line ? 0.1f : 0.85f; g = line ? 0.9f : 0.9f; b = line ? 0.4f : 0.85f; break; } // grid
                default: { const float d = std::sqrt((u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f));                            // rings
                           const bool on = int(d * 16) & 1; r = on ? 0.9f : 0.5f; g = on ? 0.3f : 0.1f; b = on ? 0.6f : 0.4f; break; }
                }
                pixels[y * kTextureSize + x] = Rgba(r, g, b);
            }
        return pixels;
    }
}

class BindlessScene final : public nox::Example
{
public:
    const char* Name() const override { return "BindlessScene"; }

    void Init(nox::Host& host) override
    {
        std::vector<uint32_t> slots;
        for (int kind = 0; kind < 6; ++kind)
        {
            const std::vector<uint32_t> pixels = MakePattern(kind);
            slots.push_back(host.CreateTexture(kTextureSize, kTextureSize, pixels.data(), m_Textures));
        }
        m_TextureCount = uint32_t(slots.size());
        m_TextureTable = host.CreateBuffer(slots.size() * 4, NRI_BUFFER_USAGE_STORAGE_STATIC, slots.data());
        m_TextureTableRef = host.Refs(m_TextureTable, slots.size() * 4, false).read;

        const std::vector<Vertex> cube = MakeCube();
        m_Vertices = host.CreateBuffer(cube.size() * sizeof(Vertex), NRI_BUFFER_USAGE_STORAGE_STATIC, cube.data());
        m_VerticesRef = host.Refs(m_Vertices, cube.size() * sizeof(Vertex), false).read;

        std::vector<Instance> instances(kInstanceCount);
        for (uint32_t i = 0; i < kInstanceCount; ++i)
        {
            const float gx = float(i % kGrid) - (kGrid - 1) * 0.5f, gz = float(i / kGrid) - (kGrid - 1) * 0.5f;
            Instance& inst = instances[i];
            inst.positionScale[0] = gx * 1.4f;
            inst.positionScale[1] = std::sin(gx * 0.7f) * std::cos(gz * 0.6f) * 0.8f;
            inst.positionScale[2] = gz * 1.4f;
            inst.positionScale[3] = 0.42f;
            const float hue = float(i) / kInstanceCount * 6.2831853f;
            inst.tint[0] = 0.75f + 0.25f * std::cos(hue);
            inst.tint[1] = 0.75f + 0.25f * std::cos(hue + 2.094f);
            inst.tint[2] = 0.75f + 0.25f * std::cos(hue + 4.188f);
            inst.tint[3] = 0.3f + 0.9f * float((i * 7) % 11) / 10.0f; // spin speed
            inst.texture = (i * 5 + i / kGrid) % m_TextureCount;
        }
        m_Instances = host.CreateBuffer(instances.size() * sizeof(Instance), NRI_BUFFER_USAGE_STORAGE_STATIC, instances.data());
        m_InstancesRef = host.Refs(m_Instances, instances.size() * sizeof(Instance), false).read;

        m_Scene.Create(host);
        m_ScenePipeline = host.Graphics("bindless_scene.slang", "vsScene", "psScene", { NRI_FORMAT_R16G16B16A16_SFLOAT });
        m_Tonemap = host.Compute("bindless_scene.slang", "csTonemap");
        m_Present = host.Graphics("bindless_scene.slang", "vsFullscreen", "psPresent");
    }

    void Render(nox::Host& host, nri::RenderGraph& graph, const nox::FrameInfo& frame) override
    {
        if (!m_Paused)
            m_AnimTime += frame.deltaTime;

        const float orbit = m_AnimTime * 0.12f;
        const nox::Float3 eye{ std::sin(orbit) * 14.0f, 7.0f, std::cos(orbit) * 14.0f };
        Scene scene{};
        scene.viewProj = nox::PerspectiveReverseZ(0.8f, frame.Aspect(), 0.1f) * nox::LookAt(eye, { 0, 0, 0 }, { 0, 1, 0 });
        scene.cameraPosition = { eye.x, eye.y, eye.z, 1 };
        const nox::Float3 light = nox::Normalize({ std::cos(m_AnimTime * 0.3f), 1.2f, std::sin(m_AnimTime * 0.3f) });
        scene.lightDirection = { light.x, light.y, light.z, 0 };
        scene.lightColor = { 3.2f, 2.9f, 2.5f, 0 };
        scene.ambient = { 0.10f, 0.13f, 0.20f, 0 };
        scene.time = m_AnimTime;
        scene.textureCount = m_TextureCount;
        const ScenePush scenePush{ m_Scene.Write(frame.frameSlot, scene), m_InstancesRef, m_VerticesRef, m_TextureTableRef };

        const nri::RGTexture hdr = graph.CreateTexture("HDR", { .Format = NRI_FORMAT_R16G16B16A16_SFLOAT, .Usage = NRI_TEXTURE_USAGE_COLOR_ATTACHMENT });
        const nri::RGTexture depth = graph.CreateTexture("Depth", { .Format = NRI_FORMAT_NONE, .Usage = NRI_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT });
        const nri::RGTexture tonemapped = graph.CreateTexture("Tonemapped", { .Format = NRI_FORMAT_R16G16B16A16_SFLOAT, .Usage = NRI_TEXTURE_USAGE_STORAGE });

        graph.AddPass("Scene", nri::RGPassFlags::Raster,
            [&](nri::RGBuilder& b) {
                b.ColorTarget(hdr, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE, { 0.02f, 0.025f, 0.04f, 1.0f });
                b.DepthTarget(depth, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_DONT_CARE, { 0.0f, 0 }); // reverse-Z: far is 0
                b.SetRenderArea(frame.extent);
            },
            [this, scenePush](nri::RGPassContext& ctx) {
                nri::CommandBuffer& cmd = ctx.Cmd();
                nox::Host::SetRasterState(cmd, { .depthTest = true, .depthWrite = true, .cull = NRI_CULL_MODE_BACK });
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, m_ScenePipeline);
                cmd.pushData(&scenePush, sizeof(scenePush));
                cmd.draw(36, kInstanceCount);
            });

        graph.AddPass("Tonemap", nri::RGPassFlags::None,
            [&](nri::RGBuilder& b) {
                b.Read(hdr);
                b.Write(tonemapped);
            },
            [this, &host, hdr, tonemapped, extent = frame.extent](nri::RGPassContext& ctx) {
                nri::CommandBuffer& cmd = ctx.Cmd();
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_COMPUTE, m_Tonemap);
                const PostPush push{ host.ImageSlot(ctx.Slot(hdr)), ctx.StorageSlot(tonemapped), extent.width, extent.height, 1.0f };
                cmd.pushData(&push, sizeof(push));
                cmd.dispatch((extent.width + 7) / 8, (extent.height + 7) / 8);
            });

        host.Probe(graph, hdr, "HDR", 8, frame);
        host.Probe(graph, tonemapped, "Tonemapped", 8, frame);

        graph.AddPass("Present", nri::RGPassFlags::Raster,
            [&](nri::RGBuilder& b) {
                b.Read(tonemapped);
                b.SwapchainTarget(host.swapchain, frame.imageIndex, NRI_LOAD_OP_DONT_CARE, NRI_STORE_OP_STORE);
                b.SetRenderArea(frame.extent);
            },
            [this, &host, tonemapped](nri::RGPassContext& ctx) {
                nri::CommandBuffer& cmd = ctx.Cmd();
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, m_Present);
                const PresentPush push{ host.ImageSlot(ctx.Slot(tonemapped)) };
                cmd.pushData(&push, sizeof(push));
                cmd.draw(3);
            });
    }

    void OnKey(SDL_Keycode key) override
    {
        if (key == SDLK_SPACE)
            m_Paused = !m_Paused;
    }

    void Shutdown(nox::Host&) override
    {
        m_ScenePipeline.reset();
        m_Tonemap.reset();
        m_Present.reset();
        m_Scene.Reset();
        m_Textures.clear();
        m_TextureTable.reset();
        m_Vertices.reset();
        m_Instances.reset();
    }

private:
    std::vector<nri::Texture> m_Textures;
    nri::Buffer m_TextureTable, m_Vertices, m_Instances;
    uint64_t m_TextureTableRef = 0, m_VerticesRef = 0, m_InstancesRef = 0;
    uint32_t m_TextureCount = 0;
    nox::FrameConstants<Scene> m_Scene;
    nri::Pipeline m_ScenePipeline, m_Tonemap, m_Present;
    float m_AnimTime = 0.0f;
    bool m_Paused = false;
};

NOX_EXAMPLE_MAIN(BindlessScene)
