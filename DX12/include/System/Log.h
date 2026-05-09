#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

// Log levels
enum class LogLevel
{
    Success,
    Info,
    Warning,
    Error
};

// Pending log work (producer pushes, consumer pops and writes)
struct LogEntry
{
    LogLevel level;
    std::string message;
    std::string file;
    int line;
    std::chrono::system_clock::time_point timestamp;
};

// Async singleton logger: the main thread only enqueues; the worker thread formats and writes to disk
class Logger
{
public:
    static Logger& Get();

    void Initialize(const std::string& fileName = "DX12Log.txt");
    void Shutdown();

    // General log (printf-style): formats and enqueues only; does not block on I/O
    void Log(LogLevel level, const char* file, int line, const char* format, ...);

    // Specialized HRESULT logging (also queries the system error string)
    void LogHResult(const char* expr, HRESULT hr, const char* file, int line);

    /** For the ImGui log panel: get a thread-safe copy of buffered log lines. Each line is (LogLevel, full string). */
    void GetLinesForImGui(std::vector<std::pair<LogLevel, std::string>>& out) const;

private:
    Logger() = default;
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    const char* LevelToString(LogLevel level) const;
    /** Worker loop: pop tasks, format, write to file, and update the ImGui buffer. */
    void ProcessLogs();
    void WriteLine(const std::string& line);
    void PushLineForImGui(LogLevel level, const std::string& line);

private:
    static const size_t kMaxImGuiLines = 2000;

    std::ofstream m_file;
    mutable std::mutex m_mutex;
    std::queue<LogEntry> m_queue;
    std::thread m_worker;
    std::condition_variable m_cv;
    std::atomic<bool> m_running{ false };
    bool m_initialized = false;
    // Ring-style recent-log buffer. std::deque pop_front is O(1); the old
    // std::vector-with-erase shifted kMaxImGuiLines strings on every add, which
    // dominated the log worker's CPU once the buffer was full.
    std::deque<std::pair<LogLevel, std::string>> m_imGuiLines;
};

// Convert a Windows wide string to a UTF-8 std::string for logging/output
std::string WStringToString(const wchar_t* wstr);

// ---------------------------------------------------------------------------
// Macros: control output by level and Debug/Release
// - Debug: output all levels
// - Release: output only Error
// ---------------------------------------------------------------------------

#define LOG_INTERNAL(level, format, ...) \
    do { \
        Logger::Get().Log((level), __FILE__, __LINE__, (format), ##__VA_ARGS__); \
    } while (0)

#define LOG_SUCCESS(format, ...) LOG_INTERNAL(LogLevel::Success, (format), ##__VA_ARGS__)
#define LOG_INFO(format, ...)    LOG_INTERNAL(LogLevel::Info,    (format), ##__VA_ARGS__)
#define LOG_WARNING(format, ...) LOG_INTERNAL(LogLevel::Warning, (format), ##__VA_ARGS__)
#define LOG_ERROR(format, ...)   LOG_INTERNAL(LogLevel::Error,   (format), ##__VA_ARGS__)

// HRESULT helper macro (use after DX calls)
#define LOG_HRESULT(expr, hr)                                             \
    do {                                                                  \
        Logger::Get().LogHResult((expr), (hr), __FILE__, __LINE__);       \
    } while (0)

// Resource loading macros
#define LOG_RESOURCE_LOADING(name)                                        \
    LOG_INFO("Loading resource: %s", (name))

#define LOG_RESOURCE_LOADED(name)                                         \
    LOG_SUCCESS("Loaded resource: %s", (name))

#define LOG_RESOURCE_FAILED(name)                                         \
    LOG_ERROR("Failed to load resource: %s", (name))

