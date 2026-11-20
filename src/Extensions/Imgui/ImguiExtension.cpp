#include "ImguiExtension.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

#include "../ShaderCompiler/ShaderCompiler.h"
#include "../../Core/Log.h"

namespace NRI
{
    namespace
    {
        constexpr const char* kImguiShaderSource = R"SLANG(
struct ImGuiVertex
{
    float2 pos;
    float2 uv;
    uint col;
};

struct PushConstants
{
    uint64_t vbAddress;
    uint vbSlot;
    uint texSlot;
    uint imageHeapOffset;
    uint samplerSlot;
    float2 scale;
    float2 translate;
    uint linearColorOutput;
    uint baseVertex;
};

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
};

float3 srgbToLinear(float3 c)
{
    float3 lo = c / 12.92;
    float3 hi = pow((c + 0.055) / 1.055, 2.4);
    return select(c <= 0.04045, lo, hi);
}

[shader("vertex")]
VSOut vertMain(uint vertexId : SV_VertexID, uniform PushConstants pc)
{
    uint resolvedVertex = vertexId + pc.baseVertex;
#ifdef NOX_VULKAN
    ImGuiVertex* verts = (ImGuiVertex*)pc.vbAddress;
    ImGuiVertex v = verts[resolvedVertex];
#else
    ByteAddressBuffer vb = ByteAddressBuffer.Handle(uint2(pc.vbSlot, 0));
    uint baseOffset = resolvedVertex * 20u;
    ImGuiVertex v;
    v.pos = asfloat(vb.Load2(baseOffset));
    v.uv  = asfloat(vb.Load2(baseOffset + 8u));
    v.col = vb.Load(baseOffset + 16u);
#endif

    float4 color = float4(
        float((v.col >>  0) & 0xFFu) / 255.0,
        float((v.col >>  8) & 0xFFu) / 255.0,
        float((v.col >> 16) & 0xFFu) / 255.0,
        float((v.col >> 24) & 0xFFu) / 255.0
    );
    if (pc.linearColorOutput != 0)
        color.rgb = srgbToLinear(color.rgb);

    VSOut outv;
    outv.position = float4(v.pos * pc.scale + pc.translate, 0.0, 1.0);
    outv.uv = v.uv;
    outv.color = color;
    return outv;
}

[shader("fragment")]
float4 fragMain(VSOut input, uniform PushConstants pc) : SV_Target
{
    Texture2D tex = Texture2D.Handle(uint2(pc.imageHeapOffset + pc.texSlot, 0));
    SamplerState linearSampler = SamplerState.Handle(uint2(pc.samplerSlot, 0));
    return input.color * tex.Sample(linearSampler, input.uv);
}
)SLANG";

        struct ImguiPushConstants
        {
            uint64_t vbAddress;
            uint32_t vbSlot;
            uint32_t texSlot;
            uint32_t imageHeapOffset;
            uint32_t samplerSlot;
            float scale[2];
            float translate[2];
            uint32_t linearColorOutput;
            uint32_t baseVertex;
        };
        static_assert(sizeof(ImguiPushConstants) == 48, "Push constant layout mismatch with Slang shader");

        uint64_t makeGeometryKey(uint32_t viewportSlot, uint32_t frameSlot)
        {
            return (static_cast<uint64_t>(viewportSlot) << 32) | frameSlot;
        }

        void applyImguiDynamicState(CommandBuffer& cmd)
        {
            cmd.setVertexInput();
            cmd.setPrimitiveTopology(PrimitiveTopology::TriangleList);
            cmd.setPrimitiveRestartEnable(false);
            cmd.setRasterizerDiscardEnable(false);
            cmd.setPolygonMode(PolygonMode::Fill);
            cmd.setCullMode(CullMode::None);
            cmd.setFrontFace(FrontFace::CounterClockWise);
            cmd.setDepthBiasEnable(false);
            cmd.setDepthClampEnable(false);
            cmd.setLineWidth(1.0f);

            cmd.setRasterizationSamples(1);
            cmd.setSampleMask(1, 0xFFFFFFFFu);
            cmd.setAlphaToCoverageEnable(false);
            cmd.setAlphaToOneEnableEXT(false);

            cmd.setDepthTestEnable(false);
            cmd.setDepthWriteEnable(false);
            cmd.setDepthCompareOp(CompareOp::Always);
            cmd.setDepthBoundsTestEnable(false);
            cmd.setStencilTestEnable(false);

            const ColorBlendEquation blend{
                BlendFactor::SrcAlpha,
                BlendFactor::OneMinusSrcAlpha,
                BlendOp::Add,
                BlendFactor::One,
                BlendFactor::OneMinusSrcAlpha,
                BlendOp::Add
            };
            cmd.setColorBlendEnable(0, true);
            cmd.setColorBlendEquation(0, blend);
            cmd.setColorWriteMask(0, ColorComponent::All);
            cmd.setLogicOpEnable(false);
        }
    }

    ImguiExtension::ImguiExtension(std::shared_ptr<Device> device,
                                   std::shared_ptr<DescriptorHeap> resourceHeap,
                                   std::shared_ptr<DescriptorHeap> samplerHeap,
                                   const NriImguiDesc& desc)
        : m_device(std::move(device))
        , m_resourceHeap(std::move(resourceHeap))
        , m_samplerHeap(std::move(samplerHeap))
        , m_samplerSlot(desc.samplerSlot)
        , m_colorFormat(desc.colorFormat != NRI_FORMAT_NONE
                            ? static_cast<ImageFormat>(desc.colorFormat)
                            : ImageFormat::Surface)
        , m_framesInFlight(desc.framesInFlight > 0 ? desc.framesInFlight : 2)
        , m_linearColorOutput(desc.linearColorOutput)
    {
        if (!m_device)
            throw std::runtime_error("ImguiExtension requires a valid device");

        if (!m_resourceHeap)
        {
            DescriptorHeapDesc resDesc{};
            resDesc.type = DescriptorHeapType::Resource;
            resDesc.maxBufferDescriptors = 256;
            resDesc.maxImageDescriptors = 256;
            m_resourceHeap = std::shared_ptr<DescriptorHeap>(m_device->createDescriptorHeap(resDesc));
            m_ownsResourceHeap = true;
        }

        if (!m_samplerHeap)
        {
            DescriptorHeapDesc sampDesc{};
            sampDesc.type = DescriptorHeapType::Sampler;
            sampDesc.maxSamplerDescriptors = 8;
            SamplerDesc linear{};
            linear.magFilter = Filter::Linear;
            linear.minFilter = Filter::Linear;
            linear.mipmapMode = SamplerMipmapMode::Linear;
            linear.addressModeU = SamplerAddressMode::ClampToEdge;
            linear.addressModeV = SamplerAddressMode::ClampToEdge;
            linear.addressModeW = SamplerAddressMode::ClampToEdge;
            linear.maxLod = 1000.0f;
            sampDesc.samplers.push_back(linear);
            m_samplerHeap = std::shared_ptr<DescriptorHeap>(m_device->createDescriptorHeap(sampDesc));
            m_ownsSamplerHeap = true;
            m_samplerSlot = 0;
        }

        m_uploadAllocator = m_device->createCommandAllocator(CommandBufferReset::PerCommandBuffer, QueueType::Graphics);
        m_uploadCmd = m_uploadAllocator->allocateCommandBuffer(1);

        ensurePipeline();

        const uint32_t whitePixel = 0xFFFFFFFFu;
        m_whiteTextureId = createTexture(1, 1, &whitePixel);
    }

    ImguiExtension::~ImguiExtension()
    {
        if (m_device && !m_device->isShuttingDown())
            m_device->waitIdle();

        m_viewports.clear();
        m_geometry.clear();
        m_textures.clear();
        m_pipeline.reset();
        m_uploadCmd.reset();
        m_uploadAllocator.reset();

        if (m_ownsSamplerHeap)
            m_samplerHeap.reset();
        if (m_ownsResourceHeap)
            m_resourceHeap.reset();
    }

    void ImguiExtension::ensurePipeline()
    {
        if (m_device->getAPI() == GraphicsAPI::None)
        {
            PipelineDesc pipeDesc{};
            pipeDesc.type = PipelineType::Graphics;
            pipeDesc.colorFormats = { m_colorFormat };
            pipeDesc.buildNow = true;
            m_pipeline = m_device->createPipelineNoCompiler(pipeDesc);
            return;
        }

        const std::string& cacheDir = m_device->getShaderCacheDir();
        std::filesystem::path folder = cacheDir.empty() ? std::filesystem::path("shader_cache") : std::filesystem::path(cacheDir);
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        std::filesystem::path shaderPath = folder / "nri_imgui_builtin.slang";

        const size_t sourceLen = std::strlen(kImguiShaderSource);
        bool needWrite = !std::filesystem::exists(shaderPath, ec);
        if (!needWrite)
        {
            std::ifstream in(shaderPath, std::ios::binary | std::ios::ate);
            if (!in || static_cast<size_t>(in.tellg()) != sourceLen)
            {
                needWrite = true;
            }
            else
            {
                std::string existing(sourceLen, '\0');
                in.seekg(0);
                in.read(existing.data(), static_cast<std::streamsize>(sourceLen));
                if (existing != kImguiShaderSource)
                    needWrite = true;
            }
        }
        if (needWrite)
        {
            std::ofstream out(shaderPath, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                folder = std::filesystem::temp_directory_path() / "noxrhi_shader_cache";
                std::filesystem::create_directories(folder, ec);
                shaderPath = folder / "nri_imgui_builtin.slang";
                out.open(shaderPath, std::ios::binary | std::ios::trunc);
                if (!out)
                    throw std::runtime_error("ImguiExtension failed to write built-in shader: " + shaderPath.string());
            }
            out.write(kImguiShaderSource, static_cast<std::streamsize>(sourceLen));
        }

        std::unique_ptr<ShaderCompiler> compiler = CreateSlangCompiler();
        compiler->setTargetAPI(m_device->getAPI());

        const std::string pathStr = shaderPath.string();
        PipelineDesc pipeDesc{};
        pipeDesc.type = PipelineType::Graphics;
        pipeDesc.shaders = {
            { ShaderStage::Vertex,   "vertMain", pathStr },
            { ShaderStage::Fragment, "fragMain", pathStr }
        };
        pipeDesc.colorFormats = { m_colorFormat };
        pipeDesc.buildNow = true;

        m_pipeline = m_device->createPipeline(pipeDesc, *compiler);
    }

    uint64_t ImguiExtension::createTexture(uint32_t width, uint32_t height, const void* rgbaPixels)
    {
        if (width == 0 || height == 0 || !rgbaPixels)
            return 0;

        m_device->waitIdle();

        TextureDesc texDesc{};
        texDesc.width = width;
        texDesc.height = height;
        texDesc.arrayLayers = 1;
        texDesc.mipLevels = 1;
        texDesc.sampleCount = 1;
        texDesc.usage = TextureUsage::ShaderResource;
        texDesc.format = ImageFormat::RGBA8;
        texDesc.sharedAcrossQueues = true;

        std::shared_ptr<Texture2D> tex(m_device->createTexture(texDesc));

        const uint64_t byteSize = static_cast<uint64_t>(width) * height * 4ull;
        BufferDesc stagingDesc{};
        stagingDesc.size = byteSize;
        stagingDesc.usage = BufferUsage::Staging;
        std::unique_ptr<Buffer> staging = m_device->createBuffer(stagingDesc);

        void* mapped = staging->map(0, byteSize);
        std::memcpy(mapped, rgbaPixels, static_cast<size_t>(byteSize));
        staging->unmap();

        m_uploadCmd->begin(0, true);
        const std::vector<size_t> mipOffsets = { 0 };
        tex->recordUpload(*m_uploadCmd, *staging, 0, mipOffsets);
        m_uploadCmd->end(0);
        m_device->submitAndWait(*m_uploadCmd, 0);

        m_resourceHeap->registerTexture(*tex, TextureUsage::Default);
        const uint32_t slot = tex->GetDescriptorIndexSlot();
        const uint64_t id = nriImguiEncodeTextureSlot(slot);

        std::scoped_lock lock(m_mutex);
        m_textures[id] = ManagedTexture{
            .texture = std::move(tex),
            .descriptorSlot = slot,
            .width = width,
            .height = height
        };
        return id;
    }

    void ImguiExtension::updateTexture(uint64_t textureId, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                                       const void* fullRgbaPixels, uint32_t texWidth, uint32_t texHeight)
    {
        (void)x;
        (void)y;
        (void)width;
        (void)height;

        if (!fullRgbaPixels || texWidth == 0 || texHeight == 0)
            return;

        std::shared_ptr<Texture2D> tex;
        {
            std::scoped_lock lock(m_mutex);
            auto it = m_textures.find(textureId);
            if (it == m_textures.end())
                return;
            tex = it->second.texture;
        }

        m_device->waitIdle();

        const uint64_t byteSize = static_cast<uint64_t>(texWidth) * texHeight * 4ull;
        BufferDesc stagingDesc{};
        stagingDesc.size = byteSize;
        stagingDesc.usage = BufferUsage::Staging;
        std::unique_ptr<Buffer> staging = m_device->createBuffer(stagingDesc);

        void* mapped = staging->map(0, byteSize);
        std::memcpy(mapped, fullRgbaPixels, static_cast<size_t>(byteSize));
        staging->unmap();

        m_uploadCmd->begin(0, true);
        const std::vector<size_t> mipOffsets = { 0 };
        tex->recordUpload(*m_uploadCmd, *staging, 0, mipOffsets);
        m_uploadCmd->end(0);
        m_device->submitAndWait(*m_uploadCmd, 0);
    }

    void ImguiExtension::destroyTexture(uint64_t textureId)
    {
        std::shared_ptr<Texture2D> removed;
        {
            std::scoped_lock lock(m_mutex);
            auto it = m_textures.find(textureId);
            if (it == m_textures.end())
                return;
            removed = std::move(it->second.texture);
            m_textures.erase(it);
        }

        if (removed)
        {
            m_device->enqueueDeferredDeletion([removed]() mutable
            {
                removed.reset();
            });
        }
    }

    uint32_t ImguiExtension::resolveTextureSlot(uint64_t textureId) const
    {
        if (textureId == 0)
            textureId = m_whiteTextureId;
        return textureId > 0 ? static_cast<uint32_t>(textureId - 1ull) : 0u;
    }

    ImguiExtension::FrameGeometry& ImguiExtension::getOrCreateGeometry(uint32_t viewportSlot, uint32_t frameSlot)
    {
        const uint64_t key = makeGeometryKey(viewportSlot, frameSlot % m_framesInFlight);
        return m_geometry[key];
    }

    void ImguiExtension::ensureGeometryCapacity(FrameGeometry& geom, uint64_t neededVtxBytes, uint64_t neededIdxBytes)
    {
        constexpr uint64_t kMinBufferBytes = 128ull * 1024ull;

        if (neededVtxBytes > geom.vtxCapacityBytes)
        {
            if (geom.vtxStaging || geom.vtxGpu)
            {
                auto oldStg = std::move(geom.vtxStaging);
                auto oldGpu = std::move(geom.vtxGpu);
                m_device->enqueueDeferredDeletion([oldStg, oldGpu]() mutable
                {
                    oldStg.reset();
                    oldGpu.reset();
                });
            }

            const uint64_t newCap = std::max<uint64_t>({ neededVtxBytes, geom.vtxCapacityBytes * 2ull, kMinBufferBytes });
            BufferDesc stgDesc{};
            stgDesc.size = newCap;
            stgDesc.usage = BufferUsage::Staging;
            geom.vtxStaging = std::shared_ptr<Buffer>(m_device->createBuffer(stgDesc));

            BufferDesc gpuDesc{};
            gpuDesc.size = newCap;
            gpuDesc.usage = BufferUsage::Storage;
            geom.vtxGpu = std::shared_ptr<Buffer>(m_device->createBuffer(gpuDesc));
            geom.vtxCapacityBytes = newCap;
            geom.vtxSlot = m_resourceHeap->registerBuffer(*geom.vtxGpu, newCap);
        }

        if (neededIdxBytes > geom.idxCapacityBytes)
        {
            if (geom.idxStaging || geom.idxGpu)
            {
                auto oldStg = std::move(geom.idxStaging);
                auto oldGpu = std::move(geom.idxGpu);
                m_device->enqueueDeferredDeletion([oldStg, oldGpu]() mutable
                {
                    oldStg.reset();
                    oldGpu.reset();
                });
            }

            const uint64_t newCap = std::max<uint64_t>({ neededIdxBytes, geom.idxCapacityBytes * 2ull, kMinBufferBytes });
            BufferDesc stgDesc{};
            stgDesc.size = newCap;
            stgDesc.usage = BufferUsage::Staging;
            geom.idxStaging = std::shared_ptr<Buffer>(m_device->createBuffer(stgDesc));

            BufferDesc gpuDesc{};
            gpuDesc.size = newCap;
            gpuDesc.usage = BufferUsage::Index;
            geom.idxGpu = std::shared_ptr<Buffer>(m_device->createBuffer(gpuDesc));
            geom.idxCapacityBytes = newCap;
        }
    }

    void ImguiExtension::copyData(CommandBuffer& cmd, const NriImguiDrawDataDesc& drawData)
    {
        FrameGeometry& geom = getOrCreateGeometry(drawData.viewportSlot, drawData.frameIndex);

        uint32_t totalVtx = 0;
        uint32_t totalIdx = 0;
        for (uint32_t i = 0; i < drawData.drawListCount; ++i)
        {
            totalVtx += drawData.drawLists[i].vertexCount;
            totalIdx += drawData.drawLists[i].indexCount;
        }

        geom.activeVtxCount = totalVtx;
        geom.activeIdxCount = totalIdx;
        if (totalVtx == 0 || totalIdx == 0)
            return;

        const uint64_t vtxBytes = static_cast<uint64_t>(totalVtx) * sizeof(NriImguiVertex);
        const uint64_t idxBytes = static_cast<uint64_t>(totalIdx) * sizeof(uint32_t);
        ensureGeometryCapacity(geom, vtxBytes, idxBytes);

        auto* vtxDst = static_cast<NriImguiVertex*>(geom.vtxStaging->map(0, vtxBytes));
        auto* idxDst = static_cast<uint32_t*>(geom.idxStaging->map(0, idxBytes));

        for (uint32_t i = 0; i < drawData.drawListCount; ++i)
        {
            const NriImguiDrawList& list = drawData.drawLists[i];
            if (list.vertexCount > 0 && list.vertices)
            {
                std::memcpy(vtxDst, list.vertices, static_cast<size_t>(list.vertexCount) * sizeof(NriImguiVertex));
                vtxDst += list.vertexCount;
            }
            if (list.indexCount > 0 && list.indices)
            {
                if (list.indexSize == 2)
                {
                    const auto* src16 = static_cast<const uint16_t*>(list.indices);
                    for (uint32_t j = 0; j < list.indexCount; ++j)
                        idxDst[j] = src16[j];
                }
                else
                {
                    std::memcpy(idxDst, list.indices, static_cast<size_t>(list.indexCount) * sizeof(uint32_t));
                }
                idxDst += list.indexCount;
            }
        }

        geom.vtxStaging->unmap();
        geom.idxStaging->unmap();

        cmd.copyBuffer(*geom.vtxStaging, *geom.vtxGpu, BufferCopyRegion{ 0, 0, vtxBytes });
        cmd.copyBuffer(*geom.idxStaging, *geom.idxGpu, BufferCopyRegion{ 0, 0, idxBytes });

        const BufferBarrierDesc barriers[] = {
            {
                .buffer = geom.vtxGpu.get(),
                .before = { AccessBits::TransferWrite, StageBits::Transfer },
                .after = { AccessBits::ShaderRead, StageBits::Fragment | StageBits::Compute | StageBits::Task | StageBits::Mesh | StageBits::Vertex }
            }
        };
        cmd.resourceBarriers({}, barriers);
        cmd.executionBarrier();
    }

    void ImguiExtension::draw(CommandBuffer& cmd, const NriImguiDrawDataDesc& drawData)
    {
        if (drawData.drawListCount == 0)
            return;

        const float fbWidth = drawData.displaySize[0] * drawData.framebufferScale[0];
        const float fbHeight = drawData.displaySize[1] * drawData.framebufferScale[1];
        if (fbWidth <= 0.0f || fbHeight <= 0.0f)
            return;

        FrameGeometry& geom = getOrCreateGeometry(drawData.viewportSlot, drawData.frameIndex);
        if (geom.activeVtxCount == 0 || geom.activeIdxCount == 0 || !geom.vtxGpu || !geom.idxGpu)
            return;

        cmd.bindDescriptorHeaps(m_resourceHeap.get(), m_samplerHeap.get());
        cmd.bindPipeline(PipelineBindPoint::Graphics, *m_pipeline);
        applyImguiDynamicState(cmd);

        const ViewportBounds viewport{ 0.0f, fbHeight, fbWidth, -fbHeight };
        cmd.setViewportWithCount(viewport, 0.0f, 1.0f);
        cmd.bindIndexBuffer(*geom.idxGpu, 0);

        const float L = drawData.displayPos[0];
        const float R = drawData.displayPos[0] + drawData.displaySize[0];
        const float T = drawData.displayPos[1];
        const float B = drawData.displayPos[1] + drawData.displaySize[1];
        const float sx = 2.0f / (R - L);
        const float sy = -2.0f / (B - T);

        ImguiPushConstants pc{};
        pc.vbAddress = geom.vtxGpu->getDeviceAddress();
        pc.vbSlot = geom.vtxSlot;
        pc.imageHeapOffset = m_resourceHeap->getImageHeapIndexOffset();
        pc.samplerSlot = m_samplerSlot;
        pc.scale[0] = sx;
        pc.scale[1] = sy;
        pc.translate[0] = -1.0f - L * sx;
        pc.translate[1] = 1.0f - T * sy;
        pc.linearColorOutput = m_linearColorOutput ? 1u : 0u;

        int32_t globalVtxOffset = 0;
        uint32_t globalIdxOffset = 0;

        for (uint32_t i = 0; i < drawData.drawListCount; ++i)
        {
            const NriImguiDrawList& list = drawData.drawLists[i];
            for (uint32_t c = 0; c < list.cmdCount; ++c)
            {
                const NriImguiDrawCmd& drawCmd = list.cmds[c];
                if (drawCmd.elemCount == 0)
                    continue;

                const float clipMinX = std::max(0.0f, (drawCmd.clipRect[0] - drawData.displayPos[0]) * drawData.framebufferScale[0]);
                const float clipMinY = std::max(0.0f, (drawCmd.clipRect[1] - drawData.displayPos[1]) * drawData.framebufferScale[1]);
                const float clipMaxX = std::min(fbWidth, (drawCmd.clipRect[2] - drawData.displayPos[0]) * drawData.framebufferScale[0]);
                const float clipMaxY = std::min(fbHeight, (drawCmd.clipRect[3] - drawData.displayPos[1]) * drawData.framebufferScale[1]);
                if (clipMaxX <= clipMinX || clipMaxY <= clipMinY)
                    continue;

                const int32_t sx0 = static_cast<int32_t>(clipMinX);
                const int32_t sy0 = static_cast<int32_t>(clipMinY);
                const uint32_t sw = static_cast<uint32_t>(std::max(1.0f, clipMaxX - static_cast<float>(sx0)));
                const uint32_t sh = static_cast<uint32_t>(std::max(1.0f, clipMaxY - static_cast<float>(sy0)));
                cmd.setScissorRect(sx0, sy0, sw, sh);

                pc.texSlot = resolveTextureSlot(drawCmd.textureId);
                pc.baseVertex = static_cast<uint32_t>(std::max<int32_t>(0, drawCmd.vtxOffset + globalVtxOffset));
                cmd.pushData(&pc, sizeof(pc));

                // Vertex pulling via SV_VertexID ignores BaseVertex on HLSL/Slang; baseVertex is added in vertMain.
                cmd.drawIndexed(drawCmd.elemCount, 1, drawCmd.idxOffset + globalIdxOffset, 0, 0);
            }
            globalVtxOffset += static_cast<int32_t>(list.vertexCount);
            globalIdxOffset += list.indexCount;
        }
    }

    namespace
    {
        SwapchainDesc buildViewportSwapchainDesc(uint32_t width, uint32_t height, bool vsync, const NriNativeWindowHandle& win)
        {
            SwapchainDesc scDesc{};
            scDesc.Width = width;
            scDesc.Height = height;
            scDesc.VSync = vsync;
            scDesc.Window.type = static_cast<WindowSystemType>(win.type);
            scDesc.Window.hwnd = win.hwnd;
            scDesc.Window.hinstance = win.hinstance;
            scDesc.Window.vkSurface = win.vkSurface;
            return scDesc;
        }
    }

    uint64_t ImguiExtension::createViewport(const NriImguiViewportDesc& desc)
    {
        auto vp = std::make_unique<ViewportContext>();
        vp->slot = m_nextViewportSlot++;
        vp->window = desc.window;
        vp->width = desc.width;
        vp->height = desc.height;
        vp->vsync = desc.vsync;

        if (vp->width > 0 && vp->height > 0)
            vp->swapchain = m_device->createSwapchain(buildViewportSwapchainDesc(vp->width, vp->height, vp->vsync, vp->window));

        vp->frames.resize(m_framesInFlight);
        for (uint32_t i = 0; i < m_framesInFlight; ++i)
        {
            vp->frames[i].allocator = m_device->createCommandAllocator(CommandBufferReset::WithAllocator, QueueType::Graphics);
            vp->frames[i].commandBuffer = vp->frames[i].allocator->allocateCommandBuffer(1);
        }

        const uint64_t handle = m_nextViewportHandle++;
        std::scoped_lock lock(m_mutex);
        m_viewports.emplace(handle, std::move(vp));
        return handle;
    }

    void ImguiExtension::resizeViewport(uint64_t viewportHandle, uint32_t width, uint32_t height)
    {
        ViewportContext* vp = nullptr;
        {
            std::scoped_lock lock(m_mutex);
            auto it = m_viewports.find(viewportHandle);
            if (it == m_viewports.end())
                return;
            vp = it->second.get();
        }

        vp->width = width;
        vp->height = height;
        vp->needsResize = true;
    }

    void ImguiExtension::destroyViewport(uint64_t viewportHandle)
    {
        std::unique_ptr<ViewportContext> vp;
        {
            std::scoped_lock lock(m_mutex);
            auto it = m_viewports.find(viewportHandle);
            if (it == m_viewports.end())
                return;
            vp = std::move(it->second);
            m_viewports.erase(it);
        }

        if (vp && m_device && !m_device->isShuttingDown())
            m_device->waitIdle();

        // Clean up buffered geometry associated with this viewport slot
        if (vp)
        {
            for (uint32_t f = 0; f < m_framesInFlight; ++f)
                m_geometry.erase(makeGeometryKey(vp->slot, f));
        }
    }

    uint32_t ImguiExtension::getViewportSlot(uint64_t viewportHandle) const
    {
        auto it = m_viewports.find(viewportHandle);
        return it != m_viewports.end() ? it->second->slot : 0u;
    }

    void ImguiExtension::renderViewport(uint64_t viewportHandle, const NriImguiDrawDataDesc& drawData,
                                        float clearR, float clearG, float clearB, float clearA)
    {
        ViewportContext* vp = nullptr;
        {
            std::scoped_lock lock(m_mutex);
            auto it = m_viewports.find(viewportHandle);
            if (it == m_viewports.end())
                return;
            vp = it->second.get();
        }

        vp->skipPresent = true;
        if (vp->width == 0 || vp->height == 0)
            return;

        if (vp->needsResize || !vp->swapchain)
        {
            m_device->waitIdle();
            vp->swapchain.reset();
            vp->swapchain = m_device->createSwapchain(buildViewportSwapchainDesc(vp->width, vp->height, vp->vsync, vp->window));
            vp->needsResize = false;
        }

        const uint32_t frameSlot = vp->frameCounter % m_framesInFlight;
        uint32_t imageIndex = 0;
        if (vp->swapchain->acquireNextImage(frameSlot, imageIndex) == FrameResult::ResizeRequired)
        {
            vp->needsResize = true;
            return;
        }
        vp->lastImageIndex = imageIndex;

        NriImguiDrawDataDesc scopedDrawData = drawData;
        scopedDrawData.frameIndex = frameSlot;
        scopedDrawData.viewportSlot = vp->slot;

        ViewportFrameResources& frameRes = vp->frames[frameSlot];
        frameRes.allocator->reset();
        CommandBuffer& cmd = *frameRes.commandBuffer;
        cmd.begin(0, true);
        copyData(cmd, scopedDrawData);

        cmd.transitionSwapchainLayout(*vp->swapchain, imageIndex, TextureLayout::Undefined, TextureLayout::ColorAttachment);

        RenderDesc renderDesc{};
        renderDesc.renderArea = vp->swapchain->getExtent();
        renderDesc.isSwapchainPass = true;
        renderDesc.colorAttachments = {
            RenderAttachDesc{
                .attachmentSwapchain = vp->swapchain.get(),
                .attachment = nullptr,
                .resolve = nullptr,
                .resolveSwapchain = nullptr,
                .resolveImageIndex = imageIndex,
                .loadOP = LoadOP::clear,
                .storeOP = StoreOP::store,
                .clearColor = { clearR, clearG, clearB, clearA },
                .clearDepth = {}
            }
        };

        cmd.beginRendering(renderDesc);
        draw(cmd, scopedDrawData);
        cmd.endRendering();

        cmd.transitionSwapchainLayout(*vp->swapchain, imageIndex, TextureLayout::ColorAttachment, TextureLayout::Present);
        cmd.end(0);

        CommandBuffer* cmds[] = { &cmd };
        m_device->submitCommandBuffers(cmds, *vp->swapchain, frameSlot, imageIndex, {});
        vp->skipPresent = false;
    }

    void ImguiExtension::presentViewport(uint64_t viewportHandle)
    {
        ViewportContext* vp = nullptr;
        {
            std::scoped_lock lock(m_mutex);
            auto it = m_viewports.find(viewportHandle);
            if (it == m_viewports.end())
                return;
            vp = it->second.get();
        }

        if (vp->skipPresent || !vp->swapchain)
            return;

        const uint32_t frameSlot = vp->frameCounter % m_framesInFlight;
        if (vp->swapchain->present(frameSlot, vp->lastImageIndex) == FrameResult::ResizeRequired)
            vp->needsResize = true;
        ++vp->frameCounter;
    }
}
