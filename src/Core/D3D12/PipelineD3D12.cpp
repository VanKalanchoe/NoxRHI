#include "PipelineD3D12.h"
#include "DeviceD3D12.h"
#include "TextureD3D12.h"
#include "../../Extensions/ShaderCompiler/ShaderCompiler.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

namespace NRI
{
    namespace
    {
        D3D12_COMPARISON_FUNC translateCompareOp(CompareOp op)
        {
            switch (op)
            {
            case CompareOp::Never:        return D3D12_COMPARISON_FUNC_NEVER;
            case CompareOp::Less:         return D3D12_COMPARISON_FUNC_LESS;
            case CompareOp::Equal:        return D3D12_COMPARISON_FUNC_EQUAL;
            case CompareOp::LessOrEqual:    return D3D12_COMPARISON_FUNC_LESS_EQUAL;
            case CompareOp::Greater:        return D3D12_COMPARISON_FUNC_GREATER;
            case CompareOp::NotEqual:       return D3D12_COMPARISON_FUNC_NOT_EQUAL;
            case CompareOp::GreaterOrEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
            case CompareOp::Always:       return D3D12_COMPARISON_FUNC_ALWAYS;
            default:                      return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
            }
        }

        D3D12_CULL_MODE translateCullMode(CullMode mode)
        {
            switch (mode)
            {
            case CullMode::None:  return D3D12_CULL_MODE_NONE;
            case CullMode::Front: return D3D12_CULL_MODE_FRONT;
            case CullMode::Back:
            default:              return D3D12_CULL_MODE_BACK;
            }
        }

        D3D12_FILL_MODE translatePolygonMode(PolygonMode mode)
        {
            return mode == PolygonMode::Line ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
        }

        D3D12_BLEND translateBlendFactor(BlendFactor factor, bool isAlpha)
        {
            switch (factor)
            {
            case BlendFactor::Zero:             return D3D12_BLEND_ZERO;
            case BlendFactor::One:              return D3D12_BLEND_ONE;
            case BlendFactor::SrcAlpha:         return D3D12_BLEND_SRC_ALPHA;
            case BlendFactor::OneMinusSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
            case BlendFactor::SrcColor:         return isAlpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
            case BlendFactor::OneMinusSrcColor: return isAlpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
            default:                            return D3D12_BLEND_ONE;
            }
        }

        D3D12_BLEND_OP translateBlendOp(BlendOp op)
        {
            switch (op)
            {
            case BlendOp::Add: return D3D12_BLEND_OP_ADD;
            default:           return D3D12_BLEND_OP_ADD;
            }
        }

        bool isIntegerDxgiFormat(DXGI_FORMAT fmt)
        {
            switch (fmt)
            {
            case DXGI_FORMAT_R8_UINT:
            case DXGI_FORMAT_R8_SINT:
            case DXGI_FORMAT_R8G8_UINT:
            case DXGI_FORMAT_R8G8_SINT:
            case DXGI_FORMAT_R8G8B8A8_UINT:
            case DXGI_FORMAT_R8G8B8A8_SINT:
            case DXGI_FORMAT_R16_UINT:
            case DXGI_FORMAT_R16_SINT:
            case DXGI_FORMAT_R16G16_UINT:
            case DXGI_FORMAT_R16G16_SINT:
            case DXGI_FORMAT_R16G16B16A16_UINT:
            case DXGI_FORMAT_R16G16B16A16_SINT:
            case DXGI_FORMAT_R32_UINT:
            case DXGI_FORMAT_R32_SINT:
            case DXGI_FORMAT_R32G32_UINT:
            case DXGI_FORMAT_R32G32_SINT:
            case DXGI_FORMAT_R32G32B32_UINT:
            case DXGI_FORMAT_R32G32B32_SINT:
            case DXGI_FORMAT_R32G32B32A32_UINT:
            case DXGI_FORMAT_R32G32B32A32_SINT:
            case DXGI_FORMAT_R10G10B10A2_UINT:
                return true;
            default:
                return false;
            }
        }

