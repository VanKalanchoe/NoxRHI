#include <slang-tag-version.h>
#include "PushLayoutCheck.h"
#include "SlangCompiler.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <thread>
#include <unordered_set>

#include "../../Core/Hash.h"
#include "../../Core/Log.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace NRI
{
    namespace
    {
        ShaderProgress* s_ShaderProgress = nullptr;
    }

    void SetShaderProgress(ShaderProgress* progress)
    {
        s_ShaderProgress = progress;
    }

    ShaderProgress* GetShaderProgress()
    {
        return s_ShaderProgress;
    }

    std::unique_ptr<ShaderCompiler> CreateSlangCompiler()
    {
        return std::make_unique<SlangCompiler>();
    }

    SlangCompiler::SlangCompiler()
    {
        const unsigned hardware = std::thread::hardware_concurrency();
        m_maxSessions = std::clamp(hardware / 2u, 1u, 6u);
    }

    void SlangCompiler::setSearchPaths(std::span<const std::string> paths)
    {
        std::lock_guard lock(m_searchPathsMutex);
        m_searchPaths.assign(paths.begin(), paths.end());
    }

    void SlangCompiler::addSearchPath(std::string_view path)
    {
        std::lock_guard lock(m_searchPathsMutex);
        m_searchPaths.emplace_back(path);
    }

    bool SlangCompiler::canCompile()
    {
        std::call_once(m_availableOnce, [this]
        {
#ifdef _WIN32
            // Delay-loaded: calling into it without the DLL would end the process, so look for it first.
            HMODULE module = LoadLibraryExW(L"slang.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            m_available = module != nullptr ? 1 : 0;
            if (module)
                FreeLibrary(module);
#else
            m_available = 1;
#endif
            if (!m_available)
                NRI_LOG_INFO("Slang is not shipped: shaders come from the precompiled cache only");
        });
        return m_available == 1;
    }

    Slang::ComPtr<slang::IGlobalSession> SlangCompiler::acquireSession()
    {
        if (!canCompile())
            return nullptr;
        std::unique_lock lock(m_sessionMutex);
        m_sessionFree.wait(lock, [this] { return !m_freeSessions.empty() || m_sessionCount < m_maxSessions; });
        if (!m_freeSessions.empty())
        {
            Slang::ComPtr<slang::IGlobalSession> session = m_freeSessions.back();
            m_freeSessions.pop_back();
            return session;
        }
        Slang::ComPtr<slang::IGlobalSession> session;
        slang::createGlobalSession(session.writeRef());
        if (session)
            ++m_sessionCount;
        return session;
    }

    void SlangCompiler::releaseSession(Slang::ComPtr<slang::IGlobalSession> session)
    {
        {
            std::lock_guard lock(m_sessionMutex);
            m_freeSessions.push_back(std::move(session));
        }
        m_sessionFree.notify_one();
    }

    static uint64_t hashSourceAndIncludes(const std::filesystem::path& source,
                                          const std::vector<std::string>& searchPaths,
                                          std::unordered_set<std::string>& visited)
    {
        if (!visited.insert(source.lexically_normal().string()).second)
            return 0;

        uint64_t hash = Hash::computeFile(source.string());

        std::ifstream file(source);
        std::string line;
        while (std::getline(file, line))
        {
            const size_t include = line.find("#include");
            if (include == std::string::npos)
                continue;

            const size_t begin = line.find('"', include);
            const size_t end = begin == std::string::npos ? std::string::npos : line.find('"', begin + 1);
            if (end == std::string::npos)
                continue;

            const std::string included = line.substr(begin + 1, end - begin - 1);
            std::filesystem::path resolved = source.parent_path() / included;
            for (const std::string& searchPath : searchPaths)
            {
                if (std::filesystem::exists(resolved))
                    break;
                resolved = std::filesystem::path(searchPath) / included;
            }

            if (std::filesystem::exists(resolved))
                hash = Hash::compute(&hash, sizeof(hash), hashSourceAndIncludes(resolved, searchPaths, visited));
        }

        return hash;
    }

    uint64_t SlangCompiler::sourceHash(const std::string& path)
    {
        std::vector<std::string> searchPathsCopy;
        {
            std::lock_guard lock(m_searchPathsMutex);
            searchPathsCopy = m_searchPaths;
        }
        std::unordered_set<std::string> visited;
        const uint64_t sourceHash = hashSourceAndIncludes(path, searchPathsCopy, visited);
        // The compiler is part of the key: binaries cached by another Slang build are compiled again (a Slang update must not
        // keep serving the old compiler's SPIR-V / DXIL from the cache).
        // (the version NoxRHI was built against -- a header define, so it needs no slang.dll: a game shipped without Slang finds the
        // binaries the build compiled)
        // ("entry-point code": Vulkan SPIR-V holds one entry point each since compileEntryPoints -- binaries of whole-file modules are not
        // served again)
        static const uint64_t compilerHash = Hash::compute(std::string("slang " SLANG_TAG_VERSION " entry-point code"));
        return Hash::compute(&compilerHash, sizeof(compilerHash), sourceHash);
    }

    Slang::ComPtr<slang::IModule> SlangCompiler::loadModule(const std::string& path, slang::IGlobalSession* globalSession, Slang::ComPtr<slang::ISession>& session)
    {
        auto slangTargets{
            std::to_array<slang::TargetDesc>({
                {
                    .format{m_targetAPI == GraphicsAPI::D3D12 ? SLANG_DXIL : SLANG_SPIRV},
                    .profile{globalSession->findProfile(m_targetAPI == GraphicsAPI::D3D12 ? "sm_6_6" : "spirv_1_6")}
                }
            })
        };

        std::vector<slang::CompilerOptionEntry> slangOptions;
        slangOptions.push_back({ slang::CompilerOptionName::GLSLForceScalarLayout, { slang::CompilerOptionValueKind::Int, 1 } });
        if (m_targetAPI != GraphicsAPI::D3D12)
        {
            slangOptions.push_back({
                slang::CompilerOptionName::MacroDefine,
                {
                    .kind = slang::CompilerOptionValueKind::String,
                    .stringValue0 = "NOX_VULKAN",
                    .stringValue1 = "1"
                }
            });
            slangOptions.push_back({ slang::CompilerOptionName::EmitSpirvDirectly, { slang::CompilerOptionValueKind::Int, 1 } });
            slangOptions.push_back({ slang::CompilerOptionName::VulkanUseEntryPointName, { slang::CompilerOptionValueKind::Int, 1 } });
            slangOptions.push_back({ slang::CompilerOptionName::Capability, { slang::CompilerOptionValueKind::Int, globalSession->findCapability("spvDescriptorHeapEXT") } });
            slangOptions.push_back({ slang::CompilerOptionName::Capability, { slang::CompilerOptionValueKind::Int, globalSession->findCapability("spvRayQueryKHR") } });
        }
        else
        {
            slangOptions.push_back({
                slang::CompilerOptionName::MacroDefine,
                {
                    .kind = slang::CompilerOptionValueKind::String,
                    .stringValue0 = "NOX_DX12",
                    .stringValue1 = "1"
                }
            });
            slangOptions.push_back({
                slang::CompilerOptionName::DownstreamArgs,
                {
                    .kind = slang::CompilerOptionValueKind::String,
                    .intValue0 = 0,
                    .intValue1 = 0,
                    .stringValue0 = "dxc",
                    .stringValue1 = "-enable-16bit-types\n-HV\n2021"
                }
            });
        }

        std::vector<std::string> searchPathsCopy;
        {
            std::lock_guard lock(m_searchPathsMutex);
            searchPathsCopy = m_searchPaths;
        }
        std::vector<const char*> searchPathPtrs;
        searchPathPtrs.reserve(searchPathsCopy.size());
        for (const std::string& p : searchPathsCopy)
            searchPathPtrs.push_back(p.c_str());

        slang::SessionDesc slangSessionDesc
        {
            .targets{slangTargets.data()},
            .targetCount{SlangInt(slangTargets.size())},
            .defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR,
            .searchPaths = searchPathPtrs.empty() ? nullptr : searchPathPtrs.data(),
            .searchPathCount = SlangInt(searchPathPtrs.size()),
            .compilerOptionEntries{slangOptions.data()},
            .compilerOptionEntryCount{uint32_t(slangOptions.size())},
        };

        globalSession->createSession(slangSessionDesc, session.writeRef());
        if (!session)
            throw std::runtime_error("Slang failed to create a session for: " + path);

        std::string moduleName = std::filesystem::path(path).stem().string();

        std::ifstream sourceStream(path, std::ios::binary);
        if (!sourceStream)
            throw std::runtime_error("Slang failed to open shader source file: " + path);
        std::string sourceCode((std::istreambuf_iterator<char>(sourceStream)), std::istreambuf_iterator<char>());

        Slang::ComPtr<slang::IBlob> diagnostics;
        Slang::ComPtr<slang::IModule> slangModule
        {
            session->loadModuleFromSourceString(moduleName.c_str(), path.c_str(), sourceCode.c_str(), diagnostics.writeRef())
        };

        std::string diagnosticText;
        if (diagnostics)
        {
            diagnosticText = static_cast<const char*>(diagnostics->getBufferPointer());
            // (warnings alone come with a loaded module: logged as warnings, so they never count as errors)
            if (slangModule)
                NRI_LOG_WARN("Slang compile diagnostics ({}): {}", path, diagnosticText);
            else
                NRI_LOG_ERROR("Slang compile diagnostics ({}): {}", path, diagnosticText);
        }

        if (!slangModule)
        {
            std::string firstError;
            if (const size_t error = diagnosticText.find("error"); error != std::string::npos)
            {
                const size_t end = diagnosticText.find("\n   |", error);
                firstError = diagnosticText.substr(error, end == std::string::npos ? std::string::npos : end - error);
                if (const size_t message = diagnosticText.find("^", end == std::string::npos ? error : end); message != std::string::npos)
                {
                    const size_t lineEnd = diagnosticText.find('\n', message);
                    std::string caret = diagnosticText.substr(message, lineEnd == std::string::npos ? std::string::npos : lineEnd - message);
                    caret.erase(0, caret.find_first_not_of("^ "));
                    if (!caret.empty())
                        firstError += " -- " + caret;
                }
                if (firstError.size() > 600)
                    firstError.resize(600);
            }
            throw std::runtime_error("Slang failed to load module: " + path + (firstError.empty() ? std::string() : "\n" + firstError));
        }
        return slangModule;
    }

    std::vector<char> SlangCompiler::compile(const std::string& path, std::string_view entryPoint)
    {
        if (!std::filesystem::exists(path))
            NRI_LOG_ERROR("SlangCompiler::compile file not found: {}", path);
        if (s_ShaderProgress && s_ShaderProgress->OnCompile)
            s_ShaderProgress->OnCompile(path);
        Slang::ComPtr<slang::IGlobalSession> globalSession = acquireSession();
        if (!globalSession)
            throw std::runtime_error("No precompiled binary for '" + path + "' and slang.dll is not available");

        struct SessionReturn
        {
            SlangCompiler* Compiler;
            Slang::ComPtr<slang::IGlobalSession> Session;
            ~SessionReturn() { Compiler->releaseSession(std::move(Session)); }
        } sessionReturn{ this, globalSession };

        Slang::ComPtr<slang::ISession> session;
        Slang::ComPtr<slang::IModule> slangModule = loadModule(path, globalSession.get(), session);

        Slang::ComPtr<ISlangBlob> targetCode;
        Slang::ComPtr<slang::IBlob> targetDiagnostics;
        SlangResult result = SLANG_OK;

        if (!entryPoint.empty())
        {
            std::string entryName(entryPoint);
            Slang::ComPtr<slang::IEntryPoint> ep;
            slangModule->findEntryPointByName(entryName.c_str(), ep.writeRef());
            if (!ep)
                throw std::runtime_error("Slang entry point '" + entryName + "' not found in: " + path);

            slang::IComponentType* components[] = { slangModule.get(), ep.get() };
            Slang::ComPtr<slang::IComponentType> composed;
            Slang::ComPtr<slang::IBlob> composeDiags;
            result = session->createCompositeComponentType(components, 2, composed.writeRef(), composeDiags.writeRef());
            if (composeDiags)
            {
                if (SLANG_FAILED(result) || !composed)
                    NRI_LOG_ERROR("Slang compose diagnostics ({}:{}): {}", path, entryName,
                                  static_cast<const char*>(composeDiags->getBufferPointer()));
                else
                    NRI_LOG_WARN("Slang compose diagnostics ({}:{}): {}", path, entryName,
                              static_cast<const char*>(composeDiags->getBufferPointer()));
            }
            if (SLANG_FAILED(result) || !composed)
                throw std::runtime_error("Slang failed to compose entry point '" + entryName + "' in: " + path);

            Slang::ComPtr<slang::IComponentType> linked;
            Slang::ComPtr<slang::IBlob> linkDiags;
            result = composed->link(linked.writeRef(), linkDiags.writeRef());
            if (linkDiags)
            {
                if (SLANG_FAILED(result) || !linked)
                    NRI_LOG_ERROR("Slang link diagnostics ({}:{}): {}", path, entryName,
                                  static_cast<const char*>(linkDiags->getBufferPointer()));
                else
                    NRI_LOG_WARN("Slang link diagnostics ({}:{}): {}", path, entryName,
                              static_cast<const char*>(linkDiags->getBufferPointer()));
            }
            if (SLANG_FAILED(result) || !linked)
                throw std::runtime_error("Slang failed to link entry point '" + entryName + "' in: " + path);

            // D3D12 packs push data as a constant buffer; Vulkan (scalar layout) packs it like the host's C++ struct. A difference reads
            // the wrong bytes on D3D12 with no other error, so it is reported here.
            if (m_targetAPI == GraphicsAPI::D3D12)
            {
                if (const std::string difference = CheckPushLayoutMatchesScalar(linked.get()); !difference.empty())
                    NRI_LOG_ERROR("Push data of '{}' ({}) differs between the APIs: {}. D3D12 packs push data as a constant buffer -- a struct "
                                  "member starts a new 16-byte row and a vector may not cross one -- while Vulkan packs it tightly. Keep "
                                  "structs out of push data (a uint64_t reference with a property, see nox_gpu.slang) and keep vectors "
                                  "inside 16-byte rows.", entryName, path, difference);
            }

            result = linked->getEntryPointCode(0, 0, targetCode.writeRef(), targetDiagnostics.writeRef());
        }
        else
        {
            result = slangModule->getTargetCode(0, targetCode.writeRef(), targetDiagnostics.writeRef());
        }

        if (targetDiagnostics)
        {
            if (SLANG_FAILED(result) || !targetCode)
                NRI_LOG_ERROR("Slang target-code diagnostics ({}): {}", path, static_cast<const char*>(targetDiagnostics->getBufferPointer()));
            else
                NRI_LOG_WARN("Slang target-code diagnostics ({}): {}", path, static_cast<const char*>(targetDiagnostics->getBufferPointer()));
        }

        if (SLANG_FAILED(result) || !targetCode)
        {
            char hexBuf[32];
            std::snprintf(hexBuf, sizeof(hexBuf), "0x%08X", static_cast<unsigned int>(result));
            throw std::runtime_error("Slang failed to generate target code (" + std::string(hexBuf) + ", entry='" + std::string(entryPoint) + "', api=" + std::to_string(static_cast<int>(m_targetAPI)) + "): " + path);
        }

        const char* dataPtr = static_cast<const char*>(targetCode->getBufferPointer());
        size_t dataSize = targetCode->getBufferSize();

        return std::vector<char>(dataPtr, dataPtr + dataSize);
    }
    std::vector<std::pair<std::string, std::vector<char>>> SlangCompiler::compileEntryPoints(const std::string& path)
    {
        if (!std::filesystem::exists(path))
            NRI_LOG_ERROR("SlangCompiler::compileEntryPoints file not found: {}", path);
        if (s_ShaderProgress && s_ShaderProgress->OnCompile)
            s_ShaderProgress->OnCompile(path);
        Slang::ComPtr<slang::IGlobalSession> globalSession = acquireSession();
        if (!globalSession)
            throw std::runtime_error("No precompiled binary for '" + path + "' and slang.dll is not available");
        struct SessionReturn
        {
            SlangCompiler* Compiler;
            Slang::ComPtr<slang::IGlobalSession> Session;
            ~SessionReturn() { Compiler->releaseSession(std::move(Session)); }
        } sessionReturn{ this, globalSession };

        Slang::ComPtr<slang::ISession> session;
        Slang::ComPtr<slang::IModule> slangModule = loadModule(path, globalSession.get(), session);

        // The module and all of its entry points, linked once; then each entry point's code on its own (getEntryPointCode emits that
        // entry point and only what it uses).
        const SlangInt32 count = slangModule->getDefinedEntryPointCount();
        std::vector<Slang::ComPtr<slang::IEntryPoint>> entryPoints(size_t(std::max<SlangInt32>(count, 0)));
        std::vector<slang::IComponentType*> components{ slangModule.get() };
        for (SlangInt32 i = 0; i < count; ++i)
        {
            slangModule->getDefinedEntryPoint(i, entryPoints[size_t(i)].writeRef());
            if (!entryPoints[size_t(i)])
                throw std::runtime_error("Slang could not read entry point " + std::to_string(i) + " of: " + path);
            components.push_back(entryPoints[size_t(i)].get());
        }
        Slang::ComPtr<slang::IComponentType> composed;
        Slang::ComPtr<slang::IBlob> diagnostics;
        SlangResult result = session->createCompositeComponentType(components.data(), SlangInt(components.size()), composed.writeRef(), diagnostics.writeRef());
        if (SLANG_FAILED(result) || !composed)
            throw std::runtime_error("Slang failed to compose the entry points of: " + path +
                                     (diagnostics ? "\n" + std::string(static_cast<const char*>(diagnostics->getBufferPointer())) : std::string()));
        Slang::ComPtr<slang::IComponentType> linked;
        diagnostics = nullptr;
        result = composed->link(linked.writeRef(), diagnostics.writeRef());
        if (SLANG_FAILED(result) || !linked)
            throw std::runtime_error("Slang failed to link the entry points of: " + path +
                                     (diagnostics ? "\n" + std::string(static_cast<const char*>(diagnostics->getBufferPointer())) : std::string()));
        if (diagnostics)
            NRI_LOG_WARN("Slang link diagnostics ({}): {}", path, static_cast<const char*>(diagnostics->getBufferPointer()));

        slang::ProgramLayout* layout = linked->getLayout(0);
        std::vector<std::pair<std::string, std::vector<char>>> codes;
        codes.reserve(size_t(std::max<SlangInt32>(count, 0)));
        for (SlangInt32 i = 0; i < count; ++i)
        {
            slang::EntryPointReflection* reflection = layout ? layout->getEntryPointByIndex(SlangUInt(i)) : nullptr;
            const std::string name = reflection && reflection->getName() ? reflection->getName() : std::string();
            if (m_targetAPI == GraphicsAPI::D3D12)
            {
                // (the same push data check as compile(): this entry point alone)
                slang::IComponentType* single[] = { slangModule.get(), entryPoints[size_t(i)].get() };
                Slang::ComPtr<slang::IComponentType> singleComposed, singleLinked;
                if (SLANG_SUCCEEDED(session->createCompositeComponentType(single, 2, singleComposed.writeRef(), nullptr)) && singleComposed &&
                    SLANG_SUCCEEDED(singleComposed->link(singleLinked.writeRef(), nullptr)) && singleLinked)
                    if (const std::string difference = CheckPushLayoutMatchesScalar(singleLinked.get()); !difference.empty())
                        NRI_LOG_ERROR("Push data of '{}' ({}) differs between the APIs: {}. Keep structs out of push data (a uint64_t reference "
                                      "with a property, see nox_gpu.slang) and keep vectors inside 16-byte rows.", name, path, difference);
            }
            Slang::ComPtr<slang::IBlob> code;
            diagnostics = nullptr;
            result = linked->getEntryPointCode(i, 0, code.writeRef(), diagnostics.writeRef());
            if (SLANG_FAILED(result) || !code)
                throw std::runtime_error("Slang failed to generate code for entry point '" + name + "' of: " + path +
                                         (diagnostics ? "\n" + std::string(static_cast<const char*>(diagnostics->getBufferPointer())) : std::string()));
            if (diagnostics)
                NRI_LOG_WARN("Slang target-code diagnostics ({}:{}): {}", path, name, static_cast<const char*>(diagnostics->getBufferPointer()));
            const char* bytes = static_cast<const char*>(code->getBufferPointer());
            codes.emplace_back(name, std::vector<char>(bytes, bytes + code->getBufferSize()));
        }
        return codes;
    }
}
