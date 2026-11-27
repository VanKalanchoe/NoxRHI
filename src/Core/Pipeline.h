#pragma once
#include <string>
#include <vector>

namespace NRI
{
    enum class ImageFormat;

    enum class PipelineType { Graphics, Compute, RayTracing };
    enum class ShaderStage { Vertex, Fragment, Compute, Task, Mesh, RayGen, Miss, ClosestHit, AnyHit, Intersection, Callable };

    struct ShaderStageDesc
    {
        ShaderStage stage;
        std::string entryPoint;
        std::string sourcePath;
    };

    struct RayTracingHitGroupDesc
    {
        uint32_t closestHitShaderIndex = ~0u; // index into PipelineDesc::shaders, or ~0u
        uint32_t anyHitShaderIndex = ~0u;     // index into PipelineDesc::shaders, or ~0u
        uint32_t intersectionShaderIndex = ~0u; // index into PipelineDesc::shaders, or ~0u
    };

    struct PipelineDesc
    {
        PipelineType type = PipelineType::Graphics; // default to graphics
        std::vector<ShaderStageDesc> shaders;
        
        std::vector<ImageFormat> colorFormats;
        std::vector<RayTracingHitGroupDesc> hitGroups;
        uint32_t maxRecursionDepth = 1;
        // Ray tracing pipelines: the largest ray payload and hit attributes (bytes) any of its shaders uses. D3D12 needs them in the
        // state object (0: 256 bytes of payload, D3D12_RAYTRACING_MAX_ATTRIBUTE_SIZE_IN_BYTES of attributes -- creation fails when a
        // shader's payload is larger); Vulkan reads them from the SPIR-V and ignores these.
        uint32_t maxPayloadSize = 0;
        uint32_t maxAttributeSize = 0;
        
        bool forceCompile = false;
        // Built on the calling thread, a failure thrown from createPipeline (never left for the first bind to find): what a pipeline
        // whose source may not compile -- a project's own shaders (Renderer/CustomLitShaders.h) -- needs to be known good before use.
        bool buildNow = false;
    };

    class Pipeline
    {
    public:
        virtual ~Pipeline() = default;
        virtual const std::vector<uint8_t>& getShaderGroupHandles() const { static const std::vector<uint8_t> empty; return empty; }
        virtual uint32_t getShaderGroupHandleSize() const { return 0; }
        virtual uint32_t getShaderGroupBaseAlignment() const { return 0; }
        virtual uint32_t getRayGenShaderGroupIndex() const { return 0; }
        virtual const std::vector<uint32_t>& getMissShaderGroupIndices() const { static const std::vector<uint32_t> empty; return empty; }
        virtual const std::vector<uint32_t>& getHitShaderGroupIndices() const { static const std::vector<uint32_t> empty; return empty; }
        virtual const std::vector<uint32_t>& getCallableShaderGroupIndices() const { static const std::vector<uint32_t> empty; return empty; }
    };
}
