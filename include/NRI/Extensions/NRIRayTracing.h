#pragma once

#include "../NRI.h"

NRI_EXTERN_C_BEGIN

typedef enum NriAccelerationStructureType
{
    NRI_ACCELERATION_STRUCTURE_BOTTOM_LEVEL = 0,
    NRI_ACCELERATION_STRUCTURE_TOP_LEVEL = 1
} NriAccelerationStructureType;

enum NriAccelerationStructureBuildFlagsBits
{
    NRI_AS_BUILD_NONE = 0u,
    NRI_AS_BUILD_ALLOW_UPDATE = 1u << 0,
    NRI_AS_BUILD_PREFER_FAST_TRACE = 1u << 1,
    NRI_AS_BUILD_PREFER_FAST_BUILD = 1u << 2,
    NRI_AS_BUILD_LOW_MEMORY = 1u << 3,
    NRI_AS_BUILD_ALLOW_COMPACTION = 1u << 4
};

typedef struct NriAccelerationStructureTrianglesDesc
{
    uint64_t vertexBufferAddress;
    uint32_t vertexStride;
    uint32_t maxVertex;
    uint64_t indexBufferAddress;
    uint32_t primitiveCount;
    uint32_t primitiveOffset;
    uint32_t firstVertex;
    bool isOpaque;
} NriAccelerationStructureTrianglesDesc;

typedef struct NriAccelerationStructureInstancesDesc
{
    uint64_t instanceBufferAddress;
    uint32_t instanceCount;
} NriAccelerationStructureInstancesDesc;

typedef struct NriAccelerationStructureBuildDesc
{
    NriAccelerationStructureType type;
    uint32_t flags;
    const NriAccelerationStructureTrianglesDesc* triangles;
    uint32_t triangleCount;
    NriAccelerationStructureInstancesDesc instances;
} NriAccelerationStructureBuildDesc;

typedef struct NriAccelerationStructureBuildSizes
{
    uint64_t accelerationStructureSize;
    uint64_t buildScratchSize;
    uint64_t updateScratchSize;
} NriAccelerationStructureBuildSizes;

typedef struct NriAccelerationStructureDesc
{
    NriAccelerationStructureType type;
    NriBuffer storageBuffer;
    uint64_t bufferOffset;
    uint64_t size;
} NriAccelerationStructureDesc;

typedef struct NriTransformMatrix
{
    float matrix[3][4];
} NriTransformMatrix;

typedef struct NriAccelerationStructureInstance
{
    NriTransformMatrix transform;
    uint32_t instanceCustomIndex : 24;
    uint32_t mask : 8;
    uint32_t instanceShaderBindingTableRecordOffset : 24;
    uint32_t flags : 8;
    uint64_t accelerationStructureReference;
} NriAccelerationStructureInstance;

typedef struct NriStridedDeviceAddressRegion
{
    uint64_t deviceAddress;
    uint64_t stride;
    uint64_t size;
} NriStridedDeviceAddressRegion;

typedef struct NriShaderTableState
{
    NriStridedDeviceAddressRegion rayGen;
    NriStridedDeviceAddressRegion miss;
    NriStridedDeviceAddressRegion hitGroups;
    NriStridedDeviceAddressRegion callable;
} NriShaderTableState;

typedef enum NriAccelerationStructureBarrierType
{
    NRI_AS_BARRIER_BUILD_TO_BUILD = 0,
    NRI_AS_BARRIER_BUILD_TO_SHADER_READ = 1,
    NRI_AS_BARRIER_TRANSFER_TO_BUILD = 2
} NriAccelerationStructureBarrierType;

NRI_API NriAccelerationStructureBuildSizes NRI_CALL nriGetAccelerationStructureBuildSizes(NriDevice device, const NriAccelerationStructureBuildDesc* desc);
NRI_API NriResult NRI_CALL nriCreateAccelerationStructure(NriDevice device, const NriAccelerationStructureDesc* desc, NriAccelerationStructure* outAS);
NRI_API void NRI_CALL nriDestroyAccelerationStructure(NriAccelerationStructure as);

