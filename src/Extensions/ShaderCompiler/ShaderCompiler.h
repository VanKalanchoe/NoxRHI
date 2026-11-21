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
        // Every entry point of a file, each as target code of its own -- one entry point and only the variables it uses -- from one load
        // and one link of the file. Vulkan creates its shaders from these: a SPIR-V module holding a file's other entry points gave the
        // driver their variables too (MeshletCulling's mesh shaders read garbage from the task payload on an NVIDIA GPU while two task
        // shaders shared the module), and Slang emits invalid SPIR-V for task shaders with different payload types in one module.
        // Empty when the compiler cannot (the caller compiles entry points one at a time).
        virtual std::vector<std::pair<std::string, std::vector<char>>> compileEntryPoints(const std::string& path) { (void)path; return {}; }
        virtual uint64_t sourceHash(const std::string& path) = 0;
        virtual bool canCompile() = 0;
        virtual void setTargetAPI(GraphicsAPI api) {}
        virtual GraphicsAPI getTargetAPI() const { return GraphicsAPI::Vulkan; }
        virtual void setSearchPaths(std::span<const std::string> paths) {}
        virtual void addSearchPath(std::string_view path) {}
        // A preprocessor define for every shader compiled from now on (an empty value removes it); part of sourceHash, so cached
        // binaries compiled with other defines are not served. NOX_SHADER_CHECKS turns on nox_gpu.slang's GPU checks.
        virtual void setDefine(std::string_view name, std::string_view value) { (void)name; (void)value; }
    };

    std::unique_ptr<ShaderCompiler> CreateSlangCompiler();
}
