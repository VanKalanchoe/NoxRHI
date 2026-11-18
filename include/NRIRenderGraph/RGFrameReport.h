#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "RGResourcePool.h"
#include "RGTypes.h"

namespace nri
{
    struct RGFrameReport
    {
        struct Row
        {
            const char* Name = nullptr;
            uint32_t Depth = 0;
            uint32_t Pass = RGInvalidIndex;
        };

        struct Access
        {
            uint32_t Resource = RGInvalidIndex;
            bool IsTexture = true;
            bool IsWrite = false;
        };

        struct Pass
        {
            const char* Name = nullptr;
            bool Culled = false;
            bool Raster = false;
            bool Presents = false;
            bool SynchronizeAfter = false;
            uint32_t Barriers = 0;
            uint32_t Chunk = 0;
            float RecordMs = 0.0f;
            uint32_t ExclusiveMask = 0;
            uint32_t FirstAccess = 0;
            uint32_t AccessCount = 0;
        };

        struct Resource
        {
            const char* Name = nullptr;
            RGResourceKind Kind = RGResourceKind::Imported;
            bool ReadOnly = false;
            RGTextureKey Texture;
            RGBufferKey Buffer;
            uint32_t FirstPass = RGInvalidIndex;
            uint32_t LastPass = 0;
            uint32_t PhysicalId = RGInvalidIndex;
            uint64_t Bytes = 0;
        };

        struct UnusedWrite
        {
            uint32_t Pass = 0;
            uint32_t Resource = 0;
            bool IsTexture = true;
        };

        uint64_t FrameNumber = 0;
        uint32_t CommandBuffers = 0;
        RGSynchronization Synchronization = RGSynchronization::Blanket;
        uint64_t PoolTextureBytes = 0;
        uint64_t PoolBufferBytes = 0;
        std::vector<Row> Rows;
        std::vector<Pass> Passes;
        std::vector<Access> Accesses;
        std::vector<Resource> Textures;
        std::vector<Resource> Buffers;
        std::vector<const char*> ExclusiveKeys;
        std::vector<std::string> ResetHistoryKeys;
        bool ResetAllHistory = false;
        std::vector<std::string> ValidationMessages;
        std::vector<UnusedWrite> UnusedWrites;
    };

    bool WriteRenderGraphDot(const RGFrameReport& report, const std::filesystem::path& path);
}
