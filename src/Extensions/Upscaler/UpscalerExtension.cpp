#include "UpscalerExtension.h"
#include "StreamlineContext.h"

namespace NRI
{
    UpscalerExtension::UpscalerExtension(std::shared_ptr<Device> device, NriUpscalerType type)
        : m_device(std::move(device)), m_type(type)
    {
    }

    UpscalerExtension::~UpscalerExtension()
    {
        reset();
    }

    NriResult UpscalerExtension::getOptimalSettings(NriUpscalerMode mode, Extent2D outputSize, NriUpscalerOptimalSettings& outSettings) const
    {
        auto* slCtx = m_device ? m_device->getStreamlineContext() : nullptr;
        if (!slCtx)
        {
            outSettings.renderSize = { outputSize.width, outputSize.height };
            outSettings.minRenderSize = { outputSize.width, outputSize.height };
            outSettings.maxRenderSize = { outputSize.width, outputSize.height };
            outSettings.optimalSharpness = 0.0f;
            return NRI_SUCCESS;
        }
        return slCtx->getOptimalSettings(m_type, mode, outputSize, outSettings);
    }

    void UpscalerExtension::reset()
    {
        if (auto* slCtx = m_device ? m_device->getStreamlineContext() : nullptr)
            slCtx->resetViewport();
    }

    bool UpscalerExtension::evaluate(CommandBuffer& cmd, const UpscalerTextures& textures, const NriUpscalerEvaluateDesc& desc)
    {
        auto* slCtx = m_device ? m_device->getStreamlineContext() : nullptr;
        return slCtx ? slCtx->evaluate(*m_device, cmd, m_type, textures, desc) : false;
    }
}
