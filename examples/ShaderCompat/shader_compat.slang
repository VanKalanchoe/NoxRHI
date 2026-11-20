// ShaderCompat: one Slang source compiled for Vulkan (SPIR-V) and D3D12 (DXIL). Every entry point is a small test that writes its
// results to `out`; the host computes what they must be and compares (and, with --all, compares the two APIs with each other).
//
// Buffers are reached the way NoxRHI shaders do it on each API, in one source:
//   Vulkan: buffer device addresses (pointers) from the push data          -- NOX_VULKAN is defined by NoxRHI's Slang compiler
//   D3D12:  bindless descriptor handles (registerBuffer / registerStorageBuffer slots) -- NOX_DX12
// Textures and samplers use bindless handles on both (Texture2D.Handle, RWTexture2D.Handle, SamplerState.Handle).

struct Push
{
    uint64_t outAddress;   //  0: Vulkan: the output buffer's device address
    uint64_t inAddress;    //  8: Vulkan: the input buffer's device address
    uint outSlot;          // 16: D3D12: the output buffer's storage (UAV) slot
    uint inSlot;           // 20: D3D12: the input buffer's (SRV) slot
    uint texSlot;          // 24: a sampled texture's slot (image heap offset added on the host)
    uint storageTexSlot;   // 28: a storage texture's slot (registerStorageTextureMip)
    uint pointSampler;     // 32: sampler heap slot of a nearest / clamp sampler
    uint linearSampler;    // 36: sampler heap slot of a linear / clamp sampler
    uint count;            // 40: elements to process
    uint tlasSlot;         // 44: D3D12: the TLAS's descriptor slot
    float4 params;         // 48: test-specific floats
    uint4 uparams;         // 64: test-specific integers
    uint64_t tlasAddress;  // 80: Vulkan: the TLAS's device address
};                         // 88 bytes: within D3D12's 32 root constants (128 bytes)

// --- buffer access, one way per API ------------------------------------------------------------------------------------------------

uint LoadIn(Push pc, uint index)
{
#ifdef NOX_VULKAN
    return ((uint*)pc.inAddress)[index];
#else
    ByteAddressBuffer buffer = ByteAddressBuffer.Handle(uint2(pc.inSlot, 0));
    return buffer.Load(index * 4);
#endif
}

float LoadInFloat(Push pc, uint index)
{
    return asfloat(LoadIn(pc, index));
}

void StoreOut(Push pc, uint index, uint value)
{
#ifdef NOX_VULKAN
    ((uint*)pc.outAddress)[index] = value;
#else
    RWByteAddressBuffer buffer = RWByteAddressBuffer.Handle(uint2(pc.outSlot, 0));
    buffer.Store(index * 4, value);
#endif
}

void StoreOutFloat(Push pc, uint index, float value)
{
    StoreOut(pc, index, asuint(value));
}

uint AtomicAddOut(Push pc, uint index, uint value)
{
    uint previous = 0;
#ifdef NOX_VULKAN
    InterlockedAdd(((uint*)pc.outAddress)[index], value, previous);
#else
    RWByteAddressBuffer buffer = RWByteAddressBuffer.Handle(uint2(pc.outSlot, 0));
    buffer.InterlockedAdd(index * 4, value, previous);
#endif
    return previous;
}

void AtomicMaxOut(Push pc, uint index, uint value)
{
#ifdef NOX_VULKAN
    InterlockedMax(((uint*)pc.outAddress)[index], value);
#else
    RWByteAddressBuffer buffer = RWByteAddressBuffer.Handle(uint2(pc.outSlot, 0));
    buffer.InterlockedMax(index * 4, value);
#endif
}

// --- 1. push data layout: every field written back as it arrived --------------------------------------------------------------------

