// GpuParticles: a compute shader moves the particles in a read-write buffer, then a vertex shader pulls them from the same buffer and
// draws each as a camera-facing billboard (additive). One source for Vulkan and D3D12: buffers through nox_gpu.slang's BufferRef /
// RWBufferRef (Vulkan: device addresses, D3D12: descriptors), no vertex buffers (vertex pulling).

#include "nox_gpu.slang"

struct Particle
{
    float4 positionLife; // xyz: position, w: life left (1 -> 0)
    float4 velocitySeed; // xyz: velocity, w: a random seed
};

struct Camera
{
    Mat4 viewProj;
    float4 right; // xyz: the camera's right vector in world space
    float4 up;    // xyz: the camera's up vector
};

// --- simulation ------------------------------------------------------------------------------------------------------------------------

struct SimulatePush
{
    RWBufferRef<Particle> particles;
    uint count;
    float time;
    float deltaTime;
    float pad;
};

uint Hash(uint x)
{
    x ^= x >> 16; x *= 0x7FEB352Du;
    x ^= x >> 15; x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

float Random(inout uint state)
{
    state = Hash(state);
    return float(state & 0xFFFFFF) / 16777216.0;
}

float3 Attractor(float time, float phase)
{
    return float3(sin(time * 0.61 + phase) * 1.1, cos(time * 0.83 + phase * 1.7) * 0.6, sin(time * 0.47 + phase * 0.6) * 0.9);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void csSimulate(uint3 id : SV_DispatchThreadID, uniform SimulatePush pc)
{
    if (id.x >= pc.count)
        return;
    Particle p = pc.particles.Load(id.x);
    float3 position = p.positionLife.xyz;
    float3 velocity = p.velocitySeed.xyz;
    float life = p.positionLife.w - pc.deltaTime * 0.12;

    // Two orbiting attractors and a swirl around the vertical axis.
    float3 acceleration = float3(0.0, 0.0, 0.0);
    [unroll]
    for (int i = 0; i < 2; ++i)
    {
        const float3 toAttractor = Attractor(pc.time, float(i) * 3.14159) - position;
        acceleration += toAttractor * (2.0 / (dot(toAttractor, toAttractor) + 0.08));
    }
    acceleration += float3(-position.z, 0.0, position.x) * 0.6 - position * 0.25;
    velocity = (velocity + acceleration * pc.deltaTime) * (1.0 - 0.35 * pc.deltaTime);
    position += velocity * pc.deltaTime;

    if (life <= 0.0)
    {
        // Respawn on a small sphere around one of the attractors.
        uint seed = asuint(p.velocitySeed.w) ^ Hash(uint(pc.time * 1000.0) + id.x);
        const float3 direction = normalize(float3(Random(seed), Random(seed), Random(seed)) * 2.0 - 1.0 + 1e-4);
        position = Attractor(pc.time, Random(seed) < 0.5 ? 0.0 : 3.14159) + direction * 0.05;
        velocity = direction * (0.3 + 0.6 * Random(seed));
        life = 0.6 + 0.4 * Random(seed);
        p.velocitySeed.w = asfloat(seed);
    }
    p.positionLife = float4(position, life);
    p.velocitySeed.xyz = velocity;
    pc.particles.Store(id.x, p);
}

// --- drawing -----------------------------------------------------------------------------------------------------------------------------

struct DrawPush
{
    BufferRef<Particle> particles;
    BufferRef<Camera> camera;
    float size;
};

struct VertexOut
{
    float4 position : SV_Position;
    float2 corner : TEXCOORD0; // -1 .. 1 across the billboard
    float4 color : COLOR0;
};

float3 SpeedColor(float speed)
{
    // slow: deep blue -> fast: orange / white
    const float t = saturate(speed * 0.45);
    return lerp(float3(0.05, 0.15, 0.9), float3(1.0, 0.45, 0.1), t) + float3(1.0, 1.0, 1.0) * pow(t, 6.0);
}

[shader("vertex")]
VertexOut vsParticle(uint vertexId : SV_VertexID, uniform DrawPush pc)
{
    // Six vertices (two triangles) per particle, pulled from the buffer: no vertex buffer, no vertex layout.
    const float2 corners[6] = { float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1) };
    const Particle p = pc.particles[vertexId / 6];
    const Camera camera = pc.camera[0];
    const float2 corner = corners[vertexId % 6];
    const float life = saturate(p.positionLife.w);
    const float size = pc.size * (0.4 + 0.6 * life);
    const float3 world = p.positionLife.xyz + (camera.right.xyz * corner.x + camera.up.xyz * corner.y) * size;

    VertexOut o;
    o.position = camera.viewProj.Transform(float4(world, 1.0));
    o.corner = corner;
    o.color = float4(SpeedColor(length(p.velocitySeed.xyz)), smoothstep(0.0, 0.15, life));
    return o;
}

[shader("fragment")]
float4 psParticle(VertexOut input) : SV_Target
{
    const float falloff = saturate(1.0 - dot(input.corner, input.corner));
    const float intensity = falloff * falloff * input.color.a * 0.35;
    return float4(input.color.rgb * intensity, intensity); // additive
}
