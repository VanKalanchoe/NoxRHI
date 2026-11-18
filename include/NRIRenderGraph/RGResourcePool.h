#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <NRIHPP/NRI.hpp>

namespace nri
{
    struct RGTextureKey
    {
        uint32_t Width = 0;
        uint32_t Height = 0;
        ImageFormat Format = NRI_FORMAT_NONE;
        TextureUsage Usage = NRI_TEXTURE_USAGE_COLOR_ATTACHMENT;
        uint32_t MipLevels = 1;

        bool operator==(const RGTextureKey&) const = default;
    };

    struct RGBufferKey
    {
        uint64_t Size = 0;
        BufferUsage Usage = NRI_BUFFER_USAGE_STORAGE;

        bool operator==(const RGBufferKey&) const = default;
    };

    // GPU memory pool behind transient and history render graph resources.
    class RGResourcePool
    {
    public:
        struct TextureEntry
        {
            RGTextureKey Key;
            Texture Handle;
            std::vector<uint32_t> StorageSlots;
            uint64_t LastUsedFrame = 0;
            uint64_t BusyFrame = ~0ull;
            uint32_t BusyUntilPass = 0;
        };

        struct BufferEntry
        {
            RGBufferKey Key;
            Buffer Handle;
            uint64_t LastUsedFrame = 0;
            uint64_t BusyFrame = ~0ull;
            uint32_t BusyUntilPass = 0;
        };

        struct TextureHistory
        {
            RGTextureKey Key;
            std::vector<std::unique_ptr<TextureEntry>> Textures;
            uint64_t LastUsedFrame = 0;
        };

        struct BufferHistory
        {
            RGBufferKey Key;
            std::vector<std::unique_ptr<BufferEntry>> Buffers;
            uint64_t LastUsedFrame = 0;
        };

        RGResourcePool() = default;
        ~RGResourcePool();

        RGResourcePool(const RGResourcePool&) = delete;
        RGResourcePool& operator=(const RGResourcePool&) = delete;

        void Initialize(Device& device, DescriptorHeap& resourceHeap);

        void BeginFrame(uint64_t frameNumber);
        void EndFrame();

        TextureEntry& AcquireTransientTexture(const RGTextureKey& key, uint32_t firstPass, uint32_t lastPass);
        BufferEntry& AcquireTransientBuffer(const RGBufferKey& key, uint32_t firstPass, uint32_t lastPass);

        TextureHistory& AcquireTextureHistory(const std::string& name, const RGTextureKey& key, uint32_t count, bool& outCreated);
        BufferHistory& AcquireBufferHistory(const std::string& name, const RGBufferKey& key, uint32_t count, bool& outCreated);

        void ReleaseAll();

        uint64_t GetTextureMemory() const { return m_TextureMemory; }
        uint64_t GetBufferMemory() const { return m_BufferMemory; }

        static uint64_t EstimateTextureBytes(const RGTextureKey& key);

    private:
        std::unique_ptr<TextureEntry> CreateTexture(const RGTextureKey& key);
        std::unique_ptr<BufferEntry> CreateBuffer(const RGBufferKey& key);
        void DestroyTexture(std::unique_ptr<TextureEntry>& texture);
        void DestroyBuffer(std::unique_ptr<BufferEntry>& buffer);
        void ScheduleRelease(std::unique_ptr<TextureEntry> texture);
        void ScheduleRelease(std::unique_ptr<BufferEntry> buffer);

    private:
        static constexpr uint64_t ReleaseDelayFrames = 4;
        static constexpr uint64_t UnusedKeepFrames = 120;

        struct PendingRelease
        {
            uint64_t ReleaseFrame = 0;
            std::unique_ptr<TextureEntry> TextureResource;
            std::unique_ptr<BufferEntry> BufferResource;
        };

    private:
        Device* m_Device = nullptr;
        DescriptorHeap* m_ResourceHeap = nullptr;
        uint64_t m_Frame = 0;

        std::vector<std::unique_ptr<TextureEntry>> m_Textures;
        std::vector<std::unique_ptr<BufferEntry>> m_Buffers;
        std::unordered_map<std::string, TextureHistory> m_TextureHistories;
        std::unordered_map<std::string, BufferHistory> m_BufferHistories;
        std::vector<PendingRelease> m_PendingReleases;

        uint64_t m_TextureMemory = 0;
        uint64_t m_BufferMemory = 0;
    };
}
