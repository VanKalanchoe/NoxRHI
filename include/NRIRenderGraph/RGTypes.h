#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <type_traits>

#include <NRIHPP/NRI.hpp>

namespace nri
{
    constexpr uint32_t RGInvalidIndex = ~0u;

    // Typed handles to graph resources. Valid only for the frame they were created in.
    struct RGTexture
    {
        uint32_t Index = RGInvalidIndex;
        uint32_t Frame = 0;
        bool IsValid() const { return Index != RGInvalidIndex; }
    };

    struct RGBuffer
    {
        uint32_t Index = RGInvalidIndex;
        uint32_t Frame = 0;
        bool IsValid() const { return Index != RGInvalidIndex; }
    };

    enum class RGResourceKind : uint8_t
    {
        Imported,
        Transient,
        History
    };

    enum class RGImportAccess : uint8_t
    {
        ReadWrite,
        ReadOnly
    };

    enum class RGSize : uint8_t
    {
        RenderResolution,
        OutputResolution,
        Absolute
    };

    struct RGTextureDesc
    {
        RGSize Size = RGSize::RenderResolution;
        uint32_t Width = 0;  // Absolute only
        uint32_t Height = 0; // Absolute only
        ImageFormat Format = NRI_FORMAT_NONE;
        TextureUsage Usage = NRI_TEXTURE_USAGE_COLOR_ATTACHMENT;
        uint32_t MipLevels = 1;
    };

    struct RGBufferDesc
    {
        uint64_t Size = 0;
        BufferUsage Usage = NRI_BUFFER_USAGE_STORAGE;
    };

    constexpr uint32_t RGMaxHistoryLength = 4;

    struct RGTextureHistory
    {
        std::array<RGTexture, RGMaxHistoryLength> Textures{};
        uint32_t Count = 0;
        bool WasReset = false;
    };

    struct RGBufferHistory
    {
        std::array<RGBuffer, RGMaxHistoryLength> Buffers{};
        uint32_t Count = 0;
        bool WasReset = false;
    };

    enum class RGTextureAccess : uint8_t
    {
        Sampled,
        StorageRead,
        StorageWrite,
        ColorTarget,
        DepthTarget,
        CopySource,
        CopyDestination
    };

    enum class RGBufferAccess : uint8_t
    {
        Read,
        IndirectRead,
        Write,
        AccelerationStructureBuild,
        AccelerationStructureBuildInput,
        AccelerationStructureRead,
        CopySource,
        CopyDestination
    };

    enum class RGSynchronization : uint8_t
    {
        Blanket,
        Precise
    };

    enum class RGPassFlags : uint32_t
    {
        None = 0,
        Raster = 1 << 0,
        NeverCull = 1 << 1,
        RayTracing = 1 << 2,
        AsyncCompute = 1 << 3
    };

    inline RGPassFlags operator|(RGPassFlags a, RGPassFlags b)
    {
        return static_cast<RGPassFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    inline bool HasFlag(RGPassFlags flags, RGPassFlags flag)
    {
        return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) != 0;
    }

    enum class RGViewport : uint8_t
    {
        FlippedY,
        None
    };

    inline bool IsWrite(RGTextureAccess access)
    {
        return access == RGTextureAccess::StorageWrite || access == RGTextureAccess::ColorTarget ||
               access == RGTextureAccess::DepthTarget || access == RGTextureAccess::CopyDestination;
    }

    inline bool IsWrite(RGBufferAccess access)
    {
        return access == RGBufferAccess::Write || access == RGBufferAccess::AccelerationStructureBuild ||
               access == RGBufferAccess::CopyDestination;
    }

    enum class RGLogLevel : uint8_t
    {
        Info = 0,
        Warn = 1,
        Error = 2
    };

    using RGLogCallback = void (*)(RGLogLevel level, std::string_view message, void* userData);

    // Optional profiler integration hooks (no-op when function pointers are null).
    struct RGProfilerHooks
    {
        void* UserData = nullptr;
        uint32_t (*RegisterNamedScope)(const char* name, void* userData) = nullptr;
        void (*BeginGpuFrame)(uint32_t frameSlot, void* userData) = nullptr;
        void (*RecordGpuFrameBegin)(CommandBuffer& cmd, void* userData) = nullptr;
        void (*RecordGpuFrameEnd)(CommandBuffer& cmd, void* userData) = nullptr;
        void (*EndGpuFrame)(std::span<void* const> chunkContexts, void* userData) = nullptr;
        void* (*CreateChunkContext)(void* userData) = nullptr;
        void (*DestroyChunkContext)(void* chunkContext, void* userData) = nullptr;
        void (*SetChunkContextAsyncCompute)(void* chunkContext, bool asyncCompute, void* userData) = nullptr;
        void (*BeginCpuZone)(uint32_t scopeId, const char* name, void* userData) = nullptr;
        void (*EndCpuZone)(uint32_t scopeId, void* userData) = nullptr;
        void (*BeginGpuZone)(void* chunkContext, CommandBuffer& cmd, uint32_t scopeId, const char* name, void* userData) = nullptr;
        void (*EndGpuZone)(void* chunkContext, CommandBuffer& cmd, void* userData) = nullptr;
        void (*BeginGpuStatistics)(void* chunkContext, CommandBuffer& cmd, uint32_t scopeId, void* userData) = nullptr;
        void (*EndGpuStatistics)(void* chunkContext, CommandBuffer& cmd, void* userData) = nullptr;
    };

    // Optional multi-threaded task dispatcher (defaults to synchronous single-thread execution).
    struct RGTaskDispatcher
    {
        static constexpr uint32_t MaxExclusiveKeys = 32;
        virtual ~RGTaskDispatcher() = default;
        virtual uint32_t GetWorkerCount() const { return 1; }
        virtual void RunTasks(std::string_view name, std::span<const uint32_t> exclusiveMasks,
                              const std::function<void(uint32_t index)>& task)
        {
            (void)name;
            for (uint32_t i = 0; i < static_cast<uint32_t>(exclusiveMasks.size()); ++i)
                task(i);
        }
    };
}
