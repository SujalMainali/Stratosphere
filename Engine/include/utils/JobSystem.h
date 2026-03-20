#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace Engine
{
    // A small, barrier-style job system meant for predictable "parallel-for" style workloads.
    // - No per-job allocations.
    // - Workers are persistent threads.
    // - Each parallelFor call distributes indices via an atomic counter.
    //
    // This is intentionally minimal: enough to safely introduce parallelism system-by-system.
    class JobSystem
    {
    public:
        using JobFn = std::function<void(uint32_t workerIndex, uint32_t itemIndex)>;

        // If workerCount == 0, the job system runs work on the calling thread only.
        explicit JobSystem(uint32_t workerCount);
        ~JobSystem();

        JobSystem(const JobSystem &) = delete;
        JobSystem &operator=(const JobSystem &) = delete;

        uint32_t workerCount() const { return m_workerCount; }
        bool isRunning() const { return m_running; }

        // Run fn over [0, itemCount) in parallel. Blocks until complete.
        void parallelFor(uint32_t itemCount, const JobFn &fn);

    private:
        void workerMain(uint32_t workerIndex);

    private:
        uint32_t m_workerCount = 0;
        std::vector<std::thread> m_workers;

        std::atomic<bool> m_running{false};

        // Serialize submissions; this job system runs one parallelFor at a time.
        std::mutex m_submitMutex;

        // Current job group
        std::mutex m_mutex;
        std::condition_variable m_cvStart;
        std::condition_variable m_cvDone;
        uint64_t m_jobId = 0;
        uint32_t m_activeWorkers = 0;

        std::atomic<uint32_t> m_nextIndex{0};
        uint32_t m_itemCount = 0;
        JobFn m_jobFn;
    };
}
