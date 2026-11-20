// BindlessScene: instanced, lit, textured cubes rendered to an HDR target with a reverse-Z depth buffer, tonemapped by a compute shader
// into a storage texture, then drawn to the swapchain. One source for Vulkan and D3D12 (nox_gpu.slang):
//   - per-frame constants in a CPU-written buffer (BufferRef: Vulkan address / D3D12 SRV on an upload-heap buffer);
//   - per-instance data and the cube's vertices in GPU buffers (vertex pulling, no vertex buffers);
//   - each instance picks its texture from a bindless table -- an index that differs within a draw (nox_gpu.slang's SampleNonUniform).

#include "nox_gpu.slang"

struct Scene
{
    Mat4 viewProj;
    float4 cameraPosition; // xyz
    float4 lightDirection; // xyz: towards the light (normalized)
    float4 lightColor;     // rgb: radiance
    float4 ambient;        // rgb
    float time;
    uint textureCount;
    uint pad0, pad1;
};

struct Instance
{
    float4 positionScale; // xyz: centre, w: half size
    float4 tint;          // rgb tint, a: spin speed
    uint texture;         // index into the texture table
    uint pad0, pad1, pad2;
};

struct Vertex
{
    float4 position; // xyz (w unused)
    float4 normal;   // xyz
    float4 uv;       // xy
};

struct ScenePush
{
    uint64_t sceneRef;
    uint64_t instancesRef;
    uint64_t verticesRef;
    uint64_t texturesRef; // texture slots (image heap offset included)

    property BufferRef<Scene> scene { get { return BufferRef<Scene>(sceneRef); } }
    property BufferRef<Instance> instances { get { return BufferRef<Instance>(instancesRef); } }
    property BufferRef<Vertex> vertices { get { return BufferRef<Vertex>(verticesRef); } }
    property BufferRef<uint> textures { get { return BufferRef<uint>(texturesRef); } }
};

struct VertexOut
{
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    nointerpolation uint texture : TEXCOORD3;
    nointerpolation float3 tint : TEXCOORD4;
};

float3 RotateY(float3 v, float angle)
{
    const float c = cos(angle), s = sin(angle);
    return float3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

float3 RotateX(float3 v, float angle)
{
    const float c = cos(angle), s = sin(angle);
    return float3(v.x, c * v.y - s * v.z, s * v.y + c * v.z);
}

[shader("vertex")]
VertexOut vsScene(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID, uniform ScenePush pc)
{
    // (SV_InstanceID: D3D12 does not add the draw's first instance, Vulkan does -- the same here: every draw starts at instance 0)
    const Scene scene = pc.scene[0];
    const Instance instance = pc.instances[instanceId];
    const Vertex v = pc.vertices[vertexId];
    const float angle = scene.time * instance.tint.a;
    const float3 local = RotateX(RotateY(v.position.xyz, angle), angle * 0.6);
    const float3 world = instance.positionScale.xyz + local * instance.positionScale.w;

    VertexOut o;
    o.position = scene.viewProj.Transform(float4(world, 1.0));
    o.worldPosition = world;
    o.normal = RotateX(RotateY(v.normal.xyz, angle), angle * 0.6);
    o.uv = v.uv.xy;
    o.texture = instance.texture;
    o.tint = instance.tint.rgb;
    return o;
}

[shader("fragment")]
float4 psScene(VertexOut input, uniform ScenePush pc) : SV_Target
{
    const Scene scene = pc.scene[0];
    // The texture slot differs between instances drawn by one call, and so within a wave: a non-uniform index (SampleNonUniform).
    const uint slot = pc.textures[input.texture % scene.textureCount];
    const float3 albedo = SampleNonUniform(slot, Sampler(0 /* linear repeat */), input.uv).rgb * input.tint;

    const float3 n = normalize(input.normal);
    const float3 l = scene.lightDirection.xyz;
    const float3 v = normalize(scene.cameraPosition.xyz - input.worldPosition);
    const float3 h = normalize(l + v);
    const float diffuse = saturate(dot(n, l));
    const float specular = pow(saturate(dot(n, h)), 48.0) * diffuse;
    const float rim = pow(1.0 - saturate(dot(n, v)), 4.0) * 0.4;
    const float3 color = albedo * (scene.ambient.rgb + scene.lightColor.rgb * diffuse) + scene.lightColor.rgb * specular * 0.5 + rim * scene.ambient.rgb;
    return float4(color, 1.0);
}

// --- post: tonemap (ACES fit) + vignette, compute, into a storage texture ---------------------------------------------------------------

struct PostPush
{
    uint hdrSlot;     // the HDR target, sampled (image heap offset included)
    uint outputSlot;  // the tonemapped result, storage
    uint width, height;
    float exposure;
};

float3 Aces(float3 x)
{
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csTonemap(uint3 id : SV_DispatchThreadID, uniform PostPush pc)
{
    if (id.x >= pc.width || id.y >= pc.height)
        return;
    const float3 hdr = Tex2D(pc.hdrSlot).Load(int3(id.xy, 0)).rgb;
    const float2 uv = (float2(id.xy) + 0.5) / float2(pc.width, pc.height);
    const float vignette = 1.0 - 0.35 * dot(uv - 0.5, uv - 0.5) * 2.0;
    RWTex2D(pc.outputSlot)[id.xy] = float4(Aces(hdr * pc.exposure) * vignette, 1.0); // linear: the sRGB swapchain encodes it
}

// --- present: a fullscreen triangle sampling the tonemapped image ---------------------------------------------------------------------

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
float4 psPresent(FullscreenOut input, uniform PresentPush pc) : SV_Target
{
    return Tex2D(pc.imageSlot).Sample(Sampler(1 /* linear clamp */), input.uv);
}
