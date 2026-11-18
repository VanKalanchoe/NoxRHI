#pragma once
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <typeindex>
#include <unordered_map>
#include <utility>

namespace nri
{
    // Typed per-frame store for handles/data shared across render graph passes.
    // Storage persists across frames; entries are valid only in the frame they were added.
    class RGBlackboard
    {
    public:
        template <typename T>
        T& Add(T value)
        {
            Holder<T>& holder = GetHolder<T>();
            holder.Value = std::move(value);
            holder.ValidFrame = m_Frame;
            return holder.Value;
        }

        template <typename T>
        T* TryGet()
        {
            auto found = m_Entries.find(std::type_index(typeid(T)));
            if (found == m_Entries.end() || found->second->ValidFrame != m_Frame)
                return nullptr;
            return &static_cast<Holder<T>*>(found->second.get())->Value;
        }

        template <typename T>
        T& Get()
        {
            T* value = TryGet<T>();
            if (!value)
                throw std::runtime_error("RGBlackboard entry was not added this frame");
            return *value;
        }

        void NextFrame() { ++m_Frame; }

    private:
        struct HolderBase
        {
            virtual ~HolderBase() = default;
            uint64_t ValidFrame = ~0ull;
        };

        template <typename T>
        struct Holder final : HolderBase
        {
            T Value{};
        };

        template <typename T>
        Holder<T>& GetHolder()
        {
            std::unique_ptr<HolderBase>& entry = m_Entries[std::type_index(typeid(T))];
            if (!entry)
                entry = std::make_unique<Holder<T>>();
            return static_cast<Holder<T>&>(*entry);
        }

    private:
        std::unordered_map<std::type_index, std::unique_ptr<HolderBase>> m_Entries;
        uint64_t m_Frame = 0;
    };
}