[shader("compute")]
[numthreads(1, 1, 1)]
void csPushLayout(uniform Push pc)
{
    StoreOut(pc, 0, pc.texSlot);
    StoreOut(pc, 1, pc.storageTexSlot);
    StoreOut(pc, 2, pc.pointSampler);
    StoreOut(pc, 3, pc.linearSampler);
    StoreOut(pc, 4, pc.count);
    StoreOut(pc, 5, pc.tlasSlot);
    StoreOutFloat(pc, 6, pc.params.x);
    StoreOutFloat(pc, 7, pc.params.y);
    StoreOutFloat(pc, 8, pc.params.z);
    StoreOutFloat(pc, 9, pc.params.w);
    StoreOut(pc, 10, pc.uparams.x);
    StoreOut(pc, 11, pc.uparams.y);
    StoreOut(pc, 12, pc.uparams.z);
    StoreOut(pc, 13, pc.uparams.w);
    StoreOut(pc, 14, uint(pc.tlasAddress & 0xFFFFFFFFull));
    StoreOut(pc, 15, uint(pc.tlasAddress >> 32));
}

// --- 2. read a buffer, write a buffer -------------------------------------------------------------------------------------------------

[shader("compute")]
[numthreads(64, 1, 1)]
void csBufferCopy(uint3 id : SV_DispatchThreadID, uniform Push pc)
{
    if (id.x < pc.count)
        StoreOut(pc, id.x, LoadIn(pc, id.x) * 3u + 1u);
}

// --- 3. groupshared memory and barriers: a tree sum per group of 64 ---------------------------------------------------------------------

groupshared uint gSum[64];

[shader("compute")]
[numthreads(64, 1, 1)]
void csGroupShared(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint lane : SV_GroupIndex, uniform Push pc)
{
    gSum[lane] = id.x < pc.count ? LoadIn(pc, id.x) : 0u;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 32; stride > 0; stride >>= 1)
    {
        if (lane < stride)
            gSum[lane] += gSum[lane + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane == 0)
        StoreOut(pc, group.x, gSum[0]);
}

// --- 4. atomics on a buffer -------------------------------------------------------------------------------------------------------------

[shader("compute")]
[numthreads(64, 1, 1)]
void csAtomics(uint3 id : SV_DispatchThreadID, uniform Push pc)
{
    if (id.x >= pc.count)
        return;
    const uint value = LoadIn(pc, id.x);
    AtomicAddOut(pc, 0, value);
    AtomicMaxOut(pc, 1, value);
    AtomicAddOut(pc, 2, 1u);
}

// --- 5. wave intrinsics ------------------------------------------------------------------------------------------------------------------

[shader("compute")]
[numthreads(64, 1, 1)]
void csWave(uint3 id : SV_DispatchThreadID, uniform Push pc)
{
    // out[0]: the wave size; per thread i: [1 + 3i] lane index, [2 + 3i] prefix count of active lanes, [3 + 3i] active lane count.
    if (id.x == 0)
        StoreOut(pc, 0, WaveGetLaneCount());
    StoreOut(pc, 1 + id.x * 3, WaveGetLaneIndex());
    StoreOut(pc, 2 + id.x * 3, WavePrefixSum(1u));
    StoreOut(pc, 3 + id.x * 3, WaveActiveSum(1u));
}

// --- 6. integer operations ---------------------------------------------------------------------------------------------------------------

[shader("compute")]
[numthreads(64, 1, 1)]
void csIntOps(uint3 id : SV_DispatchThreadID, uniform Push pc)
{
    if (id.x >= pc.count)
        return;
    const uint u = LoadIn(pc, id.x);
    const int s = int(u);
    const int divisor = int(pc.uparams.x); // (negative: -7)
    const uint base = id.x * 10;
    StoreOut(pc, base + 0, countbits(u));
    StoreOut(pc, base + 1, firstbithigh(u));   // 0xFFFFFFFF for 0 on both APIs
    StoreOut(pc, base + 2, firstbitlow(u));
    StoreOut(pc, base + 3, reversebits(u));
    StoreOut(pc, base + 4, uint(s / divisor)); // truncates toward zero
    StoreOut(pc, base + 5, uint(s % divisor)); // the sign of the dividend
    StoreOut(pc, base + 6, uint(s >> 3));      // arithmetic shift
    StoreOut(pc, base + 7, u >> 3);            // logical shift
    StoreOut(pc, base + 8, u * 2654435761u);   // wraps
    StoreOut(pc, base + 9, uint(abs(s)));
}

// --- 7. float math (compared with a tolerance) ----------------------------------------------------------------------------------------

