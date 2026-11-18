#pragma once

#include "NRITypes.h"

NRI_EXTERN_C_BEGIN

// -----------------------------------------------------------------------------
// Buffer
// -----------------------------------------------------------------------------

typedef enum NriBufferUsage
{
    NRI_BUFFER_USAGE_STAGING = 0,
    NRI_BUFFER_USAGE_UNIFORM = 1,
    NRI_BUFFER_USAGE_VERTEX = 2,
    NRI_BUFFER_USAGE_INDEX = 3,
    NRI_BUFFER_USAGE_STORAGE = 4,
    NRI_BUFFER_USAGE_STORAGE_STATIC = 5,
    NRI_BUFFER_USAGE_DESCRIPTOR_HEAP = 6,
    NRI_BUFFER_USAGE_INDIRECT = 7,
    NRI_BUFFER_USAGE_INDIRECT_STATIC = 8,
    NRI_BUFFER_USAGE_ACCELERATION_STRUCTURE = 9,
    NRI_BUFFER_USAGE_ACCELERATION_STRUCTURE_SCRATCH = 10,
    NRI_BUFFER_USAGE_ACCELERATION_STRUCTURE_INSTANCE = 11,
    NRI_BUFFER_USAGE_SHADER_BINDING_TABLE = 12
} NriBufferUsage;

typedef struct NriBufferDesc
{
    uint64_t size;
    NriBufferUsage usage;
    bool sharedAcrossQueues;
    bool sparse;
} NriBufferDesc;

NRI_API NriResult NRI_CALL nriCreateBuffer(NriDevice device, const NriBufferDesc* desc, NriBuffer* outBuffer);
NRI_API void NRI_CALL nriDestroyBuffer(NriBuffer buffer);
NRI_API void* NRI_CALL nriMapBuffer(NriBuffer buffer, uint64_t offset, uint64_t size);
NRI_API void NRI_CALL nriUnmapBuffer(NriBuffer buffer);
NRI_API uint64_t NRI_CALL nriGetBufferDeviceAddress(NriBuffer buffer);
NRI_API uint64_t NRI_CALL nriGetBufferSize(NriBuffer buffer);
NRI_API uint64_t NRI_CALL nriGetBufferSparsePageSize(NriBuffer buffer);

// -----------------------------------------------------------------------------
// Texture
// -----------------------------------------------------------------------------

typedef enum NriImageFormat
{
    NRI_FORMAT_NONE = 0,
    NRI_FORMAT_SURFACE = 1,
    NRI_FORMAT_RGB8 = 2,
    NRI_FORMAT_SRGB8 = 3,
    NRI_FORMAT_RGBA8 = 4,
    NRI_FORMAT_SRGBA8 = 5,
    NRI_FORMAT_R16_SFLOAT = 6,
    NRI_FORMAT_R32_SFLOAT = 7,
    NRI_FORMAT_R10G10B10A2_UNORM = 8,
    NRI_FORMAT_R16G16 = 9,
    NRI_FORMAT_R32_SINT = 10,
    NRI_FORMAT_R32G32_UINT = 11,
    NRI_FORMAT_R32G32_SFLOAT = 12,
    NRI_FORMAT_R32G32B32A32_UINT = 13,
    NRI_FORMAT_R16G16_SFLOAT = 14,
    NRI_FORMAT_R16G16B16A16_SFLOAT = 15,
    NRI_FORMAT_R32G32B32A32_SFLOAT = 16,
    NRI_FORMAT_BC1_UNORM = 17,
    NRI_FORMAT_BC1_UNORM_SRGB = 18,
    NRI_FORMAT_BC2_UNORM = 19,
    NRI_FORMAT_BC2_UNORM_SRGB = 20,
    NRI_FORMAT_BC3_UNORM = 21,
    NRI_FORMAT_BC3_UNORM_SRGB = 22,
    NRI_FORMAT_BC4_UNORM = 23,
    NRI_FORMAT_BC4_SNORM = 24,
    NRI_FORMAT_BC5_UNORM = 25,
    NRI_FORMAT_BC5_SNORM = 26,
    NRI_FORMAT_BC6H_UF16 = 27,
    NRI_FORMAT_BC6H_SF16 = 28,
    NRI_FORMAT_BC7_UNORM = 29,
    NRI_FORMAT_BC7_UNORM_SRGB = 30
} NriImageFormat;

