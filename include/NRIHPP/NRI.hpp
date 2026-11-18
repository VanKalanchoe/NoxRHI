#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <NRI/NRI.h>

namespace nri
{
    using GraphicsAPI = NriGraphicsAPI;
    using QueueType = NriQueueType;
    using CompareOp = NriCompareOp;
    using Extent2D = NriExtent2D;
    using ViewportBounds = NriViewportBounds;
    using MemoryHeapStats = NriMemoryHeapStats;
    using SparsePageRange = NriSparsePageRange;
    using CommandBufferReset = NriCommandBufferReset;
    using LoadOp = NriLoadOp;
    using StoreOp = NriStoreOp;
    using ClearColor = NriClearColor;
    using ClearDepth = NriClearDepth;
    using PipelineBindPoint = NriPipelineBindPoint;
    using PrimitiveTopology = NriPrimitiveTopology;
    using PolygonMode = NriPolygonMode;
    using CullMode = NriCullMode;
    using FrontFace = NriFrontFace;
    using BlendFactor = NriBlendFactor;
    using BlendOp = NriBlendOp;
    using ColorBlendEquation = NriColorBlendEquation;
    using BufferCopyRegion = NriBufferCopyRegion;
    using ResourceState = NriResourceState;
    using BufferUsage = NriBufferUsage;
    using ImageFormat = NriImageFormat;
    using TextureLayout = NriTextureLayout;
    using TextureUsage = NriTextureUsage;
    using TextureRegionUpload = NriTextureRegionUpload;
    using Filter = NriFilter;
    using SamplerMipmapMode = NriSamplerMipmapMode;
    using SamplerAddressMode = NriSamplerAddressMode;
    using BorderColor = NriBorderColor;
    using SamplerDesc = NriSamplerDesc;
    using DescriptorHeapType = NriDescriptorHeapType;
    using QueryType = NriQueryType;
    using PipelineType = NriPipelineType;
    using ShaderStage = NriShaderStage;
    using RayTracingHitGroupDesc = NriRayTracingHitGroupDesc;
    using LogLevel = NriLogLevel;
    using LogCallback = NriLogCallback;

    namespace AccessBits
    {
        inline constexpr uint32_t None = NRI_ACCESS_NONE;
        inline constexpr uint32_t ShaderRead = NRI_ACCESS_SHADER_READ;
        inline constexpr uint32_t ShaderWrite = NRI_ACCESS_SHADER_WRITE;
        inline constexpr uint32_t ColorAttachmentRead = NRI_ACCESS_COLOR_ATTACHMENT_READ;
        inline constexpr uint32_t ColorAttachmentWrite = NRI_ACCESS_COLOR_ATTACHMENT_WRITE;
        inline constexpr uint32_t DepthStencilRead = NRI_ACCESS_DEPTH_STENCIL_READ;
        inline constexpr uint32_t DepthStencilWrite = NRI_ACCESS_DEPTH_STENCIL_WRITE;
        inline constexpr uint32_t TransferRead = NRI_ACCESS_TRANSFER_READ;
        inline constexpr uint32_t TransferWrite = NRI_ACCESS_TRANSFER_WRITE;
        inline constexpr uint32_t AccelerationStructureRead = NRI_ACCESS_ACCELERATION_STRUCTURE_READ;
        inline constexpr uint32_t AccelerationStructureWrite = NRI_ACCESS_ACCELERATION_STRUCTURE_WRITE;
        inline constexpr uint32_t IndirectRead = NRI_ACCESS_INDIRECT_READ;
    }

    namespace StageBits
    {
        inline constexpr uint32_t None = NRI_STAGE_NONE;
        inline constexpr uint32_t Task = NRI_STAGE_TASK;
        inline constexpr uint32_t Mesh = NRI_STAGE_MESH;
        inline constexpr uint32_t Fragment = NRI_STAGE_FRAGMENT;
        inline constexpr uint32_t Compute = NRI_STAGE_COMPUTE;
        inline constexpr uint32_t ColorAttachmentOutput = NRI_STAGE_COLOR_ATTACHMENT_OUTPUT;
        inline constexpr uint32_t FragmentTests = NRI_STAGE_FRAGMENT_TESTS;
        inline constexpr uint32_t Transfer = NRI_STAGE_TRANSFER;
        inline constexpr uint32_t AccelerationStructureBuild = NRI_STAGE_ACCELERATION_STRUCTURE_BUILD;
        inline constexpr uint32_t Indirect = NRI_STAGE_INDIRECT;
        inline constexpr uint32_t RayTracing = NRI_STAGE_RAY_TRACING;
    }

    namespace ColorComponent
    {
        inline constexpr uint32_t R = NRI_COLOR_COMPONENT_R;
        inline constexpr uint32_t G = NRI_COLOR_COMPONENT_G;
        inline constexpr uint32_t B = NRI_COLOR_COMPONENT_B;
        inline constexpr uint32_t A = NRI_COLOR_COMPONENT_A;
        inline constexpr uint32_t All = NRI_COLOR_COMPONENT_ALL;
    }

    inline void checkResult(NriResult result, const char* context = "NRI call failed")
    {
        if (result != NRI_SUCCESS)
        {
            const char* err = nriGetLastError();
            if (err && err[0] != '\0')
                throw std::runtime_error(std::string(context) + ": " + err);
            throw std::runtime_error(context);
        }
    }