[shader("compute")]
[numthreads(64, 1, 1)]
void csFloatMath(uint3 id : SV_DispatchThreadID, uniform Push pc)
{
    if (id.x >= pc.count)
        return;
    const float x = LoadInFloat(pc, id.x);   // in -3.1 .. 3.1
    const uint base = id.x * 10;
    StoreOutFloat(pc, base + 0, sin(x));
    StoreOutFloat(pc, base + 1, cos(x));
    StoreOutFloat(pc, base + 2, exp2(x));
    StoreOutFloat(pc, base + 3, log2(abs(x) + 1.0));
    StoreOutFloat(pc, base + 4, sqrt(abs(x)));
    StoreOutFloat(pc, base + 5, rsqrt(abs(x) + 0.5));
    StoreOutFloat(pc, base + 6, pow(abs(x) + 0.25, 1.5));
    StoreOutFloat(pc, base + 7, frac(x * 3.7));
    StoreOutFloat(pc, base + 8, mad(x, 1.25, -0.5));
    StoreOut(pc, base + 9, (isnan(sqrt(x - 10.0)) ? 1u : 0u) | (isinf(1.0 / (x - x)) ? 2u : 0u));
}

// --- 8. 16-bit floats (needs shaderFloat16 / native 16-bit shader ops) -------------------------------------------------------------------

[shader("compute")]
[numthreads(64, 1, 1)]
void csHalf(uint3 id : SV_DispatchThreadID, uniform Push pc)
{
    if (id.x >= pc.count)
        return;
    const float16_t a = float16_t(LoadInFloat(pc, id.x));
    const float16_t b = float16_t(1.5);
    const float16_t c = float16_t(-0.25);
    const float16_t r = a * b + c;
    StoreOutFloat(pc, id.x, float(r));
}

// --- 9. 64-bit integers ------------------------------------------------------------------------------------------------------------------

[shader("compute")]
[numthreads(64, 1, 1)]
void csInt64(uint3 id : SV_DispatchThreadID, uniform Push pc)
{
    if (id.x >= pc.count)
        return;
    const uint64_t a = (uint64_t(LoadIn(pc, id.x)) << 32) | uint64_t(id.x * 2654435761u);
    const uint64_t r = a * 6364136223846793005ull + 1442695040888963407ull;
    StoreOut(pc, id.x * 2 + 0, uint(r & 0xFFFFFFFFull));
    StoreOut(pc, id.x * 2 + 1, uint(r >> 32));
}

// --- 10. matrix layout: a float4x4 read from a buffer, multiplied by a vector -----------------------------------------------------------

float4x4 LoadMatrixColumnMajor(Push pc, uint first)
{
    // The buffer holds four columns of four floats (the CPU writes them that way); built explicitly, so the result does not depend
    // on how each API lays out a matrix read straight from memory.
    float4 c0 = float4(LoadInFloat(pc, first + 0), LoadInFloat(pc, first + 1), LoadInFloat(pc, first + 2), LoadInFloat(pc, first + 3));
    float4 c1 = float4(LoadInFloat(pc, first + 4), LoadInFloat(pc, first + 5), LoadInFloat(pc, first + 6), LoadInFloat(pc, first + 7));
    float4 c2 = float4(LoadInFloat(pc, first + 8), LoadInFloat(pc, first + 9), LoadInFloat(pc, first + 10), LoadInFloat(pc, first + 11));
    float4 c3 = float4(LoadInFloat(pc, first + 12), LoadInFloat(pc, first + 13), LoadInFloat(pc, first + 14), LoadInFloat(pc, first + 15));
    return transpose(float4x4(c0, c1, c2, c3)); // (float4x4(a, b, c, d) takes rows)
}

[shader("compute")]
[numthreads(1, 1, 1)]
void csMatrix(uniform Push pc)
{
    const float4x4 m = LoadMatrixColumnMajor(pc, 0);
    const float4 v = float4(LoadInFloat(pc, 16), LoadInFloat(pc, 17), LoadInFloat(pc, 18), LoadInFloat(pc, 19));
    const float4 mv = mul(m, v);   // M * v
    const float4 vm = mul(v, m);   // v * M (= transpose(M) * v)
    for (uint i = 0; i < 4; ++i)
    {
        StoreOutFloat(pc, i, mv[i]);
        StoreOutFloat(pc, 4 + i, vm[i]);
        StoreOutFloat(pc, 8 + i, m[1][i]); // row 1
    }
}

