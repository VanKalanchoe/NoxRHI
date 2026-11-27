// RayTracingPipeline's present: the ray traced image (a storage texture) drawn to the swapchain with one full-screen triangle. A file of
// its own: the ray tracing shaders' module holds ray tracing entry points only.

#include "nox_gpu.slang"

struct PresentPush
{
    uint imageSlot;
};

struct FullscreenOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
FullscreenOut vsFullscreen(uint vertexId : SV_VertexID)
{
    // One triangle covering the screen (clockwise: draw it with culling off); uv (0,0) at the top-left on both APIs (NDC +Y is up).
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    FullscreenOut o;
    o.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    o.uv = uv;
    return o;
}

[shader("fragment")]
float4 psPresent(FullscreenOut input, uniform PresentPush push) : SV_Target
{
    return Tex2D(push.imageSlot).Sample(Sampler(1 /* linear clamp */), input.uv);
}
