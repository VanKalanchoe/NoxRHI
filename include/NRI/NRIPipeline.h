#pragma once

#include "NRITypes.h"
#include "NRIResource.h"

NRI_EXTERN_C_BEGIN

typedef enum NriPipelineType
{
    NRI_PIPELINE_GRAPHICS = 0,
    NRI_PIPELINE_COMPUTE = 1,
    NRI_PIPELINE_RAY_TRACING = 2
} NriPipelineType;

typedef enum NriShaderStage
{
    NRI_SHADER_STAGE_VERTEX = 0,
    NRI_SHADER_STAGE_FRAGMENT = 1,
    NRI_SHADER_STAGE_COMPUTE = 2,
    NRI_SHADER_STAGE_TASK = 3,
    NRI_SHADER_STAGE_MESH = 4,
    NRI_SHADER_STAGE_RAY_GEN = 5,
    NRI_SHADER_STAGE_MISS = 6,
    NRI_SHADER_STAGE_CLOSEST_HIT = 7,
    NRI_SHADER_STAGE_ANY_HIT = 8,
    NRI_SHADER_STAGE_INTERSECTION = 9,
    NRI_SHADER_STAGE_CALLABLE = 10
} NriShaderStage;

typedef struct NriShaderStageDesc
{
    NriShaderStage stage;
    const char* entryPoint;
    const char* sourcePath;
} NriShaderStageDesc;

typedef struct NriRayTracingHitGroupDesc
{
    uint32_t closestHitShaderIndex;
    uint32_t anyHitShaderIndex;
    uint32_t intersectionShaderIndex;
} NriRayTracingHitGroupDesc;

typedef struct NriPipelineDesc
{
    NriPipelineType type;
    const NriShaderStageDesc* shaders;
    uint32_t shaderCount;
    const NriImageFormat* colorFormats;
    uint32_t colorFormatCount;
    const NriRayTracingHitGroupDesc* hitGroups;
    uint32_t hitGroupCount;
    uint32_t maxRecursionDepth;
    bool forceCompile;
    bool buildNow;
    // Ray tracing pipelines: the largest ray payload and hit attributes in bytes (D3D12 state objects need them; 0: 256 / 32;
    // Vulkan ignores them).
    uint32_t maxPayloadSize;
    uint32_t maxAttributeSize;
} NriPipelineDesc;

NRI_API NriResult NRI_CALL nriCreatePipeline(NriDevice device, const NriPipelineDesc* desc, NriShaderCompiler compiler, NriPipeline* outPipeline);
NRI_API void NRI_CALL nriDestroyPipeline(NriPipeline pipeline);

// Timeline Semaphore
NRI_API NriResult NRI_CALL nriCreateTimelineSemaphore(NriDevice device, uint64_t initialValue, NriTimelineSemaphore* outSemaphore);
NRI_API void NRI_CALL nriDestroyTimelineSemaphore(NriTimelineSemaphore semaphore);
NRI_API uint64_t NRI_CALL nriGetTimelineSemaphoreValue(NriTimelineSemaphore semaphore);
NRI_API void NRI_CALL nriWaitTimelineSemaphore(NriTimelineSemaphore semaphore, uint64_t value);

NRI_EXTERN_C_END
