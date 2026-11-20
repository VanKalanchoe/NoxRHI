// RayQueryShadows: a rasterized scene whose fragment shader traces rays with RayQuery (inline ray tracing) against a top-level
// acceleration structure rebuilt every frame: one shadow ray towards the sun and a few short ambient-occlusion rays per pixel.
// One source for Vulkan and D3D12: the TLAS through nox_gpu.slang's LoadTlas (Vulkan: its address, D3D12: a descriptor), buffers
// through BufferRef.

#include "nox_gpu.slang"

struct Object
{
    Mat4 model;
    Mat4 normalMatrix; // inverse transpose of the model matrix (non-uniform scales)
    float4 color;      // rgb albedo, a: 1 = checker pattern
};

struct Frame
{
    Mat4 viewProj;
    float4 cameraPosition; // xyz
    float4 sunDirection;   // xyz: towards the sun
    uint objectCount;
    uint aoRays;
    uint frameIndex;
    uint shadowsOn;
};

struct Vertex
{
    float4 position;
    float4 normal;
};

struct Push
{
    BufferRef<Frame> frame;
    BufferRef<Object> objects;
    BufferRef<Vertex> vertices;
    uint64_t tlas;
};

struct VertexOut
{
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : TEXCOORD1;
    nointerpolation float4 color : COLOR0;
};

[shader("vertex")]
VertexOut vsScene(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID, uniform Push pc)
{
    const Frame frame = pc.frame[0];
    const Object object = pc.objects[instanceId];
    const Vertex v = pc.vertices[vertexId];
    VertexOut o;
    o.world = object.model.TransformPoint(v.position.xyz);
    o.position = frame.viewProj.Transform(float4(o.world, 1.0));
    o.normal = object.normalMatrix.TransformVector(v.normal.xyz);
    o.color = object.color;
    return o;
}

// True when anything lies between `origin` and `origin + direction * maxDistance`.
bool Occluded(RaytracingAccelerationStructure tlas, float3 origin, float3 direction, float maxDistance)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0;
    ray.TMax = maxDistance;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFF, ray);
    query.Proceed();
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

float Random(inout uint state)
{
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return float((word >> 22u) ^ word) / 4294967296.0;
}

// A cosine-weighted direction around `n`.
float3 CosineDirection(float3 n, float u1, float u2)
{
    const float r = sqrt(u1), phi = 6.2831853 * u2;
    const float3 t = normalize(abs(n.y) < 0.99 ? cross(n, float3(0, 1, 0)) : cross(n, float3(1, 0, 0)));
    const float3 b = cross(n, t);
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u1)));
}

[shader("fragment")]
float4 psScene(VertexOut input, uniform Push pc) : SV_Target
{
    const Frame frame = pc.frame[0];
    const RaytracingAccelerationStructure tlas = LoadTlas(pc.tlas);
    const float3 n = normalize(input.normal);
    const float3 origin = input.world + n * 0.01; // (off the surface: no self-hit)

    float3 albedo = input.color.rgb;
    if (input.color.a > 0.5)
    {
        const int2 cell = int2(floor(input.world.xz));
        albedo *= ((cell.x + cell.y) & 1) != 0 ? 1.0 : 0.75;
    }

    const float3 sun = frame.sunDirection.xyz;
    const float lambert = saturate(dot(n, sun));
    const float shadow = (frame.shadowsOn != 0 && lambert > 0.0 && Occluded(tlas, origin, sun, 100.0)) ? 0.0 : 1.0;

    uint seed = (uint(input.position.x) * 1973u + uint(input.position.y) * 9277u + frame.frameIndex * 26699u) | 1u;
    float ambient = 0.0;
    for (uint i = 0; i < frame.aoRays; ++i)
        ambient += Occluded(tlas, origin, CosineDirection(n, Random(seed), Random(seed)), 1.2) ? 0.0 : 1.0;
    ambient = frame.aoRays > 0 ? ambient / float(frame.aoRays) : 1.0;

    const float3 sunColor = float3(2.6, 2.3, 1.9), skyColor = float3(0.35, 0.45, 0.65);
    const float3 color = albedo * (sunColor * lambert * shadow + skyColor * (0.4 + 0.6 * saturate(n.y * 0.5 + 0.5)) * ambient);
    return float4(color / (1.0 + color), 1.0); // (simple tonemap; the sRGB swapchain encodes it)
}