        D3D12_BLEND_DESC buildBlendDesc(const std::vector<DXGI_FORMAT>& rtvFormats,
                                        const std::array<bool, 8>& colorBlendEnable,
                                        const std::array<ColorBlendEquation, 8>& eq,
                                        const std::array<uint32_t, 8>& colorWriteMask)
        {
            D3D12_BLEND_DESC blend{};
            const size_t count = std::min<size_t>(std::max<size_t>(rtvFormats.size(), 1), 8);
            blend.IndependentBlendEnable = (rtvFormats.size() > 1) ? TRUE : FALSE;
            for (size_t i = 0; i < count; ++i)
            {
                D3D12_RENDER_TARGET_BLEND_DESC& rt = blend.RenderTarget[i];
                const bool isInt = (i < rtvFormats.size()) && isIntegerDxgiFormat(rtvFormats[i]);
                // D3D12 forbids BlendEnable = TRUE on integer (_UINT / _SINT) render target formats.
                rt.BlendEnable = (colorBlendEnable[i] && !isInt) ? TRUE : FALSE;
                rt.SrcBlend = translateBlendFactor(eq[i].srcColorBlendFactor, false);
                rt.DestBlend = translateBlendFactor(eq[i].dstColorBlendFactor, false);
                rt.BlendOp = translateBlendOp(eq[i].colorBlendOp);
                rt.SrcBlendAlpha = translateBlendFactor(eq[i].srcAlphaBlendFactor, true);
                rt.DestBlendAlpha = translateBlendFactor(eq[i].dstAlphaBlendFactor, true);
                rt.BlendOpAlpha = translateBlendOp(eq[i].alphaBlendOp);
                rt.RenderTargetWriteMask = static_cast<UINT8>(colorWriteMask[i] & 0xFu);
            }
            return blend;
        }

        uint64_t makeGraphicsVariantKey(size_t attachmentCount,
                                        bool hasDepth, bool depthTestEnable, bool depthWriteEnable,
                                        CompareOp depthCompareOp, CullMode cullMode,
                                        FrontFace frontFace, PolygonMode polygonMode,
                                        const std::array<bool, 8>& colorBlendEnable,
                                        const std::array<ColorBlendEquation, 8>& colorBlendEquation,
                                        const std::array<uint32_t, 8>& colorWriteMask,
                                        uint32_t sampleCount,
                                        bool depthClampEnable)
        {
            uint64_t hash = 14695981039346656037ull;
            auto mix = [&](uint64_t v)
            {
                hash ^= v;
                hash *= 1099511628211ull;
            };
            mix((hasDepth ? 1ull : 0ull) |
                ((depthTestEnable ? 1ull : 0ull) << 1) |
                ((depthWriteEnable ? 1ull : 0ull) << 2) |
                ((static_cast<uint64_t>(depthCompareOp) & 0xFu) << 3) |
                ((static_cast<uint64_t>(cullMode) & 0x3u) << 7) |
                ((static_cast<uint64_t>(frontFace) & 0x3u) << 9) |
                ((static_cast<uint64_t>(polygonMode) & 0x3u) << 11) |
                ((static_cast<uint64_t>(sampleCount) & 0x7Fu) << 13) |
                ((depthClampEnable ? 1ull : 0ull) << 20));
            const size_t count = std::min<size_t>(std::max<size_t>(attachmentCount, 1), 8);
            for (size_t i = 0; i < count; ++i)
            {
                uint64_t att = (colorBlendEnable[i] ? 1ull : 0ull) |
                               ((static_cast<uint64_t>(colorWriteMask[i]) & 0xFu) << 1);
                if (colorBlendEnable[i])
                {
                    att |= (static_cast<uint64_t>(colorBlendEquation[i].srcColorBlendFactor) & 0xFu) << 5;
                    att |= (static_cast<uint64_t>(colorBlendEquation[i].dstColorBlendFactor) & 0xFu) << 9;
                    att |= (static_cast<uint64_t>(colorBlendEquation[i].colorBlendOp) & 0x3u) << 13;
                    att |= (static_cast<uint64_t>(colorBlendEquation[i].srcAlphaBlendFactor) & 0xFu) << 15;
                    att |= (static_cast<uint64_t>(colorBlendEquation[i].dstAlphaBlendFactor) & 0xFu) << 19;
                    att |= (static_cast<uint64_t>(colorBlendEquation[i].alphaBlendOp) & 0x3u) << 23;
                }
                mix(att);
            }
            return hash;
        }

