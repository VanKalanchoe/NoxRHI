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