typedef enum NriTextureLayout
{
    NRI_TEXTURE_LAYOUT_UNDEFINED = 0,
    NRI_TEXTURE_LAYOUT_COLOR_ATTACHMENT = 1,
    NRI_TEXTURE_LAYOUT_DEPTH_ATTACHMENT = 2,
    NRI_TEXTURE_LAYOUT_SHADER_RESOURCE = 3,
    NRI_TEXTURE_LAYOUT_TRANSFER_SRC = 4,
    NRI_TEXTURE_LAYOUT_TRANSFER_DST = 5,
    NRI_TEXTURE_LAYOUT_PRESENT = 6,
    NRI_TEXTURE_LAYOUT_GENERAL = 7
} NriTextureLayout;

typedef enum NriTextureUsage
{
    NRI_TEXTURE_USAGE_DEFAULT = 0,
    NRI_TEXTURE_USAGE_COLOR_ATTACHMENT = 1,
    NRI_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT = 2,
    NRI_TEXTURE_USAGE_SHADER_RESOURCE = 3,
    NRI_TEXTURE_USAGE_COLOR_RESOLVE_ATTACHMENT = 4,
    NRI_TEXTURE_USAGE_STORAGE = 5
} NriTextureUsage;

typedef struct NriTextureDesc
{
    uint32_t width;
    uint32_t height;
    uint32_t arrayLayers;
    bool isCubeMap;
    uint32_t mipLevels;
    uint32_t sampleCount;
    NriTextureUsage usage;
    NriImageFormat format;
    uint32_t directFormat; // Set UINT32_MAX when unused
    bool sharedAcrossQueues;
} NriTextureDesc;

typedef struct NriTextureRegionUpload
{
    uint64_t stagingOffset;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} NriTextureRegionUpload;

NRI_API bool NRI_CALL nriImageFormatBlock(NriImageFormat format, uint32_t* outBlockBytes, uint32_t* outBlockSize);
NRI_API uint64_t NRI_CALL nriEstimateTextureBytes(const NriTextureDesc* desc);

NRI_API NriResult NRI_CALL nriCreateTexture(NriDevice device, const NriTextureDesc* desc, NriTexture* outTexture);
NRI_API void NRI_CALL nriDestroyTexture(NriTexture texture);
NRI_API uint32_t NRI_CALL nriGetTextureWidth(NriTexture texture);
NRI_API uint32_t NRI_CALL nriGetTextureHeight(NriTexture texture);
NRI_API uint32_t NRI_CALL nriGetTextureMipLevels(NriTexture texture);
NRI_API uint32_t NRI_CALL nriGetTextureArrayLayers(NriTexture texture);
NRI_API NriTextureUsage NRI_CALL nriGetTextureUsage(NriTexture texture);
NRI_API uint32_t NRI_CALL nriGetTextureDescriptorSlot(NriTexture texture);

NRI_API void NRI_CALL nriTextureUploadFromBuffer(NriTexture texture, NriCommandBuffer cmdBuffer, NriBuffer stagingBuffer,
                                                 uint32_t width, uint32_t height, uint32_t mipLevels,
                                                 const size_t* mipOffsets, uint32_t mipOffsetCount);
NRI_API void NRI_CALL nriTextureRecordUpload(NriTexture texture, NriCommandBuffer cmdBuffer, NriBuffer stagingBuffer,
                                             uint64_t stagingOffset, const size_t* mipOffsets, uint32_t mipOffsetCount);
NRI_API void NRI_CALL nriTextureRecordInitialLayout(NriTexture texture, NriCommandBuffer cmdBuffer);
NRI_API void NRI_CALL nriTextureRecordRegionUploads(NriTexture texture, NriCommandBuffer cmdBuffer, NriBuffer stagingBuffer,
                                                    const NriTextureRegionUpload* regions, uint32_t regionCount);
NRI_API void NRI_CALL nriTextureCopyToBuffer(NriTexture texture, NriCommandBuffer cmdBuffer, NriBuffer dstBuffer,
                                             uint32_t x, uint32_t y, uint32_t width, uint32_t height);
NRI_API void NRI_CALL nriTextureBlitTo(NriTexture srcTexture, NriCommandBuffer cmdBuffer, NriTexture dstTexture);
NRI_API void NRI_CALL nriTextureGenerateMipmaps(NriTexture texture, NriCommandBuffer cmdBuffer);

// -----------------------------------------------------------------------------
// Sampler & DescriptorHeap
// -----------------------------------------------------------------------------

typedef enum NriFilter
{
    NRI_FILTER_NEAREST = 0,
    NRI_FILTER_LINEAR = 1
} NriFilter;

typedef enum NriSamplerMipmapMode
{
    NRI_SAMPLER_MIPMAP_NEAREST = 0,
    NRI_SAMPLER_MIPMAP_LINEAR = 1
} NriSamplerMipmapMode;

