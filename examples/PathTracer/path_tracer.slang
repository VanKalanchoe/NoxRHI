// PathTracer: a 1-sample-per-pixel path tracer (ray queries in a compute shader) whose noisy output is cleaned up by NVIDIA NRD
// (REBLUR, diffuse + specular) or by DLSS Ray Reconstruction (Streamline), next to an accumulated reference to compare against.
// One source for Vulkan and D3D12.
//
// Per pixel: a primary ray; at the hit, sun light (one shadow ray towards a point of the sun's disk) plus one diffuse and one specular
// bounce (cosine and GGX VNDF sampling), each lit by the sun and the sky at its hit. The result goes out in the forms the denoisers
// want (their headers' contracts, see the README):
//   NRD (REBLUR_DIFFUSE_SPECULAR): view-space Z, 2D motion vectors (prevUv = uv + mv), normal + linear roughness packed by
//     NRD_FrontEnd_PackNormalAndRoughness, and demodulated diffuse / specular radiance (material colour divided out) with the bounce's
//     hit distance normalized by REBLUR_FrontEnd_GetNormHitDist (packed by REBLUR_FrontEnd_PackRadianceAndNormHitDist).
//   DLSS-RR: the noisy colour, reverse-Z hardware depth, the same motion vectors, diffuse and specular albedo, world normal + roughness
//     (packed, RGBA16F) and the specular bounce's hit distance in world units.
// NRD's packing comes from NRD.hlsli (vendors/NRD/Shaders): compiled in when the host defines NOX_HAS_NRD.

#include "nox_gpu.slang"
#ifdef NOX_HAS_NRD
#include "NRD.hlsli"
#endif

static const float kPi = 3.14159265;
static const float kMiss = 1.0e5;     // a miss's distance (finite: NRD guides must not hold INF; beyond the denoising range)

struct Object
{
    float4x4 model;
    float4x4 normalMatrix;   // inverse transpose of the model matrix
    float4 baseColorRoughness; // rgb base colour, a linear roughness
    float4 material;           // x metalness
};

struct Vertex
{
    float4 position;
    float4 normal;
};

// The image slots of the frame's textures -- once as storage images (csPathTrace writes them, csComposite writes display) and once as
// sampled images (csComposite reads them).
struct Targets
{
    uint viewZ, motion, normalRoughness, diffuse, specular;           // NRD inputs
    uint color, depth, diffuseAlbedo, specularAlbedo, rrNormalRoughness, specularHitDistance; // DLSS-RR inputs (+ motion)
    uint nrdDiffuse, nrdSpecular, rrOutput;                           // denoiser outputs (sampled)
    uint display;                                                     // composite's output (storage)
    uint padding;
};

struct Frame
{
    float4x4 worldToView;
    float4x4 viewToClip;       // non-jittered, reverse-Z
    float4x4 worldToClipPrev;  // last frame's (motion vectors)
    float4 cameraPosition;     // xyz
    float4 cameraForward;      // xyz, w = tan(vertical fov / 2)
    float4 cameraRight;        // xyz, w = aspect
    float4 cameraUp;           // xyz
    float4 sunDirection;       // xyz towards the sun, w = tan of its angular radius
    float4 sunIrradiance;      // rgb
    float4 hitDistanceParams;  // xyz: REBLUR's hit distance normalization (A, B, C), the values NRD was given
    float2 jitter;             // sample offset in pixels
    uint width, height;
    uint frameIndex;
    uint accumulatedFrames;    // reference: frames summed so far (0: start again)
    uint mode;                 // 0 reference, 1 noisy, 2 NRD, 3 DLSS-RR
    uint guide;                // 0: the image; 1.. a guide buffer (composite)
    float exposure;
    uint objectCount;
    uint2 padding;
    Targets storage;
    Targets sampled;
};

struct Push
{
    uint64_t frameRef;
    uint64_t objectsRef;
    uint64_t verticesRef;
    uint64_t tlasRef;
    uint64_t accumulationRef;   // float4 per pixel: the reference's running sum (read-write)

