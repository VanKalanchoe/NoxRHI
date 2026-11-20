// MeshletCulling: task + mesh shaders. A task shader culls every meshlet of every instance against the view frustum and the meshlet's
// normal cone, and launches mesh shader groups only for the visible ones; a mesh shader expands a meshlet into vertices and triangles.
// One source for Vulkan and D3D12 -- and the two things the Nox engine's meshlet path needs changed for D3D12:
//   - the meshlet triangle stream is bytes (three local vertex indices per triangle). DXIL has no uint8_t, so the bytes are read out of
//     32-bit words with nox_gpu.slang's LoadByte (identical on Vulkan) instead of through a uint8_t pointer;
//   - the task payload holds 32-bit meshlet indices, not uint8_t offsets.

#include "nox_gpu.slang"

static const uint kTaskGroupSize = 32;
static const uint kMaxVertices = 64;
static const uint kMaxTriangles = 124;

struct Meshlet
{
    uint vertexOffset;   // first entry in meshletVertices
    uint triangleOffset; // first byte in meshletTriangles
    uint vertexCount;
    uint triangleCount;
    float4 sphere;       // xyz centre, w radius (mesh space)
    float4 cone;         // xyz axis, w cutoff (> 1: never cull)
};

struct Vertex
{
    float4 position; // xyz
    float4 normal;   // xyz
};

struct Instance
{
    float4 positionScale; // xyz position, w uniform scale
    float4 color;         // rgb
};

struct View
{
    Mat4 viewProj;      // the camera that draws
    float4 cullPlanes[5]; // the camera that culls (frozen with F): left, right, bottom, top, near
    float4 cullPosition;  // xyz
    uint meshletCount;    // meshlets per instance
    uint instanceCount;
    uint colorMode;       // 0: one colour per meshlet, 1: per instance
    uint flags;           // bit 0: no culling (every meshlet visible), bit 1: no counter (no atomic in the task shader)
};

struct Push
{
    uint64_t viewRef;
    uint64_t meshletsRef;
    uint64_t meshletVerticesRef;
    uint64_t meshletTrianglesRef; // packed bytes
    uint64_t verticesRef;
    uint64_t instancesRef;
    uint64_t statsRef;          // [0]: meshlets drawn this frame

    property BufferRef<View> view { get { return BufferRef<View>(viewRef); } }
    property BufferRef<Meshlet> meshlets { get { return BufferRef<Meshlet>(meshletsRef); } }
    property BufferRef<uint> meshletVertices { get { return BufferRef<uint>(meshletVerticesRef); } }
    property BufferRef<uint> meshletTriangles { get { return BufferRef<uint>(meshletTrianglesRef); } }
    property BufferRef<Vertex> vertices { get { return BufferRef<Vertex>(verticesRef); } }
    property BufferRef<Instance> instances { get { return BufferRef<Instance>(instancesRef); } }
    property RWBufferRef<uint> stats { get { return RWBufferRef<uint>(statsRef); } }
};

struct Payload
{
    uint items[kTaskGroupSize]; // instance * meshletCount + meshlet
};

groupshared Payload gPayload;
groupshared uint gVisibleCount;

bool IsVisible(View view, Meshlet meshlet, Instance instance)
{
    const float scale = instance.positionScale.w;
    const float3 center = instance.positionScale.xyz + meshlet.sphere.xyz * scale;
    const float radius = meshlet.sphere.w * scale;
    [unroll]
    for (int i = 0; i < 5; ++i)
        if (dot(view.cullPlanes[i].xyz, center) + view.cullPlanes[i].w < -radius)
            return false;
    // Back-facing cone (meshoptimizer's test; instances are only translated and uniformly scaled, so the axis is unchanged).
    const float3 toCenter = center - view.cullPosition.xyz;
    return dot(toCenter, meshlet.cone.xyz) < meshlet.cone.w * length(toCenter) + radius;
}

