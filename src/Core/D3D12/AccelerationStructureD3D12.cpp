#include "AccelerationStructureD3D12.h"
#include "BufferD3D12.h"
#include "DeviceD3D12.h"

namespace NRI
{
    D3D12AccelerationStructureInputs::D3D12AccelerationStructureInputs(const AccelerationStructureBuildDesc& desc, bool performUpdate)
    {
        using Flags = AccelerationStructureBuildFlags;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
        if (desc.flags & Flags::AllowUpdate)
            flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
        if (desc.flags & Flags::AllowCompaction)
            flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
        if (desc.flags & Flags::PreferFastTrace)
            flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        else if (desc.flags & Flags::PreferFastBuild) // (the two are exclusive on D3D12; fast trace wins, as on most Vulkan drivers)
            flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        if (desc.flags & Flags::LowMemory)
            flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_MINIMIZE_MEMORY;
        if (performUpdate)
            flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        inputs.Flags = flags;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;

        if (desc.type == AccelerationStructureType::BottomLevel)
        {
            inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            geometries.resize(desc.triangles.size());
            for (size_t i = 0; i < desc.triangles.size(); ++i)
            {
                const AccelerationStructureTrianglesDesc& tri = desc.triangles[i];
                D3D12_RAYTRACING_GEOMETRY_DESC& g = geometries[i];
                g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                g.Flags = tri.isOpaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
                // Vulkan's meaning of the fields: firstVertex is added to every index (D3D12 has no such offset: the vertex buffer
                // starts there instead), maxVertex is the highest index used (D3D12 wants a count), primitiveOffset is in bytes.
                g.Triangles.VertexBuffer.StartAddress = tri.vertexBufferAddress + uint64_t(tri.firstVertex) * tri.vertexStride;
                g.Triangles.VertexBuffer.StrideInBytes = tri.vertexStride;
                g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
                g.Triangles.VertexCount = tri.maxVertex + 1;
                g.Triangles.IndexBuffer = tri.indexBufferAddress + tri.primitiveOffset;
                g.Triangles.IndexCount = tri.primitiveCount * 3;
                g.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
            }
            inputs.NumDescs = static_cast<UINT>(geometries.size());
            inputs.pGeometryDescs = geometries.data();
        }
        else
        {
            inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
            inputs.NumDescs = desc.instances.instanceCount;
            inputs.InstanceDescs = desc.instances.instanceBufferAddress;
        }
    }

    AccelerationStructureD3D12::AccelerationStructureD3D12(DeviceD3D12& device, const AccelerationStructureDesc& desc)
        : m_deviceD3D12(device), m_type(desc.type), m_storageBuffer(desc.storageBuffer), m_size(desc.size)
    {
        if (m_storageBuffer)
        {
            m_deviceAddress = m_storageBuffer->getDeviceAddress() + desc.bufferOffset;
        }
    }
}
