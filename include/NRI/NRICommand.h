#pragma once

#include "NRITypes.h"
#include "NRIResource.h"

NRI_EXTERN_C_BEGIN

typedef enum NriCommandBufferReset
{
    NRI_COMMAND_BUFFER_RESET_PER_COMMAND_BUFFER = 0,
    NRI_COMMAND_BUFFER_RESET_WITH_ALLOCATOR = 1
} NriCommandBufferReset;

typedef enum NriLoadOp
{
    NRI_LOAD_OP_LOAD = 0,
    NRI_LOAD_OP_CLEAR = 1,
    NRI_LOAD_OP_DONT_CARE = 2,
    NRI_LOAD_OP_NONE = 3
} NriLoadOp;

typedef enum NriStoreOp
{
    NRI_STORE_OP_STORE = 0,
    NRI_STORE_OP_DONT_CARE = 1,
    NRI_STORE_OP_NONE = 2
} NriStoreOp;

typedef struct NriClearColor
{
    float r;
    float g;
    float b;
    float a;
} NriClearColor;

typedef struct NriClearDepth
{
    float depth;
    uint32_t stencil;
} NriClearDepth;

typedef struct NriRenderAttachDesc
{
    NriSwapchain attachmentSwapchain;
    NriTexture attachment;
    NriTexture resolve;
    NriSwapchain resolveSwapchain;
    uint32_t resolveImageIndex;
    NriLoadOp loadOp;
    NriStoreOp storeOp;
    NriClearColor clearColor;
    NriClearDepth clearDepth;
} NriRenderAttachDesc;

typedef struct NriRenderDesc
{
    NriExtent2D renderArea;
    const NriRenderAttachDesc* colorAttachments;
    uint32_t colorAttachmentCount;
    NriRenderAttachDesc depthAttachment;
    bool hasDepthAttachment;
    bool isSwapchainPass;
} NriRenderDesc;

typedef enum NriPipelineBindPoint
{
    NRI_PIPELINE_BIND_POINT_GRAPHICS = 0,
    NRI_PIPELINE_BIND_POINT_COMPUTE = 1,
    NRI_PIPELINE_BIND_POINT_RAY_TRACING = 2
} NriPipelineBindPoint;

typedef enum NriPrimitiveTopology
{
    NRI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST = 0
} NriPrimitiveTopology;

typedef enum NriPolygonMode
{
    NRI_POLYGON_MODE_FILL = 0,
    NRI_POLYGON_MODE_LINE = 1
} NriPolygonMode;

typedef enum NriCullMode
{
    NRI_CULL_MODE_NONE = 0,
    NRI_CULL_MODE_BACK = 1
} NriCullMode;

typedef enum NriFrontFace
{
    NRI_FRONT_FACE_CLOCKWISE = 0,
    NRI_FRONT_FACE_COUNTER_CLOCKWISE = 1
} NriFrontFace;

typedef enum NriBlendFactor
{
    NRI_BLEND_FACTOR_ONE = 0,
    NRI_BLEND_FACTOR_ZERO = 1,
    NRI_BLEND_FACTOR_SRC_ALPHA = 2,
    NRI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA = 3,
    NRI_BLEND_FACTOR_ONE_MINUS_SRC_COLOR = 4,
    NRI_BLEND_FACTOR_SRC_COLOR = 5
} NriBlendFactor;

typedef enum NriBlendOp
{
    NRI_BLEND_OP_ADD = 0
} NriBlendOp;

typedef struct NriColorBlendEquation
{
    NriBlendFactor srcColorBlendFactor;
    NriBlendFactor dstColorBlendFactor;
    NriBlendOp colorBlendOp;
    NriBlendFactor srcAlphaBlendFactor;
    NriBlendFactor dstAlphaBlendFactor;
    NriBlendOp alphaBlendOp;
} NriColorBlendEquation;