        const char* stageShortName(ShaderStage stage)
        {
            switch (stage)
            {
            case ShaderStage::Vertex:       return "vert";
            case ShaderStage::Fragment:     return "frag";
            case ShaderStage::Compute:      return "comp";
            case ShaderStage::Task:         return "task";
            case ShaderStage::Mesh:         return "mesh";
            case ShaderStage::RayGen:       return "rgen";
            case ShaderStage::Miss:         return "rmiss";
            case ShaderStage::ClosestHit:   return "rchit";
            case ShaderStage::AnyHit:       return "rahit";
            case ShaderStage::Intersection: return "rint";
            case ShaderStage::Callable:     return "rcall";
            default:                        return "unknown";
            }
        }

        struct ModuleCompile
        {
            std::mutex Mutex;
            std::condition_variable Done;
            bool Finished = false;
            std::vector<std::pair<std::string, std::vector<char>>> EntryPoints;
            std::exception_ptr Error;
        };
        std::mutex s_modulesMutex;
        std::unordered_map<std::string, std::shared_ptr<ModuleCompile>> s_modules;

        std::shared_ptr<ModuleCompile> compileModuleOnce(const std::string& sourcePath, ShaderCompiler& compiler)
        {
            const std::string key = sourcePath + "|" + std::to_string(compiler.sourceHash(sourcePath));
            std::shared_ptr<ModuleCompile> entry;
            bool compileHere = false;
            {
                std::lock_guard lock(s_modulesMutex);
                std::shared_ptr<ModuleCompile>& slot = s_modules[key];
                if (!slot || (slot->Finished && slot->Error))
                {
                    slot = std::make_shared<ModuleCompile>();
                    compileHere = true;
                }
                entry = slot;
            }
            if (compileHere)
            {
                std::vector<std::pair<std::string, std::vector<char>>> entryPoints;
                std::exception_ptr error;
                const auto started = std::chrono::steady_clock::now();
                try
                {
                    entryPoints = compiler.compileEntryPoints(sourcePath);
                }
                catch (...)
                {
                    error = std::current_exception();
                }
                if (!error)
                    NRI_LOG_INFO("Shader compiled: {} ({:.0f} ms)", sourcePath,
                                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
                {
                    std::lock_guard lock(entry->Mutex);
                    entry->EntryPoints = std::move(entryPoints);
                    entry->Error = error;
                    entry->Finished = true;
                }
                entry->Done.notify_all();
            }
            std::unique_lock lock(entry->Mutex);
            entry->Done.wait(lock, [&] { return entry->Finished; });
            if (entry->Error)
                std::rethrow_exception(entry->Error);
            return entry;
        }

        std::vector<char> compileEntryPointOnce(const std::string& sourcePath, const std::string& entryPoint, ShaderCompiler& compiler)
        {
            const std::shared_ptr<ModuleCompile> module = compileModuleOnce(sourcePath, compiler);
            {
                std::unique_lock lock(module->Mutex);
                if (module->EntryPoints.empty())
                {
                    lock.unlock();
                    return compiler.compile(sourcePath, entryPoint);
                }
            }
            std::lock_guard lock(module->Mutex);
            for (const auto& [name, dxil] : module->EntryPoints)
                if (name == entryPoint)
                    return dxil;
            std::string names;
            for (const auto& [name, dxil] : module->EntryPoints)
                names += (names.empty() ? "" : ", ") + name;
            throw std::runtime_error("Shader entry point '" + entryPoint + "' not found in " + sourcePath + " (it has: " + names + ")");
        }

        std::vector<char> loadOrCompileDxil(const ShaderStageDesc& shaderDesc,
                                              ShaderCompiler& compiler,
                                              const std::string& cacheDir,
                                              bool forceCompile)
        {
            const std::filesystem::path source(shaderDesc.sourcePath);
            const std::string stem = source.stem().string();
            const uint64_t sourceHash = compiler.sourceHash(shaderDesc.sourcePath);
            const std::filesystem::path folder = std::filesystem::path(cacheDir) / stem;
            const std::filesystem::path binaryPath =
                folder / (stem + "." + stageShortName(shaderDesc.stage) + "." + shaderDesc.entryPoint + "." + std::to_string(sourceHash) + ".dxil.bin");

            if (std::filesystem::exists(binaryPath) && (!forceCompile || !compiler.canCompile()))
            {
                std::ifstream file(binaryPath, std::ios::binary | std::ios::ate);
                if (file)
                {
                    const size_t size = static_cast<size_t>(file.tellg());
                    file.seekg(0);
                    std::vector<char> data(size);
                    if (size > 0)
                        file.read(data.data(), static_cast<std::streamsize>(size));
                    if (!data.empty())
                    {
                        NRI_LOG_INFO("PipelineD3D12 loading shader binary: {}", binaryPath.string());
                        return data;
                    }
                }
            }

            if (forceCompile)
            {
                const std::string key = shaderDesc.sourcePath + "|" + std::to_string(sourceHash);
                std::lock_guard lock(s_modulesMutex);
                s_modules.erase(key);
            }

            NRI_LOG_INFO("PipelineD3D12 compiling shader: {} ({})", shaderDesc.sourcePath, shaderDesc.entryPoint);
            std::vector<char> bytecode = compileEntryPointOnce(shaderDesc.sourcePath, shaderDesc.entryPoint, compiler);

            if (!bytecode.empty())
            {
                std::error_code ec;
                std::filesystem::create_directories(folder, ec);
                std::ofstream out(binaryPath, std::ios::binary);
                if (out)
                {
                    out.write(bytecode.data(), static_cast<std::streamsize>(bytecode.size()));
                    NRI_LOG_INFO("PipelineD3D12 saving shader binary: {}", binaryPath.string());
                }
            }
            return bytecode;
        }
    }