NRI_API uint64_t NRI_CALL nriGetAccelerationStructureDeviceAddress(NriAccelerationStructure as);
NRI_API uint64_t NRI_CALL nriGetAccelerationStructureSize(NriAccelerationStructure as);
NRI_API NriAccelerationStructureType NRI_CALL nriGetAccelerationStructureType(NriAccelerationStructure as);
NRI_API uint32_t NRI_CALL nriRegisterAccelerationStructure(NriDescriptorHeap heap, NriAccelerationStructure as, uint32_t slot);

NRI_API void NRI_CALL nriCmdBuildAccelerationStructure(NriCommandBuffer cmd, const NriAccelerationStructureBuildDesc* buildDesc, uint64_t scratchAddress, NriAccelerationStructure dstAS);
NRI_API void NRI_CALL nriCmdUpdateAccelerationStructure(NriCommandBuffer cmd, const NriAccelerationStructureBuildDesc* buildDesc, uint64_t scratchAddress, NriAccelerationStructure srcAS, NriAccelerationStructure dstAS);
NRI_API void NRI_CALL nriCmdAccelerationStructureBarrier(NriCommandBuffer cmd, NriAccelerationStructureBarrierType barrierType);
NRI_API void NRI_CALL nriCmdWriteCompactedSizes(NriCommandBuffer cmd, const NriAccelerationStructure* structures, uint32_t structureCount, NriQueryPool pool, uint32_t firstQuery);
NRI_API void NRI_CALL nriCmdCopyAccelerationStructure(NriCommandBuffer cmd, NriAccelerationStructure src, NriAccelerationStructure dst, bool compact);
NRI_API void NRI_CALL nriCmdTraceRays(NriCommandBuffer cmd,
                                      const NriStridedDeviceAddressRegion* rayGen,
                                      const NriStridedDeviceAddressRegion* miss,
                                      const NriStridedDeviceAddressRegion* hitGroup,
                                      const NriStridedDeviceAddressRegion* callable,
                                      uint32_t width, uint32_t height, uint32_t depth);

NRI_API uint32_t NRI_CALL nriGetDeviceShaderGroupHandleSize(NriDevice device);
NRI_API uint32_t NRI_CALL nriGetDeviceShaderGroupBaseAlignment(NriDevice device);
NRI_API const uint8_t* NRI_CALL nriGetPipelineShaderGroupHandles(NriPipeline pipeline, uint32_t* outByteSize);
NRI_API uint32_t NRI_CALL nriGetPipelineShaderGroupHandleSize(NriPipeline pipeline);
NRI_API uint32_t NRI_CALL nriGetPipelineShaderGroupBaseAlignment(NriPipeline pipeline);
NRI_API uint32_t NRI_CALL nriGetPipelineRayGenGroupIndex(NriPipeline pipeline);
NRI_API const uint32_t* NRI_CALL nriGetPipelineMissGroupIndices(NriPipeline pipeline, uint32_t* outCount);
NRI_API const uint32_t* NRI_CALL nriGetPipelineHitGroupIndices(NriPipeline pipeline, uint32_t* outCount);
NRI_API const uint32_t* NRI_CALL nriGetPipelineCallableGroupIndices(NriPipeline pipeline, uint32_t* outCount);

NRI_API uint64_t NRI_CALL nriCalculateShaderTableSize(uint32_t numRayGen, uint32_t numMiss, uint32_t numHit, uint32_t numCallable, uint32_t baseAlignment);
NRI_API void NRI_CALL nriBakeShaderTable(uint8_t* uploadCpuVA, uint64_t uploadGpuVA,
                                         const uint8_t* shaderGroupHandles, uint32_t handleSize, uint32_t baseAlignment,
                                         int32_t rayGenShaderGroupIndex,
                                         const uint32_t* missIndices, uint32_t missCount,
                                         const uint32_t* hitIndices, uint32_t hitCount,
                                         const uint32_t* callableIndices, uint32_t callableCount,
                                         NriShaderTableState* outState);

NRI_EXTERN_C_END