    class Buffer;
    class Texture;
    class Swapchain;
    class Pipeline;
    class CommandBuffer;
    class CommandAllocator;
    class DescriptorHeap;
    class TimelineSemaphore;
    class QueryPool;
    class ShaderCompiler;
    class Device;

    struct TimelinePoint
    {
        NriTimelineSemaphore semaphore = nullptr;
        uint64_t value = 0;

        TimelinePoint() = default;
        TimelinePoint(NriTimelineSemaphore sem, uint64_t val) : semaphore(sem), value(val) {}
        TimelinePoint(const TimelineSemaphore& sem, uint64_t val);
    };

    struct BufferDesc
    {
        uint64_t size = 0;
        BufferUsage usage = NRI_BUFFER_USAGE_STORAGE;
        bool sharedAcrossQueues = false;
        bool sparse = false;
    };

    struct TextureDesc
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t arrayLayers = 1;
        bool isCubeMap = false;
        uint32_t mipLevels = 1;
        uint32_t sampleCount = 1;
        TextureUsage usage = NRI_TEXTURE_USAGE_COLOR_ATTACHMENT;
        ImageFormat format = NRI_FORMAT_NONE;
        uint32_t directFormat = UINT32_MAX;
        bool sharedAcrossQueues = false;
    };

    struct DescriptorHeapDesc
    {
        DescriptorHeapType type = NRI_DESCRIPTOR_HEAP_RESOURCE;
        uint32_t maxSamplerDescriptors = 0;
        uint32_t maxBufferDescriptors = 0;
        uint32_t maxImageDescriptors = 0;
        std::vector<SamplerDesc> samplers;
    };

    struct QueryPoolDesc
    {
        QueryType type = NRI_QUERY_TYPE_AS_COMPACTED_SIZE;
        uint32_t capacity = 0;
    };

    struct ShaderStageDesc
    {
        ShaderStage stage = NRI_SHADER_STAGE_VERTEX;
        std::string entryPoint;
        std::string sourcePath;
    };

    struct PipelineDesc
    {
        PipelineType type = NRI_PIPELINE_GRAPHICS;
        std::vector<ShaderStageDesc> shaders;
        std::vector<ImageFormat> colorFormats;
        std::vector<RayTracingHitGroupDesc> hitGroups;
        uint32_t maxRecursionDepth = 1;
        bool forceCompile = false;
        bool buildNow = false;
    };

    struct RenderAttachDesc
    {
        NriSwapchain attachmentSwapchain = nullptr;
        NriTexture attachment = nullptr;
        NriTexture resolve = nullptr;
        NriSwapchain resolveSwapchain = nullptr;
        uint32_t resolveImageIndex = 0;
        LoadOp loadOp = NRI_LOAD_OP_DONT_CARE;
        StoreOp storeOp = NRI_STORE_OP_DONT_CARE;
        ClearColor clearColor{};
        ClearDepth clearDepth{};
    };

    struct RenderDesc
    {
        Extent2D renderArea{};
        std::vector<RenderAttachDesc> colorAttachments;
        RenderAttachDesc depthAttachment{};
        bool hasDepthAttachment = false;
        bool isSwapchainPass = false;
    };

    struct TextureBarrierDesc
    {
        NriTexture texture = nullptr;
        ResourceState before{};
        ResourceState after{};
    };

    struct BufferBarrierDesc
    {
        NriBuffer buffer = nullptr;
        ResourceState before{};
        ResourceState after{};
    };

    inline bool imageFormatBlock(ImageFormat format, uint32_t& outBlockBytes, uint32_t& outBlockSize)
    {
        return nriImageFormatBlock(format, &outBlockBytes, &outBlockSize);
    }

    inline uint64_t estimateTextureBytes(const TextureDesc& desc)
    {
        const NriTextureDesc cDesc{
            .width = desc.width,
            .height = desc.height,
            .arrayLayers = desc.arrayLayers,
            .isCubeMap = desc.isCubeMap,
            .mipLevels = desc.mipLevels,
            .sampleCount = desc.sampleCount,
            .usage = desc.usage,
            .format = desc.format,
            .directFormat = desc.directFormat,
            .sharedAcrossQueues = desc.sharedAcrossQueues
        };
        return nriEstimateTextureBytes(&cDesc);
    }

    // -------------------------------------------------------------------------
    // Move-only RAII Handle Wrappers
    // -------------------------------------------------------------------------

    class Buffer
    {
    public:
        Buffer() = default;
        explicit Buffer(NriBuffer handle) : m_handle(handle) {}
        ~Buffer() { reset(); }

        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;
        Buffer(Buffer&& other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}
        Buffer& operator=(Buffer&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                m_handle = std::exchange(other.m_handle, nullptr);
            }
            return *this;
        }

        void reset(NriBuffer h = nullptr)
        {
            if (m_handle)
                nriDestroyBuffer(m_handle);
            m_handle = h;
        }
        NriBuffer release() { return std::exchange(m_handle, nullptr); }
        NriBuffer handle() const { return m_handle; }
        explicit operator bool() const { return m_handle != nullptr; }

        void* map(uint64_t offset = 0, uint64_t size = 0) { return nriMapBuffer(m_handle, offset, size); }
        void unmap() { nriUnmapBuffer(m_handle); }
        uint64_t getDeviceAddress() const { return nriGetBufferDeviceAddress(m_handle); }
        uint64_t getSize() const { return nriGetBufferSize(m_handle); }
        uint64_t getSparsePageSize() const { return nriGetBufferSparsePageSize(m_handle); }

    private:
        NriBuffer m_handle = nullptr;
    };

    class Texture
    {
    public:
        Texture() = default;
        explicit Texture(NriTexture handle) : m_handle(handle) {}
        ~Texture() { reset(); }

        Texture(const Texture&) = delete;
        Texture& operator=(const Texture&) = delete;
        Texture(Texture&& other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}
        Texture& operator=(Texture&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                m_handle = std::exchange(other.m_handle, nullptr);
            }
            return *this;
        }

        void reset(NriTexture h = nullptr)
        {
            if (m_handle)
                nriDestroyTexture(m_handle);
            m_handle = h;
        }
        NriTexture release() { return std::exchange(m_handle, nullptr); }
        NriTexture handle() const { return m_handle; }
        explicit operator bool() const { return m_handle != nullptr; }

        uint32_t getWidth() const { return nriGetTextureWidth(m_handle); }
        uint32_t getHeight() const { return nriGetTextureHeight(m_handle); }
        uint32_t getMipLevels() const { return nriGetTextureMipLevels(m_handle); }
        uint32_t getArrayLayers() const { return nriGetTextureArrayLayers(m_handle); }
        TextureUsage getUsage() const { return nriGetTextureUsage(m_handle); }
        uint32_t getDescriptorSlot() const { return nriGetTextureDescriptorSlot(m_handle); }

        void uploadFromBuffer(NriCommandBuffer cmd, NriBuffer stagingBuffer, uint32_t width, uint32_t height,
                              uint32_t mipLevels, std::span<const size_t> mipOffsets)
        {
            nriTextureUploadFromBuffer(m_handle, cmd, stagingBuffer, width, height, mipLevels,
                                       mipOffsets.data(), static_cast<uint32_t>(mipOffsets.size()));
        }
        void recordUpload(NriCommandBuffer cmd, NriBuffer stagingBuffer, uint64_t stagingOffset, std::span<const size_t> mipOffsets)
        {
            nriTextureRecordUpload(m_handle, cmd, stagingBuffer, stagingOffset,
                                   mipOffsets.data(), static_cast<uint32_t>(mipOffsets.size()));
        }
        void recordInitialLayout(NriCommandBuffer cmd) { nriTextureRecordInitialLayout(m_handle, cmd); }
        void recordRegionUploads(NriCommandBuffer cmd, NriBuffer stagingBuffer, std::span<const TextureRegionUpload> regions)
        {
            nriTextureRecordRegionUploads(m_handle, cmd, stagingBuffer, regions.data(), static_cast<uint32_t>(regions.size()));
        }
        void copyToBuffer(NriCommandBuffer cmd, NriBuffer dstBuffer, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
        {
            nriTextureCopyToBuffer(m_handle, cmd, dstBuffer, x, y, width, height);
        }
        void blitTo(NriCommandBuffer cmd, NriTexture dstTexture) { nriTextureBlitTo(m_handle, cmd, dstTexture); }
        void generateMipmaps(NriCommandBuffer cmd) { nriTextureGenerateMipmaps(m_handle, cmd); }

    private:
        NriTexture m_handle = nullptr;
    };

    class DescriptorHeap
    {
    public:
        DescriptorHeap() = default;
        explicit DescriptorHeap(NriDescriptorHeap handle) : m_handle(handle) {}
        ~DescriptorHeap() { reset(); }

        DescriptorHeap(const DescriptorHeap&) = delete;
        DescriptorHeap& operator=(const DescriptorHeap&) = delete;
        DescriptorHeap(DescriptorHeap&& other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}
        DescriptorHeap& operator=(DescriptorHeap&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                m_handle = std::exchange(other.m_handle, nullptr);
            }
            return *this;
        }

        void reset(NriDescriptorHeap h = nullptr)
        {
            if (m_handle)
                nriDestroyDescriptorHeap(m_handle);
            m_handle = h;
        }
        NriDescriptorHeap release() { return std::exchange(m_handle, nullptr); }
        NriDescriptorHeap handle() const { return m_handle; }
        explicit operator bool() const { return m_handle != nullptr; }

        void registerTexture(const Texture& tex, TextureUsage usageOverride = NRI_TEXTURE_USAGE_DEFAULT)
        {
            nriRegisterTexture(m_handle, tex.handle(), usageOverride);
        }
        void registerTexture(NriTexture tex, TextureUsage usageOverride = NRI_TEXTURE_USAGE_DEFAULT)
        {
            nriRegisterTexture(m_handle, tex, usageOverride);
        }
        uint32_t registerStorageTextureMip(const Texture& tex, uint32_t mipLevel)
        {
            return nriRegisterStorageTextureMip(m_handle, tex.handle(), mipLevel);
        }
        void unregisterTexture(uint32_t slot) { nriUnregisterTexture(m_handle, slot); }
        uint32_t registerBuffer(const Buffer& buf, uint64_t size) { return nriRegisterBuffer(m_handle, buf.handle(), size); }
        uint32_t getImageHeapIndexOffset() const { return nriGetDescriptorHeapImageIndexOffset(m_handle); }
        uint64_t getSize() const { return nriGetDescriptorHeapSize(m_handle); }
        uint64_t getReservedRangeOffset() const { return nriGetDescriptorHeapReservedRangeOffset(m_handle); }
        uint64_t getReservedRangeSize() const { return nriGetDescriptorHeapReservedRangeSize(m_handle); }
        uint64_t getGPUAddress() const { return nriGetDescriptorHeapGPUAddress(m_handle); }

    private:
        NriDescriptorHeap m_handle = nullptr;
    };

    class QueryPool
    {
    public:
        QueryPool() = default;
        explicit QueryPool(NriQueryPool handle) : m_handle(handle) {}
        ~QueryPool() { reset(); }

        QueryPool(const QueryPool&) = delete;
        QueryPool& operator=(const QueryPool&) = delete;
        QueryPool(QueryPool&& other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}
        QueryPool& operator=(QueryPool&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                m_handle = std::exchange(other.m_handle, nullptr);
            }
            return *this;
        }

        void reset(NriQueryPool h = nullptr)
        {
            if (m_handle)
                nriDestroyQueryPool(m_handle);
            m_handle = h;
        }
        NriQueryPool release() { return std::exchange(m_handle, nullptr); }
        NriQueryPool handle() const { return m_handle; }
        explicit operator bool() const { return m_handle != nullptr; }

        uint32_t getCapacity() const { return nriGetQueryPoolCapacity(m_handle); }
        bool getResults(uint32_t first, std::span<uint64_t> outValues) const
        {
            return nriGetQueryPoolResults(m_handle, first, outValues.data(), static_cast<uint32_t>(outValues.size()));
        }

    private:
        NriQueryPool m_handle = nullptr;
    };

    class Pipeline
    {
    public:
        Pipeline() = default;
        explicit Pipeline(NriPipeline handle) : m_handle(handle) {}
        ~Pipeline() { reset(); }

        Pipeline(const Pipeline&) = delete;
        Pipeline& operator=(const Pipeline&) = delete;
        Pipeline(Pipeline&& other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}
        Pipeline& operator=(Pipeline&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                m_handle = std::exchange(other.m_handle, nullptr);
            }
            return *this;
        }

        void reset(NriPipeline h = nullptr)
        {
            if (m_handle)
                nriDestroyPipeline(m_handle);
            m_handle = h;
        }
        NriPipeline release() { return std::exchange(m_handle, nullptr); }
        NriPipeline handle() const { return m_handle; }
        explicit operator bool() const { return m_handle != nullptr; }

    private:
        NriPipeline m_handle = nullptr;
    };

    class TimelineSemaphore
    {
    public:
        TimelineSemaphore() = default;
        explicit TimelineSemaphore(NriTimelineSemaphore handle) : m_handle(handle) {}
        ~TimelineSemaphore() { reset(); }

        TimelineSemaphore(const TimelineSemaphore&) = delete;
        TimelineSemaphore& operator=(const TimelineSemaphore&) = delete;
        TimelineSemaphore(TimelineSemaphore&& other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}
        TimelineSemaphore& operator=(TimelineSemaphore&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                m_handle = std::exchange(other.m_handle, nullptr);
            }
            return *this;
        }

        void reset(NriTimelineSemaphore h = nullptr)
        {
            if (m_handle)
                nriDestroyTimelineSemaphore(m_handle);
            m_handle = h;
        }
        NriTimelineSemaphore release() { return std::exchange(m_handle, nullptr); }
        NriTimelineSemaphore handle() const { return m_handle; }
        explicit operator bool() const { return m_handle != nullptr; }

        uint64_t getValue() const { return nriGetTimelineSemaphoreValue(m_handle); }
        void wait(uint64_t value) const { nriWaitTimelineSemaphore(m_handle, value); }

    private:
        NriTimelineSemaphore m_handle = nullptr;
    };

    inline TimelinePoint::TimelinePoint(const TimelineSemaphore& sem, uint64_t val)
        : semaphore(sem.handle()), value(val) {}

    class CommandBuffer
    {
    public:
        CommandBuffer() = default;
        explicit CommandBuffer(NriCommandBuffer handle) : m_handle(handle) {}
        ~CommandBuffer() { reset(); }

        CommandBuffer(const CommandBuffer&) = delete;
        CommandBuffer& operator=(const CommandBuffer&) = delete;
        CommandBuffer(CommandBuffer&& other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}
        CommandBuffer& operator=(CommandBuffer&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                m_handle = std::exchange(other.m_handle, nullptr);
            }
            return *this;
        }

        void reset(NriCommandBuffer h = nullptr)
        {
            if (m_handle)
                nriDestroyCommandBuffer(m_handle);
            m_handle = h;
        }
        NriCommandBuffer release() { return std::exchange(m_handle, nullptr); }
        NriCommandBuffer handle() const { return m_handle; }
        explicit operator bool() const { return m_handle != nullptr; }

        void begin(uint32_t index = 0, bool oneTimeSubmit = false) { nriCmdBegin(m_handle, index, oneTimeSubmit); }
        void end(uint32_t index = 0) { nriCmdEnd(m_handle, index); }

        void beginRendering(const RenderDesc& desc)
        {
            std::vector<NriRenderAttachDesc> colorVec;
            colorVec.reserve(desc.colorAttachments.size());
            for (const auto& a : desc.colorAttachments)
            {
                colorVec.push_back(NriRenderAttachDesc{
                    .attachmentSwapchain = a.attachmentSwapchain,
                    .attachment = a.attachment,
                    .resolve = a.resolve,
                    .resolveSwapchain = a.resolveSwapchain,
                    .resolveImageIndex = a.resolveImageIndex,
                    .loadOp = a.loadOp,
                    .storeOp = a.storeOp,
                    .clearColor = a.clearColor,
                    .clearDepth = a.clearDepth
                });
            }
            NriRenderDesc cDesc{
                .renderArea = desc.renderArea,
                .colorAttachments = colorVec.data(),
                .colorAttachmentCount = static_cast<uint32_t>(colorVec.size()),
                .depthAttachment = NriRenderAttachDesc{
                    .attachmentSwapchain = desc.depthAttachment.attachmentSwapchain,
                    .attachment = desc.depthAttachment.attachment,
                    .resolve = desc.depthAttachment.resolve,
                    .resolveSwapchain = desc.depthAttachment.resolveSwapchain,
                    .resolveImageIndex = desc.depthAttachment.resolveImageIndex,
                    .loadOp = desc.depthAttachment.loadOp,
                    .storeOp = desc.depthAttachment.storeOp,
                    .clearColor = desc.depthAttachment.clearColor,
                    .clearDepth = desc.depthAttachment.clearDepth
                },
                .hasDepthAttachment = desc.hasDepthAttachment,
                .isSwapchainPass = desc.isSwapchainPass
            };
            nriCmdBeginRendering(m_handle, &cDesc);
        }
        void endRendering() { nriCmdEndRendering(m_handle); }

        void bindPipeline(PipelineBindPoint bindPoint, const Pipeline& pipeline) { nriCmdBindPipeline(m_handle, bindPoint, pipeline.handle()); }
        void setViewport(Extent2D extent) { nriCmdSetViewport(m_handle, extent); }
        void setScissor(Extent2D extent) { nriCmdSetScissor(m_handle, extent); }
        void setViewportWithCount(const ViewportBounds& bounds, float minDepth, float maxDepth) { nriCmdSetViewportWithCount(m_handle, &bounds, minDepth, maxDepth); }
        void setScissorWithCount(const Extent2D& extent) { nriCmdSetScissorWithCount(m_handle, &extent); }
        void setScissorRect(int32_t x, int32_t y, uint32_t width, uint32_t height) { nriCmdSetScissorRect(m_handle, x, y, width, height); }
        void setVertexInput() { nriCmdSetVertexInput(m_handle); }
        void setPrimitiveTopology(PrimitiveTopology topology) { nriCmdSetPrimitiveTopology(m_handle, topology); }
        void setPrimitiveRestartEnable(bool enable) { nriCmdSetPrimitiveRestartEnable(m_handle, enable); }
        void setRasterizerDiscardEnable(bool enable) { nriCmdSetRasterizerDiscardEnable(m_handle, enable); }
        void setPolygonMode(PolygonMode mode) { nriCmdSetPolygonMode(m_handle, mode); }
        void setCullMode(CullMode mode) { nriCmdSetCullMode(m_handle, mode); }
        void setFrontFace(FrontFace face) { nriCmdSetFrontFace(m_handle, face); }
        void setDepthBiasEnable(bool enable) { nriCmdSetDepthBiasEnable(m_handle, enable); }
        void setDepthBias(float constantFactor, float clamp, float slopeFactor) { nriCmdSetDepthBias(m_handle, constantFactor, clamp, slopeFactor); }
        void setDepthClampEnable(bool enable) { nriCmdSetDepthClampEnable(m_handle, enable); }
        void setLineWidth(float lineWidth) { nriCmdSetLineWidth(m_handle, lineWidth); }
        void setRasterizationSamples(uint32_t sampleCount) { nriCmdSetRasterizationSamples(m_handle, sampleCount); }
        void setSampleMask(uint32_t sampleCount, uint32_t sampleMask) { nriCmdSetSampleMask(m_handle, sampleCount, sampleMask); }
        void setAlphaToCoverageEnable(bool enable) { nriCmdSetAlphaToCoverageEnable(m_handle, enable); }
        void setAlphaToOneEnable(bool enable) { nriCmdSetAlphaToOneEnable(m_handle, enable); }
        void setDepthTestEnable(bool enable) { nriCmdSetDepthTestEnable(m_handle, enable); }
        void setDepthWriteEnable(bool enable) { nriCmdSetDepthWriteEnable(m_handle, enable); }
        void setDepthCompareOp(CompareOp op) { nriCmdSetDepthCompareOp(m_handle, op); }
        void setDepthBoundsTestEnable(bool enable) { nriCmdSetDepthBoundsTestEnable(m_handle, enable); }
        void setStencilTestEnable(bool enable) { nriCmdSetStencilTestEnable(m_handle, enable); }
        void setColorBlendEnable(uint32_t firstAttachment, bool enable) { nriCmdSetColorBlendEnable(m_handle, firstAttachment, enable); }
        void setColorBlendEquation(uint32_t firstAttachment, const ColorBlendEquation& eq) { nriCmdSetColorBlendEquation(m_handle, firstAttachment, &eq); }
        void setColorWriteMask(uint32_t firstAttachment, uint32_t mask) { nriCmdSetColorWriteMask(m_handle, firstAttachment, mask); }
        void setLogicOpEnable(bool enable) { nriCmdSetLogicOpEnable(m_handle, enable); }

        void bindDescriptorHeaps(const DescriptorHeap* resourceHeap, const DescriptorHeap* samplerHeap)
        {
            nriCmdBindDescriptorHeaps(m_handle, resourceHeap ? resourceHeap->handle() : nullptr,
                                      samplerHeap ? samplerHeap->handle() : nullptr);
        }
        void pushData(const void* data, uint32_t size) { nriCmdPushData(m_handle, data, size); }
        void bindVertexBuffer(uint32_t firstBinding, const Buffer& buffer, uint64_t offset = 0)
        {
            nriCmdBindVertexBuffer(m_handle, firstBinding, buffer.handle(), offset);
        }
        void bindIndexBuffer(const Buffer& buffer, uint64_t offset = 0)
        {
            nriCmdBindIndexBuffer(m_handle, buffer.handle(), offset);
        }
        void draw(uint32_t vertexCount, uint32_t instanceCount = 1, uint32_t firstVertex = 0, uint32_t firstInstance = 0)
        {
            nriCmdDraw(m_handle, vertexCount, instanceCount, firstVertex, firstInstance);
        }
        void drawIndexed(uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0, int32_t vertexOffset = 0, uint32_t firstInstance = 0)
        {
            nriCmdDrawIndexed(m_handle, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
        }
        void dispatch(uint32_t gx, uint32_t gy = 1, uint32_t gz = 1) { nriCmdDispatch(m_handle, gx, gy, gz); }

        void copyBuffer(const Buffer& src, const Buffer& dst, const BufferCopyRegion& region = {})
        {
            nriCmdCopyBuffer(m_handle, src.handle(), dst.handle(), &region);
        }
        void copyBuffer(const Buffer& src, const Buffer& dst, std::span<const BufferCopyRegion> regions)
        {
            nriCmdCopyBufferRegions(m_handle, src.handle(), dst.handle(), regions.data(), static_cast<uint32_t>(regions.size()));
        }
        void fillBuffer(const Buffer& buf, uint64_t offset, uint64_t size, uint32_t value)
        {
            nriCmdFillBuffer(m_handle, buf.handle(), offset, size, value);
        }

        void transitionTextureLayout(const Texture& tex, TextureLayout oldLayout, TextureLayout newLayout)
        {
            nriCmdTransitionTextureLayout(m_handle, tex.handle(), oldLayout, newLayout);
        }
        void transitionSwapchainLayout(NriSwapchain swapchain, uint32_t imageIndex, TextureLayout oldLayout, TextureLayout newLayout)
        {
            nriCmdTransitionSwapchainLayout(m_handle, swapchain, imageIndex, oldLayout, newLayout);
        }
        void resolveImage(const Texture& src, const Texture& dst, uint32_t width, uint32_t height)
        {
            nriCmdResolveImage(m_handle, src.handle(), dst.handle(), width, height);
        }
        void copyTexture(const Texture& src, const Texture& dst, uint32_t width, uint32_t height)
        {
            nriCmdCopyTexture(m_handle, src.handle(), dst.handle(), width, height);
        }
        void copyTextureMips(const Texture& src, uint32_t srcFirstMip, const Texture& dst, uint32_t dstFirstMip, uint32_t mipCount)
        {
            nriCmdCopyTextureMips(m_handle, src.handle(), srcFirstMip, dst.handle(), dstFirstMip, mipCount);
        }

        void resetQueries(const QueryPool& pool, uint32_t first, uint32_t count)
        {
            nriCmdResetQueries(m_handle, pool.handle(), first, count);
        }
        void executionBarrier() { nriCmdExecutionBarrier(m_handle); }
        void transferBarrier() { nriCmdTransferBarrier(m_handle); }
        void resourceBarriers(std::span<const TextureBarrierDesc> textures, std::span<const BufferBarrierDesc> buffers)
        {
            static_assert(sizeof(TextureBarrierDesc) == sizeof(NriTextureBarrierDesc));
            static_assert(sizeof(BufferBarrierDesc) == sizeof(NriBufferBarrierDesc));
            nriCmdResourceBarriers(m_handle,
                                   reinterpret_cast<const NriTextureBarrierDesc*>(textures.data()), static_cast<uint32_t>(textures.size()),
                                   reinterpret_cast<const NriBufferBarrierDesc*>(buffers.data()), static_cast<uint32_t>(buffers.size()));
        }

        void beginDebugLabel(const char* label) { nriCmdBeginDebugLabel(m_handle, label); }
        void endDebugLabel() { nriCmdEndDebugLabel(m_handle); }

    private:
        NriCommandBuffer m_handle = nullptr;
    };

    class CommandAllocator
    {
    public:
        CommandAllocator() = default;
        explicit CommandAllocator(NriCommandAllocator handle) : m_handle(handle) {}
        ~CommandAllocator() { reset(); }

        CommandAllocator(const CommandAllocator&) = delete;
        CommandAllocator& operator=(const CommandAllocator&) = delete;
        CommandAllocator(CommandAllocator&& other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}
        CommandAllocator& operator=(CommandAllocator&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                m_handle = std::exchange(other.m_handle, nullptr);
            }
            return *this;
        }

        void reset(NriCommandAllocator h = nullptr)
        {
            if (m_handle)
                nriDestroyCommandAllocator(m_handle);
            m_handle = h;
        }
        NriCommandAllocator release() { return std::exchange(m_handle, nullptr); }
        NriCommandAllocator handle() const { return m_handle; }
        explicit operator bool() const { return m_handle != nullptr; }

        CommandBuffer allocateCommandBuffer(uint32_t framesInFlight = 1)
        {
            NriCommandBuffer cb = nullptr;
            checkResult(nriAllocateCommandBuffer(m_handle, framesInFlight, &cb), "nriAllocateCommandBuffer");
            return CommandBuffer(cb);
        }

        void resetPool() { nriResetCommandAllocator(m_handle); }

    private:
        NriCommandAllocator m_handle = nullptr;
    };

    class Device
    {
    public:
        Device() = default;
        explicit Device(NriDevice handle) : m_handle(handle) {}
        ~Device() { reset(); }

        Device(const Device&) = delete;
        Device& operator=(const Device&) = delete;
        Device(Device&& other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}
        Device& operator=(Device&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                m_handle = std::exchange(other.m_handle, nullptr);
            }
            return *this;
        }

        void reset(NriDevice h = nullptr)
        {
            if (m_handle)
                nriDestroyDevice(m_handle);
            m_handle = h;
        }
        NriDevice release() { return std::exchange(m_handle, nullptr); }
        NriDevice handle() const { return m_handle; }
        explicit operator bool() const { return m_handle != nullptr; }

        GraphicsAPI getAPI() const { return nriGetDeviceAPI(m_handle); }
        uint32_t getMSAASampleCount() const { return nriGetMSAASampleCount(m_handle); }
        bool hasDedicatedTransferQueue() const { return nriHasDedicatedTransferQueue(m_handle); }
        bool hasAsyncComputeQueue() const { return nriHasAsyncComputeQueue(m_handle); }
        bool isSurfaceFormatBGRA() const { return nriIsSurfaceFormatBGRA(m_handle); }
        bool isMemoryBudgetSupported() const { return nriIsMemoryBudgetSupported(m_handle); }
        bool supportsSparseBuffers() const { return nriSupportsSparseBuffers(m_handle); }
        uint64_t getSparseBufferReserveLimit() const { return nriGetSparseBufferReserveLimit(m_handle); }
        TimelinePoint getSparseTimelinePoint() const
        {
            const NriTimelinePoint pt = nriGetSparseTimelinePoint(m_handle);
            return { pt.semaphore, pt.value };
        }
        uint64_t getTextureBytes() const { return nriGetTextureBytes(m_handle); }

        void beginFrame(uint32_t frameNumber) { nriBeginFrame(m_handle, frameNumber); }
        void waitIdle() { nriWaitIdle(m_handle); }
        void finishPipelineBuilds() { nriFinishPipelineBuilds(m_handle); }

        void precompileShaderModules(std::span<const std::string_view> sources, NriShaderCompiler compiler)
        {
            std::vector<std::string> owned;
            std::vector<const char*> ptrs;
            owned.reserve(sources.size());
            ptrs.reserve(sources.size());
            for (auto s : sources)
            {
                owned.emplace_back(s);
                ptrs.push_back(owned.back().c_str());
            }
            nriPrecompileShaderModules(m_handle, ptrs.data(), static_cast<uint32_t>(ptrs.size()), compiler);
        }

        bool compileShaderModules(std::span<const std::string_view> sources, NriShaderCompiler compiler)
        {
            std::vector<std::string> owned;
            std::vector<const char*> ptrs;
            owned.reserve(sources.size());
            ptrs.reserve(sources.size());
            for (auto s : sources)
            {
                owned.emplace_back(s);
                ptrs.push_back(owned.back().c_str());
            }
            return nriCompileShaderModules(m_handle, ptrs.data(), static_cast<uint32_t>(ptrs.size()), compiler);
        }

        bool updateSparseBuffer(const Buffer& buffer, std::span<const SparsePageRange> commits, std::span<const SparsePageRange> releases)
        {
            return nriUpdateSparseBuffer(m_handle, buffer.handle(),
                                         commits.data(), static_cast<uint32_t>(commits.size()),
                                         releases.data(), static_cast<uint32_t>(releases.size()));
        }

        std::vector<MemoryHeapStats> getMemoryStats() const
        {
            uint32_t count = 0;
            nriGetMemoryStats(m_handle, nullptr, &count);
            std::vector<MemoryHeapStats> out(count);
            if (count > 0)
                nriGetMemoryStats(m_handle, out.data(), &count);
            out.resize(count);
            return out;
        }

        CommandAllocator createCommandAllocator(CommandBufferReset resetMode, QueueType queue = NRI_QUEUE_GRAPHICS)
        {
            NriCommandAllocator alloc = nullptr;
            checkResult(nriCreateCommandAllocator(m_handle, resetMode, queue, &alloc), "nriCreateCommandAllocator");
            return CommandAllocator(alloc);
        }

        TimelineSemaphore createTimelineSemaphore(uint64_t initialValue = 0)
        {
            NriTimelineSemaphore sem = nullptr;
            checkResult(nriCreateTimelineSemaphore(m_handle, initialValue, &sem), "nriCreateTimelineSemaphore");
            return TimelineSemaphore(sem);
        }

        Buffer createBuffer(const BufferDesc& desc)
        {
            const NriBufferDesc cDesc{
                .size = desc.size,
                .usage = desc.usage,
                .sharedAcrossQueues = desc.sharedAcrossQueues,
                .sparse = desc.sparse
            };
            NriBuffer buf = nullptr;
            checkResult(nriCreateBuffer(m_handle, &cDesc, &buf), "nriCreateBuffer");
            return Buffer(buf);
        }

        Texture createTexture(const TextureDesc& desc)
        {
            const NriTextureDesc cDesc{
                .width = desc.width,
                .height = desc.height,
                .arrayLayers = desc.arrayLayers,
                .isCubeMap = desc.isCubeMap,
                .mipLevels = desc.mipLevels,
                .sampleCount = desc.sampleCount,
                .usage = desc.usage,
                .format = desc.format,
                .directFormat = desc.directFormat,
                .sharedAcrossQueues = desc.sharedAcrossQueues
            };
            NriTexture tex = nullptr;
            checkResult(nriCreateTexture(m_handle, &cDesc, &tex), "nriCreateTexture");
            return Texture(tex);
        }

        DescriptorHeap createDescriptorHeap(const DescriptorHeapDesc& desc)
        {
            const NriDescriptorHeapDesc cDesc{
                .type = desc.type,
                .maxSamplerDescriptors = desc.maxSamplerDescriptors,
                .maxBufferDescriptors = desc.maxBufferDescriptors,
                .maxImageDescriptors = desc.maxImageDescriptors,
                .samplers = desc.samplers.empty() ? nullptr : desc.samplers.data(),
                .samplerCount = static_cast<uint32_t>(desc.samplers.size())
            };
            NriDescriptorHeap heap = nullptr;
            checkResult(nriCreateDescriptorHeap(m_handle, &cDesc, &heap), "nriCreateDescriptorHeap");
            return DescriptorHeap(heap);
        }

        QueryPool createQueryPool(const QueryPoolDesc& desc)
        {
            const NriQueryPoolDesc cDesc{ .type = desc.type, .capacity = desc.capacity };
            NriQueryPool pool = nullptr;
            checkResult(nriCreateQueryPool(m_handle, &cDesc, &pool), "nriCreateQueryPool");
            return QueryPool(pool);
        }

        Pipeline createPipeline(const PipelineDesc& desc, NriShaderCompiler compiler)
        {
            std::vector<NriShaderStageDesc> shaders;
            shaders.reserve(desc.shaders.size());
            for (const auto& s : desc.shaders)
                shaders.push_back({ s.stage, s.entryPoint.c_str(), s.sourcePath.c_str() });
            const NriPipelineDesc cDesc{
                .type = desc.type,
                .shaders = shaders.empty() ? nullptr : shaders.data(),
                .shaderCount = static_cast<uint32_t>(shaders.size()),
                .colorFormats = desc.colorFormats.empty() ? nullptr : desc.colorFormats.data(),
                .colorFormatCount = static_cast<uint32_t>(desc.colorFormats.size()),
                .hitGroups = desc.hitGroups.empty() ? nullptr : desc.hitGroups.data(),
                .hitGroupCount = static_cast<uint32_t>(desc.hitGroups.size()),
                .maxRecursionDepth = desc.maxRecursionDepth,
                .forceCompile = desc.forceCompile,
                .buildNow = desc.buildNow
            };
            NriPipeline pip = nullptr;
            checkResult(nriCreatePipeline(m_handle, &cDesc, compiler, &pip), "nriCreatePipeline");
            return Pipeline(pip);
        }

        void submitAndWait(const CommandBuffer& cmd, uint32_t slotIndex = 0)
        {
            checkResult(nriSubmitAndWait(m_handle, cmd.handle(), slotIndex), "nriSubmitAndWait");
        }

        void submitCommandBuffers(std::span<const NriCommandBuffer> cmdBuffers, NriSwapchain swapchain,
                                  uint32_t frameIndex, uint32_t imageIndex, std::span<const TimelinePoint> waits = {})
        {
            static_assert(sizeof(TimelinePoint) == sizeof(NriTimelinePoint));
            checkResult(nriSubmitCommandBuffers(m_handle, cmdBuffers.data(), static_cast<uint32_t>(cmdBuffers.size()),
                                                swapchain, frameIndex, imageIndex,
                                                reinterpret_cast<const NriTimelinePoint*>(waits.data()),
                                                static_cast<uint32_t>(waits.size())),
                        "nriSubmitCommandBuffers");
        }

        void submit(QueueType queue, std::span<const NriCommandBuffer> cmdBuffers,
                    std::span<const TimelinePoint> waits = {}, std::span<const TimelinePoint> signals = {})
        {
            static_assert(sizeof(TimelinePoint) == sizeof(NriTimelinePoint));
            checkResult(nriSubmit(m_handle, queue, cmdBuffers.data(), static_cast<uint32_t>(cmdBuffers.size()),
                                  reinterpret_cast<const NriTimelinePoint*>(waits.data()), static_cast<uint32_t>(waits.size()),
                                  reinterpret_cast<const NriTimelinePoint*>(signals.data()), static_cast<uint32_t>(signals.size())),
                        "nriSubmit");
        }

    private:
        NriDevice m_handle = nullptr;
    };
}