    PipelineD3D12::PipelineD3D12(DeviceD3D12& device, const PipelineDesc& desc, ShaderCompiler& compiler)
        : m_deviceD3D12(device), m_type(desc.type)
    {
        if (m_type == PipelineType::Graphics)
            createGraphicsPipeline(desc, compiler);
        else if (m_type == PipelineType::Compute)
            createComputePipeline(desc, compiler);
        else
            createRayTracingPipeline(desc, compiler);
    }

    void PipelineD3D12::createGraphicsPipeline(const PipelineDesc& desc, ShaderCompiler& compiler)
    {
        m_hasMeshShaders = false;
        for (const auto& s : desc.shaders)
        {
            if (!m_debugName.empty())
                m_debugName += " + ";
            m_debugName += s.sourcePath + ":" + s.entryPoint;
            auto code = loadOrCompileDxil(s, compiler, m_deviceD3D12.getShaderCacheDir(), desc.forceCompile);
            if (s.stage == ShaderStage::Vertex) m_vsBytecode = std::move(code);
            else if (s.stage == ShaderStage::Fragment) m_psBytecode = std::move(code);
            else if (s.stage == ShaderStage::Task) { m_asBytecode = std::move(code); m_hasMeshShaders = true; }
            else if (s.stage == ShaderStage::Mesh) { m_msBytecode = std::move(code); m_hasMeshShaders = true; }
        }

        m_rtvFormats.reserve(desc.colorFormats.size());
        for (ImageFormat fmt : desc.colorFormats)
            m_rtvFormats.push_back(TranslateImageFormat(fmt));

        if (desc.buildNow)
        {
            // Prebuild the baseline depthless variant synchronously so compilation errors surface on creation.
            m_pipeline = resolveGraphicsPipeline(
                false, false, false, CompareOp::GreaterOrEqual,
                CullMode::None, FrontFace::CounterClockWise, PolygonMode::Fill);
        }
    }

