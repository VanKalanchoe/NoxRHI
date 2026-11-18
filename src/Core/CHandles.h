#pragma once
#include <memory>
#include "Device.h"
#include "../Extensions/ShaderCompiler/ShaderCompiler.h"
#include "../Extensions/Imgui/ImguiExtension.h"
#include "../Extensions/Upscaler/UpscalerExtension.h"
#include "../Extensions/Denoiser/DenoiserExtension.h"

struct NriTimelineSemaphore_T
{
    std::shared_ptr<NRI::Device> device;
    std::shared_ptr<NRI::TimelineSemaphore> impl;
};

struct NriDevice_T
{
    std::shared_ptr<NRI::Device> impl;
    NriTimelineSemaphore_T borrowedSparseSemaphore{};
};

struct NriSwapchain_T
{
    std::shared_ptr<NRI::Device> device;
    std::shared_ptr<NRI::Swapchain> impl;
};

struct NriPipeline_T
{
    std::shared_ptr<NRI::Device> device;
    std::shared_ptr<NRI::Pipeline> impl;
};

struct NriCommandAllocator_T
{
    std::shared_ptr<NRI::Device> device;
    std::shared_ptr<NRI::CommandAllocator> impl;
};

struct NriCommandBuffer_T
{
    std::shared_ptr<NRI::Device> device;
    std::shared_ptr<NRI::CommandAllocator> allocator;
    std::shared_ptr<NRI::CommandBuffer> impl;
};

struct NriTexture_T
{
    std::shared_ptr<NRI::Device> device;
    std::shared_ptr<NRI::Texture2D> impl;
};

struct NriBuffer_T
{
    std::shared_ptr<NRI::Device> device;
    std::shared_ptr<NRI::Buffer> impl;
};

struct NriDescriptorHeap_T
{
    std::shared_ptr<NRI::Device> device;
    std::shared_ptr<NRI::DescriptorHeap> impl;
};

struct NriAccelerationStructure_T
{
    std::shared_ptr<NRI::Device> device;
    std::shared_ptr<NRI::AccelerationStructure> impl;
};

struct NriQueryPool_T
{
    std::shared_ptr<NRI::Device> device;
    std::shared_ptr<NRI::QueryPool> impl;
};

struct NriGpuProfiler_T
{
    std::shared_ptr<NRI::Device> device;
    std::shared_ptr<NRI::GpuProfiler> impl;
};

struct NriShaderCompiler_T
{
    std::unique_ptr<NRI::ShaderCompiler> impl;
};

struct NriImgui_T
{
    std::shared_ptr<NRI::Device> device;
    std::unique_ptr<NRI::ImguiExtension> impl;
};

struct NriUpscaler_T
{
    std::shared_ptr<NRI::Device> device;
    std::unique_ptr<NRI::UpscalerExtension> impl;
};

struct NriDenoiser_T
{
    std::shared_ptr<NRI::Device> device;
    std::unique_ptr<NRI::DenoiserExtension> impl;
};
