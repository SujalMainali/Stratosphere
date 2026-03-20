#include "utils/JobSystem.h"

#include <algorithm>

namespace Engine
{
    JobSystem::JobSystem(uint32_t workerCount)
        : m_workerCount(workerCount)
    {
        if (m_workerCount == 0)
        {
            m_running.store(false, std::memory_order_release);
            return;
        }

        m_running.store(true, std::memory_order_release);
        m_workers.reserve(m_workerCount);
        for (uint32_t i = 0; i < m_workerCount; ++i)
        {
            m_workers.emplace_back([this, i]()
                                   { this->workerMain(i); });
        }
    }

    JobSystem::~JobSystem()
    {
        if (!m_running.load(std::memory_order_acquire))
            return;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_running.store(false, std::memory_order_release);
            ++m_jobId; // wake all
        }
        m_cvStart.notify_all();

        for (auto &t : m_workers)
        {
            if (t.joinable())
                t.join();
        }
        m_workers.clear();
    }

    void JobSystem::parallelFor(uint32_t itemCount, const JobFn &fn)
    {
        if (itemCount == 0)
            return;

        // This job system supports one in-flight parallelFor at a time.
        std::lock_guard<std::mutex> submitLock(m_submitMutex);

        // No workers: run on calling thread.
        if (m_workerCount == 0 || !m_running.load(std::memory_order_acquire))
        {
            for (uint32_t i = 0; i < itemCount; ++i)
                fn(0, i);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_jobFn = fn;
            m_itemCount = itemCount;
            m_nextIndex.store(0, std::memory_order_release);
            m_activeWorkers = m_workerCount;
            ++m_jobId;
        }

        // Wake workers.
        m_cvStart.notify_all();

        // The calling thread participates as "workerIndex = m_workerCount".
        while (true)
        {
            const uint32_t idx = m_nextIndex.fetch_add(1, std::memory_order_acq_rel);
            if (idx >= itemCount)
                break;
            fn(m_workerCount, idx);
        }

        // Wait for workers to finish.
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cvDone.wait(lock, [this]()
                      { return m_activeWorkers == 0; });
        m_jobFn = nullptr;
        m_itemCount = 0;
    }

    void JobSystem::workerMain(uint32_t workerIndex)
    {
        uint64_t seenJobId = 0;
        while (true)
        {
            JobFn fn;
            uint32_t count = 0;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cvStart.wait(lock, [this, &seenJobId]()
                               { return !m_running.load(std::memory_order_acquire) || m_jobId != seenJobId; });

                if (!m_running.load(std::memory_order_acquire))
                    break;

                seenJobId = m_jobId;

                fn = m_jobFn;
                count = m_itemCount;
            }

            // Execute until exhausted.
            while (true)
            {
                const uint32_t idx = m_nextIndex.fetch_add(1, std::memory_order_acq_rel);
                if (idx >= count)
                    break;
                if (fn)
                    fn(workerIndex, idx);
            }

            // Signal done.
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_activeWorkers > 0)
                    --m_activeWorkers;
                if (m_activeWorkers == 0)
                    m_cvDone.notify_one();
            }
        }

        // Thread exits.
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_activeWorkers > 0)
                --m_activeWorkers;
            if (m_activeWorkers == 0)
                m_cvDone.notify_one();
        }
    }
}