    ComPtr<ID3D12PipelineState> PipelineD3D12::buildGraphicsVariant(bool hasDepth,
                                                                    bool depthTestEnable,
                                                                    bool depthWriteEnable,
                                                                    CompareOp depthCompareOp,
                                                                    CullMode cullMode,
                                                                    FrontFace frontFace,
                                                                    PolygonMode polygonMode,
                                                                    const std::array<bool, 8>& colorBlendEnable,
                                                                    const std::array<ColorBlendEquation, 8>& colorBlendEquation,
                                                                    const std::array<uint32_t, 8>& colorWriteMask,
                                                                    uint32_t sampleCount,
                                                                    bool depthClampEnable)
    {
        const BOOL effDepthEnable = (hasDepth && depthTestEnable) ? TRUE : FALSE;
        const D3D12_DEPTH_WRITE_MASK effDepthWrite = (hasDepth && depthWriteEnable)
            ? D3D12_DEPTH_WRITE_MASK_ALL
            : D3D12_DEPTH_WRITE_MASK_ZERO;
        const DXGI_FORMAT effDsvFormat = hasDepth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
        const BOOL frontCCW = (frontFace == FrontFace::CounterClockWise) ? TRUE : FALSE;
        const D3D12_BLEND_DESC blendDesc = buildBlendDesc(m_rtvFormats, colorBlendEnable, colorBlendEquation, colorWriteMask);

        ComPtr<ID3D12PipelineState> pso;
        if (m_hasMeshShaders)
        {
            struct alignas(void*) MeshPipelineStateStream
            {
                struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE }; ID3D12RootSignature* pRootSignature{ nullptr }; } RootSig;
                struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS }; D3D12_SHADER_BYTECODE bytecode{}; } AS;
                struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS }; D3D12_SHADER_BYTECODE bytecode{}; } MS;
                struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS }; D3D12_SHADER_BYTECODE bytecode{}; } PS;
                struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND }; D3D12_BLEND_DESC desc{}; } Blend;
                struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER }; D3D12_RASTERIZER_DESC desc{}; } Rasterizer;
                struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL }; D3D12_DEPTH_STENCIL_DESC desc{}; } DepthStencil;
                struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS }; D3D12_RT_FORMAT_ARRAY rts{}; } RTVFormats;
                struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT }; DXGI_FORMAT format{}; } DSVFormat;
                struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{ D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC }; DXGI_SAMPLE_DESC desc{ 1, 0 }; } SampleDesc;
            } stream;

            stream.RootSig.pRootSignature = m_deviceD3D12.getGlobalRootSignature();
            if (!m_asBytecode.empty())
                stream.AS.bytecode = { m_asBytecode.data(), m_asBytecode.size() };
            stream.MS.bytecode = { m_msBytecode.data(), m_msBytecode.size() };
            if (!m_psBytecode.empty())
                stream.PS.bytecode = { m_psBytecode.data(), m_psBytecode.size() };
            stream.Blend.desc = blendDesc;

            stream.Rasterizer.desc.FillMode = translatePolygonMode(polygonMode);
            stream.Rasterizer.desc.CullMode = translateCullMode(cullMode);
            stream.Rasterizer.desc.FrontCounterClockwise = frontCCW;
            stream.Rasterizer.desc.DepthClipEnable = depthClampEnable ? FALSE : TRUE;

            stream.DepthStencil.desc.DepthEnable = effDepthEnable;
            stream.DepthStencil.desc.DepthWriteMask = effDepthWrite;
            stream.DepthStencil.desc.DepthFunc = translateCompareOp(depthCompareOp);

            stream.RTVFormats.rts.NumRenderTargets = static_cast<UINT>(m_rtvFormats.size());
            for (size_t i = 0; i < m_rtvFormats.size() && i < 8; ++i)
                stream.RTVFormats.rts.RTFormats[i] = m_rtvFormats[i];
            stream.DSVFormat.format = effDsvFormat;
            stream.SampleDesc.desc.Count = std::max(1u, sampleCount);

            D3D12_PIPELINE_STATE_STREAM_DESC streamDesc{ sizeof(stream), &stream };
            ComPtr<ID3D12Device2> dev2;
            if (SUCCEEDED(m_deviceD3D12.getNativeDevice()->QueryInterface(IID_PPV_ARGS(&dev2))))
            {
                const std::string contextMsg = "CreatePipelineState (Mesh: " + m_debugName + ")";
                ThrowIfFailed(dev2->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&pso)), contextMsg.c_str());
            }
        }
        else
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
            psoDesc.pRootSignature = m_deviceD3D12.getGlobalRootSignature();
            if (!m_vsBytecode.empty())
                psoDesc.VS = { m_vsBytecode.data(), m_vsBytecode.size() };
            if (!m_psBytecode.empty())
                psoDesc.PS = { m_psBytecode.data(), m_psBytecode.size() };

            psoDesc.RasterizerState.FillMode = translatePolygonMode(polygonMode);
            psoDesc.RasterizerState.CullMode = translateCullMode(cullMode);
            psoDesc.RasterizerState.FrontCounterClockwise = frontCCW;
            psoDesc.RasterizerState.DepthClipEnable = depthClampEnable ? FALSE : TRUE;

            psoDesc.DepthStencilState.DepthEnable = effDepthEnable;
            psoDesc.DepthStencilState.DepthWriteMask = effDepthWrite;
            psoDesc.DepthStencilState.DepthFunc = translateCompareOp(depthCompareOp);

            psoDesc.BlendState = blendDesc;
            psoDesc.SampleMask = UINT_MAX;
            psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            psoDesc.NumRenderTargets = static_cast<UINT>(m_rtvFormats.size());
            for (size_t i = 0; i < m_rtvFormats.size() && i < 8; ++i)
                psoDesc.RTVFormats[i] = m_rtvFormats[i];
            psoDesc.DSVFormat = effDsvFormat;
            psoDesc.SampleDesc.Count = std::max(1u, sampleCount); // (must match the render targets' sample count)

            ThrowIfFailed(m_deviceD3D12.getNativeDevice()->CreateGraphicsPipelineState(
                &psoDesc, IID_PPV_ARGS(&pso)), "CreateGraphicsPipelineState");
        }
        return pso;
    }

    ID3D12PipelineState* PipelineD3D12::resolveGraphicsPipeline(bool hasDepth,
                                                                bool depthTestEnable,
                                                                bool depthWriteEnable,
                                                                CompareOp depthCompareOp,
                                                                CullMode cullMode,
                                                                FrontFace frontFace,
                                                                PolygonMode polygonMode,
                                                                const std::array<bool, 8>& colorBlendEnable,
                                                                const std::array<ColorBlendEquation, 8>& colorBlendEquation,
                                                                const std::array<uint32_t, 8>& colorWriteMask,
                                                                uint32_t sampleCount,
                                                                bool depthClampEnable)
    {
        const uint64_t key = makeGraphicsVariantKey(m_rtvFormats.size(),
                                                    hasDepth, depthTestEnable, depthWriteEnable,
                                                    depthCompareOp, cullMode, frontFace, polygonMode,
                                                    colorBlendEnable, colorBlendEquation, colorWriteMask, sampleCount,
                                                    depthClampEnable);
        std::scoped_lock lock(m_variantMutex);
        auto it = m_graphicsVariants.find(key);
        if (it != m_graphicsVariants.end())
            return it->second.Get();

        ComPtr<ID3D12PipelineState> built = buildGraphicsVariant(
            hasDepth, depthTestEnable, depthWriteEnable, depthCompareOp, cullMode, frontFace, polygonMode,
            colorBlendEnable, colorBlendEquation, colorWriteMask, sampleCount, depthClampEnable);
        ID3D12PipelineState* raw = built.Get();
        m_graphicsVariants.emplace(key, std::move(built));
        if (!m_pipeline)
            m_pipeline = raw;
        return raw;
    }

    void PipelineD3D12::createComputePipeline(const PipelineDesc& desc, ShaderCompiler& compiler)
    {
        std::vector<char> csBytecode;
        for (const auto& s : desc.shaders)
        {
            if (s.stage == ShaderStage::Compute)
            {
                csBytecode = loadOrCompileDxil(s, compiler, m_deviceD3D12.getShaderCacheDir(), desc.forceCompile);
                break;
            }
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = m_deviceD3D12.getGlobalRootSignature();
        psoDesc.CS = { csBytecode.data(), csBytecode.size() };

        ThrowIfFailed(m_deviceD3D12.getNativeDevice()->CreateComputePipelineState(
            &psoDesc, IID_PPV_ARGS(&m_pipeline)), "CreateComputePipelineState");
    }

    void PipelineD3D12::createRayTracingPipeline(const PipelineDesc& desc, ShaderCompiler& compiler)
    {
        (void)compiler;
        std::vector<D3D12_STATE_SUBOBJECT> subobjects;

        D3D12_GLOBAL_ROOT_SIGNATURE globalRootSigDesc{};
        globalRootSigDesc.pGlobalRootSignature = m_deviceD3D12.getGlobalRootSignature();
        D3D12_STATE_SUBOBJECT globalRootSigSubobject{};
        globalRootSigSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
        globalRootSigSubobject.pDesc = &globalRootSigDesc;
        subobjects.push_back(globalRootSigSubobject);

        D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{};
        pipelineConfig.MaxTraceRecursionDepth = desc.maxRecursionDepth;
        D3D12_STATE_SUBOBJECT configSubobject{};
        configSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
        configSubobject.pDesc = &pipelineConfig;
        subobjects.push_back(configSubobject);

        D3D12_STATE_OBJECT_DESC stateObjectDesc{};
        stateObjectDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
        stateObjectDesc.NumSubobjects = static_cast<UINT>(subobjects.size());
        stateObjectDesc.pSubobjects = subobjects.data();

        ThrowIfFailed(m_deviceD3D12.getNativeDevice()->CreateStateObject(
            &stateObjectDesc, IID_PPV_ARGS(&m_stateObject)), "CreateStateObject (RTPSO)");

        m_shaderGroupHandles.resize(D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
    }
}
