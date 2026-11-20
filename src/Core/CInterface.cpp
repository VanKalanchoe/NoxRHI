#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <vector>

#include "NRI/NRI.h"
#include "NRI/Extensions/NRIDeviceCreation.h"
#include "NRI/Extensions/NRISwapchain.h"
#include "NRI/Extensions/NRIRayTracing.h"
#include "NRI/Extensions/NRIMeshShader.h"
#include "NRI/Extensions/NRIGpuProfiler.h"
#include "NRI/Extensions/NRIShaderCompiler.h"
#include "NRI/Extensions/NRIImgui.h"
#include "NRI/Extensions/NRIUpscaler.h"
#include "NRI/Extensions/NRIDenoiser.h"

#include "CHandles.h"
#include "Log.h"
#include "ShaderBindingTable.h"

namespace
{
    template <typename TWrapper>
    void deferredRelease(TWrapper* wrapper)
    {
        if (!wrapper)
            return;
        std::unique_ptr<TWrapper> owned(wrapper);
        if (owned->device && owned->impl)
        {
            auto dev = owned->device;
            auto impl = std::move(owned->impl);
            dev->enqueueDeferredDeletion([dev, impl]() mutable
            {
                impl.reset();
            });
        }
    }

    NRI::GraphicsAPI toCpp(NriGraphicsAPI api)
    {
        return static_cast<NRI::GraphicsAPI>(api);
    }

    NriGraphicsAPI toC(NRI::GraphicsAPI api)
    {
        return static_cast<NriGraphicsAPI>(api);
    }

    NRI::QueueType toCpp(NriQueueType q)
    {
        return static_cast<NRI::QueueType>(q);
    }

    NRI::CompareOp toCpp(NriCompareOp op)
    {
        return static_cast<NRI::CompareOp>(op);
    }

    NRI::ImageFormat toCpp(NriImageFormat fmt)
    {
        return static_cast<NRI::ImageFormat>(fmt);
    }

    NRI::TextureUsage toCpp(NriTextureUsage u)
    {
        return static_cast<NRI::TextureUsage>(u);
    }

    NriTextureUsage toC(NRI::TextureUsage u)
    {
        return static_cast<NriTextureUsage>(u);
    }

    NRI::TextureLayout toCpp(NriTextureLayout l)
    {
        return static_cast<NRI::TextureLayout>(l);
    }

    NRI::BufferUsage toCpp(NriBufferUsage u)
    {
        return static_cast<NRI::BufferUsage>(u);
    }

    NRI::LoadOP toCpp(NriLoadOp op)
    {
        return static_cast<NRI::LoadOP>(op);
    }

    NRI::StoreOP toCpp(NriStoreOp op)
    {
        return static_cast<NRI::StoreOP>(op);
    }

    NRI::NativeWindowHandle toCpp(const NriNativeWindowHandle& w)
    {
        NRI::NativeWindowHandle out{};
        out.type = static_cast<NRI::WindowSystemType>(w.type);
        out.hwnd = w.hwnd;
        out.hinstance = w.hinstance;
        out.vkSurface = w.vkSurface;
        return out;
    }

    NRI::TextureDesc toCpp(const NriTextureDesc& d)
    {
        NRI::TextureDesc out{};
        out.width = d.width;
        out.height = d.height;
        out.arrayLayers = d.arrayLayers ? d.arrayLayers : 1;
        out.isCubeMap = d.isCubeMap;
        out.mipLevels = d.mipLevels ? d.mipLevels : 1;
        out.sampleCount = d.sampleCount ? d.sampleCount : 1;
        out.usage = toCpp(d.usage);
        out.format = toCpp(d.format);
        out.directFormat = d.directFormat;
        out.sharedAcrossQueues = d.sharedAcrossQueues;
        return out;
    }

    NRI::AccelerationStructureBuildDesc toCpp(const NriAccelerationStructureBuildDesc& d,
                                              std::vector<NRI::AccelerationStructureTrianglesDesc>& triStorage)
    {
        NRI::AccelerationStructureBuildDesc out{};
        out.type = static_cast<NRI::AccelerationStructureType>(d.type);
        out.flags = static_cast<NRI::AccelerationStructureBuildFlags>(d.flags);
        if (d.triangles && d.triangleCount > 0)
        {
            triStorage.resize(d.triangleCount);
            for (uint32_t i = 0; i < d.triangleCount; ++i)
            {
                const auto& src = d.triangles[i];
                triStorage[i] = NRI::AccelerationStructureTrianglesDesc{
                    .vertexBufferAddress = src.vertexBufferAddress,
                    .vertexStride = src.vertexStride,
                    .maxVertex = src.maxVertex,
                    .indexBufferAddress = src.indexBufferAddress,
                    .primitiveCount = src.primitiveCount,
                    .primitiveOffset = src.primitiveOffset,
                    .firstVertex = src.firstVertex,
                    .isOpaque = src.isOpaque
                };
            }
            out.triangles = triStorage;
        }
        out.instances = { d.instances.instanceBufferAddress, d.instances.instanceCount };
        return out;
    }

    NriResult translateException(const std::exception& e)
    {
        NRI::SetLastError(e.what());
        NRI_LOG_ERROR("{}", e.what());
        return NRI_ERROR_INTERNAL;
    }

    NriShaderProgressCallbacks g_cShaderProgress{};
    NRI::ShaderProgress g_cppShaderProgress{};

    NriLogCallback g_cLogCallback = nullptr;
    void* g_cLogUserData = nullptr;

    void forwardCLog(NRI::LogLevel level, const char* message, void* userData)
    {
        (void)userData;
        if (g_cLogCallback)
            g_cLogCallback(static_cast<NriLogLevel>(level), message, g_cLogUserData);
    }
}

