#pragma once
#include <mutex>
#include <unordered_map>
#include <vector>
#include "D3D12Common.h"
#include "../CommandBuffer.h"
#include "../Pipeline.h"

namespace NRI
{
    class DeviceD3D12;
    class ShaderCompiler;

    class PipelineD3D12 final : public Pipeline
    {
    public:
        PipelineD3D12(DeviceD3D12& device, const PipelineDesc& desc, ShaderCompiler& compiler);
        ~PipelineD3D12() override = default;

        ID3D12PipelineState* getNativePipeline() const { return m_pipeline.Get(); }
        ID3D12PipelineState* resolveGraphicsPipeline(bool hasDepth,
                                                     bool depthTestEnable,
                                                     bool depthWriteEnable,
                                                     CompareOp depthCompareOp,
                                                     CullMode cullMode,
                                                     FrontFace frontFace,
                                                     PolygonMode polygonMode);
        ID3D12StateObject* getNativeStateObject() const { return m_stateObject.Get(); }
        PipelineType getType() const { return m_type; }

        const std::vector<uint8_t>& getShaderGroupHandles() const override { return m_shaderGroupHandles; }
        uint32_t getShaderGroupHandleSize() const override { return D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT; }
        uint32_t getShaderGroupBaseAlignment() const override { return D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT; }
        uint32_t getRayGenShaderGroupIndex() const override { return 0; }
        const std::vector<uint32_t>& getMissShaderGroupIndices() const override { return m_missShaderGroupIndices; }
        const std::vector<uint32_t>& getHitShaderGroupIndices() const override { return m_hitShaderGroupIndices; }
        const std::vector<uint32_t>& getCallableShaderGroupIndices() const override { return m_callableShaderGroupIndices; }

    private:
        void createGraphicsPipeline(const PipelineDesc& desc, ShaderCompiler& compiler);
        void createComputePipeline(const PipelineDesc& desc, ShaderCompiler& compiler);
        void createRayTracingPipeline(const PipelineDesc& desc, ShaderCompiler& compiler);
        ComPtr<ID3D12PipelineState> buildGraphicsVariant(bool hasDepth,
                                                         bool depthTestEnable,
                                                         bool depthWriteEnable,
                                                         CompareOp depthCompareOp,
                                                         CullMode cullMode,
                                                         FrontFace frontFace,
                                                         PolygonMode polygonMode);

    private:
        DeviceD3D12& m_deviceD3D12;
        PipelineType m_type;
        ComPtr<ID3D12PipelineState> m_pipeline;
        ComPtr<ID3D12StateObject> m_stateObject;

        // Graphics state stored so dynamic Vulkan-style state toggles select matching D3D12 PSOs.
        std::vector<char> m_vsBytecode;
        std::vector<char> m_psBytecode;
        std::vector<char> m_asBytecode;
        std::vector<char> m_msBytecode;
        bool m_hasMeshShaders = false;
        std::vector<DXGI_FORMAT> m_rtvFormats;
        std::mutex m_variantMutex;
        std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> m_graphicsVariants;

        std::vector<uint8_t> m_shaderGroupHandles;
        std::vector<uint32_t> m_missShaderGroupIndices;
        std::vector<uint32_t> m_hitShaderGroupIndices;
        std::vector<uint32_t> m_callableShaderGroupIndices;
    };
}
