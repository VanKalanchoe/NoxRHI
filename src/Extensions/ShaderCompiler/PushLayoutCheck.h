#pragma once
// Push data must be laid out the same on both APIs, because the host writes one C++ struct for both. NoxRHI compiles Vulkan with
// scalar layout (tightly packed, like C), but on D3D12 push data is a constant buffer (root constants) with HLSL's packing: a struct
// member starts a new 16-byte row (and so does the member after it), and a vector may not cross a 16-byte boundary. A push struct that
// differs reads the wrong bytes on D3D12 without any error -- so the D3D12 compile checks every entry point's push data against the
// scalar layout and reports the first field that moved.

#include <algorithm>
#include <cstddef>
#include <string>

#include <slang.h>

namespace NRI
{
    namespace PushLayoutDetail
    {
        inline size_t AlignUp(size_t value, size_t alignment) { return alignment > 1 ? (value + alignment - 1) / alignment * alignment : value; }

        inline size_t ScalarBytes(slang::TypeReflection* type)
        {
            switch (type ? type->getScalarType() : slang::TypeReflection::ScalarType::None)
            {
            case slang::TypeReflection::ScalarType::Int64:
            case slang::TypeReflection::ScalarType::UInt64:
            case slang::TypeReflection::ScalarType::Float64: return 8;
            case slang::TypeReflection::ScalarType::Int16:
            case slang::TypeReflection::ScalarType::UInt16:
            case slang::TypeReflection::ScalarType::Float16: return 2;
            case slang::TypeReflection::ScalarType::Int8:
            case slang::TypeReflection::ScalarType::UInt8: return 1;
            default: return 4;
            }
        }

        // Alignment and size of a type in Vulkan's scalar layout.
        inline size_t ScalarAlignment(slang::TypeLayoutReflection* layout)
        {
            switch (layout->getKind())
            {
            case slang::TypeReflection::Kind::Struct:
            {
                size_t alignment = 1;
                for (unsigned i = 0; i < layout->getFieldCount(); ++i)
                    alignment = std::max(alignment, ScalarAlignment(layout->getFieldByIndex(i)->getTypeLayout()));
                return alignment;
            }
            case slang::TypeReflection::Kind::Array: return ScalarAlignment(layout->getElementTypeLayout());
            case slang::TypeReflection::Kind::Vector:
            case slang::TypeReflection::Kind::Matrix: return ScalarBytes(layout->getElementTypeLayout() ? layout->getElementTypeLayout()->getType() : layout->getType());
            default: return ScalarBytes(layout->getType());
            }
        }

        inline size_t ScalarSize(slang::TypeLayoutReflection* layout)
        {
            switch (layout->getKind())
            {
            case slang::TypeReflection::Kind::Struct:
            {
                size_t cursor = 0;
                for (unsigned i = 0; i < layout->getFieldCount(); ++i)
                {
                    slang::TypeLayoutReflection* field = layout->getFieldByIndex(i)->getTypeLayout();
                    cursor = AlignUp(cursor, ScalarAlignment(field)) + ScalarSize(field);
                }
                return AlignUp(cursor, ScalarAlignment(layout));
            }
            case slang::TypeReflection::Kind::Array:
            {
                slang::TypeLayoutReflection* element = layout->getElementTypeLayout();
                return layout->getElementCount() * AlignUp(ScalarSize(element), ScalarAlignment(element));
            }
            case slang::TypeReflection::Kind::Vector: return layout->getType()->getElementCount() * ScalarAlignment(layout);
            case slang::TypeReflection::Kind::Matrix: return layout->getType()->getRowCount() * layout->getType()->getColumnCount() * ScalarAlignment(layout);
            default: return ScalarBytes(layout->getType());
            }
        }

        // Walks a struct's fields: `actualBase` is where the struct starts in the D3D12 layout, `scalarBase` in the scalar one.
        inline bool CompareFields(slang::TypeLayoutReflection* layout, size_t actualBase, size_t scalarBase, const std::string& path, std::string& difference)
        {
            if (layout->getKind() != slang::TypeReflection::Kind::Struct)
                return true;
            size_t cursor = scalarBase;
            for (unsigned i = 0; i < layout->getFieldCount(); ++i)
            {
                slang::VariableLayoutReflection* field = layout->getFieldByIndex(i);
                slang::TypeLayoutReflection* type = field->getTypeLayout();
                const size_t scalarOffset = AlignUp(cursor, ScalarAlignment(type));
                const size_t actualOffset = actualBase + field->getOffset(slang::ParameterCategory::Uniform);
                const std::string name = path + "." + (field->getName() ? field->getName() : "?");
                if (actualOffset != scalarOffset)
                {
                    difference = "'" + name + "' is at byte " + std::to_string(actualOffset) + " on D3D12 but byte " + std::to_string(scalarOffset) +
                                 " on Vulkan (the host struct's layout)";
                    return false;
                }
                if (!CompareFields(type, actualOffset, scalarOffset, name, difference))
                    return false;
                cursor = scalarOffset + ScalarSize(type);
            }
            return true;
        }
    }

    // For a linked D3D12 program of one entry point: empty when its push data (the entry point's uniform parameters) has the same
    // layout as on Vulkan; otherwise which field moved and where.
    inline std::string CheckPushLayoutMatchesScalar(slang::IComponentType* linked)
    {
        slang::ProgramLayout* program = linked ? linked->getLayout(0) : nullptr;
        slang::EntryPointReflection* entry = program && program->getEntryPointCount() > 0 ? program->getEntryPointByIndex(0) : nullptr;
        if (!entry)
            return {};
        size_t scalarCursor = 0;
        for (unsigned i = 0; i < entry->getParameterCount(); ++i)
        {
            slang::VariableLayoutReflection* parameter = entry->getParameterByIndex(i);
            if (parameter->getCategory() != slang::ParameterCategory::Uniform)
                continue; // (varyings, system values)
            slang::TypeLayoutReflection* type = parameter->getTypeLayout();
            const size_t scalarOffset = PushLayoutDetail::AlignUp(scalarCursor, PushLayoutDetail::ScalarAlignment(type));
            const size_t actualOffset = parameter->getOffset(slang::ParameterCategory::Uniform);
            const std::string name = parameter->getName() ? parameter->getName() : "?";
            std::string difference;
            if (actualOffset != scalarOffset)
                return "'" + name + "' is at byte " + std::to_string(actualOffset) + " on D3D12 but byte " + std::to_string(scalarOffset) + " on Vulkan";
            if (!PushLayoutDetail::CompareFields(type, actualOffset, scalarOffset, name, difference))
                return difference;
            scalarCursor = scalarOffset + PushLayoutDetail::ScalarSize(type);
        }
        return {};
    }
}