[shader("amplification")]
[numthreads(kTaskGroupSize, 1, 1)]
void tsCull(uint3 groupId : SV_GroupID, uint threadIndex : SV_GroupIndex, uniform Push pc)
{
    if (threadIndex == 0)
        gVisibleCount = 0;
    GroupMemoryBarrierWithGroupSync();

    const View view = pc.view[0];
    const uint item = groupId.x * kTaskGroupSize + threadIndex;
    if (item < view.meshletCount * view.instanceCount)
    {
        const Meshlet meshlet = pc.meshlets[item % view.meshletCount];
        const Instance instance = pc.instances[item / view.meshletCount];
        if ((view.flags & 1) != 0 || IsVisible(view, meshlet, instance))
        {
            // (a groupshared counter, not wave intrinsics: the group may span several waves, e.g. on 16-wide hardware)
            uint slot;
            InterlockedAdd(gVisibleCount, 1, slot);
            gPayload.items[slot] = item;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (threadIndex == 0 && (view.flags & 2) == 0)
        AtomicAdd(pc.stats, 0, gVisibleCount);
    DispatchMesh(gVisibleCount, 1, 1, gPayload);
}

struct MeshVertex
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
    nointerpolation float3 color : COLOR0;
};

float3 HashColor(uint x)
{
    x = (x ^ 61u) ^ (x >> 16); x *= 9u; x ^= x >> 4; x *= 0x27d4eb2du; x ^= x >> 15;
    return float3(float(x & 255u), float((x >> 8) & 255u), float((x >> 16) & 255u)) / 255.0 * 0.7 + 0.3;
}

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(kMaxVertices, 1, 1)]
void msMeshlet(uint3 groupId : SV_GroupID, uint threadIndex : SV_GroupIndex, in payload Payload payload, uniform Push pc,
               OutputVertices<MeshVertex, kMaxVertices> outVertices, OutputIndices<uint3, kMaxTriangles> outTriangles)
{
    const View view = pc.view[0];
    const uint item = payload.items[groupId.x];
    const Meshlet meshlet = pc.meshlets[item % view.meshletCount];
    const Instance instance = pc.instances[item / view.meshletCount];
    SetMeshOutputCounts(meshlet.vertexCount, meshlet.triangleCount);

    const float3 color = view.colorMode == 0 ? HashColor(item % view.meshletCount) : instance.color.rgb;
    if (threadIndex < meshlet.vertexCount)
    {
        const Vertex v = pc.vertices[pc.meshletVertices[meshlet.vertexOffset + threadIndex]];
        const float3 world = instance.positionScale.xyz + v.position.xyz * instance.positionScale.w;
        MeshVertex o;
        o.position = view.viewProj.Transform(float4(world, 1.0));
        o.normal = v.normal.xyz;
        o.color = color;
        outVertices[threadIndex] = o;
    }
    for (uint t = threadIndex; t < meshlet.triangleCount; t += kMaxVertices)
    {
        const uint first = meshlet.triangleOffset + t * 3;
        outTriangles[t] = uint3(LoadByte(pc.meshletTriangles, first), LoadByte(pc.meshletTriangles, first + 1), LoadByte(pc.meshletTriangles, first + 2));
    }
}

// The same without a task shader (--mesh-only): one mesh shader group per meshlet of every instance, no payload, no culling.
[shader("mesh")]
[outputtopology("triangle")]
[numthreads(kMaxVertices, 1, 1)]
void msMeshletDirect(uint3 groupId : SV_GroupID, uint threadIndex : SV_GroupIndex, uniform Push pc,
               OutputVertices<MeshVertex, kMaxVertices> outVertices, OutputIndices<uint3, kMaxTriangles> outTriangles)
{
    const View view = pc.view[0];
    const uint item = groupId.x; // (no task shader: one group per meshlet of every instance, nothing culled)
    const Meshlet meshlet = pc.meshlets[item % view.meshletCount];
    const Instance instance = pc.instances[item / view.meshletCount];
    SetMeshOutputCounts(meshlet.vertexCount, meshlet.triangleCount);

    const float3 color = view.colorMode == 0 ? HashColor(item % view.meshletCount) : instance.color.rgb;
    if (threadIndex < meshlet.vertexCount)
    {
        const Vertex v = pc.vertices[pc.meshletVertices[meshlet.vertexOffset + threadIndex]];
        const float3 world = instance.positionScale.xyz + v.position.xyz * instance.positionScale.w;
        MeshVertex o;
        o.position = view.viewProj.Transform(float4(world, 1.0));
        o.normal = v.normal.xyz;
        o.color = color;
        outVertices[threadIndex] = o;
    }
    for (uint t = threadIndex; t < meshlet.triangleCount; t += kMaxVertices)
    {
        const uint first = meshlet.triangleOffset + t * 3;
        outTriangles[t] = uint3(LoadByte(pc.meshletTriangles, first), LoadByte(pc.meshletTriangles, first + 1), LoadByte(pc.meshletTriangles, first + 2));
    }
}

[shader("fragment")]
float4 psMeshlet(MeshVertex input) : SV_Target
{
    const float3 n = normalize(input.normal);
    const float light = saturate(dot(n, normalize(float3(0.4, 1.0, 0.3)))) * 0.8 + 0.2;
    return float4(input.color * light, 1.0);
}
