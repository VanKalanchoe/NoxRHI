// MeshletCulling's mesh shaders (with and without a task shader) and fragment shader.
#pragma once

#include "meshlet_common.slang"

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
[numthreads(kMeshThreads, 1, 1)]
void msMeshlet(uint3 groupId : SV_GroupID, uint threadIndex : SV_GroupIndex, in payload Payload payload, uniform Push pc,
               OutputVertices<MeshVertex, kMaxVertices> outVertices, OutputIndices<uint3, kMaxTriangles> outTriangles)
{
    const View view = pc.view[0];
    // --checked: the payload entry and everything derived from it are tested before they are read; a group that fails draws nothing
    // and the first failure is reported (main.cpp prints it), so an out-of-range index shows up instead of faulting the GPU.
    const bool checked = (view.flags & 4) != 0;
    // (one SetMeshOutputCounts call, as DXIL requires: a failed group outputs 0 vertices and triangles; item 0 keeps its reads in range)
    const bool inRange = !checked || MeshletInRange(pc, view, groupId.x, kTaskGroupSize, groupId.x < kTaskGroupSize ? payload.items[groupId.x] : 0, threadIndex);
    const uint item = inRange ? payload.items[groupId.x] : 0;
    const Meshlet meshlet = pc.meshlets[item % view.meshletCount];
    const Instance instance = pc.instances[item / view.meshletCount];
    const uint vertexCount = inRange ? meshlet.vertexCount : 0, triangleCount = inRange ? meshlet.triangleCount : 0;
    SetMeshOutputCounts(vertexCount, triangleCount);

    const float3 color = view.colorMode == 0 ? HashColor(item % view.meshletCount) : instance.color.rgb;
    for (uint i = threadIndex; i < vertexCount; i += kMeshThreads)
    {
        const uint vertexIndex = pc.meshletVertices[meshlet.vertexOffset + i];
        MeshVertex o;
        if (checked && vertexIndex >= view.vertexTotal)
        {
            ReportCheck(pc, kCheckVertexIndex, vertexIndex, item); // (not read: the vertex collapses to the origin)
            o.position = float4(0, 0, 0, 1);
            o.normal = float3(0, 1, 0);
        }
        else
        {
            const Vertex v = pc.vertices[vertexIndex];
            const float3 world = instance.positionScale.xyz + v.position.xyz * instance.positionScale.w;
            o.position = view.viewProj.Transform(float4(world, 1.0));
            o.normal = v.normal.xyz;
        }
        o.color = color;
        outVertices[i] = o;
    }
    for (uint t = threadIndex; t < triangleCount; t += kMeshThreads)
    {
        const uint first = meshlet.triangleOffset + t * 3;
        uint3 triangle = uint3(LoadByte(pc.meshletTriangles, first), LoadByte(pc.meshletTriangles, first + 1), LoadByte(pc.meshletTriangles, first + 2));
        if (checked && max(triangle.x, max(triangle.y, triangle.z)) >= vertexCount)
        {
            ReportCheck(pc, kCheckTriangleIndex, max(triangle.x, max(triangle.y, triangle.z)), item);
            triangle = uint3(0, 0, 0); // (degenerate: not drawn)
        }
        outTriangles[t] = triangle;
    }
}

// The same without a task shader (--mesh-only): one mesh shader group per meshlet of every instance, no payload, no culling.
[shader("mesh")]
[outputtopology("triangle")]
[numthreads(kMeshThreads, 1, 1)]
void msMeshletDirect(uint3 groupId : SV_GroupID, uint threadIndex : SV_GroupIndex, uniform Push pc,
               OutputVertices<MeshVertex, kMaxVertices> outVertices, OutputIndices<uint3, kMaxTriangles> outTriangles)
{
    const View view = pc.view[0];
    const uint item = groupId.x; // (no task shader: one group per meshlet of every instance, nothing culled)
    const Meshlet meshlet = pc.meshlets[item % view.meshletCount];
    const Instance instance = pc.instances[item / view.meshletCount];
    SetMeshOutputCounts(meshlet.vertexCount, meshlet.triangleCount);

    const float3 color = view.colorMode == 0 ? HashColor(item % view.meshletCount) : instance.color.rgb;
    for (uint i = threadIndex; i < meshlet.vertexCount; i += kMeshThreads)
    {
        const Vertex v = pc.vertices[pc.meshletVertices[meshlet.vertexOffset + i]];
        const float3 world = instance.positionScale.xyz + v.position.xyz * instance.positionScale.w;
        MeshVertex o;
        o.position = view.viewProj.Transform(float4(world, 1.0));
        o.normal = v.normal.xyz;
        o.color = color;
        outVertices[i] = o;
    }
    for (uint t = threadIndex; t < meshlet.triangleCount; t += kMeshThreads)
    {
        const uint first = meshlet.triangleOffset + t * 3;
        outTriangles[t] = uint3(LoadByte(pc.meshletTriangles, first), LoadByte(pc.meshletTriangles, first + 1), LoadByte(pc.meshletTriangles, first + 2));
    }
}