// --- 11. sampled texture: nearest and linear samplers -------------------------------------------------------------------------------------

[shader("compute")]
[numthreads(16, 1, 1)]
void csTextureSample(uint3 id : SV_DispatchThreadID, uniform Push pc)
{
    // A 4 x 4 RGBA8 texture; 16 sample points: (i % 4, i / 4) texel centres with the nearest sampler (exact texels), then the same
    // points + half a texel with the linear sampler (the average of four texels).
    Texture2D texture = Texture2D.Handle(uint2(pc.texSlot, 0));
    SamplerState pointSampler = SamplerState.Handle(uint2(pc.pointSampler, 0));
    SamplerState linearSampler = SamplerState.Handle(uint2(pc.linearSampler, 0));
    const uint i = id.x;
    const float2 centre = (float2(i % 4, i / 4) + 0.5) / 4.0;
    const float4 nearest = texture.SampleLevel(pointSampler, centre, 0);
    const float2 between = min(centre + 0.5 / 4.0, float2(3.0 / 4.0, 3.0 / 4.0)); // (stays inside: clamp addressing not needed)
    const float4 linear = texture.SampleLevel(linearSampler, between, 0);
    for (uint c = 0; c < 4; ++c)
    {
        StoreOutFloat(pc, i * 8 + c, nearest[c]);
        StoreOutFloat(pc, i * 8 + 4 + c, linear[c]);
    }
}

// --- 12. storage texture: write every texel, the host copies it back ------------------------------------------------------------------

[shader("compute")]
[numthreads(8, 8, 1)]
void csStorageTexture(uint3 id : SV_DispatchThreadID, uniform Push pc)
{
    RWTexture2D<float4> output = RWTexture2D<float4>.Handle(uint2(pc.storageTexSlot, 0));
    const uint2 size = pc.uparams.xy;
    if (any(id.xy >= size))
        return;
    // RGBA8 UNORM: values the host can predict exactly (multiples of 1/255).
    output[id.xy] = float4(float(id.x * 4) / 255.0, float(id.y * 4) / 255.0, float((id.x ^ id.y) & 0xFF) / 255.0, 1.0);
}

// --- 13. ray query (needs ray tracing: the acceleration structure from its address on Vulkan, from a descriptor on D3D12) ----------------

[shader("compute")]
[numthreads(8, 8, 1)]
void csRayQuery(uint3 id : SV_DispatchThreadID, uniform Push pc)
{
    // One triangle (0,0,5) (1,0,5) (0,1,5); 8 x 8 rays straight along +Z from a grid starting at -0.23 in steps of 0.1875 (no ray
    // lands on an edge, where APIs may break ties differently). out = hit t (5.0) or -1.
#ifdef NOX_VULKAN
    RaytracingAccelerationStructure tlas = RaytracingAccelerationStructure(pc.tlasAddress);
#else
    RaytracingAccelerationStructure tlas = RaytracingAccelerationStructure.Handle(uint2(pc.tlasSlot, 0));
#endif
    RayDesc ray;
    ray.Origin = float3(-0.23 + (float(id.x) + 0.5) * (1.5 / 8.0), -0.23 + (float(id.y) + 0.5) * (1.5 / 8.0), 0.0);
    ray.Direction = float3(0.0, 0.0, 1.0);
    ray.TMin = 0.0;
    ray.TMax = 100.0;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFF, ray);
    query.Proceed();
    const float t = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? query.CommittedRayT() : -1.0;
    StoreOutFloat(pc, id.y * 8 + id.x, t);
}

// =====================================================================================================================================
// Graphics: rendered offscreen into a 64 x 64 RGBA8 target (cleared to opaque black) with NoxRHI's +Y-up viewport
// (setViewportWithCount with a negative height: Vulkan is flipped to D3D's convention, D3D12 needs no flip), then copied back.
// uparams.x picks the shape; the host knows which pixels each shape must touch.
// =====================================================================================================================================

struct GfxOut
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    nointerpolation float4 flatColor : COLOR1;
};

GfxOut MakeVertex(float2 xy, float z, float4 color)
{
    GfxOut o;
    o.position = float4(xy, z, 1.0);
    o.color = color;
    o.flatColor = color;
    return o;
}

