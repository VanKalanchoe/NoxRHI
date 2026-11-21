// MeshletCulling's --task-engine task shader, in a file of its own: a Slang module with several task shaders whose payloads are of
// different types gives only the first type's payload the task payload storage class (TaskPayloadWorkgroupEXT); the others become
// plain groupshared memory (Workgroup) -- invalid SPIR-V that spirv-val rejects (Slang 2026.22.1; each entry point compiled on its
// own is valid). NoxRHI compiles a whole file into one SPIR-V module on Vulkan, so this payload type (PayloadEngine) must not share a
// file with tsCull / tsCullSingle (Payload).
#pragma once

#include "meshlet_common.slang"

// --task-engine: the Nox engine's task shader (Material_PBR_MeshTask.slang taskMain) without its scene data -- 64 threads, one meshlet
// each; visible meshlets set a bit in a groupshared mask; each visible thread writes its offset at its prefix count (the visible
// threads before it), so the payload is compacted in meshlet order without an atomic counter. The engine calls DispatchMesh from
// thread 0 only; here every thread calls it with the same count, as tsCull does.
groupshared PayloadEngine gPayloadEngine;
groupshared uint gVisibleMask[kEngineTaskGroupSize / 32];
groupshared uint gSlotOffsets[kEngineTaskGroupSize]; // (compacted offsets, before they are packed into the payload's bytes)

[shader("amplification")]
[numthreads(kEngineTaskGroupSize, 1, 1)]
void tsCullEngine(uint3 groupId : SV_GroupID, uint threadIndex : SV_GroupIndex, uniform Push pc)
{
    if (threadIndex < kEngineTaskGroupSize / 32)
        gVisibleMask[threadIndex] = 0;
    if (threadIndex == 0)
    {
        gPayloadEngine.drawId = 0;
        gPayloadEngine.baseItem = groupId.x * kEngineTaskGroupSize;
    }
    GroupMemoryBarrierWithGroupSync();

    const View view = pc.view[0];
    const uint item = groupId.x * kEngineTaskGroupSize + threadIndex;
    bool visible = false;
    if (item < view.meshletCount * view.instanceCount)
    {
        const Meshlet meshlet = pc.meshlets[item % view.meshletCount];
        const Instance instance = pc.instances[item / view.meshletCount];
        visible = (view.flags & 1) != 0 || IsVisible(view, meshlet, instance);
        if (visible)
            InterlockedOr(gVisibleMask[threadIndex >> 5], 1u << (threadIndex & 31));
    }
    GroupMemoryBarrierWithGroupSync();

    uint count = 0;
    for (uint word = 0; word < kEngineTaskGroupSize / 32; ++word)
    {
        const uint bits = gVisibleMask[word];
        if (visible && word == (threadIndex >> 5))
            gSlotOffsets[count + countbits(bits & ((1u << (threadIndex & 31)) - 1u))] = threadIndex;
        count += countbits(bits);
    }
    GroupMemoryBarrierWithGroupSync();

    // The engine stores uint8_t offsets; four per uint here (DXIL has no 8-bit types): thread i packs slots 4i..4i+3.
    if (threadIndex < kEngineTaskGroupSize / 4)
    {
        uint packed = 0;
        for (uint k = 0; k < 4; ++k)
            if (threadIndex * 4 + k < count)
                packed |= (gSlotOffsets[threadIndex * 4 + k] & 0xFF) << (k * 8);
        gPayloadEngine.packedOffsets[threadIndex] = packed;
    }
    GroupMemoryBarrierWithGroupSync();

    if (threadIndex == 0 && (view.flags & 2) == 0 && count > 0)
        AtomicAdd(pc.stats, 0, count);
    DispatchMesh(count, 1, 1, gPayloadEngine);
}
