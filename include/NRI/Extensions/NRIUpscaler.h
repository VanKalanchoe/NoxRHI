#pragma once

#include "../NRI.h"

NRI_EXTERN_C_BEGIN

typedef enum NriUpscalerType
{
    NRI_UPSCALER_TYPE_DLSS_SR = 0, // DLSS Super Resolution
    NRI_UPSCALER_TYPE_DLSS_RR = 1  // DLSS Ray Reconstruction
} NriUpscalerType;

typedef enum NriUpscalerMode
{
    NRI_UPSCALER_MODE_OFF = 0,
    NRI_UPSCALER_MODE_DLAA = 1,
    NRI_UPSCALER_MODE_QUALITY = 2,
    NRI_UPSCALER_MODE_BALANCED = 3,
    NRI_UPSCALER_MODE_PERFORMANCE = 4,
    NRI_UPSCALER_MODE_ULTRA_PERFORMANCE = 5
} NriUpscalerMode;

typedef struct NriUpscalerDesc
{
    NriUpscalerType type;
} NriUpscalerDesc;

typedef struct NriUpscalerOptimalSettings
{
    NriExtent2D renderSize;
    NriExtent2D minRenderSize;
    NriExtent2D maxRenderSize;
    float optimalSharpness;
} NriUpscalerOptimalSettings;

typedef struct NriUpscalerEvaluateDesc
{
    NriTexture inputColor;
    NriTexture outputColor;
    NriTexture depth;
    NriTexture motionVectors;

    // Optional Ray Reconstruction (DLSS-RR) guide inputs
    NriTexture albedo;
    NriTexture specularAlbedo;
    NriTexture normal;
    NriTexture roughness;
    bool normalRoughnessPacked;
    NriTexture specularHitDistance;
    NriTexture specularMotionVectors;
    NriTexture colorBeforeTransparency;

    // Column-major 4x4 camera transforms (non-jittered projections)
    float nonJitteredProj[16];
    float view[16];
    float prevNonJitteredProj[16];
    float prevView[16];

    float jitterOffset[2];
    float motionVectorScale[2]; // {0, 0} defaults to {1, 1}
    float cameraPos[3];
    float cameraUp[3];
    float cameraRight[3];
    float cameraFwd[3];

    float cameraNear;
    float cameraFar;            // <= 0 defaults to 10000.0f
    float cameraFovRad;
    bool reset;
    bool disableHDR;            // False (default) treats input/output color buffers as HDR
    NriUpscalerMode mode;
} NriUpscalerEvaluateDesc;

NRI_API bool NRI_CALL nriIsStreamlineInitialized(NriDevice device);
NRI_API bool NRI_CALL nriIsUpscalerSupported(NriDevice device, NriUpscalerType type);

NRI_API NriResult NRI_CALL nriCreateUpscaler(NriDevice device, const NriUpscalerDesc* desc, NriUpscaler* outUpscaler);
NRI_API void NRI_CALL nriDestroyUpscaler(NriUpscaler upscaler);

NRI_API NriResult NRI_CALL nriUpscalerGetOptimalSettings(NriUpscaler upscaler, NriUpscalerMode mode, NriExtent2D outputSize,
                                                        NriUpscalerOptimalSettings* outSettings);
NRI_API void NRI_CALL nriUpscalerReset(NriUpscaler upscaler);
NRI_API bool NRI_CALL nriCmdEvaluateUpscaler(NriCommandBuffer cmd, NriUpscaler upscaler, const NriUpscalerEvaluateDesc* desc);

NRI_EXTERN_C_END
