#pragma once

#include <cstdint>
#include <memory>

#include "../../Core/Device.h"
#include "NRI/Extensions/NRIDenoiser.h"

namespace NRI
{
    struct DenoiserTextures
    {
        Texture2D* inMotionVectors = nullptr;
        Texture2D* inNormalRoughness = nullptr;
        Texture2D* inViewZ = nullptr;
        Texture2D* inPenumbra = nullptr;
        Texture2D* outShadowTranslucent = nullptr;
        Texture2D* inSpecRadianceHitDist = nullptr;
        Texture2D* outSpecRadianceHitDist = nullptr;
        Texture2D* inDiffRadianceHitDist = nullptr;
        Texture2D* outDiffRadianceHitDist = nullptr;
    };

    class DenoiserExtension
    {
    public:
        DenoiserExtension(std::shared_ptr<Device> device, const NriDenoiserDesc& desc);
        ~DenoiserExtension();

        static bool isAvailable(const Device& device);

        NriResult resize(uint32_t width, uint32_t height);
        bool beginFrame(const NriDenoiserCommonSettings& settings);
        void setSigmaSettings(uint32_t identifier, const NriDenoiserSigmaSettings& settings);
        void setReblurSettings(uint32_t identifier, const NriDenoiserReblurSettings& settings);
        void setRelaxSettings(uint32_t identifier, const NriDenoiserRelaxSettings& settings);

        bool denoise(CommandBuffer& cmd,
                     const uint32_t* identifiers,
                     uint32_t identifierCount,
                     const DenoiserTextures& textures,
                     bool restoreInitialState);

    private:
        bool recreate(uint32_t width, uint32_t height);

    private:
        struct Impl;
        std::shared_ptr<Device> m_device;
        std::unique_ptr<Impl> m_impl;
    };
}
