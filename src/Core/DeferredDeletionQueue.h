#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>

namespace NRI
{
    struct QueueTimelineValues
    {
        uint64_t graphics = 0;
        uint64_t transfer = 0;
        uint64_t compute = 0;
    };

    // Thread-safe deferred deletion queue keyed by GPU queue timeline values.
    // Every destroyed GPU resource is held here until all queues have reached or
    // passed the timeline values that were submitted when the resource was released.
    class DeferredDeletionQueue
    {
    public:
        template <typename Fn>
        void enqueue(const QueueTimelineValues& fenceValues, Fn&& destroyer)
        {
            std::lock_guard lock(m_mutex);
            m_entries.push_back(Entry{ fenceValues, std::function<void()>(std::forward<Fn>(destroyer)) });
        }

        void collect(const QueueTimelineValues& completed)
        {
            std::deque<Entry> ready;
            {
                std::lock_guard lock(m_mutex);
                auto it = m_entries.begin();
                while (it != m_entries.end())
                {
                    if (it->values.graphics <= completed.graphics &&
                        it->values.transfer <= completed.transfer &&
                        it->values.compute <= completed.compute)
                    {
                        ready.push_back(std::move(*it));
                        it = m_entries.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }
            for (Entry& entry : ready)
            {
                if (entry.destroy)
                    entry.destroy();
            }
        }

        void drain()
        {
            for (;;)
            {
                std::deque<Entry> batch;
                {
                    std::lock_guard lock(m_mutex);
                    if (m_entries.empty())
                        return;
                    batch.swap(m_entries);
                }
                for (Entry& entry : batch)
                {
                    if (entry.destroy)
                        entry.destroy();
                }
            }
        }

        bool empty() const
        {
            std::lock_guard lock(m_mutex);
            return m_entries.empty();
        }

    private:
        struct Entry
        {
            QueueTimelineValues values;
            std::function<void()> destroy;
        };

        mutable std::mutex m_mutex;
        std::deque<Entry> m_entries;
    };
}
