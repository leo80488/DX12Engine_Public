#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

/**
 * Unified thread pool (Task System) shared by Resource Manager, physics, animation, etc.
 * - Worker count tracks hardware cores (N-1) to avoid excessive threads and context switching.
 * - Priority queues: High (physics/animation) before Low (resource loading/I/O) so critical work doesn't stall.
 */
class TaskSystem
{
public:
    enum class TaskPriority
    {
        Low,   // Resource loading, I/O
        High   // Physics, animation updates, etc.
    };

    static TaskSystem& Get();

    TaskSystem(const TaskSystem&) = delete;
    TaskSystem& operator=(const TaskSystem&) = delete;

    /** Submit a task; workers execute High before Low. */
    void Push(std::function<void()> task, TaskPriority priority = TaskPriority::Low);

    /** Block until all currently queued tasks have completed. */
    void WaitAll();

    /** Parallel for: splits [begin, end) across worker threads, blocks until done.
     *  fn(uint32_t index) is called for each index in [begin, end). */
    template<typename Func>
    void ParallelFor(uint32_t begin, uint32_t end, Func&& fn,
                     TaskPriority pri = TaskPriority::High)
    {
        if (begin >= end) return;
        const uint32_t count = end - begin;
        const uint32_t numChunks = (std::min)(count, m_numWorkers);
        const uint32_t chunkSize = (count + numChunks - 1) / numChunks;

        std::atomic<uint32_t> remaining(numChunks);
        std::mutex doneMtx;
        std::condition_variable doneCv;

        for (uint32_t t = 0; t < numChunks; ++t)
        {
            uint32_t lo = begin + t * chunkSize;
            uint32_t hi = (std::min)(end, lo + chunkSize);
            Push([&fn, lo, hi, &remaining, &doneMtx, &doneCv]()
            {
                for (uint32_t i = lo; i < hi; ++i)
                    fn(i);
                std::lock_guard<std::mutex> lk(doneMtx);
                remaining.fetch_sub(1, std::memory_order_relaxed);
                doneCv.notify_one();
            }, pri);
        }

        std::unique_lock<std::mutex> lk(doneMtx);
        doneCv.wait(lk, [&]{ return remaining.load(std::memory_order_acquire) == 0; });
    }

    unsigned GetWorkerCount() const { return m_numWorkers; }

    /** Call at program shutdown; stops all workers. */
    void Shutdown();

private:
    TaskSystem();
    ~TaskSystem();
    void WorkerLoop();

    unsigned m_numWorkers;
    std::vector<std::thread> m_workers;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::queue<std::function<void()>> m_highQueue;
    std::queue<std::function<void()>> m_lowQueue;
    std::atomic<bool> m_running{ true };
};
