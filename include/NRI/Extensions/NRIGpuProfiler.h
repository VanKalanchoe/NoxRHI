#pragma once

#include "../NRI.h"

NRI_EXTERN_C_BEGIN

#define NRI_GPU_PROFILER_INVALID_QUERY_ID (~0u)

typedef struct NriGpuTimestamp
{
    uint32_t queryId;
    uint64_t ticks;
} NriGpuTimestamp;

typedef struct NriGpuPipelineStatistics
{
    uint32_t queryId;
    uint64_t fragments;
    uint64_t taskInvocations;
    uint64_t meshInvocations;
} NriGpuPipelineStatistics;

NRI_API NriResult NRI_CALL nriCreateGpuProfiler(NriDevice device, uint32_t framesInFlight, NriGpuProfiler* outProfiler);
NRI_API void NRI_CALL nriDestroyGpuProfiler(NriGpuProfiler profiler);

NRI_API bool NRI_CALL nriGpuProfilerIsSupported(NriGpuProfiler profiler);
NRI_API bool NRI_CALL nriGpuProfilerIsComputeQueueSupported(NriGpuProfiler profiler);
NRI_API float NRI_CALL nriGpuProfilerGetTimestampPeriod(NriGpuProfiler profiler);
NRI_API uint32_t NRI_CALL nriGpuProfilerGetTimestampValidBits(NriGpuProfiler profiler);
NRI_API bool NRI_CALL nriGpuProfilerCalibrate(NriGpuProfiler profiler, uint64_t* outGpuTicks, int64_t* outHostNanoseconds);

NRI_API void NRI_CALL nriGpuProfilerBeginFrame(NriGpuProfiler profiler, uint32_t frameSlot);
NRI_API const NriGpuTimestamp* NRI_CALL nriGpuProfilerGetReadbackTimestamps(NriGpuProfiler profiler, uint32_t* outCount);
NRI_API void NRI_CALL nriGpuProfilerResetQueries(NriGpuProfiler profiler, NriCommandBuffer cmd);

NRI_API uint32_t NRI_CALL nriGpuProfilerAllocateQueryPair(NriGpuProfiler profiler);
NRI_API void NRI_CALL nriGpuProfilerWriteTimestamp(NriGpuProfiler profiler, NriCommandBuffer cmd, uint32_t queryId);

NRI_API bool NRI_CALL nriGpuProfilerIsPipelineStatisticsSupported(NriGpuProfiler profiler);
NRI_API uint32_t NRI_CALL nriGpuProfilerAllocateStatisticsQuery(NriGpuProfiler profiler);
NRI_API void NRI_CALL nriGpuProfilerBeginStatistics(NriGpuProfiler profiler, NriCommandBuffer cmd, uint32_t queryId);
NRI_API void NRI_CALL nriGpuProfilerEndStatistics(NriGpuProfiler profiler, NriCommandBuffer cmd, uint32_t queryId);
NRI_API const NriGpuPipelineStatistics* NRI_CALL nriGpuProfilerGetReadbackStatistics(NriGpuProfiler profiler, uint32_t* outCount);

NRI_API bool NRI_CALL nriGpuProfilerIsTransferQueueSupported(NriGpuProfiler profiler);
NRI_API uint32_t NRI_CALL nriGpuProfilerBeginTransferQuery(NriGpuProfiler profiler, NriCommandBuffer cmd);
NRI_API void NRI_CALL nriGpuProfilerEndTransferQuery(NriGpuProfiler profiler, NriCommandBuffer cmd, uint32_t queryId);
NRI_API bool NRI_CALL nriGpuProfilerReadTransferQuery(NriGpuProfiler profiler, uint32_t queryId, uint64_t* outBeginTicks, uint64_t* outEndTicks);

NRI_EXTERN_C_END
