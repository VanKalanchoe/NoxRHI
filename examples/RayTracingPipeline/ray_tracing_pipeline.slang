// RayTracingPipeline: the scene of RayQueryShadows rendered by a ray tracing pipeline -- ray generation, closest hit, any hit and miss
// shaders, a shader binding table, TraceRay / DispatchRays -- instead of the rasterizer with inline ray queries. One source for Vulkan
// and D3D12, like every example (nox_gpu.slang).
//
// What it exercises (all of NoxRHI's ray tracing pipeline path):
//   - ray generation: one camera ray per pixel, the colour written to a storage texture;
//   - two miss shaders: 0 the sky (camera rays), 1 "nothing in the way" (shadow rays, selected by TraceRay's miss index);
//   - two hit groups: 0 opaque (closest hit), 1 cut-out (closest hit + an any hit that ignores hits in a lattice of holes), picked per
//     instance by its shader binding table record offset;
//   - recursion: the closest hit traces the shadow ray towards the sun (maxRecursionDepth 2).
//
// PUSH DATA in ray tracing stages: a global [[vk::push_constant]] ConstantBuffer (D3D12: b0, the root constants), NOT a `uniform`
// entry-point parameter as the compute / raster shaders take theirs -- Slang puts a ray tracing entry point's uniform parameters in the
// shader record (the bytes after the shader's handle in the shader binding table), so they would read whatever the table holds there.

#include "nox_gpu.slang"

struct Object
{
    float4x4 model;
    float4x4 normalMatrix; // inverse transpose of the model matrix (non-uniform scales)
    float4 color;          // rgb albedo, a: 1 = checker pattern, 2 = cut-out (holes, hit group 1)
};

struct Frame
{
    float4 cameraPosition; // xyz
    float4 cameraForward;  // xyz: the view direction
    float4 cameraRight;    // xyz: right, scaled by tan(fov / 2) x aspect
    float4 cameraUp;       // xyz: up, scaled by tan(fov / 2)
    float4 sunDirection;   // xyz: towards the sun
    uint objectCount;
    uint frameIndex;
    uint shadowsOn;
    uint padding;
};

struct Vertex
{
    float4 position;
    float4 normal;
};

struct Push
{
    uint64_t frameRef;
    uint64_t objectsRef;
    uint64_t verticesRef;
    uint64_t tlas;
    uint outputSlot; // the storage texture the ray generation writes (RWTex2D slot)
    uint width;
    uint height;
    uint padding;

    property BufferRef<Frame> frame { get { return BufferRef<Frame>(frameRef); } }
    property BufferRef<Object> objects { get { return BufferRef<Object>(objectsRef); } }
    property BufferRef<Vertex> vertices { get { return BufferRef<Vertex>(verticesRef); } }
};

[[vk::push_constant]]
ConstantBuffer<Push> pc : register(b0, space0);

// One payload type for every ray (a shader invoked with another payload type than it declares is undefined): camera rays return a
// colour, shadow rays a visibility the shadow miss sets.
struct RayPayload
{
    float3 color;
    uint visible;
};

static const uint kMissSky = 0;
static const uint kMissShadow = 1;

// A shadow ray: 1 when nothing lies between `origin` and `origin + direction * maxDistance`. The closest hit is skipped and the first
// hit ends the search; the cut-out any hit still runs (a ray through a hole passes).
float TraceShadow(RaytracingAccelerationStructure tlas, float3 origin, float3 direction, float maxDistance)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0;
    ray.TMax = maxDistance;
    RayPayload payload;
    payload.color = float3(0.0, 0.0, 0.0);
    payload.visible = 0;
    TraceRay(tlas, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, 0xFF, 0, 0, kMissShadow, ray, payload);
    return float(payload.visible);
}

[shader("raygeneration")]
void rayGenMain()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    if (pixel.x >= pc.width || pixel.y >= pc.height)
        return;
    const Frame frame = pc.frame[0];

    // The pixel's ray from the camera's basis (+Y up on screen).
    const float2 ndc = (float2(pixel) + 0.5) / float2(pc.width, pc.height) * float2(2.0, -2.0) + float2(-1.0, 1.0);
    RayDesc ray;
    ray.Origin = frame.cameraPosition.xyz;
    ray.Direction = normalize(frame.cameraForward.xyz + frame.cameraRight.xyz * ndc.x + frame.cameraUp.xyz * ndc.y);
    ray.TMin = 0.01;
    ray.TMax = 1000.0;

    RayPayload payload;
    payload.color = float3(0.0, 0.0, 0.0);
    payload.visible = 0;
    // Hit group = the instance's record offset (0 opaque, 1 cut-out) + 0 x geometry; miss shader 0 (the sky).
    TraceRay(LoadTlas(pc.tlas), RAY_FLAG_NONE, 0xFF, 0, 0, kMissSky, ray, payload);

    const float3 color = payload.color / (1.0 + payload.color); // (simple tonemap; the sRGB swapchain encodes it)
    RWTex2D(pc.outputSlot)[pixel] = float4(color, 1.0);
}

[shader("miss")]
void missSkyMain(inout RayPayload payload)
{
    const float up = saturate(WorldRayDirection().y * 0.5 + 0.5);
    payload.color = lerp(float3(0.55, 0.62, 0.72), float3(0.25, 0.40, 0.70), up);
}

[shader("miss")]
void missShadowMain(inout RayPayload payload)
{
    payload.visible = 1;
}

[shader("closesthit")]
void closestHitMain(inout RayPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    const Frame frame = pc.frame[0];
    const Object object = pc.objects[InstanceID()]; // (InstanceID: the instance's custom index -- its object)
    // The cube is not indexed: triangle t is vertices 3t .. 3t + 2, one normal per face.
    const Vertex v = pc.vertices[PrimitiveIndex() * 3];
    float3 n = normalize(mul(object.normalMatrix, float4(v.normal.xyz, 0.0)).xyz);
    if (dot(n, WorldRayDirection()) > 0.0)
        n = -n; // (seen from inside: through a cut-out's hole)
    const float3 position = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();

    float3 albedo = object.color.rgb;
    if (object.color.a > 0.5 && object.color.a < 1.5)
    {
        const int2 cell = int2(floor(position.xz));
        albedo *= ((cell.x + cell.y) & 1) != 0 ? 1.0 : 0.75;
    }

    const float3 sun = frame.sunDirection.xyz;
    const float lambert = saturate(dot(n, sun));
    float shadow = 1.0;
    if (frame.shadowsOn != 0 && lambert > 0.0)
        shadow = TraceShadow(LoadTlas(pc.tlas), position + n * 0.01, sun, 100.0); // (off the surface: no self-hit)

    const float3 sunColor = float3(2.6, 2.3, 1.9), skyColor = float3(0.35, 0.45, 0.65);
    payload.color = albedo * (sunColor * lambert * shadow + skyColor * (0.4 + 0.6 * saturate(n.y * 0.5 + 0.5)));
}

// Hit group 1 (cut-out boxes): the box is a lattice -- hits in every other cell of a 4 x 4 x 4 grid over its local [-1, 1] cube are
// ignored, for camera and shadow rays alike, so light and the view pass through the holes.
[shader("anyhit")]
void anyHitCutoutMain(inout RayPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    const float3 local = ObjectRayOrigin() + ObjectRayDirection() * RayTCurrent();
    const int3 cell = int3(floor((clamp(local, -0.999, 0.999) + 1.0) * 2.0));
    if (((cell.x + cell.y + cell.z) & 1) == 0)
        IgnoreHit();
}