// One of six quads' corners: quad q spans x0..x1 over the whole height at depth z.
GfxOut QuadVertex(uint vid, float x0, float x1, float z, float4 color)
{
    static const float2 corners[6] = { float2(0, 0), float2(1, 0), float2(1, 1), float2(0, 0), float2(1, 1), float2(0, 1) };
    const float2 c = corners[vid % 6];
    return MakeVertex(float2(lerp(x0, x1, c.x), lerp(-1.0, 1.0, c.y)), z, color);
}

[shader("vertex")]
GfxOut vsGfx(uint vid : SV_VertexID, uniform Push pc)
{
    const float4 red = float4(1, 0, 0, 1), green = float4(0, 1, 0, 1), blue = float4(0, 0, 1, 1), white = float4(1, 1, 1, 1);
    switch (pc.uparams.x)
    {
    case 0: // orientation: a triangle in the top-left corner (NDC +Y up)
    {
        const float2 p[3] = { float2(-1, 1), float2(0, 1), float2(-1, 0) };
        return MakeVertex(p[vid], 0.5, red);
    }
    case 1: // culling: a counter-clockwise triangle on the left (green), a clockwise one on the right (blue)
    {
        const float2 p[6] = { float2(-0.9, -0.5), float2(-0.1, -0.5), float2(-0.5, 0.5),
                              float2(0.1, -0.5), float2(0.5, 0.5), float2(0.9, -0.5) };
        return MakeVertex(p[vid], 0.5, vid < 3 ? green : blue);
    }
    case 2: // depth: far green over everything, near blue on the left half, middle red on the right 3/4, drawn in that order
    {
        const uint quad = vid / 6;
        if (quad == 0) return QuadVertex(vid, -1.0, 1.0, 0.8, green);
        if (quad == 1) return QuadVertex(vid, -1.0, 0.0, 0.2, blue);
        return QuadVertex(vid, -0.5, 1.0, 0.5, red);
    }
    case 3: // blending: the whole target, colour (1, 0.5, 0.25) at alpha 0.5
        return QuadVertex(vid, -1.0, 1.0, 0.5, float4(1.0, 0.5, 0.25, 0.5));
    case 4: // interpolation: a triangle over the whole target, red / green / blue corners (the flat colour: the first vertex's)
    {
        const float2 p[3] = { float2(-1, -1), float2(3, -1), float2(-1, 3) };
        const float4 c[3] = { red, green, blue };
        return MakeVertex(p[vid], 0.5, c[vid]);
    }
    default: // 5, MSAA: white below the diagonal from the top-left to the bottom-right corner
    {
        const float2 p[3] = { float2(-1, -1), float2(1, -1), float2(-1, 1) };
        return MakeVertex(p[vid], 0.5, white);
    }
    }
}

[shader("fragment")]
float4 psGfx(GfxOut input, uniform Push pc) : SV_Target
{
    // Interpolation: the smooth colour, and in alpha the red of the flat (provoking vertex) colour -- 1 when the first vertex provokes.
    if (pc.uparams.x == 4)
        return float4(input.color.rgb, input.flatColor.r);
    return input.color;
}

// Mesh shading: the orientation test's triangle from a task + mesh shader pair.
struct MeshPayload
{
    uint unused;
};
groupshared MeshPayload gMeshPayload; // (the mesh shader must take the payload the task shader sends: D3D12 checks their sizes match)

[shader("amplification")]
[numthreads(1, 1, 1)]
void tsGfx()
{
    gMeshPayload.unused = 0;
    DispatchMesh(1, 1, 1, gMeshPayload);
}

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(3, 1, 1)]
// (Slang's own output types, not HLSL's "out indices" / "out vertices" modifiers: some Slang versions write those back out with a second
// "out", which dxc 1.10 rejects)
void msGfx(uint tid : SV_GroupIndex, in payload MeshPayload meshPayload, OutputIndices<uint3, 1> triangles, OutputVertices<GfxOut, 3> verts, uniform Push pc)
{
    SetMeshOutputCounts(3, 1);
    const float2 p[3] = { float2(-1, 1), float2(0, 1), float2(-1, 0) };
    if (tid < 3)
        verts[tid] = MakeVertex(p[tid], 0.5, float4(1, 0, 0, 1));
    if (tid == 0)
        triangles[0] = uint3(0, 1, 2);
}
