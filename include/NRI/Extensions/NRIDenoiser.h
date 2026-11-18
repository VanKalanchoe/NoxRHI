#pragma once

#include "../NRI.h"

NRI_EXTERN_C_BEGIN

typedef enum NriDenoiserMethod
{
    NRI_DENOISER_SIGMA_SHADOW = 0,
    NRI_DENOISER_REBLUR_SPECULAR = 1,
    NRI_DENOISER_RELAX_SPECULAR = 2,
    NRI_DENOISER_REBLUR_DIFFUSE = 3,
    NRI_DENOISER_RELAX_DIFFUSE = 4,
    NRI_DENOISER_REBLUR_DIFFUSE_SPECULAR = 5,
    NRI_DENOISER_RELAX_DIFFUSE_SPECULAR = 6
} NriDenoiserMethod;

typedef struct NriDenoiserInstanceDesc
{
    uint32_t identifier;
    NriDenoiserMethod method;
} NriDenoiserInstanceDesc;

typedef struct NriDenoiserDesc
{
    uint32_t width;
    uint32_t height;
    const NriDenoiserInstanceDesc* instances; // NULL creates a default suite (identifiers 0..6 matching NriDenoiserMethod values)
    uint32_t instanceCount;
    uint32_t queuedFrameNum;                  // 0 defaults to 3
    bool enableWholeLifetimeDescriptorCaching;
} NriDenoiserDesc;

typedef struct NriDenoiserCommonSettings
{
    // Column-major 4x4 matrices
    float view[16];
    float proj[16];
    float prevView[16];
    float prevProj[16];

    float motionVectorScale[3]; // {0, 0, 0} defaults to {1, 1, 0}
    float cameraJitter[2];
    float cameraJitterPrev[2];

    uint32_t frameIndex;
    float denoisingRange;       // <= 0 defaults to 5000.0f
    bool resetHistory;
} NriDenoiserCommonSettings;

typedef struct NriDenoiserSigmaSettings
{
    float lightDirection[3];
    float planeDistanceSensitivity; // <= 0 defaults to 0.02f
    uint32_t maxStabilizedFrameNum; // 0 defaults to 5
} NriDenoiserSigmaSettings;

typedef struct NriDenoiserReblurSettings
{
    float hitDistanceA;             // <= 0 defaults to 3.0f
    float hitDistanceB;
    float hitDistanceC;
    float hitDistanceD;
    uint32_t maxAccumulatedFrameNum;  // 0 defaults to 50
    float diffusePrepassBlurRadius;   // <= 0 defaults to 15.0f
    float specularPrepassBlurRadius;  // <= 0 defaults to 40.0f
    float planeDistanceSensitivity;   // <= 0 defaults to 0.02f
    bool disableAntiFirefly;
    bool disableArea5x5HitDistanceReconstruction;
} NriDenoiserReblurSettings;

typedef struct NriDenoiserRelaxSettings
{
    float diffusePrepassBlurRadius;   // <= 0 defaults to 15.0f
    float specularPrepassBlurRadius;  // <= 0 defaults to 30.0f
    uint32_t diffuseMaxAccumulatedFrameNum;  // 0 defaults to 30
    uint32_t specularMaxAccumulatedFrameNum; // 0 defaults to 30
    bool disableAntiFirefly;
} NriDenoiserRelaxSettings;

typedef struct NriDenoiserResourcesDesc
{
    NriTexture inMotionVectors;
    NriTexture inNormalRoughness;
    NriTexture inViewZ;

    // Sigma Shadow
    NriTexture inPenumbra;
    NriTexture outShadowTranslucent;

    // Specular / DiffuseSpecular
    NriTexture inSpecRadianceHitDist;
    NriTexture outSpecRadianceHitDist;

    // Diffuse / DiffuseSpecular
    NriTexture inDiffRadianceHitDist;
    NriTexture outDiffRadianceHitDist;

    bool restoreInitialState;
} NriDenoiserResourcesDesc;

NRI_API bool NRI_CALL nriIsDenoiserAvailable(NriDevice device);

NRI_API NriResult NRI_CALL nriCreateDenoiser(NriDevice device, const NriDenoiserDesc* desc, NriDenoiser* outDenoiser);
NRI_API void NRI_CALL nriDestroyDenoiser(NriDenoiser denoiser);
NRI_API NriResult NRI_CALL nriResizeDenoiser(NriDenoiser denoiser, uint32_t width, uint32_t height);

NRI_API bool NRI_CALL nriDenoiserBeginFrame(NriDenoiser denoiser, const NriDenoiserCommonSettings* settings);
NRI_API void NRI_CALL nriDenoiserSetSigmaSettings(NriDenoiser denoiser, uint32_t identifier, const NriDenoiserSigmaSettings* settings);
NRI_API void NRI_CALL nriDenoiserSetReblurSettings(NriDenoiser denoiser, uint32_t identifier, const NriDenoiserReblurSettings* settings);
NRI_API void NRI_CALL nriDenoiserSetRelaxSettings(NriDenoiser denoiser, uint32_t identifier, const NriDenoiserRelaxSettings* settings);

NRI_API bool NRI_CALL nriCmdDenoise(NriCommandBuffer cmd, NriDenoiser denoiser, const uint32_t* identifiers, uint32_t identifierCount,
                                    const NriDenoiserResourcesDesc* resources);

NRI_EXTERN_C_END
