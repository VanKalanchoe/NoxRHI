#pragma once

#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include <slang/slang.h>
#include <slang/slang-com-ptr.h>

#include "ShaderCompiler.h"

namespace NRI
{
    class SlangCompiler final : public ShaderCompiler
    {
    public:
        SlangCompiler();
        ~SlangCompiler() override = default;

        std::vector<char> compile(const std::string& path, std::string_view entryPoint = {}) override;
        uint64_t sourceHash(const std::string& path) override;
        bool canCompile() override;
        void setTargetAPI(GraphicsAPI api) override { m_targetAPI = api; }
        GraphicsAPI getTargetAPI() const override { return m_targetAPI; }
        void setSearchPaths(std::span<const std::string> paths) override;
        void addSearchPath(std::string_view path) override;

    private:
        Slang::ComPtr<slang::IGlobalSession> acquireSession();
        void releaseSession(Slang::ComPtr<slang::IGlobalSession> session);

        std::mutex m_sessionMutex;
        std::condition_variable m_sessionFree;
        std::vector<Slang::ComPtr<slang::IGlobalSession>> m_freeSessions;
        uint32_t m_sessionCount = 0;
        uint32_t m_maxSessions = 1;
        std::once_flag m_availableOnce;
        int m_available = -1; // -1 not checked yet, 0 / 1
        GraphicsAPI m_targetAPI = GraphicsAPI::Vulkan;
        std::mutex m_searchPathsMutex;
        std::vector<std::string> m_searchPaths{ ".", "shaders" };
    };
}
