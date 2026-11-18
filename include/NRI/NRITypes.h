#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

NRI_EXTERN_C_BEGIN

#define NRI_DECLARE_HANDLE(name) typedef struct name##_T* name

NRI_DECLARE_HANDLE(NriDevice);
NRI_DECLARE_HANDLE(NriSwapchain);
NRI_DECLARE_HANDLE(NriPipeline);
NRI_DECLARE_HANDLE(NriCommandAllocator);
NRI_DECLARE_HANDLE(NriCommandBuffer);
NRI_DECLARE_HANDLE(NriTimelineSemaphore);
NRI_DECLARE_HANDLE(NriTexture);
NRI_DECLARE_HANDLE(NriBuffer);
NRI_DECLARE_HANDLE(NriDescriptorHeap);
NRI_DECLARE_HANDLE(NriAccelerationStructure);
NRI_DECLARE_HANDLE(NriQueryPool);
NRI_DECLARE_HANDLE(NriGpuProfiler);
NRI_DECLARE_HANDLE(NriShaderCompiler);

typedef enum NriResult
{
    NRI_SUCCESS = 0,
    NRI_ERROR_INVALID_ARGUMENT = -1,
    NRI_ERROR_OUT_OF_MEMORY = -2,
    NRI_ERROR_UNSUPPORTED = -3,
    NRI_ERROR_INTERNAL = -4
} NriResult;

typedef enum NriGraphicsAPI
{
    NRI_GRAPHICS_API_VULKAN = 0,
    NRI_GRAPHICS_API_METAL = 1,
    NRI_GRAPHICS_API_D3D12 = 2
} NriGraphicsAPI;

typedef enum NriLogLevel
{
    NRI_LOG_INFO = 0,
    NRI_LOG_WARN = 1,
    NRI_LOG_ERROR = 2
} NriLogLevel;

typedef void (NRI_CALL *NriLogCallback)(NriLogLevel level, const char* message, void* userData);

typedef enum NriQueueType
{
    NRI_QUEUE_GRAPHICS = 0,
    NRI_QUEUE_TRANSFER = 1,
    NRI_QUEUE_COMPUTE = 2
} NriQueueType;

typedef enum NriCompareOp
{
    NRI_COMPARE_NEVER = 0,
    NRI_COMPARE_LESS = 1,
    NRI_COMPARE_EQUAL = 2,
    NRI_COMPARE_LESS_OR_EQUAL = 3,
    NRI_COMPARE_GREATER = 4,
    NRI_COMPARE_NOT_EQUAL = 5,
    NRI_COMPARE_GREATER_OR_EQUAL = 6,
    NRI_COMPARE_ALWAYS = 7
} NriCompareOp;

typedef struct NriExtent2D
{
    uint32_t width;
    uint32_t height;
} NriExtent2D;

typedef struct NriViewportBounds
{
    float x;
    float y;
    float width;
    float height;
} NriViewportBounds;

typedef struct NriMemoryHeapStats
{
    bool deviceLocal;
    uint64_t usage;
    uint64_t budget;
    uint64_t blockBytes;
    uint64_t allocationBytes;
    uint32_t allocationCount;
} NriMemoryHeapStats;

typedef struct NriTimelinePoint
{
    NriTimelineSemaphore semaphore;
    uint64_t value;
} NriTimelinePoint;

typedef struct NriSparsePageRange
{
    uint64_t firstPage;
    uint64_t pageCount;
} NriSparsePageRange;

NRI_API void NRI_CALL nriSetLogCallback(NriLogCallback callback, void* userData);
NRI_API const char* NRI_CALL nriGetLastError(void);

NRI_EXTERN_C_END
