#pragma once
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>

namespace NRI
{
    class Hash
    {
    public:
        static uint64_t compute(const void* data, size_t size, uint64_t seed = 0)
        {
            // 64-bit FNV-1a seeded + Murmur3 finalizer avalanche
            uint64_t h = 14695981039346656037ULL ^ (seed * 0x9e3779b97f4a7c15ULL);
            const auto* bytes = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < size; ++i)
            {
                h ^= bytes[i];
                h *= 1099511628211ULL;
            }
            h ^= h >> 33;
            h *= 0xff51afd7ed558ccdULL;
            h ^= h >> 33;
            h *= 0xc4ceb9fe1a85ec53ULL;
            h ^= h >> 33;
            return h;
        }

        static uint64_t compute(const std::string& str, uint64_t seed = 0)
        {
            return compute(str.data(), str.size(), seed);
        }

        static uint64_t computeFile(const std::string& path, uint64_t seed = 0)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file)
                return seed;
            const std::streamsize size = file.tellg();
            if (size <= 0)
                return seed;
            file.seekg(0, std::ios::beg);
            std::string buf(static_cast<size_t>(size), '\0');
            if (!file.read(buf.data(), size))
                return seed;
            return compute(buf.data(), buf.size(), seed);
        }
    };
}