    property BufferRef<Frame> frame { get { return BufferRef<Frame>(frameRef); } }
    property BufferRef<Object> objects { get { return BufferRef<Object>(objectsRef); } }
    property BufferRef<Vertex> vertices { get { return BufferRef<Vertex>(verticesRef); } }
    property RWBufferRef<float4> accumulation { get { return RWBufferRef<float4>(accumulationRef); } }
};

// --- random numbers -----------------------------------------------------------------------------------------------------------------

uint Pcg(inout uint state)
{
    state = state * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float Random(inout uint state) { return float(Pcg(state) >> 8) * (1.0 / 16777216.0); }

// --- scene ----------------------------------------------------------------------------------------------------------------------------

struct Hit
{
    float t;       // kMiss when nothing was hit
    float3 normal; // world space, facing the ray
    float3 baseColor;
    float roughness;
    float metalness;
};

Hit Trace(Push pc, float3 origin, float3 direction)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.001;
    ray.TMax = kMiss;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(LoadTlas(pc.tlasRef), RAY_FLAG_NONE, 0xFF, ray);
    query.Proceed();
    Hit hit;
    hit.t = kMiss;
    hit.normal = -direction;
    hit.baseColor = 0;
    hit.roughness = 1;
    hit.metalness = 0;
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return hit;
    const Object object = pc.objects[query.CommittedInstanceID()];
    const float3 local = pc.vertices[query.CommittedPrimitiveIndex() * 3].normal.xyz; // (a cube: one normal per triangle)
    float3 n = normalize(mul(object.normalMatrix, float4(local, 0.0)).xyz);
    if (dot(n, direction) > 0.0)
        n = -n;
    hit.t = query.CommittedRayT();
    hit.normal = n;
    hit.baseColor = object.baseColorRoughness.rgb;
    hit.roughness = max(object.baseColorRoughness.a, 0.03);
    hit.metalness = object.material.x;
    return hit;
}

bool Visible(Push pc, float3 origin, float3 direction)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.001;
    ray.TMax = kMiss;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(LoadTlas(pc.tlasRef), RAY_FLAG_NONE, 0xFF, ray);
    query.Proceed();
    return query.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
}

float3 Sky(float3 direction)
{
    const float up = direction.y;
    const float3 zenith = float3(0.18, 0.32, 0.75), horizon = float3(0.62, 0.72, 0.85), ground = float3(0.12, 0.11, 0.10);
    return up >= 0.0 ? lerp(horizon, zenith, sqrt(up)) : lerp(horizon, ground, saturate(-up * 4.0));
}

// --- shading --------------------------------------------------------------------------------------------------------------------------

void Basis(float3 n, out float3 t, out float3 b)
{
    const float s = n.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (s + n.z), c = n.x * n.y * a;
    t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}

float3 FresnelSchlick(float3 f0, float cosine) { return f0 + (1.0 - f0) * pow(1.0 - saturate(cosine), 5.0); }

float SmithG1(float a2, float cosine) { return 2.0 * cosine / (cosine + sqrt(a2 + (1.0 - a2) * cosine * cosine)); }

float SmithG2(float a2, float noL, float noV)
{
    return 2.0 * noL * noV / (noV * sqrt(a2 + (1.0 - a2) * noL * noL) + noL * sqrt(a2 + (1.0 - a2) * noV * noV));
}

// Karis' analytic approximation of the GGX environment BRDF: the specular albedo NRD's demodulation and DLSS-RR's guide use.
float3 SpecularAlbedo(float3 f0, float roughness, float noV)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022), c1 = float4(1.0, 0.0425, 1.04, -0.04);
    const float4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * noV)) * r.x + r.y;
    const float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
    return saturate(f0 * ab.x + ab.y);
}

// Heitz 2018: a GGX normal visible from `v` (tangent space, z up).
float3 SampleVisibleNormal(float3 v, float alpha, float2 u)
{
    const float3 vh = normalize(float3(alpha * v.x, alpha * v.y, v.z));
    const float lensq = vh.x * vh.x + vh.y * vh.y;
    const float3 t1 = lensq > 0.0 ? float3(-vh.y, vh.x, 0.0) * rsqrt(lensq) : float3(1.0, 0.0, 0.0);
    const float3 t2 = cross(vh, t1);
    const float r = sqrt(u.x), phi = 2.0 * kPi * u.y;
    float p1 = r * cos(phi), p2 = r * sin(phi);
    const float s = 0.5 * (1.0 + vh.z);
    p2 = (1.0 - s) * sqrt(max(0.0, 1.0 - p1 * p1)) + s * p2;
    const float3 nh = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * vh;
    return normalize(float3(alpha * nh.x, alpha * nh.y, max(0.0, nh.z)));
}

