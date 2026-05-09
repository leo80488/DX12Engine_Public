#pragma once

// FileWatcher — async directory watch via ReadDirectoryChangesW.
//
// Background thread receives WRITE/RENAME events for files under a directory,
// pushes the changed paths into a deduplicating set. Main thread drains via
// PollChanges() — never blocks for filesystem I/O.
//
// Used by hot-reload: watch shaders/ for .hlsl edits, signal the renderer to
// rebuild affected PSOs.

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace HotReload
{

class FileWatcher
{
public:
    FileWatcher() = default;
    ~FileWatcher() { Shutdown(); }

    FileWatcher(const FileWatcher&)            = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    // Begin watching @p dir recursively. Returns false if the directory can't
    // be opened. Call Shutdown() before destruction.
    bool Init(const std::string& dir);

    // Stop the worker thread, close the directory handle. Idempotent.
    void Shutdown();

    // Drain the pending change set. Returns paths (relative to the watched
    // dir, lowercased, forward slashes) seen since the last call. Never
    // blocks. Empty result == nothing happened.
    std::vector<std::string> PollChanges();

private:
    void WorkerLoop();

    std::string                    m_dir;
    void*                          m_dirHandle = nullptr;   // HANDLE; void* keeps <windows.h> out of the header
    std::thread                    m_thread;
    std::atomic<bool>              m_running{false};
    std::mutex                     m_mutex;
    std::unordered_set<std::string> m_pending;             // dedup
};

} // namespace HotReload
