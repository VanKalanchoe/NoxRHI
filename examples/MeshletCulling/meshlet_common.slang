// MeshletCulling's shared declarations: limits, buffers, push data and the task -> mesh payload (included by the task and mesh files).
#pragma once

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

