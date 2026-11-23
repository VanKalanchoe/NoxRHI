#pragma once

#include "../NRI.h"

NRI_EXTERN_C_BEGIN

NRI_API void NRI_CALL nriCmdDrawMeshTasks(NriCommandBuffer cmd, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ);
NRI_API void NRI_CALL nriCmdDrawMeshTasksIndirect(NriCommandBuffer cmd, NriBuffer indirectBuffer, uint64_t offset, uint32_t drawCount, uint32_t stride);
NRI_API void NRI_CALL nriCmdDrawMeshTasksIndirectAddress(NriCommandBuffer cmd, uint64_t indirectBufferDeviceAddress, uint64_t offset, uint32_t drawCount, uint32_t stride);
NRI_API void NRI_CALL nriCmdDrawMeshTasksIndirectCount(NriCommandBuffer cmd, NriBuffer indirectBuffer, uint64_t offset, NriBuffer countBuffer, uint64_t countOffset, uint32_t maxDrawCount, uint32_t stride);
// Indirect mesh draw where each command record begins with a uint32_t drawID followed by { groupCountX, groupCountY, groupCountZ }
// (16-byte record minimum). On D3D12, ExecuteIndirect writes drawID into root constant b1 (space0) before DispatchMesh; on Vulkan,
// vkCmdDrawMeshTasksIndirectCountEXT reads the dispatch arguments at offset + 4 while shaders use SV_DrawIndex.
NRI_API void NRI_CALL nriCmdDrawMeshTasksIndirectCountWithDrawId(NriCommandBuffer cmd, NriBuffer indirectBuffer, uint64_t offset, NriBuffer countBuffer, uint64_t countOffset, uint32_t maxDrawCount, uint32_t stride);

NRI_EXTERN_C_END
