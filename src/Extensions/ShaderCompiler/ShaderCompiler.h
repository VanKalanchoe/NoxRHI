#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../../Core/NRITypes.h"

namespace NRI
{
    struct ShaderProgress
    {
        std::function<void(const std::string& source)> OnCompile;
        std::function<void()> OnPipelineCreated;
        std::function<void()> OnIdle;
    };
    void SetShaderProgress(ShaderProgress* progress);
    ShaderProgress* GetShaderProgress();

    class ShaderCompiler
    {
    public:
        virtual ~ShaderCompiler() = default;

        virtual std::vector<char> compile(const std::string& path, std::string_view entryPoint = {}) = 0;
        virtual uint64_t sourceHash(const std::string& path) = 0;
        virtual bool canCompile() = 0;
        virtual void setTargetAPI(GraphicsAPI api) {}
        virtual GraphicsAPI getTargetAPI() const { return GraphicsAPI::Vulkan; }
        virtual void setSearchPaths(std::span<const std::string> paths) {}
        virtual void addSearchPath(std::string_view path) {}
    };

    std::unique_ptr<ShaderCompiler> CreateSlangCompiler();
}
