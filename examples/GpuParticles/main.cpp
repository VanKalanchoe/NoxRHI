// GpuParticles -- one Slang source (gpu_particles.slang) on Vulkan and Direct3D 12.
//
//   GpuParticles.exe --vk      GpuParticles.exe --dx12
//
// Each frame a compute pass moves 131072 particles in a GPU buffer (RWBufferRef), then a raster pass draws them with a vertex shader
// that pulls them from the same buffer (BufferRef) -- the render graph puts the barrier between the two. Inspired by the Nox engine's
// GPU particles. Space pauses the simulation.

#include "../Common/ExampleApp.h"

#include <bit>
#include <random>

namespace
{
    struct Particle
    {
        float positionLife[4];
        float velocitySeed[4];
    };

    struct Camera
    {
        nox::Mat4 viewProj;
        nox::Float4 right;
        nox::Float4 up;
    };

    struct SimulatePush
    {
        uint64_t particles; // RWBufferRef<Particle>
        uint32_t count;
        float time;
        float deltaTime;
        float pad;
    };

    struct DrawPush
    {
        uint64_t particles; // BufferRef<Particle>
        uint64_t camera;    // BufferRef<Camera>
        float size;
    };

    constexpr uint32_t kParticleCount = 128 * 1024;
}

class GpuParticles final : public nox::Example
{
public:
    const char* Name() const override { return "GpuParticles"; }

    void Init(nox::Host& host) override
    {
        std::vector<Particle> particles(kParticleCount);
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f), life(0.0f, 1.0f);
        for (Particle& p : particles)
        {
            p.positionLife[0] = unit(rng) * 0.8f;
            p.positionLife[1] = unit(rng) * 0.5f;
            p.positionLife[2] = unit(rng) * 0.8f;
            p.positionLife[3] = life(rng);
            p.velocitySeed[3] = std::bit_cast<float>(static_cast<uint32_t>(rng()));
        }
        const uint64_t bytes = sizeof(Particle) * kParticleCount;
        m_Particles = host.CreateBuffer(bytes, NRI_BUFFER_USAGE_STORAGE_STATIC, particles.data());
        m_ParticleRefs = host.Refs(m_Particles, bytes, true);
        m_Camera.Create(host);

        m_Simulate = host.Compute("gpu_particles.slang", "csSimulate");
        m_Draw = host.Graphics("gpu_particles.slang", "vsParticle", "psParticle");
    }

    void Render(nox::Host& host, nri::RenderGraph& graph, const nox::FrameInfo& frame) override
    {
        if (!m_Paused)
            m_SimTime += frame.deltaTime;

        const float orbit = frame.time * 0.15f;
        const nox::Float3 eye{ std::sin(orbit) * 3.2f, 1.3f, std::cos(orbit) * 3.2f };
        const nox::Mat4 view = nox::LookAt(eye, { 0, 0, 0 }, { 0, 1, 0 });
        Camera camera;
        camera.viewProj = nox::PerspectiveReverseZ(0.9f, frame.Aspect(), 0.05f) * view;
        camera.right = { view.At(0, 0), view.At(0, 1), view.At(0, 2), 0 }; // (rows of the view matrix: the camera's axes)
        camera.up = { view.At(1, 0), view.At(1, 1), view.At(1, 2), 0 };
        const uint64_t cameraRef = m_Camera.Write(frame.frameSlot, camera);

        const nri::RGBuffer particles = graph.ImportBuffer("Particles", m_Particles);
        const float deltaTime = m_Paused ? 0.0f : frame.deltaTime;

        graph.AddPass("Simulate", nri::RGPassFlags::None,
            [&](nri::RGBuilder& b) { b.Write(particles); },
            [this, deltaTime](nri::RGPassContext& ctx) {
                nri::CommandBuffer& cmd = ctx.Cmd();
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_COMPUTE, m_Simulate);
                const SimulatePush push{ m_ParticleRefs.write, kParticleCount, m_SimTime, deltaTime, 0.0f };
                cmd.pushData(&push, sizeof(push));
                cmd.dispatch((kParticleCount + 255) / 256);
            });

        graph.AddPass("Draw", nri::RGPassFlags::Raster,
            [&](nri::RGBuilder& b) {
                b.Read(particles);
                b.SwapchainTarget(host.swapchain, frame.imageIndex, NRI_LOAD_OP_CLEAR, NRI_STORE_OP_STORE, { 0.004f, 0.005f, 0.012f, 1.0f });
                b.SetRenderArea(frame.extent);
            },
            [this, cameraRef](nri::RGPassContext& ctx) {
                nri::CommandBuffer& cmd = ctx.Cmd();
                nox::Host::SetRasterState(cmd, { .blend = nox::RasterState::Blend::Additive });
                cmd.bindPipeline(NRI_PIPELINE_BIND_POINT_GRAPHICS, m_Draw);
                const DrawPush push{ m_ParticleRefs.read, cameraRef, 0.012f };
                cmd.pushData(&push, sizeof(push));
                cmd.draw(kParticleCount * 6);
            });
    }

    void OnKey(SDL_Keycode key) override
    {
        if (key == SDLK_SPACE)
            m_Paused = !m_Paused;
    }

    void Shutdown(nox::Host&) override
    {
        m_Draw.reset();
        m_Simulate.reset();
        m_Camera.Reset();
        m_Particles.reset();
    }

private:
    nri::Buffer m_Particles;
    nox::BufferRefs m_ParticleRefs;
    nox::FrameConstants<Camera> m_Camera;
    nri::Pipeline m_Simulate, m_Draw;
    float m_SimTime = 0.0f;
    bool m_Paused = false;
};

NOX_EXAMPLE_MAIN(GpuParticles)
