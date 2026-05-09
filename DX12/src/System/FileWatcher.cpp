#include "System/FileWatcher.h"

#include "System/Log.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cwchar>

namespace HotReload
{

namespace
{
    std::string Narrow(const wchar_t* w, DWORD wlen)
    {
        if (wlen == 0) return {};
        const int len = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(wlen),
                                            nullptr, 0, nullptr, nullptr);
        std::string out(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(wlen),
                            out.data(), len, nullptr, nullptr);
        return out;
    }

    void Normalize(std::string& s)
    {
        for (char& c : s)
        {
            if (c == '\\') c = '/';
            else           c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        }
    }
}

bool FileWatcher::Init(const std::string& dir)
{
    if (m_running.load()) return true;

    m_dir = dir;

    HANDLE h = CreateFileA(
        dir.c_str(), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);

    if (h == INVALID_HANDLE_VALUE)
    {
        LOG_ERROR("FileWatcher::Init: CreateFile('%s') failed (err=%lu)",
                  dir.c_str(), GetLastError());
        return false;
    }

    m_dirHandle = h;
    m_running.store(true);
    m_thread = std::thread([this]{ WorkerLoop(); });

    LOG_INFO("FileWatcher: watching '%s'", dir.c_str());
    return true;
}

void FileWatcher::Shutdown()
{
    if (!m_running.exchange(false)) return;

    // Closing the handle wakes any in-flight ReadDirectoryChangesW with
    // ERROR_OPERATION_ABORTED, letting the worker thread exit cleanly. The
    // worker also checks m_running so it bails immediately on the next loop.
    if (m_dirHandle)
    {
        CancelIoEx(static_cast<HANDLE>(m_dirHandle), nullptr);
        CloseHandle(static_cast<HANDLE>(m_dirHandle));
        m_dirHandle = nullptr;
    }
    if (m_thread.joinable())
        m_thread.join();
}

std::vector<std::string> FileWatcher::PollChanges()
{
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_pending.empty()) return out;

    out.reserve(m_pending.size());
    for (auto& p : m_pending) out.push_back(std::move(const_cast<std::string&>(p)));
    m_pending.clear();
    return out;
}

void FileWatcher::WorkerLoop()
{
    // Buffer aligned for DWORD per ReadDirectoryChangesW docs. 16 KB is enough
    // for hundreds of pending entries; if it ever overflows, ReadDirectory
    // reports ERROR_NOTIFY_ENUM_DIR — we fall back to "anything could have
    // changed" by emitting an empty-string sentinel.
    alignas(DWORD) std::byte buffer[16 * 1024];

    OVERLAPPED ov{};
    HANDLE evt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!evt) return;
    ov.hEvent = evt;

    while (m_running.load())
    {
        DWORD bytesReturned = 0;
        BOOL ok = ReadDirectoryChangesW(
            static_cast<HANDLE>(m_dirHandle),
            buffer, static_cast<DWORD>(sizeof(buffer)),
            TRUE,           // recursive
            FILE_NOTIFY_CHANGE_LAST_WRITE
            | FILE_NOTIFY_CHANGE_FILE_NAME
            | FILE_NOTIFY_CHANGE_SIZE,
            &bytesReturned, &ov, nullptr);

        if (!ok)
        {
            const DWORD err = GetLastError();
            if (err == ERROR_OPERATION_ABORTED) break;   // shutdown
            LOG_WARNING("FileWatcher: ReadDirectoryChangesW failed (err=%lu)", err);
            break;
        }

        // Wait for completion or shutdown.
        const DWORD wait = WaitForSingleObject(evt, INFINITE);
        if (wait != WAIT_OBJECT_0 || !m_running.load()) break;

        DWORD transferred = 0;
        if (!GetOverlappedResult(static_cast<HANDLE>(m_dirHandle),
                                 &ov, &transferred, FALSE))
            break;
        ResetEvent(evt);

        if (transferred == 0)
        {
            // Buffer overflow — too many events. Signal "something changed".
            std::lock_guard<std::mutex> lk(m_mutex);
            m_pending.insert("");
            continue;
        }

        // Walk the linked list of FILE_NOTIFY_INFORMATION records.
        const std::byte* p = buffer;
        for (;;)
        {
            const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
            std::string path = Narrow(info->FileName,
                                      info->FileNameLength / sizeof(wchar_t));
            Normalize(path);

            if (!path.empty())
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                m_pending.insert(std::move(path));
            }

            if (info->NextEntryOffset == 0) break;
            p += info->NextEntryOffset;
        }
    }

    CloseHandle(evt);
}

} // namespace HotReload