enum NriColorComponentBits
{
    NRI_COLOR_COMPONENT_R = 0x00000001u,
    NRI_COLOR_COMPONENT_G = 0x00000002u,
    NRI_COLOR_COMPONENT_B = 0x00000004u,
    NRI_COLOR_COMPONENT_A = 0x00000008u,
    NRI_COLOR_COMPONENT_ALL = 0x0000000Fu
};

typedef struct NriBufferCopyRegion
{
    uint64_t srcOffset;
    uint64_t dstOffset;
    uint64_t size;
} NriBufferCopyRegion;

enum NriAccessBits
{
    NRI_ACCESS_NONE = 0u,
    NRI_ACCESS_SHADER_READ = 1u << 0,
    NRI_ACCESS_SHADER_WRITE = 1u << 1,
    NRI_ACCESS_COLOR_ATTACHMENT_READ = 1u << 2,
    NRI_ACCESS_COLOR_ATTACHMENT_WRITE = 1u << 3,
    NRI_ACCESS_DEPTH_STENCIL_READ = 1u << 4,
    NRI_ACCESS_DEPTH_STENCIL_WRITE = 1u << 5,
    NRI_ACCESS_TRANSFER_READ = 1u << 6,
    NRI_ACCESS_TRANSFER_WRITE = 1u << 7,
    NRI_ACCESS_ACCELERATION_STRUCTURE_READ = 1u << 8,
    NRI_ACCESS_ACCELERATION_STRUCTURE_WRITE = 1u << 9,
    NRI_ACCESS_INDIRECT_READ = 1u << 10
};

enum NriStageBits
{
    NRI_STAGE_NONE = 0u,
    NRI_STAGE_TASK = 1u << 0,
    NRI_STAGE_MESH = 1u << 1,
    NRI_STAGE_FRAGMENT = 1u << 2,
    NRI_STAGE_COMPUTE = 1u << 3,
    NRI_STAGE_COLOR_ATTACHMENT_OUTPUT = 1u << 4,
    NRI_STAGE_FRAGMENT_TESTS = 1u << 5,
    NRI_STAGE_TRANSFER = 1u << 6,
    NRI_STAGE_ACCELERATION_STRUCTURE_BUILD = 1u << 7,
    NRI_STAGE_INDIRECT = 1u << 8,
    NRI_STAGE_RAY_TRACING = 1u << 9,
    NRI_STAGE_VERTEX = 1u << 10 // vertex shaders (a buffer a vertex shader pulls from)
};

typedef struct NriResourceState
{
    uint32_t access;
    uint32_t stages;
} NriResourceState;

typedef struct NriTextureBarrierDesc
{
    NriTexture texture;
    NriResourceState before;
    NriResourceState after;
} NriTextureBarrierDesc;

typedef struct NriBufferBarrierDesc
{
    NriBuffer buffer;
    NriResourceState before;
    NriResourceState after;
} NriBufferBarrierDesc;

// Command Allocator
NRI_API NriResult NRI_CALL nriCreateCommandAllocator(NriDevice device, NriCommandBufferReset resetMode, NriQueueType queue, NriCommandAllocator* outAllocator);
NRI_API void NRI_CALL nriDestroyCommandAllocator(NriCommandAllocator allocator);
NRI_API void NRI_CALL nriResetCommandAllocator(NriCommandAllocator allocator);

// Command Buffer
NRI_API NriResult NRI_CALL nriAllocateCommandBuffer(NriCommandAllocator allocator, uint32_t framesInFlight, NriCommandBuffer* outCommandBuffer);
NRI_API void NRI_CALL nriDestroyCommandBuffer(NriCommandBuffer commandBuffer);

