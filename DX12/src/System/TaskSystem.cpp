#include "System/TaskSystem.h"
#include <algorithm>

TaskSystem::TaskSystem()
{
    unsigned hw = std::thread::hardware_concurrency();
	m_numWorkers = (std::max)(1u, hw > 0 ? hw - 2u : 1u); // 1 for main thread, 1 for logger thread
    m_workers.reserve(m_numWorkers);
    for (unsigned i = 0; i < m_numWorkers; ++i)
        m_workers.emplace_back(&TaskSystem::WorkerLoop, this);
}

TaskSystem::~TaskSystem()
{
    Shutdown();
}

TaskSystem& TaskSystem::Get()
{
    static TaskSystem s_instance;
    return s_instance;
}

void TaskSystem::Push(std::function<void()> task, TaskPriority priority)
{
    if (!task)
        return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (priority == TaskPriority::High)
            m_highQueue.push(std::move(task));
        else
            m_lowQueue.push(std::move(task));
    }
    m_cv.notify_one();
}

void TaskSystem::WaitAll()
{
    // Spin-wait until both queues are empty AND no workers are busy.
    // Simple and correct for frame-sync use cases.
    while (true)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_highQueue.empty() && m_lowQueue.empty())
                return;
        }
        std::this_thread::yield();
    }
}

void TaskSystem::Shutdown()
{
    m_running = false;
    m_cv.notify_all();
    for (auto& w : m_workers)
    {
        if (w.joinable())
            w.join();
    }
    m_workers.clear();
}

void TaskSystem::WorkerLoop()
{
    while (true)
    {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] {
                return !m_highQueue.empty() || !m_lowQueue.empty() || !m_running;
            });

            if (!m_running && m_highQueue.empty() && m_lowQueue.empty())
                break;

            if (!m_highQueue.empty())
            {
                task = std::move(m_highQueue.front());
                m_highQueue.pop();
            }
            else if (!m_lowQueue.empty())
            {
                task = std::move(m_lowQueue.front());
                m_lowQueue.pop();
            }
        }

        if (task)
            task();
    }
}