// --task-engine's mesh shader: the item is the payload's base plus the group's byte offset (the Nox engine's VisibilityBuffer meshMain
// reads groupMeshletOffset + meshletIndices[groupId] the same way). Otherwise msMeshlet, --checked included.
[shader("mesh")]
[outputtopology("triangle")]
[numthreads(kMeshThreads, 1, 1)]
void msMeshletEngine(uint3 groupId : SV_GroupID, uint threadIndex : SV_GroupIndex, in payload PayloadEngine payload, uniform Push pc,
                     OutputVertices<MeshVertex, kMaxVertices> outVertices, OutputIndices<uint3, kMaxTriangles> outTriangles)
{
    const View view = pc.view[0];
    const bool checked = (view.flags & 4) != 0;
    const bool inRange = !checked || MeshletInRange(pc, view, groupId.x, kEngineTaskGroupSize,
                                                    groupId.x < kEngineTaskGroupSize ? payload.baseItem + PayloadOffset(payload, groupId.x) : 0, threadIndex);
    const uint item = inRange ? payload.baseItem + PayloadOffset(payload, groupId.x) : 0;
    const Meshlet meshlet = pc.meshlets[item % view.meshletCount];
    const Instance instance = pc.instances[item / view.meshletCount];
    const uint vertexCount = inRange ? meshlet.vertexCount : 0, triangleCount = inRange ? meshlet.triangleCount : 0;
    SetMeshOutputCounts(vertexCount, triangleCount);

    const float3 color = view.colorMode == 0 ? HashColor(item % view.meshletCount) : instance.color.rgb;
    for (uint i = threadIndex; i < vertexCount; i += kMeshThreads)
    {
        const uint vertexIndex = pc.meshletVertices[meshlet.vertexOffset + i];
        MeshVertex o;
        if (checked && vertexIndex >= view.vertexTotal)
        {
            ReportCheck(pc, kCheckVertexIndex, vertexIndex, item); // (not read: the vertex collapses to the origin)
            o.position = float4(0, 0, 0, 1);
            o.normal = float3(0, 1, 0);
        }
        else
        {
            const Vertex v = pc.vertices[vertexIndex];
            const float3 world = instance.positionScale.xyz + v.position.xyz * instance.positionScale.w;
            o.position = view.viewProj.Transform(float4(world, 1.0));
            o.normal = v.normal.xyz;
        }
        o.color = color;
        outVertices[i] = o;
    }
    for (uint t = threadIndex; t < triangleCount; t += kMeshThreads)
    {
        const uint first = meshlet.triangleOffset + t * 3;
        uint3 triangle = uint3(LoadByte(pc.meshletTriangles, first), LoadByte(pc.meshletTriangles, first + 1), LoadByte(pc.meshletTriangles, first + 2));
        if (checked && max(triangle.x, max(triangle.y, triangle.z)) >= vertexCount)
        {
            ReportCheck(pc, kCheckTriangleIndex, max(triangle.x, max(triangle.y, triangle.z)), item);
            triangle = uint3(0, 0, 0); // (degenerate: not drawn)
        }
        outTriangles[t] = triangle;
    }
}

[shader("fragment")]
float4 psMeshlet(MeshVertex input) : SV_Target
{
    const float3 n = normalize(input.normal);
    const float light = saturate(dot(n, normalize(float3(0.4, 1.0, 0.3)))) * 0.8 + 0.2;
    return float4(input.color * light, 1.0);
}
