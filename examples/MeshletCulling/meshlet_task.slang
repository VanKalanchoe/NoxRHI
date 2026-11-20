// MeshletCulling's task shader (amplification): culls meshlets and launches mesh shader groups for the visible ones.
#pragma once

#include "meshlet_common.slang"

groupshared Payload gPayload;
groupshared uint gVisibleCount;

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