typedef enum NriSamplerAddressMode
{
    NRI_SAMPLER_ADDRESS_REPEAT = 0,
    NRI_SAMPLER_ADDRESS_MIRRORED_REPEAT = 1,
    NRI_SAMPLER_ADDRESS_CLAMP_TO_EDGE = 2,
    NRI_SAMPLER_ADDRESS_CLAMP_TO_BORDER = 3,
    NRI_SAMPLER_ADDRESS_MIRROR_CLAMP_TO_EDGE = 4
} NriSamplerAddressMode;

typedef enum NriBorderColor
{
    NRI_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK = 0,
    NRI_BORDER_COLOR_INT_TRANSPARENT_BLACK = 1,
    NRI_BORDER_COLOR_FLOAT_OPAQUE_BLACK = 2,
    NRI_BORDER_COLOR_INT_OPAQUE_BLACK = 3,
    NRI_BORDER_COLOR_FLOAT_OPAQUE_WHITE = 4,
    NRI_BORDER_COLOR_INT_OPAQUE_WHITE = 5
} NriBorderColor;

typedef struct NriSamplerDesc
{
    NriFilter magFilter;
    NriFilter minFilter;
    NriSamplerMipmapMode mipmapMode;
    NriSamplerAddressMode addressModeU;
    NriSamplerAddressMode addressModeV;
    NriSamplerAddressMode addressModeW;
    float mipLodBias;
    bool anisotropyEnable;
    float maxAnisotropy;
    bool compareEnable;
    NriCompareOp compareOp;
    float minLod;
    float maxLod;
    NriBorderColor borderColor;
} NriSamplerDesc;

typedef enum NriDescriptorHeapType
{
    NRI_DESCRIPTOR_HEAP_RESOURCE = 0,
    NRI_DESCRIPTOR_HEAP_SAMPLER = 1
} NriDescriptorHeapType;

typedef struct NriDescriptorHeapDesc
{
    NriDescriptorHeapType type;
    uint32_t maxSamplerDescriptors;
    uint32_t maxBufferDescriptors;
    uint32_t maxImageDescriptors;
    const NriSamplerDesc* samplers;
    uint32_t samplerCount;
} NriDescriptorHeapDesc;

NRI_API NriResult NRI_CALL nriCreateDescriptorHeap(NriDevice device, const NriDescriptorHeapDesc* desc, NriDescriptorHeap* outHeap);
NRI_API void NRI_CALL nriDestroyDescriptorHeap(NriDescriptorHeap heap);
NRI_API void NRI_CALL nriRegisterTexture(NriDescriptorHeap heap, NriTexture texture, NriTextureUsage usageOverride);
NRI_API uint32_t NRI_CALL nriRegisterStorageTextureMip(NriDescriptorHeap heap, NriTexture texture, uint32_t mipLevel);
NRI_API void NRI_CALL nriUnregisterTexture(NriDescriptorHeap heap, uint32_t slot);
NRI_API uint32_t NRI_CALL nriRegisterBuffer(NriDescriptorHeap heap, NriBuffer buffer, uint64_t size);
NRI_API uint32_t NRI_CALL nriGetDescriptorHeapImageIndexOffset(NriDescriptorHeap heap);
NRI_API uint64_t NRI_CALL nriGetDescriptorHeapSize(NriDescriptorHeap heap);
NRI_API uint64_t NRI_CALL nriGetDescriptorHeapReservedRangeOffset(NriDescriptorHeap heap);
NRI_API uint64_t NRI_CALL nriGetDescriptorHeapReservedRangeSize(NriDescriptorHeap heap);
NRI_API uint64_t NRI_CALL nriGetDescriptorHeapGPUAddress(NriDescriptorHeap heap);

// -----------------------------------------------------------------------------
// QueryPool
// -----------------------------------------------------------------------------

typedef enum NriQueryType
{
    NRI_QUERY_TYPE_AS_COMPACTED_SIZE = 0
} NriQueryType;

typedef struct NriQueryPoolDesc
{
    NriQueryType type;
    uint32_t capacity;
} NriQueryPoolDesc;

NRI_API NriResult NRI_CALL nriCreateQueryPool(NriDevice device, const NriQueryPoolDesc* desc, NriQueryPool* outPool);
NRI_API void NRI_CALL nriDestroyQueryPool(NriQueryPool pool);
NRI_API uint32_t NRI_CALL nriGetQueryPoolCapacity(NriQueryPool pool);
NRI_API bool NRI_CALL nriGetQueryPoolResults(NriQueryPool pool, uint32_t first, uint64_t* outValues, uint32_t count);

NRI_EXTERN_C_END