// A direction towards a random point of the sun's disk.
float3 SunSample(Frame frame, inout uint rng)
{
    float3 t, b;
    Basis(frame.sunDirection.xyz, t, b);
    const float r = sqrt(Random(rng)), phi = 2.0 * kPi * Random(rng);
    return normalize(frame.sunDirection.xyz + (t * cos(phi) + b * sin(phi)) * (r * frame.sunDirection.w));
}

// The light leaving a secondary hit towards the ray that found it: diffuse sun light (one shadow ray) plus the sky as ambient, or the
// sky itself on a miss.
float3 SecondaryRadiance(Push pc, Frame frame, float3 origin, float3 direction, Hit hit, inout uint rng)
{
    if (hit.t >= kMiss)
        return Sky(direction);
    const float3 position = origin + direction * hit.t + hit.normal * 0.002;
    const float3 albedo = hit.baseColor * (1.0 - hit.metalness);
    const float3 l = SunSample(frame, rng);
    const float noL = saturate(dot(hit.normal, l));
    const float3 sun = noL > 0.0 && Visible(pc, position, l) ? frame.sunIrradiance.rgb * (noL / kPi) : 0.0;
    return albedo * (sun + Sky(hit.normal) * 0.6);
}

// --- the path tracer ------------------------------------------------------------------------------------------------------------------

void Store(uint slot, uint2 pixel, float4 value) { RWTex2D(slot)[pixel] = value; }

