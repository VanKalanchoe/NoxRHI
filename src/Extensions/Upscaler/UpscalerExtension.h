#pragma once

#include <memory>

#include "../../Core/Device.h"
#include "StreamlineContext.h"
#include "NRI/Extensions/NRIUpscaler.h"

namespace NRI
{
    class UpscalerExtension
    {
    public:
        UpscalerExtension(std::shared_ptr<Device> device, NriUpscalerType type);
        ~UpscalerExtension();

        NriResult getOptimalSettings(NriUpscalerMode mode, Extent2D outputSize, NriUpscalerOptimalSettings& outSettings) const;
        void reset();
        bool evaluate(CommandBuffer& cmd, const UpscalerTextures& textures, const NriUpscalerEvaluateDesc& desc);

    private:
        std::shared_ptr<Device> m_device;
        NriUpscalerType m_type = NRI_UPSCALER_TYPE_DLSS_SR;
    };
}
