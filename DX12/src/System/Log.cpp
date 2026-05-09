#include "System/Log.h"

#include <chrono>
#include <ctime>
#include <cstdarg>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>

Logger& Logger::Get()
{
    static Logger instance;
    return instance;
}

Logger::~Logger()
{
    Shutdown();
}

void Logger::Initialize(const std::string& fileName)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_initialized)
            return;

        m_file.open(fileName, std::ios::out | std::ios::app);
        m_initialized = m_file.is_open();
        if (!m_initialized)
            return;

        m_running = true;
    }

    m_worker = std::thread(&Logger::ProcessLogs, this);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push(LogEntry{
            LogLevel::Info,
            "========== Logger initialized ==========",
            "",
            0,
            std::chrono::system_clock::now()
        });
    }
    m_cv.notify_one();
}

void Logger::Shutdown()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_initialized)
            return;

        m_queue.push(LogEntry{
            LogLevel::Info,
            "========== Logger shutdown ==========",
            "",
            0,
            std::chrono::system_clock::now()
        });
        m_running = false;
    }
    m_cv.notify_one();

    if (m_worker.joinable())
        m_worker.join();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_file.is_open())
            m_file.close();
        m_initialized = false;
    }
}

const char* Logger::LevelToString(LogLevel level) const
{
    switch (level)
    {
    case LogLevel::Success: return "SUCCESS";
    case LogLevel::Info:    return "INFO";
    case LogLevel::Warning: return "WARNING";
    case LogLevel::Error:   return "ERROR";
    default:                return "UNKNOWN";
    }
}

void Logger::ProcessLogs()
{
    // Reusable batch of entries drained per wake-up. One lock acquire covers
    // the whole batch (vs one per entry); large batches amortise the
    // condition_variable wake-up cost.
    std::vector<LogEntry> batch;
    batch.reserve(128);

    // Cache the formatted "YYYY-MM-DD HH:MM:SS" timestamp; only rebuild when
    // the wall-clock second changes. localtime_s + put_time are surprisingly
    // expensive (~microseconds) and every log entry in the same second has
    // the same prefix anyway.
    std::time_t cachedSec = 0;
    char        cachedStamp[20] = { 0 };  // "YYYY-MM-DD HH:MM:SS\0"

    const bool debuggerAttached = (IsDebuggerPresent() != FALSE);

    while (true)
    {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return !m_queue.empty() || !m_running; });

            if (!m_running && m_queue.empty())
                break;

            // Drain the whole queue at once.
            while (!m_queue.empty())
            {
                batch.emplace_back(std::move(m_queue.front()));
                m_queue.pop();
            }
        }

        bool sawError = false;
        for (auto& entry : batch)
        {
            // Second-granularity timestamp cache.
            const std::time_t sec =
                std::chrono::system_clock::to_time_t(entry.timestamp);
            if (sec != cachedSec)
            {
                cachedSec = sec;
                std::tm localTime{};
                localtime_s(&localTime, &sec);
                std::strftime(cachedStamp, sizeof(cachedStamp),
                              "%Y-%m-%d %H:%M:%S", &localTime);
            }

            // Build the formatted line with one reserve'd std::string instead
            // of two ostringstreams + many small concatenations.
            std::string line;
            line.reserve(96 + entry.file.size() + entry.message.size());
            line.push_back('[');
            line.append(cachedStamp);
            line.append("] [");
            line.append(LevelToString(entry.level));
            line.append("] [");
            line.append(entry.file);
            line.push_back(':');
            line.append(std::to_string(entry.line));
            line.append("] ");
            line.append(entry.message);

            // File write — "\n" (no flush). Error entries flush once below.
            if (m_file.is_open())
            {
                m_file.write(line.data(), static_cast<std::streamsize>(line.size()));
                m_file.put('\n');
            }

            // OutputDebugStringA is a syscall. Skip it unless a debugger is
            // attached — ETL/sampling profilers don't attach as debugger.
            if (debuggerAttached)
            {
                std::string withNewLine = line;
                withNewLine.push_back('\n');
                OutputDebugStringA(withNewLine.c_str());
            }

            sawError = sawError || (entry.level == LogLevel::Error);

            // ImGui buffer update: deque pop_front is O(1) — replaces the old
            // vector::erase(begin, begin+N) that shifted ~kMaxImGuiLines on
            // every add once the buffer was full.
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_imGuiLines.emplace_back(entry.level, std::move(line));
                while (m_imGuiLines.size() > kMaxImGuiLines)
                    m_imGuiLines.pop_front();
            }
        }

        // Flush once per batch when an error appeared — old code flushed per
        // error entry, expensive when several errors queue up back-to-back.
        if (sawError && m_file.is_open())
            m_file.flush();

        batch.clear();
    }
}

void Logger::WriteLine(const std::string& line)
{
    // Kept for API compatibility; ProcessLogs no longer uses it.
    if (m_file.is_open())
    {
        m_file.write(line.data(), static_cast<std::streamsize>(line.size()));
        m_file.put('\n');
    }
}

void Logger::PushLineForImGui(LogLevel level, const std::string& line)
{
    m_imGuiLines.emplace_back(level, line);
    while (m_imGuiLines.size() > kMaxImGuiLines)
        m_imGuiLines.pop_front();
}

void Logger::GetLinesForImGui(std::vector<std::pair<LogLevel, std::string>>& out) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    out.assign(m_imGuiLines.begin(), m_imGuiLines.end());
}

void Logger::Log(LogLevel level, const char* file, int line, const char* format, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    std::string message(buffer);
    std::string fileStr(file ? file : "");

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push(LogEntry{
            level,
            std::move(message),
            std::move(fileStr),
            line,
            std::chrono::system_clock::now()
        });
    }
    m_cv.notify_one();
}

void Logger::LogHResult(const char* expr, HRESULT hr, const char* file, int line)
{
    char* messageBuffer = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                  FORMAT_MESSAGE_FROM_SYSTEM |
                  FORMAT_MESSAGE_IGNORE_INSERTS;

    DWORD length = FormatMessageA(
        flags,
        nullptr,
        static_cast<DWORD>(hr),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&messageBuffer),
        0,
        nullptr);

    std::string hrMessage;
    if (length != 0 && messageBuffer != nullptr)
    {
        hrMessage.assign(messageBuffer, length);
        LocalFree(messageBuffer);
    }

    std::ostringstream oss;
    oss << "HRESULT failed: " << expr
        << " (hr=0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr) << ")";

    if (!hrMessage.empty())
    {
        while (!hrMessage.empty() && (hrMessage.back() == '\n' || hrMessage.back() == '\r'))
            hrMessage.pop_back();
        oss << " - " << hrMessage;
    }

    Log(LogLevel::Error, file, line, "%s", oss.str().c_str());
}

std::string WStringToString(const wchar_t* wstr)
{
    if (!wstr)
        return {};

    int size_needed = WideCharToMultiByte(
        CP_UTF8,
        0,
        wstr,
        -1,
        nullptr,
        0,
        nullptr,
        nullptr);

    if (size_needed <= 0)
        return {};

    std::string strTo(static_cast<size_t>(size_needed - 1), 0);

    WideCharToMultiByte(
        CP_UTF8,
        0,
        wstr,
        -1,
        &strTo[0],
        size_needed,
        nullptr,
        nullptr);

    return strTo;
}
