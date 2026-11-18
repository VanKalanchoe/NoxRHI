#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../../Core/Device.h"
#include "NRI/Extensions/NRIImgui.h"

namespace NRI
{
    class ImguiExtension
    {
    public:
        ImguiExtension(std::shared_ptr<Device> device,
                       std::shared_ptr<DescriptorHeap> resourceHeap,
                       std::shared_ptr<DescriptorHeap> samplerHeap,
                       const NriImguiDesc& desc);
        ~ImguiExtension();

        uint64_t createTexture(uint32_t width, uint32_t height, const void* rgbaPixels);
        void updateTexture(uint64_t textureId, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                           const void* fullRgbaPixels, uint32_t texWidth, uint32_t texHeight);
        void destroyTexture(uint64_t textureId);

        void copyData(CommandBuffer& cmd, const NriImguiDrawDataDesc& drawData);
        void draw(CommandBuffer& cmd, const NriImguiDrawDataDesc& drawData);

        uint64_t createViewport(const NriImguiViewportDesc& desc);
        void resizeViewport(uint64_t viewportHandle, uint32_t width, uint32_t height);
        void destroyViewport(uint64_t viewportHandle);
        uint32_t getViewportSlot(uint64_t viewportHandle) const;
        void renderViewport(uint64_t viewportHandle, const NriImguiDrawDataDesc& drawData,
                            float clearR, float clearG, float clearB, float clearA);
        void presentViewport(uint64_t viewportHandle);

    private:
        struct ManagedTexture
        {
            std::shared_ptr<Texture2D> texture;
            uint32_t descriptorSlot = ~0u;
            uint32_t width = 0;
            uint32_t height = 0;
        };

        struct FrameGeometry
        {
            std::shared_ptr<Buffer> vtxStaging;
            std::shared_ptr<Buffer> idxStaging;
            std::shared_ptr<Buffer> vtxGpu;
            std::shared_ptr<Buffer> idxGpu;
            uint64_t vtxCapacityBytes = 0;
            uint64_t idxCapacityBytes = 0;
            uint32_t vtxSlot = ~0u;
            uint32_t activeVtxCount = 0;
            uint32_t activeIdxCount = 0;
        };

        struct ViewportFrameResources
        {
            std::unique_ptr<CommandAllocator> allocator;
            std::unique_ptr<CommandBuffer> commandBuffer;
        };

        struct ViewportContext
        {
            uint32_t slot = 0;
            NriNativeWindowHandle window{};
            uint32_t width = 0;
            uint32_t height = 0;
            bool vsync = true;
            bool needsResize = false;
            bool skipPresent = false;
            uint32_t frameCounter = 0;
            uint32_t lastImageIndex = 0;

            std::unique_ptr<Swapchain> swapchain;
            std::vector<ViewportFrameResources> frames;
        };

        FrameGeometry& getOrCreateGeometry(uint32_t viewportSlot, uint32_t frameSlot);
        void ensureGeometryCapacity(FrameGeometry& geom, uint64_t neededVtxBytes, uint64_t neededIdxBytes);
        uint32_t resolveTextureSlot(uint64_t textureId) const;
        void ensurePipeline();

    private:
        std::shared_ptr<Device> m_device;
        std::shared_ptr<DescriptorHeap> m_resourceHeap;
        std::shared_ptr<DescriptorHeap> m_samplerHeap;
        bool m_ownsResourceHeap = false;
        bool m_ownsSamplerHeap = false;
        uint32_t m_samplerSlot = 0;
        ImageFormat m_colorFormat = ImageFormat::Surface;
        uint32_t m_framesInFlight = 2;
        bool m_linearColorOutput = true;

        std::unique_ptr<Pipeline> m_pipeline;
        std::unique_ptr<CommandAllocator> m_uploadAllocator;
        std::unique_ptr<CommandBuffer> m_uploadCmd;

        std::mutex m_mutex;
        std::unordered_map<uint64_t, ManagedTexture> m_textures;
        uint64_t m_whiteTextureId = 0;

        // Key: (uint64_t(viewportSlot) << 32) | frameSlot
        std::unordered_map<uint64_t, FrameGeometry> m_geometry;

        std::unordered_map<uint64_t, std::unique_ptr<ViewportContext>> m_viewports;
        uint64_t m_nextViewportHandle = 1;
        uint32_t m_nextViewportSlot = 1; // Slot 0 is reserved for the main viewport
    };
}
