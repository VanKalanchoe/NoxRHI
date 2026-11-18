struct PushConstants
{
    float time;
};

struct VSOut
{
    float4 position : SV_Position;
    float3 color    : COLOR0;
};

[shader("vertex")]
VSOut vertMain(uint vertexId : SV_VertexID, uniform PushConstants pc)
{
    static const float2 positions[3] = {
        float2( 0.0,  0.6),
        float2(-0.6, -0.6),
        float2( 0.6, -0.6)
    };
    static const float3 colors[3] = {
        float3(1.0, 0.2, 0.2),
        float3(0.2, 1.0, 0.3),
        float3(0.2, 0.4, 1.0)
    };

    float2 p = positions[vertexId];
    float s = sin(pc.time);
    float c = cos(pc.time);
    float2 rotated = float2(p.x * c - p.y * s, p.x * s + p.y * c);

    VSOut outv;
    outv.position = float4(rotated, 0.0, 1.0);
    outv.color = colors[vertexId];
    return outv;
}

[shader("fragment")]
float4 fragMain(VSOut input) : SV_Target
{
    return float4(input.color, 1.0);
}
