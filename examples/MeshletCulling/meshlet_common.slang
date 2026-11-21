// MeshletCulling's shared declarations: limits, buffers, push data and the task -> mesh payload (included by the task and mesh files).
#pragma once

#include "nox_gpu.slang"

static const uint kTaskGroupSize = 32;
// The Nox engine's meshlet configuration (shaderIO.h: MAX_VERTICES / MAX_PRIMITIVES 64, MESH_SHADER_DISPATCH_X 32), proven on Vulkan:
// 64 vertices and 64 triangles per meshlet, 32 mesh shader threads looping over both.
static const uint kMaxVertices = 64;
static const uint kMaxTriangles = 64;
static const uint kMeshThreads = 32;

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
    uint flags;           // bit 0: no culling (every meshlet visible), bit 1: no counter (no atomic in the task shader), bit 2: --checked
    uint vertexTotal;         // entries in vertices          (--checked: the mesh shader's bounds)
    uint meshletVertexTotal;  // entries in meshletVertices
    uint triangleByteTotal;   // bytes in meshletTriangles
    uint padding;
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
    uint64_t checksRef;         // --checked: [0] failures, [1] first failure's code, [2] / [3] its values

    property BufferRef<View> view { get { return BufferRef<View>(viewRef); } }
    property BufferRef<Meshlet> meshlets { get { return BufferRef<Meshlet>(meshletsRef); } }
    property BufferRef<uint> meshletVertices { get { return BufferRef<uint>(meshletVerticesRef); } }
    property BufferRef<uint> meshletTriangles { get { return BufferRef<uint>(meshletTrianglesRef); } }
    property BufferRef<Vertex> vertices { get { return BufferRef<Vertex>(verticesRef); } }
    property BufferRef<Instance> instances { get { return BufferRef<Instance>(instancesRef); } }
    property RWBufferRef<uint> stats { get { return RWBufferRef<uint>(statsRef); } }
    property RWBufferRef<uint> checks { get { return RWBufferRef<uint>(checksRef); } }
};

struct Payload
{
    uint items[kTaskGroupSize]; // instance * meshletCount + meshlet
};

// --task-engine: the Nox engine's task shader design (Material_PBR_MeshTask.slang) -- 64 meshlets per task group, the visible ones
// compacted in meshlet order by a bitmask and a prefix count -- and its payload, byte for byte: MeshletPayload is
// { uint drawID; uint groupMeshletOffset; uint8_t meshletIndices[64]; }, 72 bytes. DXIL has no uint8_t, so the 64 offsets are packed four
// per uint (the same bytes, little-endian).
static const uint kEngineTaskGroupSize = 64;

struct PayloadEngine
{
    uint drawId;                                    // (unused here; the engine's instance slot)
    uint baseItem;                                  // the group's first item (the engine's groupMeshletOffset)
    uint packedOffsets[kEngineTaskGroupSize / 4];   // visible items relative to baseItem, one byte each
};

uint PayloadOffset(PayloadEngine payload, uint index)
{
    return (payload.packedOffsets[index >> 2] >> ((index & 3) * 8)) & 0xFF;
}

// --checked: failure codes the mesh shaders report (main.cpp prints them).
static const uint kCheckGroupPastPayload = 1;    // a: mesh group index -- more mesh groups than the payload holds
static const uint kCheckItemOutOfRange = 2;      // a: item from the payload, b: mesh group index
static const uint kCheckTooManyVertices = 3;     // a: meshlet vertexCount, b: item
static const uint kCheckTooManyTriangles = 4;    // a: meshlet triangleCount, b: item
static const uint kCheckVertexRange = 5;         // a: vertexOffset + vertexCount, b: item
static const uint kCheckTriangleRange = 6;       // a: triangleOffset + 3 * triangleCount, b: item
static const uint kCheckVertexIndex = 7;         // a: vertex index from meshletVertices, b: item
static const uint kCheckTriangleIndex = 8;       // a: largest local index of the triangle, b: item

void ReportCheck(Push pc, uint code, uint a, uint b)
{
    AtomicAdd(pc.checks, 0, 1);
    if (AtomicCompareExchange(pc.checks, 1, 0, code) == 0)
    {
        pc.checks.Store(2, a);
        pc.checks.Store(3, b);
    }
}

// --checked: everything the mesh shader derives from its payload entry, tested before it is read. False (reported once, by thread 0)
// when anything is out of range; the caller then outputs nothing for the group.
bool MeshletInRange(Push pc, View view, uint groupIndex, uint payloadCapacity, uint item, uint threadIndex)
{
    uint code = 0, a = 0, b = 0;
    if (groupIndex >= payloadCapacity)
    {
        code = kCheckGroupPastPayload; a = groupIndex;
    }
    else if (item >= view.meshletCount * view.instanceCount)
    {
        code = kCheckItemOutOfRange; a = item; b = groupIndex;
    }
    else
    {
        const Meshlet meshlet = pc.meshlets[item % view.meshletCount];
        if (meshlet.vertexCount > kMaxVertices) { code = kCheckTooManyVertices; a = meshlet.vertexCount; b = item; }
        else if (meshlet.triangleCount > kMaxTriangles) { code = kCheckTooManyTriangles; a = meshlet.triangleCount; b = item; }
        else if (meshlet.vertexOffset + meshlet.vertexCount > view.meshletVertexTotal) { code = kCheckVertexRange; a = meshlet.vertexOffset + meshlet.vertexCount; b = item; }
        else if (meshlet.triangleOffset + meshlet.triangleCount * 3 > view.triangleByteTotal) { code = kCheckTriangleRange; a = meshlet.triangleOffset + meshlet.triangleCount * 3; b = item; }
    }
    if (code != 0 && threadIndex == 0)
        ReportCheck(pc, code, a, b);
    return code == 0;
}

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