extern "C" {

// -----------------------------------------------------------------------------
// Logging & Error
// -----------------------------------------------------------------------------

void NRI_CALL nriSetLogCallback(NriLogCallback callback, void* userData)
{
    g_cLogCallback = callback;
    g_cLogUserData = userData;
    NRI::SetLogCallback(callback ? &forwardCLog : nullptr, nullptr);
}

const char* NRI_CALL nriGetLastError(void)
{
    return NRI::GetLastError();
}

// -----------------------------------------------------------------------------
// Device Creation & Lifetime
// -----------------------------------------------------------------------------

NriResult NRI_CALL nriCreateDevice(const NriDeviceDesc* desc, NriDevice* outDevice)
{
    if (!desc || !outDevice)
        return NRI_ERROR_INVALID_ARGUMENT;
    *outDevice = nullptr;

    if (desc->logCallback)
        nriSetLogCallback(desc->logCallback, desc->logUserData);

    try
    {
        NRI::DeviceInitDesc init{};
        init.api = toCpp(desc->api);
        init.window = toCpp(desc->window);
        init.enableValidation = desc->enableValidation;
        init.enableStreamline = desc->enableStreamline;
        init.streamlineAppId = desc->streamlineAppId;
        if (desc->streamlineProjectId && desc->streamlineProjectId[0] != '\0')
            init.streamlineProjectId = desc->streamlineProjectId;
        if (desc->shaderCacheDir && desc->shaderCacheDir[0] != '\0')
            init.shaderCacheDir = desc->shaderCacheDir;
        if (desc->extraInstanceExtensions && desc->extraInstanceExtensionCount > 0)
        {
            for (uint32_t i = 0; i < desc->extraInstanceExtensionCount; ++i)
            {
                if (desc->extraInstanceExtensions[i])
                    init.extraInstanceExtensions.emplace_back(desc->extraInstanceExtensions[i]);
            }
        }
        if (desc->extraDeviceExtensions && desc->extraDeviceExtensionCount > 0)
        {
            for (uint32_t i = 0; i < desc->extraDeviceExtensionCount; ++i)
            {
                if (desc->extraDeviceExtensions[i])
                    init.extraDeviceExtensions.emplace_back(desc->extraDeviceExtensions[i]);
            }
        }

        std::shared_ptr<NRI::Device> dev = NRI::Device::create(init);
        if (!dev)
            return NRI_ERROR_INTERNAL;

        auto* handle = new NriDevice_T{ std::move(dev) };
        *outDevice = handle;
        return NRI_SUCCESS;
    }
    catch (const std::bad_alloc&)
    {
        return NRI_ERROR_OUT_OF_MEMORY;
    }
    catch (const std::exception& e)
    {
        return translateException(e);
    }
}

void NRI_CALL nriDestroyDevice(NriDevice device)
{
    if (!device)
        return;
    std::unique_ptr<NriDevice_T> owned(device);
    if (owned->impl)
    {
        owned->impl->shutdown();
    }
}

NriGraphicsAPI NRI_CALL nriGetDeviceAPI(NriDevice device)
{
    return device && device->impl ? toC(device->impl->getAPI()) : NRI_GRAPHICS_API_VULKAN;
}

uint32_t NRI_CALL nriGetMSAASampleCount(NriDevice device)
{
    return device && device->impl ? device->impl->getMSAASampleCount() : 1;
}

bool NRI_CALL nriHasDedicatedTransferQueue(NriDevice device)
{
    return device && device->impl ? device->impl->hasDedicatedTransferQueue() : false;
}

bool NRI_CALL nriHasAsyncComputeQueue(NriDevice device)
{
    return device && device->impl ? device->impl->hasAsyncComputeQueue() : false;
}

bool NRI_CALL nriIsSurfaceFormatBGRA(NriDevice device)
{
    return device && device->impl ? device->impl->isSurfaceFormatBGRA() : false;
}

bool NRI_CALL nriIsMemoryBudgetSupported(NriDevice device)
{
    return device && device->impl ? device->impl->isMemoryBudgetSupported() : false;
}

bool NRI_CALL nriSupportsSparseBuffers(NriDevice device)
{
    return device && device->impl ? device->impl->supportsSparseBuffers() : false;
}

uint64_t NRI_CALL nriGetSparseBufferReserveLimit(NriDevice device)
{
    return device && device->impl ? device->impl->getSparseBufferReserveLimit() : 0;
}

NriTimelinePoint NRI_CALL nriGetSparseTimelinePoint(NriDevice device)
{
    if (!device || !device->impl)
        return { nullptr, 0 };
    const NRI::TimelinePoint pt = device->impl->getSparseTimelinePoint();
    if (!pt.semaphore)
        return { nullptr, 0 };
    device->borrowedSparseSemaphore.device = device->impl;
    device->borrowedSparseSemaphore.impl = std::shared_ptr<NRI::TimelineSemaphore>(device->impl, pt.semaphore);
    return { &device->borrowedSparseSemaphore, pt.value };
}

uint64_t NRI_CALL nriGetTextureBytes(NriDevice device)
{
    return device && device->impl ? device->impl->getTextureBytes() : 0;
}

void NRI_CALL nriBeginFrame(NriDevice device, uint32_t frameNumber)
{
    if (device && device->impl)
        device->impl->beginFrame(frameNumber);
}

void NRI_CALL nriWaitIdle(NriDevice device)
{
    if (device && device->impl)
        device->impl->waitIdle();
}

void NRI_CALL nriFinishPipelineBuilds(NriDevice device)
{
    if (device && device->impl)
        device->impl->finishPipelineBuilds();
}

void NRI_CALL nriPrecompileShaderModules(NriDevice device, const char* const* sources, uint32_t sourceCount, NriShaderCompiler compiler)
{
    if (!device || !device->impl || !compiler || !compiler->impl || !sources || sourceCount == 0)
        return;
    std::vector<std::string> list;
    list.reserve(sourceCount);
    for (uint32_t i = 0; i < sourceCount; ++i)
    {
        if (sources[i])
            list.emplace_back(sources[i]);
    }
    device->impl->precompileShaderModules(list, *compiler->impl);
}

bool NRI_CALL nriCompileShaderModules(NriDevice device, const char* const* sources, uint32_t sourceCount, NriShaderCompiler compiler)
{
    if (!device || !device->impl || !compiler || !compiler->impl)
        return false;
    std::vector<std::string> list;
    if (sources && sourceCount > 0)
    {
        list.reserve(sourceCount);
        for (uint32_t i = 0; i < sourceCount; ++i)
        {
            if (sources[i])
                list.emplace_back(sources[i]);
        }
    }
    return device->impl->compileShaderModules(list, *compiler->impl);
}

bool NRI_CALL nriUpdateSparseBuffer(NriDevice device,
                                    NriBuffer buffer,
                                    const NriSparsePageRange* commits, uint32_t commitCount,
                                    const NriSparsePageRange* releases, uint32_t releaseCount)
{
    if (!device || !device->impl || !buffer || !buffer->impl)
        return false;
    std::vector<NRI::SparsePageRange> c(commitCount), r(releaseCount);
    for (uint32_t i = 0; i < commitCount; ++i)
        c[i] = { commits[i].firstPage, commits[i].pageCount };
    for (uint32_t i = 0; i < releaseCount; ++i)
        r[i] = { releases[i].firstPage, releases[i].pageCount };
    return device->impl->updateSparseBuffer(*buffer->impl, c, r);
}

void NRI_CALL nriGetMemoryStats(NriDevice device, NriMemoryHeapStats* outHeaps, uint32_t* inOutCount)
{
    if (!device || !device->impl || !inOutCount)
        return;
    std::vector<NRI::MemoryHeapStats> stats;
    device->impl->getMemoryStats(stats);
    const uint32_t available = static_cast<uint32_t>(stats.size());
    if (!outHeaps)
    {
        *inOutCount = available;
        return;
    }
    const uint32_t count = std::min(*inOutCount, available);
    for (uint32_t i = 0; i < count; ++i)
    {
        outHeaps[i] = NriMemoryHeapStats{
            .deviceLocal = stats[i].deviceLocal,
            .usage = stats[i].usage,
            .budget = stats[i].budget,
            .blockBytes = stats[i].blockBytes,
            .allocationBytes = stats[i].allocationBytes,
            .allocationCount = stats[i].allocationCount
        };
    }
    *inOutCount = count;
}

NriResult NRI_CALL nriSubmitAndWait(NriDevice device, NriCommandBuffer cmdBuffer, uint32_t slotIndex)
{
    if (!device || !device->impl || !cmdBuffer || !cmdBuffer->impl)
        return NRI_ERROR_INVALID_ARGUMENT;
    try
    {
        device->impl->submitAndWait(*cmdBuffer->impl, slotIndex);
        return NRI_SUCCESS;
    }
    catch (const std::exception& e)
    {
        return translateException(e);
    }
}

NriResult NRI_CALL nriSubmitCommandBuffers(NriDevice device,
                                           const NriCommandBuffer* cmdBuffers, uint32_t cmdBufferCount,
                                           NriSwapchain swapchain,
                                           uint32_t frameIndex, uint32_t imageIndex,
                                           const NriTimelinePoint* timelineWaits, uint32_t timelineWaitCount)
{
    if (!device || !device->impl || !swapchain || !swapchain->impl)
        return NRI_ERROR_INVALID_ARGUMENT;
    try
    {
        std::vector<NRI::CommandBuffer*> cmds;
        cmds.reserve(cmdBufferCount);
        for (uint32_t i = 0; i < cmdBufferCount; ++i)
        {
            if (cmdBuffers[i] && cmdBuffers[i]->impl)
                cmds.push_back(cmdBuffers[i]->impl.get());
        }
        std::vector<NRI::TimelinePoint> waits;
        waits.reserve(timelineWaitCount);
        for (uint32_t i = 0; i < timelineWaitCount; ++i)
        {
            if (timelineWaits[i].semaphore && timelineWaits[i].semaphore->impl)
                waits.push_back({ timelineWaits[i].semaphore->impl.get(), timelineWaits[i].value });
        }
        device->impl->submitCommandBuffers(cmds, *swapchain->impl, frameIndex, imageIndex, waits);
        return NRI_SUCCESS;
    }
    catch (const std::exception& e)
    {
        return translateException(e);
    }
}

NriResult NRI_CALL nriSubmit(NriDevice device,
                             NriQueueType queue,
                             const NriCommandBuffer* cmdBuffers, uint32_t cmdBufferCount,
                             const NriTimelinePoint* waits, uint32_t waitCount,
                             const NriTimelinePoint* signals, uint32_t signalCount)
{
    if (!device || !device->impl)
        return NRI_ERROR_INVALID_ARGUMENT;
    try
    {
        std::vector<NRI::CommandBuffer*> cmds;
        cmds.reserve(cmdBufferCount);
        for (uint32_t i = 0; i < cmdBufferCount; ++i)
        {
            if (cmdBuffers[i] && cmdBuffers[i]->impl)
                cmds.push_back(cmdBuffers[i]->impl.get());
        }
        std::vector<NRI::TimelinePoint> cppWaits, cppSignals;
        cppWaits.reserve(waitCount);
        for (uint32_t i = 0; i < waitCount; ++i)
        {
            if (waits[i].semaphore && waits[i].semaphore->impl)
                cppWaits.push_back({ waits[i].semaphore->impl.get(), waits[i].value });
        }
        cppSignals.reserve(signalCount);
        for (uint32_t i = 0; i < signalCount; ++i)
        {
            if (signals[i].semaphore && signals[i].semaphore->impl)
                cppSignals.push_back({ signals[i].semaphore->impl.get(), signals[i].value });
        }
        device->impl->submit(toCpp(queue), cmds, cppWaits, cppSignals);
        return NRI_SUCCESS;
    }
    catch (const std::exception& e)
    {
        return translateException(e);
    }
}

// -----------------------------------------------------------------------------
// Swapchain
// -----------------------------------------------------------------------------

NriResult NRI_CALL nriCreateSwapchain(NriDevice device, const NriSwapchainDesc* desc, NriSwapchain* outSwapchain)
{
    if (!device || !device->impl || !desc || !outSwapchain)
        return NRI_ERROR_INVALID_ARGUMENT;
    *outSwapchain = nullptr;
    try
    {
        NRI::SwapchainDesc cppDesc{};
        cppDesc.Width = desc->width;
        cppDesc.Height = desc->height;
        cppDesc.VSync = desc->vsync;
        cppDesc.Window = toCpp(desc->window);
        std::shared_ptr<NRI::Swapchain> impl = device->impl->createSwapchain(cppDesc);
        *outSwapchain = new NriSwapchain_T{ device->impl, std::move(impl) };
        return NRI_SUCCESS;
    }
    catch (const std::exception& e)
    {
        return translateException(e);
    }
}

void NRI_CALL nriDestroySwapchain(NriSwapchain swapchain)
{
    if (!swapchain)
        return;
    std::unique_ptr<NriSwapchain_T> owned(swapchain);
    if (owned->device && owned->impl)
    {
        // A Vulkan VkSurfaceKHR cannot host a new VkSwapchainKHR until the previous
        // VkSwapchainKHR is destroyed, so drain GPU work and destroy synchronously.
        owned->device->waitIdle();
        owned->device->collectDeferredDeletions();
        owned->impl.reset();
    }
}

NriExtent2D NRI_CALL nriGetSwapchainExtent(NriSwapchain swapchain)
{
    if (!swapchain || !swapchain->impl)
        return { 0, 0 };
    const NRI::Extent2D ext = swapchain->impl->getExtent();
    return { ext.width, ext.height };
}

NriFrameResult NRI_CALL nriAcquireNextSwapchainImage(NriSwapchain swapchain, uint32_t frameIndex, uint32_t* outImageIndex)
{
    if (!swapchain || !swapchain->impl || !outImageIndex)
        return NRI_FRAME_RESULT_RESIZE_REQUIRED;
    try
    {
        const NRI::FrameResult r = swapchain->impl->acquireNextImage(frameIndex, *outImageIndex);
        return r == NRI::FrameResult::Success ? NRI_FRAME_RESULT_SUCCESS : NRI_FRAME_RESULT_RESIZE_REQUIRED;
    }
    catch (const std::exception& e)
    {
        translateException(e);
        return NRI_FRAME_RESULT_RESIZE_REQUIRED;
    }
}

NriFrameResult NRI_CALL nriPresentSwapchain(NriSwapchain swapchain, uint32_t frameIndex, uint32_t imageIndex)
{
    if (!swapchain || !swapchain->impl)
        return NRI_FRAME_RESULT_RESIZE_REQUIRED;
    try
    {
        const NRI::FrameResult r = swapchain->impl->present(frameIndex, imageIndex);
        return r == NRI::FrameResult::Success ? NRI_FRAME_RESULT_SUCCESS : NRI_FRAME_RESULT_RESIZE_REQUIRED;
    }
    catch (const std::exception& e)
    {
        translateException(e);
        return NRI_FRAME_RESULT_RESIZE_REQUIRED;
    }
}

// -----------------------------------------------------------------------------
// CommandAllocator & CommandBuffer
// -----------------------------------------------------------------------------

NriResult NRI_CALL nriCreateCommandAllocator(NriDevice device, NriCommandBufferReset resetMode, NriQueueType queue, NriCommandAllocator* outAllocator)
{
    if (!device || !device->impl || !outAllocator)
        return NRI_ERROR_INVALID_ARGUMENT;
    *outAllocator = nullptr;
    try
    {
        std::shared_ptr<NRI::CommandAllocator> impl = device->impl->createCommandAllocator(static_cast<NRI::CommandBufferReset>(resetMode), toCpp(queue));
        *outAllocator = new NriCommandAllocator_T{ device->impl, std::move(impl) };
        return NRI_SUCCESS;
    }
    catch (const std::exception& e)
    {
        return translateException(e);
    }
}

void NRI_CALL nriDestroyCommandAllocator(NriCommandAllocator allocator)
{
    deferredRelease(allocator);
}

void NRI_CALL nriResetCommandAllocator(NriCommandAllocator allocator)
{
    if (allocator && allocator->impl)
        allocator->impl->reset();
}

NriResult NRI_CALL nriAllocateCommandBuffer(NriCommandAllocator allocator, uint32_t framesInFlight, NriCommandBuffer* outCommandBuffer)
{
    if (!allocator || !allocator->impl || !outCommandBuffer)
        return NRI_ERROR_INVALID_ARGUMENT;
    *outCommandBuffer = nullptr;
    try
    {
        std::shared_ptr<NRI::CommandBuffer> impl = allocator->impl->allocateCommandBuffer(framesInFlight);
        *outCommandBuffer = new NriCommandBuffer_T{ allocator->device, allocator->impl, std::move(impl) };
        return NRI_SUCCESS;
    }
    catch (const std::exception& e)
    {
        return translateException(e);
    }
}

void NRI_CALL nriDestroyCommandBuffer(NriCommandBuffer commandBuffer)
{
    if (!commandBuffer)
        return;
    std::unique_ptr<NriCommandBuffer_T> owned(commandBuffer);
    if (owned->device && owned->impl)
    {
        auto dev = owned->device;
        auto alloc = owned->allocator;
        auto impl = std::move(owned->impl);
        dev->enqueueDeferredDeletion([dev, alloc, impl]() mutable
        {
            impl.reset();
            alloc.reset();
        });
    }
}

void NRI_CALL nriCmdBegin(NriCommandBuffer cmd, uint32_t index, bool oneTimeSubmit)
{
    if (cmd && cmd->impl) cmd->impl->begin(index, oneTimeSubmit);
}

void NRI_CALL nriCmdEnd(NriCommandBuffer cmd, uint32_t index)
{
    if (cmd && cmd->impl) cmd->impl->end(index);
}

void NRI_CALL nriCmdBeginRendering(NriCommandBuffer cmd, const NriRenderDesc* desc)
{
    if (!cmd || !cmd->impl || !desc)
        return;
    NRI::RenderDesc cppDesc{};
    cppDesc.renderArea = { desc->renderArea.width, desc->renderArea.height };
    cppDesc.isSwapchainPass = desc->isSwapchainPass;
    cppDesc.colorAttachments.reserve(desc->colorAttachmentCount);
    for (uint32_t i = 0; i < desc->colorAttachmentCount; ++i)
    {
        const NriRenderAttachDesc& src = desc->colorAttachments[i];
        NRI::RenderAttachDesc dst{};
        dst.attachmentSwapchain = src.attachmentSwapchain ? src.attachmentSwapchain->impl.get() : nullptr;
        dst.attachment = src.attachment ? src.attachment->impl.get() : nullptr;
        dst.resolve = src.resolve ? src.resolve->impl.get() : nullptr;
        dst.resolveSwapchain = src.resolveSwapchain ? src.resolveSwapchain->impl.get() : nullptr;
        dst.resolveImageIndex = src.resolveImageIndex;
        dst.loadOP = toCpp(src.loadOp);
        dst.storeOP = toCpp(src.storeOp);
        dst.clearColor = { src.clearColor.r, src.clearColor.g, src.clearColor.b, src.clearColor.a };
        dst.clearDepth = { src.clearDepth.depth, src.clearDepth.stencil };
        cppDesc.colorAttachments.push_back(dst);
    }
    if (desc->hasDepthAttachment)
    {
        const NriRenderAttachDesc& src = desc->depthAttachment;
        cppDesc.depthAttachment.attachmentSwapchain = src.attachmentSwapchain ? src.attachmentSwapchain->impl.get() : nullptr;
        cppDesc.depthAttachment.attachment = src.attachment ? src.attachment->impl.get() : nullptr;
        cppDesc.depthAttachment.resolve = src.resolve ? src.resolve->impl.get() : nullptr;
        cppDesc.depthAttachment.resolveSwapchain = src.resolveSwapchain ? src.resolveSwapchain->impl.get() : nullptr;
        cppDesc.depthAttachment.resolveImageIndex = src.resolveImageIndex;
        cppDesc.depthAttachment.loadOP = toCpp(src.loadOp);
        cppDesc.depthAttachment.storeOP = toCpp(src.storeOp);
        cppDesc.depthAttachment.clearColor = { src.clearColor.r, src.clearColor.g, src.clearColor.b, src.clearColor.a };
        cppDesc.depthAttachment.clearDepth = { src.clearDepth.depth, src.clearDepth.stencil };
    }
    cmd->impl->beginRendering(cppDesc);
}

void NRI_CALL nriCmdEndRendering(NriCommandBuffer cmd)
{
    if (cmd && cmd->impl) cmd->impl->endRendering();
}

void NRI_CALL nriCmdBindPipeline(NriCommandBuffer cmd, NriPipelineBindPoint bindPoint, NriPipeline pipeline)
{
    if (cmd && cmd->impl && pipeline && pipeline->impl)
        cmd->impl->bindPipeline(static_cast<NRI::PipelineBindPoint>(bindPoint), *pipeline->impl);
}

void NRI_CALL nriCmdSetViewport(NriCommandBuffer cmd, NriExtent2D extent)
{
    if (cmd && cmd->impl) cmd->impl->setViewport({ extent.width, extent.height });
}

void NRI_CALL nriCmdSetScissor(NriCommandBuffer cmd, NriExtent2D extent)
{
    if (cmd && cmd->impl) cmd->impl->setScissor({ extent.width, extent.height });
}

void NRI_CALL nriCmdSetViewportWithCount(NriCommandBuffer cmd, const NriViewportBounds* bounds, float minDepth, float maxDepth)
{
    if (cmd && cmd->impl && bounds)
        cmd->impl->setViewportWithCount({ bounds->x, bounds->y, bounds->width, bounds->height }, minDepth, maxDepth);
}

void NRI_CALL nriCmdSetScissorWithCount(NriCommandBuffer cmd, const NriExtent2D* extent)
{
    if (cmd && cmd->impl && extent)
        cmd->impl->setScissorWithCount({ extent->width, extent->height });
}

void NRI_CALL nriCmdSetScissorRect(NriCommandBuffer cmd, int32_t x, int32_t y, uint32_t width, uint32_t height)
{
    if (cmd && cmd->impl) cmd->impl->setScissorRect(x, y, width, height);
}

void NRI_CALL nriCmdSetVertexInput(NriCommandBuffer cmd)
{
    if (cmd && cmd->impl) cmd->impl->setVertexInput();
}

void NRI_CALL nriCmdSetPrimitiveTopology(NriCommandBuffer cmd, NriPrimitiveTopology topology)
{
    if (cmd && cmd->impl) cmd->impl->setPrimitiveTopology(static_cast<NRI::PrimitiveTopology>(topology));
}

void NRI_CALL nriCmdSetPrimitiveRestartEnable(NriCommandBuffer cmd, bool enable)
{
    if (cmd && cmd->impl) cmd->impl->setPrimitiveRestartEnable(enable);
}

void NRI_CALL nriCmdSetRasterizerDiscardEnable(NriCommandBuffer cmd, bool enable)
{
    if (cmd && cmd->impl) cmd->impl->setRasterizerDiscardEnable(enable);
}

void NRI_CALL nriCmdSetPolygonMode(NriCommandBuffer cmd, NriPolygonMode polygon)
{
    if (cmd && cmd->impl) cmd->impl->setPolygonMode(static_cast<NRI::PolygonMode>(polygon));
}

void NRI_CALL nriCmdSetCullMode(NriCommandBuffer cmd, NriCullMode cullMode)
{
    if (cmd && cmd->impl) cmd->impl->setCullMode(static_cast<NRI::CullMode>(cullMode));
}

void NRI_CALL nriCmdSetFrontFace(NriCommandBuffer cmd, NriFrontFace frontFace)
{
    if (cmd && cmd->impl) cmd->impl->setFrontFace(static_cast<NRI::FrontFace>(frontFace));
}

void NRI_CALL nriCmdSetDepthBiasEnable(NriCommandBuffer cmd, bool enable)
{
    if (cmd && cmd->impl) cmd->impl->setDepthBiasEnable(enable);
}

void NRI_CALL nriCmdSetDepthBias(NriCommandBuffer cmd, float constantFactor, float clamp, float slopeFactor)
{
    if (cmd && cmd->impl) cmd->impl->setDepthBias(constantFactor, clamp, slopeFactor);
}

void NRI_CALL nriCmdSetDepthClampEnable(NriCommandBuffer cmd, bool enable)
{
    if (cmd && cmd->impl) cmd->impl->setDepthClampEnable(enable);
}

void NRI_CALL nriCmdSetLineWidth(NriCommandBuffer cmd, float lineWidth)
{
    if (cmd && cmd->impl) cmd->impl->setLineWidth(lineWidth);
}

void NRI_CALL nriCmdSetRasterizationSamples(NriCommandBuffer cmd, uint32_t sampleCount)
{
    if (cmd && cmd->impl) cmd->impl->setRasterizationSamples(sampleCount);
}

void NRI_CALL nriCmdSetSampleMask(NriCommandBuffer cmd, uint32_t sampleCount, uint32_t sampleMask)
{
    if (cmd && cmd->impl) cmd->impl->setSampleMask(sampleCount, sampleMask);
}

void NRI_CALL nriCmdSetAlphaToCoverageEnable(NriCommandBuffer cmd, bool enable)
{
    if (cmd && cmd->impl) cmd->impl->setAlphaToCoverageEnable(enable);
}

void NRI_CALL nriCmdSetAlphaToOneEnable(NriCommandBuffer cmd, bool enable)
{
    if (cmd && cmd->impl) cmd->impl->setAlphaToOneEnableEXT(enable);
}

void NRI_CALL nriCmdSetDepthTestEnable(NriCommandBuffer cmd, bool enable)
{
    if (cmd && cmd->impl) cmd->impl->setDepthTestEnable(enable);
}

void NRI_CALL nriCmdSetDepthWriteEnable(NriCommandBuffer cmd, bool enable)
{
    if (cmd && cmd->impl) cmd->impl->setDepthWriteEnable(enable);
}

void NRI_CALL nriCmdSetDepthCompareOp(NriCommandBuffer cmd, NriCompareOp compareOp)
{
    if (cmd && cmd->impl) cmd->impl->setDepthCompareOp(toCpp(compareOp));
}

void NRI_CALL nriCmdSetDepthBoundsTestEnable(NriCommandBuffer cmd, bool enable)
{
    if (cmd && cmd->impl) cmd->impl->setDepthBoundsTestEnable(enable);
}

void NRI_CALL nriCmdSetStencilTestEnable(NriCommandBuffer cmd, bool enable)
{
    if (cmd && cmd->impl) cmd->impl->setStencilTestEnable(enable);
}

void NRI_CALL nriCmdSetColorBlendEnable(NriCommandBuffer cmd, uint32_t firstAttachment, bool enable)
{
    if (cmd && cmd->impl) cmd->impl->setColorBlendEnable(firstAttachment, enable);
}

void NRI_CALL nriCmdSetColorBlendEquation(NriCommandBuffer cmd, uint32_t firstAttachment, const NriColorBlendEquation* blendEquation)
{
    if (cmd && cmd->impl && blendEquation)
    {
        NRI::ColorBlendEquation eq{
            static_cast<NRI::BlendFactor>(blendEquation->srcColorBlendFactor),
            static_cast<NRI::BlendFactor>(blendEquation->dstColorBlendFactor),
            static_cast<NRI::BlendOp>(blendEquation->colorBlendOp),
            static_cast<NRI::BlendFactor>(blendEquation->srcAlphaBlendFactor),
            static_cast<NRI::BlendFactor>(blendEquation->dstAlphaBlendFactor),
            static_cast<NRI::BlendOp>(blendEquation->alphaBlendOp)
        };
        cmd->impl->setColorBlendEquation(firstAttachment, eq);
    }
}

void NRI_CALL nriCmdSetColorWriteMask(NriCommandBuffer cmd, uint32_t firstAttachment, uint32_t colorWriteMask)
{
    if (cmd && cmd->impl) cmd->impl->setColorWriteMask(firstAttachment, colorWriteMask);
}

void NRI_CALL nriCmdSetLogicOpEnable(NriCommandBuffer cmd, bool enable)
{
    if (cmd && cmd->impl) cmd->impl->setLogicOpEnable(enable);
}

void NRI_CALL nriCmdBindDescriptorHeaps(NriCommandBuffer cmd, NriDescriptorHeap resourceHeap, NriDescriptorHeap samplerHeap)
{
    if (cmd && cmd->impl)
    {
        cmd->impl->bindDescriptorHeaps(
            resourceHeap ? resourceHeap->impl.get() : nullptr,
            samplerHeap ? samplerHeap->impl.get() : nullptr);
    }
}

void NRI_CALL nriCmdPushData(NriCommandBuffer cmd, const void* data, uint32_t size)
{
    if (cmd && cmd->impl) cmd->impl->pushData(data, size);
}

void NRI_CALL nriCmdBindVertexBuffer(NriCommandBuffer cmd, uint32_t firstBinding, NriBuffer buffer, uint64_t offset)
{
    if (cmd && cmd->impl && buffer && buffer->impl)
        cmd->impl->bindVertexBuffers(firstBinding, *buffer->impl, offset);
}

void NRI_CALL nriCmdBindIndexBuffer(NriCommandBuffer cmd, NriBuffer buffer, uint64_t offset)
{
    if (cmd && cmd->impl && buffer && buffer->impl)
        cmd->impl->bindIndexBuffer(*buffer->impl, offset);
}

void NRI_CALL nriCmdDraw(NriCommandBuffer cmd, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
{
    if (cmd && cmd->impl) cmd->impl->draw(vertexCount, instanceCount, firstVertex, firstInstance);
}

void NRI_CALL nriCmdDrawIndexed(NriCommandBuffer cmd, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
{
    if (cmd && cmd->impl) cmd->impl->drawIndexed(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void NRI_CALL nriCmdDispatch(NriCommandBuffer cmd, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
    if (cmd && cmd->impl) cmd->impl->dispatch(groupCountX, groupCountY, groupCountZ);
}

void NRI_CALL nriCmdDrawMeshTasks(NriCommandBuffer cmd, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
    if (cmd && cmd->impl) cmd->impl->drawMeshTasks(groupCountX, groupCountY, groupCountZ);
}

void NRI_CALL nriCmdDrawMeshTasksIndirect(NriCommandBuffer cmd, NriBuffer indirectBuffer, uint64_t offset, uint32_t drawCount, uint32_t stride)
{
    if (cmd && cmd->impl && indirectBuffer && indirectBuffer->impl)
        cmd->impl->drawMeshTasksIndirect(*indirectBuffer->impl, offset, drawCount, stride);
}

void NRI_CALL nriCmdDrawMeshTasksIndirectAddress(NriCommandBuffer cmd, uint64_t indirectBufferDeviceAddress, uint64_t offset, uint32_t drawCount, uint32_t stride)
{
    if (cmd && cmd->impl)
        cmd->impl->drawMeshTasksIndirect(indirectBufferDeviceAddress, offset, drawCount, stride);
}

void NRI_CALL nriCmdDrawMeshTasksIndirectCount(NriCommandBuffer cmd, NriBuffer indirectBuffer, uint64_t offset, NriBuffer countBuffer, uint64_t countOffset, uint32_t maxDrawCount, uint32_t stride)
{
    if (cmd && cmd->impl && indirectBuffer && indirectBuffer->impl && countBuffer && countBuffer->impl)
        cmd->impl->drawMeshTasksIndirectCount(*indirectBuffer->impl, offset, *countBuffer->impl, countOffset, maxDrawCount, stride);
}

void NRI_CALL nriCmdCopyBuffer(NriCommandBuffer cmd, NriBuffer srcBuffer, NriBuffer dstBuffer, const NriBufferCopyRegion* region)
{
    if (!cmd || !cmd->impl || !srcBuffer || !srcBuffer->impl || !dstBuffer || !dstBuffer->impl)
        return;
    NRI::BufferCopyRegion r{};
    if (region)
        r = { region->srcOffset, region->dstOffset, region->size };
    cmd->impl->copyBuffer(*srcBuffer->impl, *dstBuffer->impl, r);
}

void NRI_CALL nriCmdCopyBufferRegions(NriCommandBuffer cmd, NriBuffer srcBuffer, NriBuffer dstBuffer, const NriBufferCopyRegion* regions, uint32_t regionCount)
{
    if (!cmd || !cmd->impl || !srcBuffer || !srcBuffer->impl || !dstBuffer || !dstBuffer->impl || !regions || regionCount == 0)
        return;
    std::vector<NRI::BufferCopyRegion> cppRegions(regionCount);
    for (uint32_t i = 0; i < regionCount; ++i)
        cppRegions[i] = { regions[i].srcOffset, regions[i].dstOffset, regions[i].size };
    cmd->impl->copyBuffer(*srcBuffer->impl, *dstBuffer->impl, cppRegions);
}

void NRI_CALL nriCmdFillBuffer(NriCommandBuffer cmd, NriBuffer buffer, uint64_t offset, uint64_t size, uint32_t value)
{
    if (cmd && cmd->impl && buffer && buffer->impl)
        cmd->impl->fillBuffer(*buffer->impl, offset, size, value);
}

void NRI_CALL nriCmdTransitionTextureLayout(NriCommandBuffer cmd, NriTexture texture, NriTextureLayout oldLayout, NriTextureLayout newLayout)
{
    if (cmd && cmd->impl && texture && texture->impl)
        cmd->impl->transitionTextureLayout(*texture->impl, toCpp(oldLayout), toCpp(newLayout));
}

void NRI_CALL nriCmdTransitionSwapchainLayout(NriCommandBuffer cmd, NriSwapchain swapchain, uint32_t imageIndex, NriTextureLayout oldLayout, NriTextureLayout newLayout)
{
    if (cmd && cmd->impl && swapchain && swapchain->impl)
        cmd->impl->transitionSwapchainLayout(*swapchain->impl, imageIndex, toCpp(oldLayout), toCpp(newLayout));
}

void NRI_CALL nriCmdResolveImage(NriCommandBuffer cmd, NriTexture srcTexture, NriTexture dstTexture, uint32_t width, uint32_t height)
{
    if (cmd && cmd->impl && srcTexture && srcTexture->impl && dstTexture && dstTexture->impl)
        cmd->impl->resolveImage(*srcTexture->impl, *dstTexture->impl, width, height);
}

void NRI_CALL nriCmdCopyTexture(NriCommandBuffer cmd, NriTexture srcTexture, NriTexture dstTexture, uint32_t width, uint32_t height)
{
    if (cmd && cmd->impl && srcTexture && srcTexture->impl && dstTexture && dstTexture->impl)
        cmd->impl->copyTexture(*srcTexture->impl, *dstTexture->impl, width, height);
}

void NRI_CALL nriCmdCopyTextureMips(NriCommandBuffer cmd, NriTexture srcTexture, uint32_t srcFirstMip, NriTexture dstTexture, uint32_t dstFirstMip, uint32_t mipCount)
{
    if (cmd && cmd->impl && srcTexture && srcTexture->impl && dstTexture && dstTexture->impl)
        cmd->impl->copyTextureMips(*srcTexture->impl, srcFirstMip, *dstTexture->impl, dstFirstMip, mipCount);
}

void NRI_CALL nriCmdResetQueries(NriCommandBuffer cmd, NriQueryPool pool, uint32_t first, uint32_t count)
{
    if (cmd && cmd->impl && pool && pool->impl)
        cmd->impl->resetQueries(*pool->impl, first, count);
}

void NRI_CALL nriCmdExecutionBarrier(NriCommandBuffer cmd)
{
    if (cmd && cmd->impl) cmd->impl->executionBarrier();
}

void NRI_CALL nriCmdTransferBarrier(NriCommandBuffer cmd)
{
    if (cmd && cmd->impl) cmd->impl->transferBarrier();
}

void NRI_CALL nriCmdResourceBarriers(NriCommandBuffer cmd,
                                      const NriTextureBarrierDesc* textures, uint32_t textureCount,
                                      const NriBufferBarrierDesc* buffers, uint32_t bufferCount)
{
    if (!cmd || !cmd->impl)
        return;
    std::vector<NRI::TextureBarrierDesc> texVec;
    texVec.reserve(textureCount);
    for (uint32_t i = 0; i < textureCount; ++i)
    {
        texVec.push_back({
            textures[i].texture ? textures[i].texture->impl.get() : nullptr,
            { textures[i].before.access, textures[i].before.stages },
            { textures[i].after.access, textures[i].after.stages }
        });
    }
    std::vector<NRI::BufferBarrierDesc> bufVec;
    bufVec.reserve(bufferCount);
    for (uint32_t i = 0; i < bufferCount; ++i)
    {
        bufVec.push_back({
            buffers[i].buffer ? buffers[i].buffer->impl.get() : nullptr,
            { buffers[i].before.access, buffers[i].before.stages },
            { buffers[i].after.access, buffers[i].after.stages }
        });
    }
    cmd->impl->resourceBarriers(texVec, bufVec);
}

void NRI_CALL nriCmdBeginDebugLabel(NriCommandBuffer cmd, const char* label)
{
    if (cmd && cmd->impl) cmd->impl->beginDebugLabel(label ? label : "");
}

void NRI_CALL nriCmdEndDebugLabel(NriCommandBuffer cmd)
{
    if (cmd && cmd->impl) cmd->impl->endDebugLabel();
}

// -----------------------------------------------------------------------------
// Buffer
// -----------------------------------------------------------------------------

NriResult NRI_CALL nriCreateBuffer(NriDevice device, const NriBufferDesc* desc, NriBuffer* outBuffer)
{
    if (!device || !device->impl || !desc || !outBuffer)
        return NRI_ERROR_INVALID_ARGUMENT;
    *outBuffer = nullptr;
    try
    {
        NRI::BufferDesc cppDesc{};
        cppDesc.size = desc->size;
        cppDesc.usage = toCpp(desc->usage);
        cppDesc.sharedAcrossQueues = desc->sharedAcrossQueues;
        cppDesc.sparse = desc->sparse;
        std::shared_ptr<NRI::Buffer> impl = device->impl->createBuffer(cppDesc);
        *outBuffer = new NriBuffer_T{ device->impl, std::move(impl) };
        return NRI_SUCCESS;
    }
    catch (const std::exception& e)
    {
        return translateException(e);
    }
}

void NRI_CALL nriDestroyBuffer(NriBuffer buffer)
{
    deferredRelease(buffer);
}

void* NRI_CALL nriMapBuffer(NriBuffer buffer, uint64_t offset, uint64_t size)
{
    return buffer && buffer->impl ? buffer->impl->map(offset, size) : nullptr;
}

void NRI_CALL nriUnmapBuffer(NriBuffer buffer)
{
    if (buffer && buffer->impl) buffer->impl->unmap();
}

uint64_t NRI_CALL nriGetBufferDeviceAddress(NriBuffer buffer)
{
    return buffer && buffer->impl ? buffer->impl->getDeviceAddress() : 0;
}

uint64_t NRI_CALL nriGetBufferSize(NriBuffer buffer)
{
    return buffer && buffer->impl ? buffer->impl->getSize() : 0;
}

uint64_t NRI_CALL nriGetBufferSparsePageSize(NriBuffer buffer)
{
    return buffer && buffer->impl ? buffer->impl->getSparsePageSize() : 0;
}

// -----------------------------------------------------------------------------
// Texture
// -----------------------------------------------------------------------------

bool NRI_CALL nriImageFormatBlock(NriImageFormat format, uint32_t* outBlockBytes, uint32_t* outBlockSize)
{
    uint32_t bb = 4, bs = 1;
    const bool ok = NRI::imageFormatBlock(toCpp(format), bb, bs);
    if (outBlockBytes) *outBlockBytes = bb;
    if (outBlockSize) *outBlockSize = bs;
    return ok;
}

uint64_t NRI_CALL nriEstimateTextureBytes(const NriTextureDesc* desc)
{
    if (!desc) return 0;
    return NRI::estimateTextureBytes(toCpp(*desc));
}

NriResult NRI_CALL nriCreateTexture(NriDevice device, const NriTextureDesc* desc, NriTexture* outTexture)
{
    if (!device || !device->impl || !desc || !outTexture)
        return NRI_ERROR_INVALID_ARGUMENT;
    *outTexture = nullptr;
    try
    {
        std::shared_ptr<NRI::Texture2D> impl = device->impl->createTexture(toCpp(*desc));
        *outTexture = new NriTexture_T{ device->impl, std::move(impl) };
        return NRI_SUCCESS;
    }
    catch (const std::exception& e)
    {
        return translateException(e);
    }
}

void NRI_CALL nriDestroyTexture(NriTexture texture)
{
    deferredRelease(texture);
}

uint32_t NRI_CALL nriGetTextureWidth(NriTexture texture)
{
    return texture && texture->impl ? texture->impl->GetWidth() : 0;
}

uint32_t NRI_CALL nriGetTextureHeight(NriTexture texture)
{
    return texture && texture->impl ? texture->impl->GetHeight() : 0;
}

uint32_t NRI_CALL nriGetTextureMipLevels(NriTexture texture)
{
    return texture && texture->impl ? texture->impl->getMipLevels() : 0;
}

uint32_t NRI_CALL nriGetTextureArrayLayers(NriTexture texture)
{
    return texture && texture->impl ? texture->impl->getArrayLayers() : 0;
}

NriTextureUsage NRI_CALL nriGetTextureUsage(NriTexture texture)
{
    return texture && texture->impl ? toC(texture->impl->getUsage()) : NRI_TEXTURE_USAGE_DEFAULT;
}

uint32_t NRI_CALL nriGetTextureDescriptorSlot(NriTexture texture)
{
    return texture && texture->impl ? texture->impl->GetDescriptorIndexSlot() : ~0u;
}

void NRI_CALL nriTextureUploadFromBuffer(NriTexture texture, NriCommandBuffer cmdBuffer, NriBuffer stagingBuffer,
                                        uint32_t width, uint32_t height, uint32_t mipLevels,
                                        const size_t* mipOffsets, uint32_t mipOffsetCount)
{
    if (!texture || !texture->impl || !cmdBuffer || !cmdBuffer->impl || !stagingBuffer || !stagingBuffer->impl)
        return;
    std::vector<size_t> offsets(mipOffsets, mipOffsets + mipOffsetCount);
    texture->impl->uploadFromBuffer(*cmdBuffer->impl, *stagingBuffer->impl, width, height, mipLevels, offsets);
}

void NRI_CALL nriTextureRecordUpload(NriTexture texture, NriCommandBuffer cmdBuffer, NriBuffer stagingBuffer,
                                    uint64_t stagingOffset, const size_t* mipOffsets, uint32_t mipOffsetCount)
{
    if (!texture || !texture->impl || !cmdBuffer || !cmdBuffer->impl || !stagingBuffer || !stagingBuffer->impl)
        return;
    std::vector<size_t> offsets(mipOffsets, mipOffsets + mipOffsetCount);
    texture->impl->recordUpload(*cmdBuffer->impl, *stagingBuffer->impl, stagingOffset, offsets);
}

void NRI_CALL nriTextureRecordInitialLayout(NriTexture texture, NriCommandBuffer cmdBuffer)
{
    if (texture && texture->impl && cmdBuffer && cmdBuffer->impl)
        texture->impl->recordInitialLayout(*cmdBuffer->impl);
}

void NRI_CALL nriTextureRecordRegionUploads(NriTexture texture, NriCommandBuffer cmdBuffer, NriBuffer stagingBuffer,
                                           const NriTextureRegionUpload* regions, uint32_t regionCount)
{
    if (!texture || !texture->impl || !cmdBuffer || !cmdBuffer->impl || !stagingBuffer || !stagingBuffer->impl || !regions)
        return;
    std::vector<NRI::TextureRegionUpload> cppRegions(regionCount);
    for (uint32_t i = 0; i < regionCount; ++i)
        cppRegions[i] = { regions[i].stagingOffset, regions[i].x, regions[i].y, regions[i].width, regions[i].height, regions[i].rowPitch };
    texture->impl->recordRegionUploads(*cmdBuffer->impl, *stagingBuffer->impl, cppRegions);
}

void NRI_CALL nriTextureCopyToBuffer(NriTexture texture, NriCommandBuffer cmdBuffer, NriBuffer dstBuffer,
                                    uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    if (texture && texture->impl && cmdBuffer && cmdBuffer->impl && dstBuffer && dstBuffer->impl)
        texture->impl->copyImageToBuffer(*cmdBuffer->impl, *dstBuffer->impl, x, y, width, height);
}

void NRI_CALL nriTextureBlitTo(NriTexture srcTexture, NriCommandBuffer cmdBuffer, NriTexture dstTexture)
{
    if (srcTexture && srcTexture->impl && cmdBuffer && cmdBuffer->impl && dstTexture && dstTexture->impl)
        srcTexture->impl->blitTo(*cmdBuffer->impl, *dstTexture->impl);
}

void NRI_CALL nriTextureGenerateMipmaps(NriTexture texture, NriCommandBuffer cmdBuffer)
{
    if (texture && texture->impl && cmdBuffer && cmdBuffer->impl)
        texture->impl->generateMipmaps(*cmdBuffer->impl);
}

// -----------------------------------------------------------------------------
// DescriptorHeap
// -----------------------------------------------------------------------------

NriResult NRI_CALL nriCreateDescriptorHeap(NriDevice device, const NriDescriptorHeapDesc* desc, NriDescriptorHeap* outHeap)
{
    if (!device || !device->impl || !desc || !outHeap)
        return NRI_ERROR_INVALID_ARGUMENT;
    *outHeap = nullptr;
    try
    {
        NRI::DescriptorHeapDesc cppDesc{};
        cppDesc.type = static_cast<NRI::DescriptorHeapType>(desc->type);
        cppDesc.maxSamplerDescriptors = desc->maxSamplerDescriptors;
        cppDesc.maxBufferDescriptors = desc->maxBufferDescriptors;
        cppDesc.maxImageDescriptors = desc->maxImageDescriptors;
        if (desc->samplers && desc->samplerCount > 0)
        {
            cppDesc.samplers.reserve(desc->samplerCount);
            for (uint32_t i = 0; i < desc->samplerCount; ++i)
            {
                const auto& s = desc->samplers[i];
                cppDesc.samplers.push_back(NRI::SamplerDesc{
                    .magFilter = static_cast<NRI::Filter>(s.magFilter),
                    .minFilter = static_cast<NRI::Filter>(s.minFilter),
                    .mipmapMode = static_cast<NRI::SamplerMipmapMode>(s.mipmapMode),
                    .addressModeU = static_cast<NRI::SamplerAddressMode>(s.addressModeU),
                    .addressModeV = static_cast<NRI::SamplerAddressMode>(s.addressModeV),
                    .addressModeW = static_cast<NRI::SamplerAddressMode>(s.addressModeW),
                    .mipLodBias = s.mipLodBias,
                    .anisotropyEnable = s.anisotropyEnable,
                    .maxAnisotropy = s.maxAnisotropy,
                    .compareEnable = s.compareEnable,
                    .compareOp = toCpp(s.compareOp),
                    .minLod = s.minLod,
                    .maxLod = s.maxLod,
                    .borderColor = static_cast<NRI::BorderColor>(s.borderColor)
                });
            }
        }
        std::shared_ptr<NRI::DescriptorHeap> impl = device->impl->createDescriptorHeap(cppDesc);
        *outHeap = new NriDescriptorHeap_T{ device->impl, std::move(impl) };
        return NRI_SUCCESS;
    }
    catch (const std::exception& e)
    {
        return translateException(e);
    }
}

void NRI_CALL nriDestroyDescriptorHeap(NriDescriptorHeap heap)
{
    deferredRelease(heap);
}

void NRI_CALL nriRegisterTexture(NriDescriptorHeap heap, NriTexture texture, NriTextureUsage usageOverride)
{
    if (heap && heap->impl && texture && texture->impl)
        heap->impl->registerTexture(*texture->impl, toCpp(usageOverride));
}

uint32_t NRI_CALL nriRegisterStorageTextureMip(NriDescriptorHeap heap, NriTexture texture, uint32_t mipLevel)
{
    return (heap && heap->impl && texture && texture->impl)
        ? heap->impl->registerStorageTextureMip(*texture->impl, mipLevel)
        : ~0u;
}

void NRI_CALL nriUnregisterTexture(NriDescriptorHeap heap, uint32_t slot)
{
    if (heap && heap->impl) heap->impl->unregisterTexture(slot);
}

uint32_t NRI_CALL nriRegisterBuffer(NriDescriptorHeap heap, NriBuffer buffer, uint64_t size)
{
    return (heap && heap->impl && buffer && buffer->impl)
        ? heap->impl->registerBuffer(*buffer->impl, size)
        : ~0u;
}

uint32_t NRI_CALL nriGetDescriptorHeapImageIndexOffset(NriDescriptorHeap heap)
{
    return heap && heap->impl ? heap->impl->getImageHeapIndexOffset() : 0;
}

uint64_t NRI_CALL nriGetDescriptorHeapSize(NriDescriptorHeap heap)
{
    return heap && heap->impl ? heap->impl->getSize() : 0;
}

uint64_t NRI_CALL nriGetDescriptorHeapReservedRangeOffset(NriDescriptorHeap heap)
{
    return heap && heap->impl ? heap->impl->getReservedRangeOffset() : 0;
}

uint64_t NRI_CALL nriGetDescriptorHeapReservedRangeSize(NriDescriptorHeap heap)
{
    return heap && heap->impl ? heap->impl->getReservedRangeSize() : 0;
}

uint64_t NRI_CALL nriGetDescriptorHeapGPUAddress(NriDescriptorHeap heap)
{
    return heap && heap->impl ? heap->impl->getGPUAddress() : 0;
}

// -----------------------------------------------------------------------------
// QueryPool
// -----------------------------------------------------------------------------

NriResult NRI_CALL nriCreateQueryPool(NriDevice device, const NriQueryPoolDesc* desc, NriQueryPool* outPool)
{
    if (!device || !device->impl || !desc || !outPool)
        return NRI_ERROR_INVALID_ARGUMENT;
    *outPool = nullptr;
    try
    {
        NRI::QueryPoolDesc cppDesc{ static_cast<NRI::QueryType>(desc->type), desc->capacity };
        std::shared_ptr<NRI::QueryPool> impl = device->impl->createQueryPool(cppDesc);
        *outPool = new NriQueryPool_T{ device->impl, std::move(impl) };
        return NRI_SUCCESS;
    }
    catch (const std::exception& e)
    {
        return translateException(e);
    }
}

void NRI_CALL nriDestroyQueryPool(NriQueryPool pool)
{
    deferredRelease(pool);
}

uint32_t NRI_CALL nriGetQueryPoolCapacity(NriQueryPool pool)
{
    return pool && pool->impl ? pool->impl->getCapacity() : 0;
}

bool NRI_CALL nriGetQueryPoolResults(NriQueryPool pool, uint32_t first, uint64_t* outValues, uint32_t count)
{
    if (!pool || !pool->impl || !outValues)
        return false;
    return pool->impl->getResults(first, std::span<uint64_t>(outValues, count));
}

// -----------------------------------------------------------------------------
// Pipeline & TimelineSemaphore
// -----------------------------------------------------------------------------

NriResult NRI_CALL nriCreatePipeline(NriDevice device, const NriPipelineDesc* desc, NriShaderCompiler compiler, NriPipeline* outPipeline)
{
    if (!device || !device->impl || !desc || !outPipeline)
        return NRI_ERROR_INVALID_ARGUMENT;
    const bool isNoneApi = (device->impl->getAPI() == NRI::GraphicsAPI::None);
    if (!isNoneApi && (!compiler || !compiler->impl))
        return NRI_ERROR_INVALID_ARGUMENT;
    *outPipeline = nullptr;
    try
    {
        NRI::PipelineDesc cppDesc{};
        cppDesc.type = static_cast<NRI::PipelineType>(desc->type);
        cppDesc.maxRecursionDepth = desc->maxRecursionDepth ? desc->maxRecursionDepth : 1;
        cppDesc.forceCompile = desc->forceCompile;
        cppDesc.buildNow = desc->buildNow;
        if (desc->shaders && desc->shaderCount > 0)
        {
            cppDesc.shaders.reserve(desc->shaderCount);
            for (uint32_t i = 0; i < desc->shaderCount; ++i)
            {
                cppDesc.shaders.push_back(NRI::ShaderStageDesc{
                    static_cast<NRI::ShaderStage>(desc->shaders[i].stage),
                    desc->shaders[i].entryPoint ? desc->shaders[i].entryPoint : "",
                    desc->shaders[i].sourcePath ? desc->shaders[i].sourcePath : ""
                });
            }
        }
        if (desc->colorFormats && desc->colorFormatCount > 0)
        {
            cppDesc.colorFormats.reserve(desc->colorFormatCount);
            for (uint32_t i = 0; i < desc->colorFormatCount; ++i)
                cppDesc.colorFormats.push_back(toCpp(desc->colorFormats[i]));
        }
        if (desc->hitGroups && desc->hitGroupCount > 0)
        {
            cppDesc.hitGroups.reserve(desc->hitGroupCount);
            for (uint32_t i = 0; i < desc->hitGroupCount; ++i)
            {
                cppDesc.hitGroups.push_back({
                    desc->hitGroups[i].closestHitShaderIndex,
                    desc->hitGroups[i].anyHitShaderIndex,
                    desc->hitGroups[i].intersectionShaderIndex
                });
            }
        }
        std::shared_ptr<NRI::Pipeline> impl = (compiler && compiler->impl)
            ? device->impl->createPipeline(cppDesc, *compiler->impl)
            : device->impl->createPipelineNoCompiler(cppDesc);
        *outPipeline = new NriPipeline_T{ device->impl, std::move(impl) };
        return NRI_SUCCESS;
    }
    catch (const std::exception& e)
    {
        return translateException(e);
    }
}

void NRI_CALL nriDestroyPipeline(NriPipeline pipeline)
{
    deferredRelease(pipeline);
}

NriResult NRI_CALL nriCreateTimelineSemaphore(NriDevice device, uint64_t initialValue, NriTimelineSemaphore* outSemaphore)
{
    if (!device || !device->impl || !outSemaphore)
        return NRI_ERROR_INVALID_ARGUMENT;
    *outSemaphore = nullptr;
    try
    {
        std::shared_ptr<NRI::TimelineSemaphore> impl = device->impl->createTimelineSemaphore(initialValue);
        *outSemaphore = new NriTimelineSemaphore_T{ device->impl, std::move(impl) };
        return NRI_SUCCESS;
    }
    catch (const std::exception& e)
    {
        return translateException(e);
    }
}

void NRI_CALL nriDestroyTimelineSemaphore(NriTimelineSemaphore semaphore)
{
    deferredRelease(semaphore);
}

uint64_t NRI_CALL nriGetTimelineSemaphoreValue(NriTimelineSemaphore semaphore)
{
    return semaphore && semaphore->impl ? semaphore->impl->getValue() : 0;
}

void NRI_CALL nriWaitTimelineSemaphore(NriTimelineSemaphore semaphore, uint64_t value)
{
    if (semaphore && semaphore->impl) semaphore->impl->wait(value);
}

// -----------------------------------------------------------------------------
// Ray Tracing Extension
// -----------------------------------------------------------------------------

NriAccelerationStructureBuildSizes NRI_CALL nriGetAccelerationStructureBuildSizes(NriDevice device, const NriAccelerationStructureBuildDesc* desc)
{
    if (!device || !device->impl || !desc)
        return {};
    std::vector<NRI::AccelerationStructureTrianglesDesc> tris;
    const auto cppDesc = toCpp(*desc, tris);
    const auto sizes = device->impl->getAccelerationStructureBuildSizes(cppDesc);
    return { sizes.accelerationStructureSize, sizes.buildScratchSize, sizes.updateScratchSize };
}

NriResult NRI_CALL nriCreateAccelerationStructure(NriDevice device, const NriAccelerationStructureDesc* desc, NriAccelerationStructure* outAS)
{
    if (!device || !device->impl || !desc || !outAS)
        return NRI_ERROR_INVALID_ARGUMENT;
    *outAS = nullptr;
    try
    {
        NRI::AccelerationStructureDesc cppDesc{};
        cppDesc.type = static_cast<NRI::AccelerationStructureType>(desc->type);
        cppDesc.storageBuffer = desc->storageBuffer ? desc->storageBuffer->impl.get() : nullptr;
        cppDesc.bufferOffset = desc->bufferOffset;
        cppDesc.size = desc->size;
        std::shared_ptr<NRI::AccelerationStructure> impl = device->impl->createAccelerationStructure(cppDesc);
        *outAS = new NriAccelerationStructure_T{ device->impl, std::move(impl) };
        return NRI_SUCCESS;
    }
    catch (const std::exception& e)
    {
        return translateException(e);
    }
}

void NRI_CALL nriDestroyAccelerationStructure(NriAccelerationStructure as)
{
    deferredRelease(as);
}

uint64_t NRI_CALL nriGetAccelerationStructureDeviceAddress(NriAccelerationStructure as)
{
    return as && as->impl ? as->impl->getDeviceAddress() : 0;
}

uint64_t NRI_CALL nriGetAccelerationStructureSize(NriAccelerationStructure as)
{
    return as && as->impl ? as->impl->getSize() : 0;
}

NriAccelerationStructureType NRI_CALL nriGetAccelerationStructureType(NriAccelerationStructure as)
{
    return as && as->impl ? static_cast<NriAccelerationStructureType>(as->impl->getType()) : NRI_ACCELERATION_STRUCTURE_BOTTOM_LEVEL;
}

uint32_t NRI_CALL nriRegisterAccelerationStructure(NriDescriptorHeap heap, NriAccelerationStructure as, uint32_t slot)
{
    return (heap && heap->impl && as && as->impl)
        ? heap->impl->registerAccelerationStructure(*as->impl, slot)
        : ~0u;
}

void NRI_CALL nriCmdBuildAccelerationStructure(NriCommandBuffer cmd, const NriAccelerationStructureBuildDesc* buildDesc, uint64_t scratchAddress, NriAccelerationStructure dstAS)
{
    if (!cmd || !cmd->impl || !buildDesc || !dstAS || !dstAS->impl)
        return;
    std::vector<NRI::AccelerationStructureTrianglesDesc> tris;
    cmd->impl->buildAccelerationStructure(toCpp(*buildDesc, tris), scratchAddress, *dstAS->impl);
}

void NRI_CALL nriCmdUpdateAccelerationStructure(NriCommandBuffer cmd, const NriAccelerationStructureBuildDesc* buildDesc, uint64_t scratchAddress, NriAccelerationStructure srcAS, NriAccelerationStructure dstAS)
{
    if (!cmd || !cmd->impl || !buildDesc || !srcAS || !srcAS->impl || !dstAS || !dstAS->impl)
        return;
    std::vector<NRI::AccelerationStructureTrianglesDesc> tris;
    cmd->impl->updateAccelerationStructure(toCpp(*buildDesc, tris), scratchAddress, *srcAS->impl, *dstAS->impl);
}

void NRI_CALL nriCmdAccelerationStructureBarrier(NriCommandBuffer cmd, NriAccelerationStructureBarrierType barrierType)
{
    if (cmd && cmd->impl)
        cmd->impl->accelerationStructureBarrier(static_cast<NRI::AccelerationStructureBarrierType>(barrierType));
}

void NRI_CALL nriCmdWriteCompactedSizes(NriCommandBuffer cmd, const NriAccelerationStructure* structures, uint32_t structureCount, NriQueryPool pool, uint32_t firstQuery)
{
    if (!cmd || !cmd->impl || !pool || !pool->impl || !structures)
        return;
    std::vector<NRI::AccelerationStructure*> vec;
    vec.reserve(structureCount);
    for (uint32_t i = 0; i < structureCount; ++i)
    {
        if (structures[i] && structures[i]->impl)
            vec.push_back(structures[i]->impl.get());
    }
    cmd->impl->writeCompactedSizes(vec, *pool->impl, firstQuery);
}

void NRI_CALL nriCmdCopyAccelerationStructure(NriCommandBuffer cmd, NriAccelerationStructure src, NriAccelerationStructure dst, bool compact)
{
    if (cmd && cmd->impl && src && src->impl && dst && dst->impl)
        cmd->impl->copyAccelerationStructure(*src->impl, *dst->impl, compact);
}

void NRI_CALL nriCmdTraceRays(NriCommandBuffer cmd,
                              const NriStridedDeviceAddressRegion* rayGen,
                              const NriStridedDeviceAddressRegion* miss,
                              const NriStridedDeviceAddressRegion* hitGroup,
                              const NriStridedDeviceAddressRegion* callable,
                              uint32_t width, uint32_t height, uint32_t depth)
{
    if (!cmd || !cmd->impl)
        return;
    auto conv = [](const NriStridedDeviceAddressRegion* r) -> NRI::StridedDeviceAddressRegion
    {
        return r ? NRI::StridedDeviceAddressRegion{ r->deviceAddress, r->stride, r->size } : NRI::StridedDeviceAddressRegion{};
    };
    cmd->impl->traceRays(conv(rayGen), conv(miss), conv(hitGroup), conv(callable), width, height, depth);
}

uint32_t NRI_CALL nriGetDeviceShaderGroupHandleSize(NriDevice device)
{
    return device && device->impl ? device->impl->getShaderGroupHandleSize() : 32;
}

uint32_t NRI_CALL nriGetDeviceShaderGroupBaseAlignment(NriDevice device)
{
    return device && device->impl ? device->impl->getShaderGroupBaseAlignment() : 64;
}

const uint8_t* NRI_CALL nriGetPipelineShaderGroupHandles(NriPipeline pipeline, uint32_t* outByteSize)
{
    if (!pipeline || !pipeline->impl)
    {
        if (outByteSize) *outByteSize = 0;
        return nullptr;
    }
    const auto& vec = pipeline->impl->getShaderGroupHandles();
    if (outByteSize) *outByteSize = static_cast<uint32_t>(vec.size());
    return vec.data();
}

uint32_t NRI_CALL nriGetPipelineShaderGroupHandleSize(NriPipeline pipeline)
{
    return pipeline && pipeline->impl ? pipeline->impl->getShaderGroupHandleSize() : 0;
}

uint32_t NRI_CALL nriGetPipelineShaderGroupBaseAlignment(NriPipeline pipeline)
{
    return pipeline && pipeline->impl ? pipeline->impl->getShaderGroupBaseAlignment() : 0;
}

uint32_t NRI_CALL nriGetPipelineRayGenGroupIndex(NriPipeline pipeline)
{
    return pipeline && pipeline->impl ? pipeline->impl->getRayGenShaderGroupIndex() : 0;
}

const uint32_t* NRI_CALL nriGetPipelineMissGroupIndices(NriPipeline pipeline, uint32_t* outCount)
{
    if (!pipeline || !pipeline->impl)
    {
        if (outCount) *outCount = 0;
        return nullptr;
    }
    const auto& vec = pipeline->impl->getMissShaderGroupIndices();
    if (outCount) *outCount = static_cast<uint32_t>(vec.size());
    return vec.data();
}

const uint32_t* NRI_CALL nriGetPipelineHitGroupIndices(NriPipeline pipeline, uint32_t* outCount)
{
    if (!pipeline || !pipeline->impl)
    {
        if (outCount) *outCount = 0;
        return nullptr;
    }
    const auto& vec = pipeline->impl->getHitShaderGroupIndices();
    if (outCount) *outCount = static_cast<uint32_t>(vec.size());
    return vec.data();
}

const uint32_t* NRI_CALL nriGetPipelineCallableGroupIndices(NriPipeline pipeline, uint32_t* outCount)
{
    if (!pipeline || !pipeline->impl)
    {
        if (outCount) *outCount = 0;
        return nullptr;
    }
    const auto& vec = pipeline->impl->getCallableShaderGroupIndices();
    if (outCount) *outCount = static_cast<uint32_t>(vec.size());
    return vec.data();
}

uint64_t NRI_CALL nriCalculateShaderTableSize(uint32_t numRayGen, uint32_t numMiss, uint32_t numHit, uint32_t numCallable, uint32_t baseAlignment)
{
    return NRI::calculateShaderTableSize(numRayGen, numMiss, numHit, numCallable, baseAlignment);
}

void NRI_CALL nriBakeShaderTable(uint8_t* uploadCpuVA, uint64_t uploadGpuVA,
                                 const uint8_t* shaderGroupHandles, uint32_t handleSize, uint32_t baseAlignment,
                                 int32_t rayGenShaderGroupIndex,
                                 const uint32_t* missIndices, uint32_t missCount,
                                 const uint32_t* hitIndices, uint32_t hitCount,
                                 const uint32_t* callableIndices, uint32_t callableCount,
                                 NriShaderTableState* outState)
{
    if (!outState) return;
    NRI::ShaderTableState state{};
    NRI::bakeShaderTable(
        uploadCpuVA, uploadGpuVA, shaderGroupHandles, handleSize, baseAlignment, rayGenShaderGroupIndex,
        std::span<const uint32_t>(missIndices, missCount),
        std::span<const uint32_t>(hitIndices, hitCount),
        std::span<const uint32_t>(callableIndices, callableCount),
        state);
    outState->rayGen = { state.rayGen.deviceAddress, state.rayGen.stride, state.rayGen.size };
    outState->miss = { state.miss.deviceAddress, state.miss.stride, state.miss.size };
    outState->hitGroups = { state.hitGroups.deviceAddress, state.hitGroups.stride, state.hitGroups.size };
    outState->callable = { state.callable.deviceAddress, state.callable.stride, state.callable.size };
}

// -----------------------------------------------------------------------------
// GpuProfiler Extension
// -----------------------------------------------------------------------------

NriResult NRI_CALL nriCreateGpuProfiler(NriDevice device, uint32_t framesInFlight, NriGpuProfiler* outProfiler)
{
    if (!device || !device->impl || !outProfiler)
        return NRI_ERROR_INVALID_ARGUMENT;
    *outProfiler = nullptr;
    try
    {
        std::shared_ptr<NRI::GpuProfiler> impl = device->impl->createGpuProfiler(framesInFlight);
        *outProfiler = new NriGpuProfiler_T{ device->impl, std::move(impl) };
        return NRI_SUCCESS;
    }
    catch (const std::exception& e)
    {
        return translateException(e);
    }
}

void NRI_CALL nriDestroyGpuProfiler(NriGpuProfiler profiler)
{
    deferredRelease(profiler);
}

bool NRI_CALL nriGpuProfilerIsSupported(NriGpuProfiler profiler)
{
    return profiler && profiler->impl ? profiler->impl->isSupported() : false;
}

bool NRI_CALL nriGpuProfilerIsComputeQueueSupported(NriGpuProfiler profiler)
{
    return profiler && profiler->impl ? profiler->impl->isComputeQueueSupported() : false;
}

float NRI_CALL nriGpuProfilerGetTimestampPeriod(NriGpuProfiler profiler)
{
    return profiler && profiler->impl ? profiler->impl->getTimestampPeriod() : 0.0f;
}

uint32_t NRI_CALL nriGpuProfilerGetTimestampValidBits(NriGpuProfiler profiler)
{
    return profiler && profiler->impl ? profiler->impl->getTimestampValidBits() : 0;
}

bool NRI_CALL nriGpuProfilerCalibrate(NriGpuProfiler profiler, uint64_t* outGpuTicks, int64_t* outHostNanoseconds)
{
    if (!profiler || !profiler->impl || !outGpuTicks || !outHostNanoseconds)
        return false;
    return profiler->impl->calibrate(*outGpuTicks, *outHostNanoseconds);
}

void NRI_CALL nriGpuProfilerBeginFrame(NriGpuProfiler profiler, uint32_t frameSlot)
{
    if (profiler && profiler->impl) profiler->impl->beginFrame(frameSlot);
}

const NriGpuTimestamp* NRI_CALL nriGpuProfilerGetReadbackTimestamps(NriGpuProfiler profiler, uint32_t* outCount)
{
    if (!profiler || !profiler->impl)
    {
        if (outCount) *outCount = 0;
        return nullptr;
    }
    const auto span = profiler->impl->getReadbackTimestamps();
    if (outCount) *outCount = static_cast<uint32_t>(span.size());
    static_assert(sizeof(NriGpuTimestamp) == sizeof(NRI::GpuTimestamp));
    return reinterpret_cast<const NriGpuTimestamp*>(span.data());
}

void NRI_CALL nriGpuProfilerResetQueries(NriGpuProfiler profiler, NriCommandBuffer cmd)
{
    if (profiler && profiler->impl && cmd && cmd->impl)
        profiler->impl->resetQueries(*cmd->impl);
}

uint32_t NRI_CALL nriGpuProfilerAllocateQueryPair(NriGpuProfiler profiler)
{
    return profiler && profiler->impl ? profiler->impl->allocateQueryPair() : NRI_GPU_PROFILER_INVALID_QUERY_ID;
}

void NRI_CALL nriGpuProfilerWriteTimestamp(NriGpuProfiler profiler, NriCommandBuffer cmd, uint32_t queryId)
{
    if (profiler && profiler->impl && cmd && cmd->impl)
        profiler->impl->writeTimestamp(*cmd->impl, queryId);
}

bool NRI_CALL nriGpuProfilerIsPipelineStatisticsSupported(NriGpuProfiler profiler)
{
    return profiler && profiler->impl ? profiler->impl->isPipelineStatisticsSupported() : false;
}

uint32_t NRI_CALL nriGpuProfilerAllocateStatisticsQuery(NriGpuProfiler profiler)
{
    return profiler && profiler->impl ? profiler->impl->allocateStatisticsQuery() : NRI_GPU_PROFILER_INVALID_QUERY_ID;
}

void NRI_CALL nriGpuProfilerBeginStatistics(NriGpuProfiler profiler, NriCommandBuffer cmd, uint32_t queryId)
{
    if (profiler && profiler->impl && cmd && cmd->impl)
        profiler->impl->beginStatistics(*cmd->impl, queryId);
}

void NRI_CALL nriGpuProfilerEndStatistics(NriGpuProfiler profiler, NriCommandBuffer cmd, uint32_t queryId)
{
    if (profiler && profiler->impl && cmd && cmd->impl)
        profiler->impl->endStatistics(*cmd->impl, queryId);
}

const NriGpuPipelineStatistics* NRI_CALL nriGpuProfilerGetReadbackStatistics(NriGpuProfiler profiler, uint32_t* outCount)
{
    if (!profiler || !profiler->impl)
    {
        if (outCount) *outCount = 0;
        return nullptr;
    }
    const auto span = profiler->impl->getReadbackStatistics();
    if (outCount) *outCount = static_cast<uint32_t>(span.size());
    static_assert(sizeof(NriGpuPipelineStatistics) == sizeof(NRI::GpuPipelineStatistics));
    return reinterpret_cast<const NriGpuPipelineStatistics*>(span.data());
}

bool NRI_CALL nriGpuProfilerIsTransferQueueSupported(NriGpuProfiler profiler)
{
    return profiler && profiler->impl ? profiler->impl->isTransferQueueSupported() : false;
}

uint32_t NRI_CALL nriGpuProfilerBeginTransferQuery(NriGpuProfiler profiler, NriCommandBuffer cmd)
{
    return (profiler && profiler->impl && cmd && cmd->impl)
        ? profiler->impl->beginTransferQuery(*cmd->impl)
        : NRI_GPU_PROFILER_INVALID_QUERY_ID;
}

void NRI_CALL nriGpuProfilerEndTransferQuery(NriGpuProfiler profiler, NriCommandBuffer cmd, uint32_t queryId)
{
    if (profiler && profiler->impl && cmd && cmd->impl)
        profiler->impl->endTransferQuery(*cmd->impl, queryId);
}

bool NRI_CALL nriGpuProfilerReadTransferQuery(NriGpuProfiler profiler, uint32_t queryId, uint64_t* outBeginTicks, uint64_t* outEndTicks)
{
    if (!profiler || !profiler->impl || !outBeginTicks || !outEndTicks)
        return false;
    return profiler->impl->readTransferQuery(queryId, *outBeginTicks, *outEndTicks);
}

// -----------------------------------------------------------------------------
// ShaderCompiler Extension
// -----------------------------------------------------------------------------

NriResult NRI_CALL nriCreateSlangCompiler(NriShaderCompiler* outCompiler)
{
    if (!outCompiler)
        return NRI_ERROR_INVALID_ARGUMENT;
    *outCompiler = nullptr;
#ifdef NRI_ENABLE_SLANG
    try
    {
        auto impl = NRI::CreateSlangCompiler();
        *outCompiler = new NriShaderCompiler_T{ std::move(impl) };
        return NRI_SUCCESS;
    }
    catch (const std::exception& e)
    {
        return translateException(e);
    }
#else
    NRI::SetLastError("Slang shader compiler is disabled in this NoxRHI build.");
    return NRI_ERROR_UNSUPPORTED;
#endif
}

void NRI_CALL nriDestroyShaderCompiler(NriShaderCompiler compiler)
{
    delete compiler;
}

void NRI_CALL nriShaderCompilerSetTargetAPI(NriShaderCompiler compiler, NriGraphicsAPI api)
{
    if (compiler && compiler->impl) compiler->impl->setTargetAPI(toCpp(api));
}

NriGraphicsAPI NRI_CALL nriShaderCompilerGetTargetAPI(NriShaderCompiler compiler)
{
    return compiler && compiler->impl ? toC(compiler->impl->getTargetAPI()) : NRI_GRAPHICS_API_VULKAN;
}

void NRI_CALL nriShaderCompilerSetSearchPaths(NriShaderCompiler compiler, const char* const* paths, uint32_t pathCount)
{
    if (!compiler || !compiler->impl)
        return;
    std::vector<std::string> vec;
    if (paths && pathCount > 0)
    {
        vec.reserve(pathCount);
        for (uint32_t i = 0; i < pathCount; ++i)
        {
            if (paths[i]) vec.emplace_back(paths[i]);
        }
    }
    compiler->impl->setSearchPaths(vec);
}

void NRI_CALL nriShaderCompilerAddSearchPath(NriShaderCompiler compiler, const char* path)
{
    if (compiler && compiler->impl && path)
        compiler->impl->addSearchPath(path);
}

bool NRI_CALL nriShaderCompilerCanCompile(NriShaderCompiler compiler)
{
    return compiler && compiler->impl ? compiler->impl->canCompile() : false;
}

uint64_t NRI_CALL nriShaderCompilerSourceHash(NriShaderCompiler compiler, const char* path)
{
    return (compiler && compiler->impl && path) ? compiler->impl->sourceHash(path) : 0;
}

NriResult NRI_CALL nriShaderCompilerCompile(NriShaderCompiler compiler, const char* path, char** outBytes, size_t* outSize)
{
    if (!compiler || !compiler->impl || !path || !outBytes || !outSize)
        return NRI_ERROR_INVALID_ARGUMENT;
    *outBytes = nullptr;
    *outSize = 0;
    try
    {
        std::vector<char> bytes = compiler->impl->compile(path);
        *outSize = bytes.size();
        if (!bytes.empty())
        {
            *outBytes = static_cast<char*>(std::malloc(bytes.size()));
            if (!*outBytes)
                return NRI_ERROR_OUT_OF_MEMORY;
            std::memcpy(*outBytes, bytes.data(), bytes.size());
        }
        return NRI_SUCCESS;
    }
    catch (const std::exception& e)
    {
        return translateException(e);
    }
}

void NRI_CALL nriFreeCompiledShaderBytes(char* bytes)
{
    std::free(bytes);
}

void NRI_CALL nriSetShaderProgressCallbacks(const NriShaderProgressCallbacks* callbacks)
{
    if (!callbacks)
    {
        NRI::SetShaderProgress(nullptr);
        return;
    }
    g_cShaderProgress = *callbacks;
    g_cppShaderProgress.OnCompile = [](const std::string& src)
    {
        if (g_cShaderProgress.onCompile)
            g_cShaderProgress.onCompile(src.c_str(), g_cShaderProgress.userData);
    };
    g_cppShaderProgress.OnPipelineCreated = []()
    {
        if (g_cShaderProgress.onPipelineCreated)
            g_cShaderProgress.onPipelineCreated(g_cShaderProgress.userData);
    };
    g_cppShaderProgress.OnIdle = []()
    {
        if (g_cShaderProgress.onIdle)
            g_cShaderProgress.onIdle(g_cShaderProgress.userData);
    };
    NRI::SetShaderProgress(&g_cppShaderProgress);
}

NriResult NRI_CALL nriCreateImgui(NriDevice device, const NriImguiDesc* desc, NriImgui* outImgui)
{
    if (!device || !device->impl || !desc || !outImgui)
        return NRI_ERROR_INVALID_ARGUMENT;
    *outImgui = nullptr;
    try
    {
        auto wrap = std::make_unique<NriImgui_T>();
        wrap->device = device->impl;
        std::shared_ptr<NRI::DescriptorHeap> resHeap = desc->resourceHeap ? desc->resourceHeap->impl : nullptr;
        std::shared_ptr<NRI::DescriptorHeap> sampHeap = desc->samplerHeap ? desc->samplerHeap->impl : nullptr;
        wrap->impl = std::make_unique<NRI::ImguiExtension>(device->impl, std::move(resHeap), std::move(sampHeap), *desc);
        *outImgui = wrap.release();
        return NRI_SUCCESS;
    }
    catch (const std::exception& e)
    {
        return translateException(e);
    }
}

void NRI_CALL nriDestroyImgui(NriImgui imgui)
{
    if (!imgui)
        return;
    std::unique_ptr<NriImgui_T> owned(imgui);
    owned->impl.reset();
    owned->device.reset();
}

NriResult NRI_CALL nriImguiCreateTexture(NriImgui imgui, uint32_t width, uint32_t height, const void* rgbaPixels, uint64_t* outTextureId)
{
    if (!imgui || !imgui->impl || !outTextureId || !rgbaPixels)
        return NRI_ERROR_INVALID_ARGUMENT;
    try
    {
        *outTextureId = imgui->impl->createTexture(width, height, rgbaPixels);
        return NRI_SUCCESS;
    }
    catch (const std::exception& e)
    {
        return translateException(e);
    }
}

NriResult NRI_CALL nriImguiUpdateTexture(NriImgui imgui, uint64_t textureId, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                                         const void* fullRgbaPixels, uint32_t texWidth, uint32_t texHeight)
{
    if (!imgui || !imgui->impl || !fullRgbaPixels)
        return NRI_ERROR_INVALID_ARGUMENT;
    try
    {
        imgui->impl->updateTexture(textureId, x, y, width, height, fullRgbaPixels, texWidth, texHeight);
        return NRI_SUCCESS;
    }
    catch (const std::exception& e)
    {
        return translateException(e);
    }
}

void NRI_CALL nriImguiDestroyTexture(NriImgui imgui, uint64_t textureId)
{
    if (imgui && imgui->impl)
        imgui->impl->destroyTexture(textureId);
}

void NRI_CALL nriCmdCopyImguiData(NriCommandBuffer cmd, NriImgui imgui, const NriImguiDrawDataDesc* drawData)
{
    if (cmd && cmd->impl && imgui && imgui->impl && drawData)
        imgui->impl->copyData(*cmd->impl, *drawData);
}

void NRI_CALL nriCmdDrawImgui(NriCommandBuffer cmd, NriImgui imgui, const NriImguiDrawDataDesc* drawData)
{
    if (cmd && cmd->impl && imgui && imgui->impl && drawData)
        imgui->impl->draw(*cmd->impl, *drawData);
}

NriResult NRI_CALL nriImguiCreateViewport(NriImgui imgui, const NriImguiViewportDesc* desc, uint64_t* outViewportHandle)
{
    if (!imgui || !imgui->impl || !desc || !outViewportHandle)
        return NRI_ERROR_INVALID_ARGUMENT;
    try
    {
        *outViewportHandle = imgui->impl->createViewport(*desc);
        return NRI_SUCCESS;
    }
    catch (const std::exception& e)
    {
        return translateException(e);
    }
}

void NRI_CALL nriImguiResizeViewport(NriImgui imgui, uint64_t viewportHandle, uint32_t width, uint32_t height)
{
    if (imgui && imgui->impl)
        imgui->impl->resizeViewport(viewportHandle, width, height);
}

void NRI_CALL nriImguiDestroyViewport(NriImgui imgui, uint64_t viewportHandle)
{
    if (imgui && imgui->impl)
        imgui->impl->destroyViewport(viewportHandle);
}

uint32_t NRI_CALL nriImguiGetViewportSlot(NriImgui imgui, uint64_t viewportHandle)
{
    return (imgui && imgui->impl) ? imgui->impl->getViewportSlot(viewportHandle) : 0u;
}

void NRI_CALL nriImguiRenderViewport(NriImgui imgui, uint64_t viewportHandle, const NriImguiDrawDataDesc* drawData,
                                   float clearR, float clearG, float clearB, float clearA)
{
    if (imgui && imgui->impl && drawData)
        imgui->impl->renderViewport(viewportHandle, *drawData, clearR, clearG, clearB, clearA);
}

void NRI_CALL nriImguiPresentViewport(NriImgui imgui, uint64_t viewportHandle)
{
    if (imgui && imgui->impl)
        imgui->impl->presentViewport(viewportHandle);
}

// -----------------------------------------------------------------------------
// Upscaler (Streamline / DLSS-SR / DLSS-RR)
// -----------------------------------------------------------------------------

bool NRI_CALL nriIsStreamlineInitialized(NriDevice device)
{
    auto* slCtx = (device && device->impl) ? device->impl->getStreamlineContext() : nullptr;
    return slCtx ? slCtx->isInitialized() : false;
}

bool NRI_CALL nriIsUpscalerSupported(NriDevice device, NriUpscalerType type)
{
    auto* slCtx = (device && device->impl) ? device->impl->getStreamlineContext() : nullptr;
    return slCtx ? slCtx->isUpscalerSupported(type) : false;
}

NriResult NRI_CALL nriCreateUpscaler(NriDevice device, const NriUpscalerDesc* desc, NriUpscaler* outUpscaler)
{
    if (!device || !device->impl || !desc || !outUpscaler)
        return NRI_ERROR_INVALID_ARGUMENT;
    *outUpscaler = nullptr;
    try
    {
        auto wrap = std::make_unique<NriUpscaler_T>();
        wrap->device = device->impl;
        wrap->impl = std::make_unique<NRI::UpscalerExtension>(device->impl, desc->type);
        *outUpscaler = wrap.release();
        return NRI_SUCCESS;
    }
    catch (const std::exception& e)
    {
        return translateException(e);
    }
}

void NRI_CALL nriDestroyUpscaler(NriUpscaler upscaler)
{
    if (!upscaler)
        return;
    std::unique_ptr<NriUpscaler_T> owned(upscaler);
    owned->impl.reset();
    owned->device.reset();
}

NriResult NRI_CALL nriUpscalerGetOptimalSettings(NriUpscaler upscaler, NriUpscalerMode mode, NriExtent2D outputSize,
                                                NriUpscalerOptimalSettings* outSettings)
{
    if (!upscaler || !upscaler->impl || !outSettings)
        return NRI_ERROR_INVALID_ARGUMENT;
    return upscaler->impl->getOptimalSettings(mode, { outputSize.width, outputSize.height }, *outSettings);
}

void NRI_CALL nriUpscalerReset(NriUpscaler upscaler)
{
    if (upscaler && upscaler->impl)
        upscaler->impl->reset();
}

bool NRI_CALL nriCmdEvaluateUpscaler(NriCommandBuffer cmd, NriUpscaler upscaler, const NriUpscalerEvaluateDesc* desc)
{
    if (!cmd || !cmd->impl || !upscaler || !upscaler->impl || !desc)
        return false;

    auto unwrapTex = [](NriTexture t) -> NRI::Texture2D* { return t ? t->impl.get() : nullptr; };
    const NRI::UpscalerTextures textures{
        .inputColor = unwrapTex(desc->inputColor),
        .outputColor = unwrapTex(desc->outputColor),
        .depth = unwrapTex(desc->depth),
        .motionVectors = unwrapTex(desc->motionVectors),
        .albedo = unwrapTex(desc->albedo),
        .specularAlbedo = unwrapTex(desc->specularAlbedo),
        .normal = unwrapTex(desc->normal),
        .roughness = unwrapTex(desc->roughness),
        .specularHitDistance = unwrapTex(desc->specularHitDistance),
        .specularMotionVectors = unwrapTex(desc->specularMotionVectors),
        .colorBeforeTransparency = unwrapTex(desc->colorBeforeTransparency)
    };
    return upscaler->impl->evaluate(*cmd->impl, textures, *desc);
}

// -----------------------------------------------------------------------------
// Denoiser (NRD: SIGMA / REBLUR / RELAX)
// -----------------------------------------------------------------------------

bool NRI_CALL nriIsDenoiserAvailable(NriDevice device)
{
    return (device && device->impl) ? NRI::DenoiserExtension::isAvailable(*device->impl) : false;
}

NriResult NRI_CALL nriCreateDenoiser(NriDevice device, const NriDenoiserDesc* desc, NriDenoiser* outDenoiser)
{
    if (!device || !device->impl || !desc || !outDenoiser)
        return NRI_ERROR_INVALID_ARGUMENT;
    *outDenoiser = nullptr;
    try
    {
        auto wrap = std::make_unique<NriDenoiser_T>();
        wrap->device = device->impl;
        wrap->impl = std::make_unique<NRI::DenoiserExtension>(device->impl, *desc);
        *outDenoiser = wrap.release();
        return NRI_SUCCESS;
    }
    catch (const std::exception& e)
    {
        return translateException(e);
    }
}

void NRI_CALL nriDestroyDenoiser(NriDenoiser denoiser)
{
    if (!denoiser)
        return;
    std::unique_ptr<NriDenoiser_T> owned(denoiser);
    owned->impl.reset();
    owned->device.reset();
}

NriResult NRI_CALL nriResizeDenoiser(NriDenoiser denoiser, uint32_t width, uint32_t height)
{
    if (!denoiser || !denoiser->impl)
        return NRI_ERROR_INVALID_ARGUMENT;
    return denoiser->impl->resize(width, height);
}

bool NRI_CALL nriDenoiserBeginFrame(NriDenoiser denoiser, const NriDenoiserCommonSettings* settings)
{
    if (!denoiser || !denoiser->impl || !settings)
        return false;
    return denoiser->impl->beginFrame(*settings);
}

void NRI_CALL nriDenoiserSetSigmaSettings(NriDenoiser denoiser, uint32_t identifier, const NriDenoiserSigmaSettings* settings)
{
    if (denoiser && denoiser->impl && settings)
        denoiser->impl->setSigmaSettings(identifier, *settings);
}

void NRI_CALL nriDenoiserSetReblurSettings(NriDenoiser denoiser, uint32_t identifier, const NriDenoiserReblurSettings* settings)
{
    if (denoiser && denoiser->impl && settings)
        denoiser->impl->setReblurSettings(identifier, *settings);
}

void NRI_CALL nriDenoiserSetRelaxSettings(NriDenoiser denoiser, uint32_t identifier, const NriDenoiserRelaxSettings* settings)
{
    if (denoiser && denoiser->impl && settings)
        denoiser->impl->setRelaxSettings(identifier, *settings);
}

bool NRI_CALL nriCmdDenoise(NriCommandBuffer cmd, NriDenoiser denoiser, const uint32_t* identifiers, uint32_t identifierCount,
                            const NriDenoiserResourcesDesc* resources)
{
    if (!cmd || !cmd->impl || !denoiser || !denoiser->impl || !identifiers || identifierCount == 0 || !resources)
        return false;

    auto unwrapTex = [](NriTexture t) -> NRI::Texture2D* { return t ? t->impl.get() : nullptr; };
    const NRI::DenoiserTextures textures{
        .inMotionVectors = unwrapTex(resources->inMotionVectors),
        .inNormalRoughness = unwrapTex(resources->inNormalRoughness),
        .inViewZ = unwrapTex(resources->inViewZ),
        .inPenumbra = unwrapTex(resources->inPenumbra),
        .outShadowTranslucent = unwrapTex(resources->outShadowTranslucent),
        .inSpecRadianceHitDist = unwrapTex(resources->inSpecRadianceHitDist),
        .outSpecRadianceHitDist = unwrapTex(resources->outSpecRadianceHitDist),
        .inDiffRadianceHitDist = unwrapTex(resources->inDiffRadianceHitDist),
        .outDiffRadianceHitDist = unwrapTex(resources->outDiffRadianceHitDist)
    };
    return denoiser->impl->denoise(*cmd->impl, identifiers, identifierCount, textures, resources->restoreInitialState);
}

} // extern "C"
