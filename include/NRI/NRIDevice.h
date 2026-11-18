#pragma once

#include "NRITypes.h"

NRI_EXTERN_C_BEGIN

NRI_API void NRI_CALL nriDestroyDevice(NriDevice device);

NRI_API NriGraphicsAPI NRI_CALL nriGetDeviceAPI(NriDevice device);
NRI_API uint32_t NRI_CALL nriGetMSAASampleCount(NriDevice device);
NRI_API bool NRI_CALL nriHasDedicatedTransferQueue(NriDevice device);
NRI_API bool NRI_CALL nriHasAsyncComputeQueue(NriDevice device);
NRI_API bool NRI_CALL nriIsSurfaceFormatBGRA(NriDevice device);
NRI_API bool NRI_CALL nriIsMemoryBudgetSupported(NriDevice device);
NRI_API bool NRI_CALL nriSupportsSparseBuffers(NriDevice device);
NRI_API uint64_t NRI_CALL nriGetSparseBufferReserveLimit(NriDevice device);
NRI_API NriTimelinePoint NRI_CALL nriGetSparseTimelinePoint(NriDevice device);
NRI_API uint64_t NRI_CALL nriGetTextureBytes(NriDevice device);

NRI_API void NRI_CALL nriBeginFrame(NriDevice device, uint32_t frameNumber);
NRI_API void NRI_CALL nriWaitIdle(NriDevice device);
NRI_API void NRI_CALL nriFinishPipelineBuilds(NriDevice device);

NRI_API void NRI_CALL nriPrecompileShaderModules(NriDevice device, const char* const* sources, uint32_t sourceCount, NriShaderCompiler compiler);
NRI_API bool NRI_CALL nriCompileShaderModules(NriDevice device, const char* const* sources, uint32_t sourceCount, NriShaderCompiler compiler);

NRI_API bool NRI_CALL nriUpdateSparseBuffer(NriDevice device,
                                            NriBuffer buffer,
                                            const NriSparsePageRange* commits, uint32_t commitCount,
                                            const NriSparsePageRange* releases, uint32_t releaseCount);

// If outHeaps is NULL, writes required count into *inOutCount.
NRI_API void NRI_CALL nriGetMemoryStats(NriDevice device, NriMemoryHeapStats* outHeaps, uint32_t* inOutCount);

NRI_API NriResult NRI_CALL nriSubmitAndWait(NriDevice device, NriCommandBuffer cmdBuffer, uint32_t slotIndex);

NRI_API NriResult NRI_CALL nriSubmitCommandBuffers(NriDevice device,
                                                   const NriCommandBuffer* cmdBuffers, uint32_t cmdBufferCount,
                                                   NriSwapchain swapchain,
                                                   uint32_t frameIndex, uint32_t imageIndex,
                                                   const NriTimelinePoint* timelineWaits, uint32_t timelineWaitCount);

NRI_API NriResult NRI_CALL nriSubmit(NriDevice device,
                                     NriQueueType queue,
                                     const NriCommandBuffer* cmdBuffers, uint32_t cmdBufferCount,
                                     const NriTimelinePoint* waits, uint32_t waitCount,
                                     const NriTimelinePoint* signals, uint32_t signalCount);

NRI_EXTERN_C_END
