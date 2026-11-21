// nox_gpu.slang -- one shader source for Vulkan and Direct3D 12 with NoxRHI.
//
// NoxRHI's Slang compiler defines NOX_VULKAN for SPIR-V and NOX_DX12 for DXIL. Everything that differs between the two is in this file,
// so a shader that includes it has no #ifdefs of its own:
//
//   buffers   Vulkan reaches them by device address (pointers, "BDA"); D3D12 has no pointers in shaders, so it reaches them through
//             bindless descriptors (ByteAddressBuffer / RWByteAddressBuffer handles). BufferRef<T> / RWBufferRef<T> hold either, in 64 bits.
//             The host fills them with nox::BufferRefs (ExampleApp.h): the address on Vulkan, a descriptor slot on D3D12.
//   TLAS      by address on Vulkan, by descriptor on D3D12 (registerAccelerationStructure): LoadTlas.
//   textures  bindless handles on both (no difference): Tex2D, RWTex2D, Sampler.
//   8-bit     DXIL has no 8-bit types: read bytes out of 32-bit words with LoadByte (the same code on Vulkan).
//   non-uniform texture indices: SampleNonUniform (see there: Slang drops NonUniform for Vulkan descriptor heaps).
//
// Struct layout: Vulkan uses scalar layout (NoxRHI forces it), D3D12's ByteAddressBuffer.Load<T> uses C-like layout -- the same for
// structs of 32-bit scalars and vectors. Keep 64-bit members 8-byte aligned and use float4 rather than float3 in arrays of structs;
// ShaderCompat's csStructLayout checks a struct read through BufferRef on both APIs.

// PUSH DATA: keep BufferRef / RWBufferRef (and every other struct) out of push structs. On D3D12, push data is a constant buffer
// (root constants) with HLSL packing: a struct member starts a new 16-byte row, and so does the member after it -- while Vulkan (scalar
// layout) packs them tightly, so the two APIs would read different bytes. Store the reference as a plain uint64_t and expose the typed
// reference with a property, which costs no space:
//
//     struct DrawPush
//     {
//         uint64_t particlesRef;
//         property BufferRef<Particle> particles { get { return BufferRef<Particle>(particlesRef); } }
//     };
//
// Scalars and vectors in push data follow the same rule as constant buffers: a vector must not cross a 16-byte boundary on D3D12.

// A read-only buffer of T (D3D12: an SRV from registerBuffer).
struct BufferRef<T>
{
    uint64_t handle;

    __init(uint64_t h) { handle = h; }

    T Load(uint index)
    {
#ifdef NOX_VULKAN
        return ((T*)handle)[index];
#else
        return ByteAddressBuffer.Handle(uint2(uint(handle), 0)).Load<T>(index * sizeof(T));
#endif
    }

    __subscript(uint index)->T { get { return Load(index); } }
};

// A read-write buffer of T (D3D12: a UAV from registerStorageBuffer).
struct RWBufferRef<T>
{
    uint64_t handle;

    __init(uint64_t h) { handle = h; }

    T Load(uint index)
    {
#ifdef NOX_VULKAN
        return ((T*)handle)[index];
#else
        return RWByteAddressBuffer.Handle(uint2(uint(handle), 0)).Load<T>(index * sizeof(T));
#endif
    }

    void Store(uint index, T value)
    {
#ifdef NOX_VULKAN
        ((T*)handle)[index] = value;
#else
        RWByteAddressBuffer.Handle(uint2(uint(handle), 0)).Store<T>(index * sizeof(T), value);
#endif
    }
};

// Atomics on a uint buffer (counters, append lists).
uint AtomicAdd(RWBufferRef<uint> buffer, uint index, uint value)
{
    uint previous = 0;
#ifdef NOX_VULKAN
    InterlockedAdd(((uint*)buffer.handle)[index], value, previous);
#else
    RWByteAddressBuffer.Handle(uint2(uint(buffer.handle), 0)).InterlockedAdd(index * 4, value, previous);
#endif
    return previous;
}

