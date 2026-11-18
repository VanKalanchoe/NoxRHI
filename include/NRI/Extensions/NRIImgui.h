#pragma once

#include "../NRI.h"
#include "NRIDeviceCreation.h"

NRI_EXTERN_C_BEGIN

typedef struct NriImguiVertex
{
    float pos[2];
    float uv[2];
    uint32_t col;
} NriImguiVertex;

typedef struct NriImguiDrawCmd
{
    float clipRect[4]; // x1, y1, x2, y2
    uint32_t elemCount;
    uint32_t idxOffset;
    int32_t vtxOffset;
    uint64_t textureId;
} NriImguiDrawCmd;

typedef struct NriImguiDrawList
{
    const NriImguiVertex* vertices;
    uint32_t vertexCount;
    const void* indices;
    uint32_t indexCount;
    uint32_t indexSize; // 2 or 4 bytes
    const NriImguiDrawCmd* cmds;
    uint32_t cmdCount;
} NriImguiDrawList;

typedef struct NriImguiDrawDataDesc
{
    const NriImguiDrawList* drawLists;
    uint32_t drawListCount;
    float displayPos[2];
    float displaySize[2];
    float framebufferScale[2];
    uint32_t frameIndex;
    uint32_t viewportSlot;
} NriImguiDrawDataDesc;

typedef struct NriImguiDesc
{
    NriDescriptorHeap resourceHeap; // Optional shared resource heap; NULL allocates an internal heap
    NriDescriptorHeap samplerHeap;  // Optional shared sampler heap; NULL allocates an internal linear sampler heap
    uint32_t samplerSlot;           // Sampler index inside samplerHeap
    NriImageFormat colorFormat;     // Target format (NRI_FORMAT_NONE defaults to NRI_FORMAT_SURFACE)
    uint32_t framesInFlight;        // Buffering depth (0 defaults to 2)
    bool linearColorOutput;         // Set true when rendering into an sRGB swapchain target (e.g. NRI_FORMAT_SURFACE)
} NriImguiDesc;

typedef struct NriImguiViewportDesc
{
    NriNativeWindowHandle window;
    uint32_t width;
    uint32_t height;
    bool vsync;
} NriImguiViewportDesc;

// Encodes a DescriptorHeap texture slot into a non-zero uint64_t textureId suitable for UI draw commands.
static inline uint64_t nriImguiEncodeTextureSlot(uint32_t descriptorSlot)
{
    return (uint64_t)descriptorSlot + 1ull;
}

NRI_API NriResult NRI_CALL nriCreateImgui(NriDevice device, const NriImguiDesc* desc, NriImgui* outImgui);
NRI_API void NRI_CALL nriDestroyImgui(NriImgui imgui);

NRI_API NriResult NRI_CALL nriImguiCreateTexture(NriImgui imgui, uint32_t width, uint32_t height, const void* rgbaPixels, uint64_t* outTextureId);
NRI_API NriResult NRI_CALL nriImguiUpdateTexture(NriImgui imgui, uint64_t textureId, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                                                 const void* fullRgbaPixels, uint32_t texWidth, uint32_t texHeight);
NRI_API void NRI_CALL nriImguiDestroyTexture(NriImgui imgui, uint64_t textureId);

// Upload vertex/index buffers before beginning the render pass, then record UI draws inside the pass.
NRI_API void NRI_CALL nriCmdCopyImguiData(NriCommandBuffer cmd, NriImgui imgui, const NriImguiDrawDataDesc* drawData);
NRI_API void NRI_CALL nriCmdDrawImgui(NriCommandBuffer cmd, NriImgui imgui, const NriImguiDrawDataDesc* drawData);

// Multi-viewport secondary swapchain helpers implemented internally over NRI primitives.
NRI_API NriResult NRI_CALL nriImguiCreateViewport(NriImgui imgui, const NriImguiViewportDesc* desc, uint64_t* outViewportHandle);
NRI_API void NRI_CALL nriImguiResizeViewport(NriImgui imgui, uint64_t viewportHandle, uint32_t width, uint32_t height);
NRI_API void NRI_CALL nriImguiDestroyViewport(NriImgui imgui, uint64_t viewportHandle);
NRI_API uint32_t NRI_CALL nriImguiGetViewportSlot(NriImgui imgui, uint64_t viewportHandle);
NRI_API void NRI_CALL nriImguiRenderViewport(NriImgui imgui, uint64_t viewportHandle, const NriImguiDrawDataDesc* drawData,
                                           float clearR, float clearG, float clearB, float clearA);
NRI_API void NRI_CALL nriImguiPresentViewport(NriImgui imgui, uint64_t viewportHandle);

NRI_EXTERN_C_END
