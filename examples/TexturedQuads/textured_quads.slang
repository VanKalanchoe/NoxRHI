struct PushConstants
{
    uint4 texIndices;
    uint imageHeapOffset;
    float time;
    float aspectRatio;
};

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    nointerpolation uint texSlot : TEXCOORD1;
};

[shader("vertex")]
VSOut vertMain(uint vertexId : SV_VertexID, uniform PushConstants pc)
{
    const uint quadId = vertexId / 6;
    const uint cornerId = vertexId % 6;

    // 2x2 grid centers in NDC
    static const float2 centers[4] = {
        float2(-0.46,  0.46),
        float2( 0.46,  0.46),
        float2(-0.46, -0.46),
        float2( 0.46, -0.46)
    };

    // Two CCW triangles per quad
    static const float2 quadOffsets[6] = {
        float2(-1.0,  1.0),
        float2(-1.0, -1.0),
        float2( 1.0, -1.0),
        float2(-1.0,  1.0),
        float2( 1.0, -1.0),
        float2( 1.0,  1.0)
    };
    static const float2 quadUVs[6] = {
        float2(0.0, 0.0),
        float2(0.0, 1.0),
        float2(1.0, 1.0),
        float2(0.0, 0.0),
        float2(1.0, 1.0),
        float2(1.0, 0.0)
    };

    const float pulse = 0.34 + 0.03 * sin(pc.time * 2.0 + float(quadId) * 1.57);
    const float2 local = quadOffsets[cornerId] * float2(pulse / max(pc.aspectRatio, 0.25), pulse);

    VSOut outv;
    outv.position = float4(centers[quadId] + local, 0.0, 1.0);
    outv.uv = quadUVs[cornerId];
    outv.texSlot = pc.texIndices[quadId];
    return outv;
}

[shader("fragment")]
float4 fragMain(VSOut input, uniform PushConstants pc) : SV_Target
{
    Texture2D tex = Texture2D.Handle(uint2(pc.imageHeapOffset + input.texSlot, 0));
    SamplerState linearSampler = SamplerState.Handle(uint2(0, 0));
    float4 sampled = tex.Sample(linearSampler, input.uv);
    float3 cardBg = float3(0.11, 0.13, 0.18);
    return float4(lerp(cardBg, sampled.rgb, sampled.a), 1.0);
}
