#include "PipelineD3D12.h"
#include "DeviceD3D12.h"
#include "TextureD3D12.h"
#include "../../Extensions/ShaderCompiler/ShaderCompiler.h"

#include <chrono>
#include <filesystem>
#include <fstream>

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
            return mode == CullMode::None ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
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

        D3D12_BLEND_DESC buildBlendDesc(bool colorBlendEnable,
                                        const ColorBlendEquation& eq,
                                        uint32_t colorWriteMask)
        {
            D3D12_BLEND_DESC blend{};
            D3D12_RENDER_TARGET_BLEND_DESC& rt = blend.RenderTarget[0];
            rt.BlendEnable = colorBlendEnable ? TRUE : FALSE;
            rt.SrcBlend = translateBlendFactor(eq.srcColorBlendFactor, false);
            rt.DestBlend = translateBlendFactor(eq.dstColorBlendFactor, false);
            rt.BlendOp = translateBlendOp(eq.colorBlendOp);
            rt.SrcBlendAlpha = translateBlendFactor(eq.srcAlphaBlendFactor, true);
            rt.DestBlendAlpha = translateBlendFactor(eq.dstAlphaBlendFactor, true);
            rt.BlendOpAlpha = translateBlendOp(eq.alphaBlendOp);
            rt.RenderTargetWriteMask = static_cast<UINT8>(colorWriteMask & 0xFu);
            return blend;
        }

        uint64_t makeGraphicsVariantKey(bool hasDepth, bool depthTestEnable, bool depthWriteEnable,
                                        CompareOp depthCompareOp, CullMode cullMode,
                                        FrontFace frontFace, PolygonMode polygonMode,
                                        bool colorBlendEnable, const ColorBlendEquation& colorBlendEquation,
                                        uint32_t colorWriteMask, uint32_t sampleCount)
        {
            uint64_t key = 0;
            key |= (hasDepth ? 1ull : 0ull) << 0;
            key |= (depthTestEnable ? 1ull : 0ull) << 1;
            key |= (depthWriteEnable ? 1ull : 0ull) << 2;
            key |= (static_cast<uint64_t>(depthCompareOp) & 0xFu) << 3;
            key |= (static_cast<uint64_t>(cullMode) & 0x3u) << 7;
            key |= (static_cast<uint64_t>(frontFace) & 0x3u) << 9;
            key |= (static_cast<uint64_t>(polygonMode) & 0x3u) << 11;
            key |= (colorBlendEnable ? 1ull : 0ull) << 13;
            if (colorBlendEnable)
            {
                key |= (static_cast<uint64_t>(colorBlendEquation.srcColorBlendFactor) & 0xFu) << 14;
                key |= (static_cast<uint64_t>(colorBlendEquation.dstColorBlendFactor) & 0xFu) << 18;
                key |= (static_cast<uint64_t>(colorBlendEquation.colorBlendOp) & 0x3u) << 22;
                key |= (static_cast<uint64_t>(colorBlendEquation.srcAlphaBlendFactor) & 0xFu) << 24;
                key |= (static_cast<uint64_t>(colorBlendEquation.dstAlphaBlendFactor) & 0xFu) << 28;
                key |= (static_cast<uint64_t>(colorBlendEquation.alphaBlendOp) & 0x3u) << 32;
            }
            key |= (static_cast<uint64_t>(colorWriteMask) & 0xFu) << 34;
            key |= (static_cast<uint64_t>(sampleCount) & 0x7Fu) << 38; // (1..64: the render target's samples)
            return key;
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

            NRI_LOG_INFO("PipelineD3D12 compiling shader: {} ({})", shaderDesc.sourcePath, shaderDesc.entryPoint);
            const auto started = std::chrono::steady_clock::now();
            std::vector<char> bytecode = compiler.compile(shaderDesc.sourcePath, shaderDesc.entryPoint);
            const double elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            NRI_LOG_INFO("Shader compiled: {} ({:.0f} ms)", shaderDesc.sourcePath, elapsedMs);

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
                                                                    bool colorBlendEnable,
                                                                    const ColorBlendEquation& colorBlendEquation,
                                                                    uint32_t colorWriteMask,
                                                                    uint32_t sampleCount)
    {
        const BOOL effDepthEnable = (hasDepth && depthTestEnable) ? TRUE : FALSE;
        const D3D12_DEPTH_WRITE_MASK effDepthWrite = (hasDepth && depthWriteEnable)
            ? D3D12_DEPTH_WRITE_MASK_ALL
            : D3D12_DEPTH_WRITE_MASK_ZERO;
        const DXGI_FORMAT effDsvFormat = hasDepth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
        const BOOL frontCCW = (frontFace == FrontFace::CounterClockWise) ? TRUE : FALSE;
        const D3D12_BLEND_DESC blendDesc = buildBlendDesc(colorBlendEnable, colorBlendEquation, colorWriteMask);

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
            stream.Rasterizer.desc.DepthClipEnable = TRUE;

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
                ThrowIfFailed(dev2->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&pso)), "CreatePipelineState (Mesh)");
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
            psoDesc.RasterizerState.DepthClipEnable = TRUE;

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
                                                                bool colorBlendEnable,
                                                                const ColorBlendEquation& colorBlendEquation,
                                                                uint32_t colorWriteMask,
                                                                uint32_t sampleCount)
    {
        const uint64_t key = makeGraphicsVariantKey(hasDepth, depthTestEnable, depthWriteEnable,
                                                    depthCompareOp, cullMode, frontFace, polygonMode,
                                                    colorBlendEnable, colorBlendEquation, colorWriteMask, sampleCount);
        std::scoped_lock lock(m_variantMutex);
        auto it = m_graphicsVariants.find(key);
        if (it != m_graphicsVariants.end())
            return it->second.Get();

        ComPtr<ID3D12PipelineState> built = buildGraphicsVariant(
            hasDepth, depthTestEnable, depthWriteEnable, depthCompareOp, cullMode, frontFace, polygonMode,
            colorBlendEnable, colorBlendEquation, colorWriteMask, sampleCount);
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