NRI_API void NRI_CALL nriCmdBegin(NriCommandBuffer cmd, uint32_t index, bool oneTimeSubmit);
NRI_API void NRI_CALL nriCmdEnd(NriCommandBuffer cmd, uint32_t index);
NRI_API void NRI_CALL nriCmdBeginRendering(NriCommandBuffer cmd, const NriRenderDesc* desc);
NRI_API void NRI_CALL nriCmdEndRendering(NriCommandBuffer cmd);
NRI_API void NRI_CALL nriCmdBindPipeline(NriCommandBuffer cmd, NriPipelineBindPoint bindPoint, NriPipeline pipeline);

NRI_API void NRI_CALL nriCmdSetViewport(NriCommandBuffer cmd, NriExtent2D extent);
NRI_API void NRI_CALL nriCmdSetScissor(NriCommandBuffer cmd, NriExtent2D extent);
NRI_API void NRI_CALL nriCmdSetViewportWithCount(NriCommandBuffer cmd, const NriViewportBounds* bounds, float minDepth, float maxDepth);
NRI_API void NRI_CALL nriCmdSetScissorWithCount(NriCommandBuffer cmd, const NriExtent2D* extent);
NRI_API void NRI_CALL nriCmdSetScissorRect(NriCommandBuffer cmd, int32_t x, int32_t y, uint32_t width, uint32_t height);
NRI_API void NRI_CALL nriCmdSetVertexInput(NriCommandBuffer cmd);
NRI_API void NRI_CALL nriCmdSetPrimitiveTopology(NriCommandBuffer cmd, NriPrimitiveTopology topology);
NRI_API void NRI_CALL nriCmdSetPrimitiveRestartEnable(NriCommandBuffer cmd, bool enable);
NRI_API void NRI_CALL nriCmdSetRasterizerDiscardEnable(NriCommandBuffer cmd, bool enable);
NRI_API void NRI_CALL nriCmdSetPolygonMode(NriCommandBuffer cmd, NriPolygonMode polygon);
NRI_API void NRI_CALL nriCmdSetCullMode(NriCommandBuffer cmd, NriCullMode cullMode);
NRI_API void NRI_CALL nriCmdSetFrontFace(NriCommandBuffer cmd, NriFrontFace frontFace);
NRI_API void NRI_CALL nriCmdSetDepthBiasEnable(NriCommandBuffer cmd, bool enable);
NRI_API void NRI_CALL nriCmdSetDepthBias(NriCommandBuffer cmd, float constantFactor, float clamp, float slopeFactor);
NRI_API void NRI_CALL nriCmdSetDepthClampEnable(NriCommandBuffer cmd, bool enable);
NRI_API void NRI_CALL nriCmdSetLineWidth(NriCommandBuffer cmd, float lineWidth);
NRI_API void NRI_CALL nriCmdSetRasterizationSamples(NriCommandBuffer cmd, uint32_t sampleCount);
NRI_API void NRI_CALL nriCmdSetSampleMask(NriCommandBuffer cmd, uint32_t sampleCount, uint32_t sampleMask);
NRI_API void NRI_CALL nriCmdSetAlphaToCoverageEnable(NriCommandBuffer cmd, bool enable);
NRI_API void NRI_CALL nriCmdSetAlphaToOneEnable(NriCommandBuffer cmd, bool enable);
NRI_API void NRI_CALL nriCmdSetDepthTestEnable(NriCommandBuffer cmd, bool enable);
NRI_API void NRI_CALL nriCmdSetDepthWriteEnable(NriCommandBuffer cmd, bool enable);
NRI_API void NRI_CALL nriCmdSetDepthCompareOp(NriCommandBuffer cmd, NriCompareOp compareOp);
NRI_API void NRI_CALL nriCmdSetDepthBoundsTestEnable(NriCommandBuffer cmd, bool enable);
NRI_API void NRI_CALL nriCmdSetStencilTestEnable(NriCommandBuffer cmd, bool enable);
NRI_API void NRI_CALL nriCmdSetColorBlendEnable(NriCommandBuffer cmd, uint32_t firstAttachment, bool enable);
NRI_API void NRI_CALL nriCmdSetColorBlendEquation(NriCommandBuffer cmd, uint32_t firstAttachment, const NriColorBlendEquation* blendEquation);
NRI_API void NRI_CALL nriCmdSetColorWriteMask(NriCommandBuffer cmd, uint32_t firstAttachment, uint32_t colorWriteMask);
NRI_API void NRI_CALL nriCmdSetLogicOpEnable(NriCommandBuffer cmd, bool enable);

