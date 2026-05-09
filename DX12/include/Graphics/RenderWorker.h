#pragma once

// RenderWorker — one dedicated CPU thread for per-frame parallel CL recording.
//
// Uses binary_semaphore for nanosecond-latency wakeup, which is critical for
// tight per-frame synchronisation where TaskSystem's condition_variable latency
// (~1-5 us) would eliminate the overlap benefit.
//
// TaskSystem is better suited for fire-and-forget async work (resource I/O).
// RenderWorker is better suited for fixed, per-frame GPU recording steps.
//
// Lifecycle per frame:
//   1. Main thread sets job and calls Kick().
//   2. Worker thread wakes, executes job(), signals doneSem.
//   3. Main thread calls doneSem.acquire() to sync.
//
// Shutdown: call Shutdown() after GPU is idle (before device destruction).

#include <semaphore>
#include <thread>
#include <functional>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>   // SetThreadDescription

struct RenderWorker
{
    int                   index = 0;
    std::binary_semaphore kickSem{ 0 };
    std::binary_semaphore doneSem{ 0 };
    std::function<void()> job;
    std::thread           thread;

    // Spawn the persistent worker loop.  Call once at startup.
    void Start(const wchar_t* name = L"RenderWorker")
    {
        thread = std::thread([this]
        {
            while (true)
            {
                kickSem.acquire();
                if (!job) break;   // null job = shutdown signal
                job();
                doneSem.release();
            }
        });

        // Name the thread so it appears in VS / RenderDoc / PIX thread views.
        if (thread.native_handle())
            SetThreadDescription(static_cast<HANDLE>(thread.native_handle()), name);
    }

    // Submit a job and wake the worker.
    void Kick(std::function<void()> j)
    {
        job = std::move(j);
        kickSem.release();
    }

    // Signal shutdown and join.  Call after GPU is idle.
    void Shutdown()
    {
        job = nullptr;
        kickSem.release();
        if (thread.joinable()) thread.join();
    }
};