[shader("compute")]
[numthreads(8, 8, 1)]
void csPathTrace(uint3 id : SV_DispatchThreadID, uniform Push pc)
{
    const Frame frame = pc.frame[0];
    const uint2 pixel = id.xy;
    if (pixel.x >= frame.width || pixel.y >= frame.height)
        return;
    uint rng = (pixel.x * 1973u + pixel.y * 9277u + frame.frameIndex * 26699u) | 1u;
    Pcg(rng);

    // Primary ray (through the jittered sample position) and the guide position (the pixel centre, unjittered).
    const float2 size = float2(frame.width, frame.height);
    const float2 sampleUv = (float2(pixel) + 0.5 + frame.jitter) / size;
    const float2 ndc = float2(sampleUv.x * 2.0 - 1.0, 1.0 - sampleUv.y * 2.0);
    const float tanHalf = frame.cameraForward.w, aspect = frame.cameraRight.w;
    const float3 origin = frame.cameraPosition.xyz;
    const float3 direction = normalize(frame.cameraForward.xyz + frame.cameraRight.xyz * (ndc.x * tanHalf * aspect) + frame.cameraUp.xyz * (ndc.y * tanHalf));
    const Hit primary = Trace(pc, origin, direction);

    const float3 position = origin + direction * min(primary.t, 1.0e4);
    const float3 viewPosition = mul(frame.worldToView, float4(position, 1.0)).xyz;
    const float4 clip = mul(frame.viewToClip, float4(viewPosition, 1.0));
    const float4 clipPrev = mul(frame.worldToClipPrev, float4(position, 1.0));
    const float2 uv = (float2(pixel) + 0.5) / size;
    const float2 uvPrev = float2(clipPrev.x / clipPrev.w * 0.5 + 0.5, 0.5 - clipPrev.y / clipPrev.w * 0.5);
    const float2 motion = uvPrev - uv; // (prevUv = uv + motion: NRD's and Streamline's convention, mvecScale 1)
    const bool hit = primary.t < kMiss;
    const float viewZ = hit ? viewPosition.z : -kMiss; // (RH view: negative in front; NRD takes either sign)

    float3 diffuse = 0.0, specular = 0.0;          // demodulated (material colour divided out)
    float diffuseHit = kMiss, specularHit = kMiss; // the bounce's hit distance (kMiss: the sky)
    float3 diffuseAlbedo = 0.0, specularAlbedo = 0.0, color = Sky(direction);
    if (hit)
    {
        const float3 n = primary.normal, v = -direction;
        const float3 p = position + n * 0.002;
        const float noV = max(dot(n, v), 1e-4);
        const float alpha = primary.roughness * primary.roughness, a2 = alpha * alpha;
        const float3 f0 = lerp(float3(0.04, 0.04, 0.04), primary.baseColor, primary.metalness);
        diffuseAlbedo = primary.baseColor * (1.0 - primary.metalness);
        specularAlbedo = SpecularAlbedo(f0, primary.roughness, noV);

        // Sun (one shadow ray to a point of its disk): Lambert and GGX, divided by their albedos.
        const float3 l = SunSample(frame, rng);
        const float noL = dot(n, l);
        if (noL > 0.0 && Visible(pc, p, l))
        {
            const float3 h = normalize(v + l);
            const float noH = saturate(dot(n, h)), voH = saturate(dot(v, h));
            const float d = a2 / (kPi * pow(noH * noH * (a2 - 1.0) + 1.0, 2.0));
            const float3 brdf = d * SmithG2(a2, noL, noV) * FresnelSchlick(f0, voH) / (4.0 * noV * noL);
            diffuse += frame.sunIrradiance.rgb * (noL / kPi);
            specular += frame.sunIrradiance.rgb * brdf * noL / max(specularAlbedo, 1e-3);
        }

        float3 t, b;
        Basis(n, t, b);
        // Diffuse bounce (cosine sampling: the Lambert estimate is albedo * incoming light, so the demodulated value is the light).
        {
            const float r = sqrt(Random(rng)), phi = 2.0 * kPi * Random(rng);
            const float3 l2 = normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - r * r)));
            const Hit h2 = Trace(pc, p, l2);
            diffuse += SecondaryRadiance(pc, frame, p, l2, h2, rng);
            diffuseHit = h2.t;
        }
        // Specular bounce (GGX VNDF sampling: weight F * G2 / G1).
        {
            const float3 vt = float3(dot(v, t), dot(v, b), noV);
            const float3 ht = SampleVisibleNormal(vt, alpha, float2(Random(rng), Random(rng)));
            const float3 h = t * ht.x + b * ht.y + n * ht.z;
            const float3 l2 = reflect(-v, h);
            const float noL2 = dot(n, l2);
            if (noL2 > 0.0)
            {
                const Hit h2 = Trace(pc, p, l2);
                const float3 weight = FresnelSchlick(f0, dot(v, h)) * (SmithG2(a2, noL2, noV) / SmithG1(a2, noV));
                specular += SecondaryRadiance(pc, frame, p, l2, h2, rng) * weight / max(specularAlbedo, 1e-3);
                specularHit = h2.t;
            }
            else
                specularHit = 0.0; // (a ray into the surface: NRD wants 0)
        }
        color = diffuse * diffuseAlbedo + specular * specularAlbedo;
    }

    const Targets targets = frame.storage;
    // Reference: the running sum of this estimate (a fresh sum when the camera moved).
    const uint index = pixel.y * frame.width + pixel.x;
    const float4 sum = frame.accumulatedFrames == 0 ? float4(0, 0, 0, 0) : pc.accumulation.Load(index);
    pc.accumulation.Store(index, sum + float4(color, 1.0));

    // DLSS-RR inputs.
    Store(targets.color, pixel, float4(color, 1.0));
    Store(targets.depth, pixel, float4(hit ? clip.z / clip.w : 0.0, 0, 0, 0)); // (reverse-Z: 1 at the near plane, 0 at infinity)
    Store(targets.motion, pixel, float4(motion, 0, 0));
    Store(targets.diffuseAlbedo, pixel, float4(diffuseAlbedo, 1.0));
    Store(targets.specularAlbedo, pixel, float4(specularAlbedo, 1.0));
    Store(targets.rrNormalRoughness, pixel, float4(primary.normal, primary.roughness));
    Store(targets.specularHitDistance, pixel, float4(hit ? min(specularHit, kMiss) : 0.0, 0, 0, 0));

    // NRD inputs.
    Store(targets.viewZ, pixel, float4(viewZ, 0, 0, 0));