NRI_API void NRI_CALL nriCmdBindDescriptorHeaps(NriCommandBuffer cmd, NriDescriptorHeap resourceHeap, NriDescriptorHeap samplerHeap);
NRI_API void NRI_CALL nriCmdPushData(NriCommandBuffer cmd, const void* data, uint32_t size);
NRI_API void NRI_CALL nriCmdBindVertexBuffer(NriCommandBuffer cmd, uint32_t firstBinding, NriBuffer buffer, uint64_t offset);
NRI_API void NRI_CALL nriCmdBindIndexBuffer(NriCommandBuffer cmd, NriBuffer buffer, uint64_t offset);
NRI_API void NRI_CALL nriCmdDraw(NriCommandBuffer cmd, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance);
NRI_API void NRI_CALL nriCmdDrawIndexed(NriCommandBuffer cmd, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance);
NRI_API void NRI_CALL nriCmdDispatch(NriCommandBuffer cmd, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ);

NRI_API void NRI_CALL nriCmdCopyBuffer(NriCommandBuffer cmd, NriBuffer srcBuffer, NriBuffer dstBuffer, const NriBufferCopyRegion* region);
NRI_API void NRI_CALL nriCmdCopyBufferRegions(NriCommandBuffer cmd, NriBuffer srcBuffer, NriBuffer dstBuffer, const NriBufferCopyRegion* regions, uint32_t regionCount);
NRI_API void NRI_CALL nriCmdFillBuffer(NriCommandBuffer cmd, NriBuffer buffer, uint64_t offset, uint64_t size, uint32_t value);

NRI_API void NRI_CALL nriCmdTransitionTextureLayout(NriCommandBuffer cmd, NriTexture texture, NriTextureLayout oldLayout, NriTextureLayout newLayout);
NRI_API void NRI_CALL nriCmdTransitionSwapchainLayout(NriCommandBuffer cmd, NriSwapchain swapchain, uint32_t imageIndex, NriTextureLayout oldLayout, NriTextureLayout newLayout);
NRI_API void NRI_CALL nriCmdResolveImage(NriCommandBuffer cmd, NriTexture srcTexture, NriTexture dstTexture, uint32_t width, uint32_t height);
NRI_API void NRI_CALL nriCmdCopyTexture(NriCommandBuffer cmd, NriTexture srcTexture, NriTexture dstTexture, uint32_t width, uint32_t height);
NRI_API void NRI_CALL nriCmdCopyTextureMips(NriCommandBuffer cmd, NriTexture srcTexture, uint32_t srcFirstMip, NriTexture dstTexture, uint32_t dstFirstMip, uint32_t mipCount);

NRI_API void NRI_CALL nriCmdResetQueries(NriCommandBuffer cmd, NriQueryPool pool, uint32_t first, uint32_t count);
NRI_API void NRI_CALL nriCmdExecutionBarrier(NriCommandBuffer cmd);
NRI_API void NRI_CALL nriCmdTransferBarrier(NriCommandBuffer cmd);
NRI_API void NRI_CALL nriCmdResourceBarriers(NriCommandBuffer cmd,
                                             const NriTextureBarrierDesc* textures, uint32_t textureCount,
                                             const NriBufferBarrierDesc* buffers, uint32_t bufferCount);

NRI_API void NRI_CALL nriCmdBeginDebugLabel(NriCommandBuffer cmd, const char* label);
NRI_API void NRI_CALL nriCmdEndDebugLabel(NriCommandBuffer cmd);

NRI_EXTERN_C_END
