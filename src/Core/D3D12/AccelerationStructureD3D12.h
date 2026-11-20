#pragma once
#include <vector>
#include "D3D12Common.h"
#include "../AccelerationStructure.h"

namespace NRI
{
    class DeviceD3D12;

    // One NRI build description as D3D12 build inputs. The size query, the build and the update all use it: D3D12 requires the
    // geometry and the flags of a build to be the ones its sizes were queried with.
    struct D3D12AccelerationStructureInputs
    {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries; // (inputs.pGeometryDescs points here: do not copy the struct)

        D3D12AccelerationStructureInputs(const AccelerationStructureBuildDesc& desc, bool performUpdate);
        D3D12AccelerationStructureInputs(const D3D12AccelerationStructureInputs&) = delete;
        D3D12AccelerationStructureInputs& operator=(const D3D12AccelerationStructureInputs&) = delete;
    };

    class AccelerationStructureD3D12 final : public AccelerationStructure
    {
    public:
        AccelerationStructureD3D12(DeviceD3D12& device, const AccelerationStructureDesc& desc);
        ~AccelerationStructureD3D12() override = default;

        uint64_t getDeviceAddress() const override { return m_deviceAddress; }
        uint64_t getSize() const override { return m_size; }
        AccelerationStructureType getType() const override { return m_type; }
        Buffer* getBuffer() const override { return m_storageBuffer; }
        DeviceD3D12& getDevice() const { return m_deviceD3D12; }

    private:
        DeviceD3D12& m_deviceD3D12;
        AccelerationStructureType m_type;
        Buffer* m_storageBuffer = nullptr;
        uint64_t m_size = 0;
        uint64_t m_deviceAddress = 0;
    };
}