#ifdef NOX_HAS_NRD
    Store(targets.normalRoughness, pixel, NRD_FrontEnd_PackNormalAndRoughness(primary.normal, primary.roughness, 0.0));
    const float3 hitParams = frame.hitDistanceParams.xyz;
    Store(targets.diffuse, pixel, REBLUR_FrontEnd_PackRadianceAndNormHitDist(diffuse, REBLUR_FrontEnd_GetNormHitDist(diffuseHit, viewZ, hitParams, 1.0), true));
    Store(targets.specular, pixel, specularHit > 0.0
        ? REBLUR_FrontEnd_PackRadianceAndNormHitDist(specular, REBLUR_FrontEnd_GetNormHitDist(specularHit, viewZ, hitParams, primary.roughness), true)
        : float4(0, 0, 0, 0));
#else
    Store(targets.normalRoughness, pixel, float4(primary.normal * 0.5 + 0.5, primary.roughness)); // (no NRD: a plain guide)
    Store(targets.diffuse, pixel, float4(diffuse, diffuseHit));
    Store(targets.specular, pixel, float4(specular, specularHit));
#endif
}

// --- composite: the mode's image (or a guide), tonemapped -------------------------------------------------------------------------------

float4 Read(uint slot, uint2 pixel) { return Tex2D(slot).Load(int3(pixel, 0)); }

// Narkowicz's ACES fit.
float3 Tonemap(float3 x) { return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)); }

[shader("compute")]
[numthreads(8, 8, 1)]
void csComposite(uint3 id : SV_DispatchThreadID, uniform Push pc)
{
    const Frame frame = pc.frame[0];
    const uint2 pixel = id.xy;
    if (pixel.x >= frame.width || pixel.y >= frame.height)
        return;
    const Targets targets = frame.sampled;
    float3 result;
    bool tonemap = true;
    if (frame.guide == 0)
    {
        const bool hit = abs(Read(targets.viewZ, pixel).x) < kMiss * 0.5;
        if (frame.mode == 0)
        {
            const float4 sum = pc.accumulation.Load(pixel.y * frame.width + pixel.x);
            result = sum.rgb / max(sum.w, 1.0);
        }
        else if (frame.mode == 2 && hit)
        {
#ifdef NOX_HAS_NRD
            // NRD's output is demodulated: the material colours go back on (the noisy colour stays for the sky).
            const float3 diffuse = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(Read(targets.nrdDiffuse, pixel)).rgb;
            const float3 specular = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(Read(targets.nrdSpecular, pixel)).rgb;
            result = diffuse * Read(targets.diffuseAlbedo, pixel).rgb + specular * Read(targets.specularAlbedo, pixel).rgb;
#else
            result = Read(targets.color, pixel).rgb;
#endif
        }
        else if (frame.mode == 3)
            result = Read(targets.rrOutput, pixel).rgb;
        else
            result = Read(targets.color, pixel).rgb;
        result *= frame.exposure;
    }
    else
    {
        // Guides: what the denoisers are given, to check them by eye.
        tonemap = false;
        switch (frame.guide)
        {
        case 1: result = Read(targets.rrNormalRoughness, pixel).xyz * 0.5 + 0.5; break;                // world normal
        case 2: result = Read(targets.rrNormalRoughness, pixel).www; break;                            // linear roughness
        case 3: result = frac(abs(Read(targets.viewZ, pixel).xxx) / 10.0); break;                      // view Z (bands of 10 units)
        case 4: result = float3(abs(Read(targets.motion, pixel).xy) * 50.0, 0.0); break;               // motion (x red, y green, x50)
        case 5: result = Read(targets.diffuseAlbedo, pixel).rgb; break;
        case 6: result = Read(targets.specularAlbedo, pixel).rgb; break;
        case 7: result = saturate(Read(targets.specularHitDistance, pixel).xxx / 20.0); break;         // specular hit distance / 20
        default: result = Read(targets.depth, pixel).xxx * 10.0; break;                                // reverse-Z depth x10
        }
    }
    RWTex2D(frame.storage.display)[pixel] = float4(tonemap ? Tonemap(result) : result, 1.0);
}

// --- present -----------------------------------------------------------------------------------------------------------------------------

struct FullscreenOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

struct PresentPush
{
    uint imageSlot;
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