// Writes `value` at [index] if it holds `compare`; returns what it held (claims a slot once, e.g. the first error report).
uint AtomicCompareExchange(RWBufferRef<uint> buffer, uint index, uint compare, uint value)
{
    uint previous = 0;
#ifdef NOX_VULKAN
    InterlockedCompareExchange(((uint*)buffer.handle)[index], compare, value, previous);
#else
    RWByteAddressBuffer.Handle(uint2(uint(buffer.handle), 0)).InterlockedCompareExchange(index * 4, compare, value, previous);
#endif
    return previous;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// GPU checks: catch an out-of-range index before it faults the GPU. On Vulkan a buffer reference is a raw device address, so a bad index
// loses the device with no message; D3D12 reads through bounds-checked descriptors and returns 0, so the same bug looks fine there.
//   NOX_CHECK(condition, a, b)  -- when `condition` is false: counts a failure and reports the first one's line and the values a, b.
//   NoxIndex(index, count)      -- `index` when it is below `count`, else 0 (reported with index and count): the read stays in range.
// Both compile to nothing / to `index` unless the compiler defines NOX_SHADER_CHECKS (the examples: --shader-checks; any app:
// nriShaderCompilerSetDefine("NOX_SHADER_CHECKS", "1") plus NOX_CHECKS_SLOT). The report is an R32_SINT storage texture in the resource
// heap's image range at slot NOX_CHECKS_SLOT -- [0] failures, [1] first failure's line, [2] / [3] its values -- read back by the host.
#ifdef NOX_SHADER_CHECKS
#ifndef NOX_CHECKS_SLOT
#error NOX_SHADER_CHECKS needs NOX_CHECKS_SLOT: the image slot of the report texture
#endif
void NoxCheckFailed(uint line, uint a, uint b)
{
    RWTexture2D<int> report = RWTexture2D<int>.Handle(uint2(NOX_CHECKS_SLOT, 0));
    int previous;
    InterlockedAdd(report[uint2(0, 0)], 1, previous);
    int first;
    InterlockedCompareExchange(report[uint2(1, 0)], 0, int(line), first);
    if (first == 0)
    {
        report[uint2(2, 0)] = int(a);
        report[uint2(3, 0)] = int(b);
    }
}

uint NoxCheckedIndex(uint index, uint count, uint line)
{
    if (index < count)
        return index;
    NoxCheckFailed(line, index, count);
    return 0;
}

#define NOX_CHECK(condition, a, b) do { if (!(condition)) NoxCheckFailed(__LINE__, uint(a), uint(b)); } while (false)
#define NoxIndex(index, count) NoxCheckedIndex(uint(index), uint(count), __LINE__)
#else
#define NOX_CHECK(condition, a, b) do { } while (false)
#define NoxIndex(index, count) (index)
#endif

// One byte of a buffer of packed bytes (DXIL has no uint8_t: four bytes per word, little-endian, as the CPU wrote them).
uint LoadByte(BufferRef<uint> words, uint byteIndex)
{
    return (words.Load(byteIndex >> 2) >> ((byteIndex & 3) * 8)) & 0xFF;
}

// The top-level acceleration structure for ray queries.
RaytracingAccelerationStructure LoadTlas(uint64_t handle)
{
#ifdef NOX_VULKAN
    return RaytracingAccelerationStructure(handle);
#else
    return RaytracingAccelerationStructure.Handle(uint2(uint(handle), 0));
#endif
}

// A column-major 4 x 4 matrix as four columns (ExampleMath.h's Mat4 on the host). Multiplying with the columns explicitly means no
// matrix-layout setting (row / column major, per API or per buffer kind) can change its meaning.
struct Mat4
{
    float4 c0, c1, c2, c3;

    float4 Transform(float4 v) { return c0 * v.x + c1 * v.y + c2 * v.z + c3 * v.w; }
    float3 TransformPoint(float3 p) { return (c0 * p.x + c1 * p.y + c2 * p.z + c3).xyz; }
    float3 TransformVector(float3 v) { return (c0 * v.x + c1 * v.y + c2 * v.z).xyz; }
};

// Samples a texture whose slot may differ between the lanes of a wave (per-instance or per-pixel material textures).
//   D3D12: NonUniformResourceIndex reaches DXIL (CreateHandleFromHeap's nonUniformIndex flag).
//   Vulkan: Slang (checked with 2026.14.1 and 2026.22.1) emits no NonUniform decoration for an index into VK_EXT_descriptor_heap's heap,
//   so a non-uniform index is not legal there. Until it does, the lanes are served one slot at a time (scalarization): every pass of
//   the loop takes the slot of the first active lane, so each heap index is uniform across the wave. Gradients are taken before the
//   loop, because implicit derivatives are undefined in divergent control flow.
float4 SampleNonUniform(uint slot, SamplerState samplerState, float2 uv)
{
#ifdef NOX_VULKAN
    const float2 dx = ddx(uv), dy = ddy(uv);
    float4 result = float4(0.0, 0.0, 0.0, 0.0);
    for (;;)
    {
        const uint first = WaveReadLaneFirst(slot);
        if (first == slot)
        {
            result = Texture2D.Handle(uint2(first, 0)).SampleGrad(samplerState, uv, dx, dy);
            break;
        }
    }
    return result;
#else
    return Texture2D.Handle(uint2(NonUniformResourceIndex(slot), 0)).Sample(samplerState, uv);
#endif
}

// Bindless textures and samplers: the same on both APIs. `slot` is the texture's descriptor slot plus the heap's image offset
// (DescriptorHeap::getImageHeapIndexOffset, added on the host); storage slots come from registerStorageTextureMip.
Texture2D Tex2D(uint slot) { return Texture2D.Handle(uint2(slot, 0)); }
RWTexture2D<float4> RWTex2D(uint slot) { return RWTexture2D<float4>.Handle(uint2(slot, 0)); }
SamplerState Sampler(uint slot) { return SamplerState.Handle(uint2(slot, 0)); }
