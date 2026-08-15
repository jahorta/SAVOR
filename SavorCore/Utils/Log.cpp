#include "Log.h"

#include "ThreadName.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <share.h>
#include <windows.h>
#endif

namespace savor::logger {
namespace {

constexpr std::size_t kQueueCapacity = 16 * 1024;
constexpr auto kFlushInterval = std::chrono::milliseconds(100);
constexpr std::size_t kLevelCount = static_cast<std::size_t>(Level::Off) + 1;

[[nodiscard]] std::size_t LevelIndex(Level level) noexcept
{
    const auto value = static_cast<std::size_t>(level);
    return value < kLevelCount ? value : static_cast<std::size_t>(Level::Off);
}

[[nodiscard]] const char* LevelTag(Level level) noexcept
{
    switch (level)
    {
    case Level::Trace: return "TRACE";
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO ";
    case Level::Warn: return "WARN ";
    case Level::Error: return "ERROR";
    case Level::Fatal: return "FATAL";
    default: return "     ";
    }
}

[[nodiscard]] const char* LevelColor(Level level) noexcept
{
    switch (level)
    {
    case Level::Trace: return "\x1b[90m";
    case Level::Debug: return "\x1b[36m";
    case Level::Info: return "\x1b[37m";
    case Level::Warn: return "\x1b[33m";
    case Level::Error: return "\x1b[31m";
    case Level::Fatal: return "\x1b[41;97m";
    default: return "";
    }
}

[[nodiscard]] bool IsValidTagChar(char value) noexcept
{
    const unsigned char ch = static_cast<unsigned char>(value);
    return (ch >= '0' && ch <= '9') ||
        (ch >= 'A' && ch <= 'Z') ||
        (ch >= 'a' && ch <= 'z') ||
        value == '_' || value == '-' || value == '.';
}

[[nodiscard]] std::string FormatMessage(const char* format, std::va_list args)
{
    std::va_list copy;
    va_copy(copy, args);
    const int length = std::vsnprintf(nullptr, 0, format, copy);
    va_end(copy);
    if (length <= 0)
        return {};

    std::vector<char> buffer(static_cast<std::size_t>(length) + 1);
    std::vsnprintf(buffer.data(), buffer.size(), format, args);
    return std::string(buffer.data(), static_cast<std::size_t>(length));
}

[[nodiscard]] std::string ShortenPath(
    const char* full,
    std::string_view anchor)
{
    if (full == nullptr)
        return {};

    const std::string_view path(full);
    std::size_t basename = 0;
    std::optional<std::size_t> best;
    const auto equal_ascii = [](char lhs, char rhs) {
        if (lhs >= 'A' && lhs <= 'Z') lhs = static_cast<char>(lhs - 'A' + 'a');
        if (rhs >= 'A' && rhs <= 'Z') rhs = static_cast<char>(rhs - 'A' + 'a');
        return lhs == rhs;
    };
    const auto segment_matches = [&](std::size_t start) {
        if (anchor.empty() || start + anchor.size() >= path.size())
            return false;
        for (std::size_t index = 0; index < anchor.size(); ++index)
            if (!equal_ascii(path[start + index], anchor[index])) return false;
        const char following = path[start + anchor.size()];
        return following == '/' || following == '\\';
    };

    if (segment_matches(0))
        best = anchor.size() + 1;
    for (std::size_t index = 0; index < path.size(); ++index)
    {
        if (path[index] != '/' && path[index] != '\\')
            continue;
        basename = index + 1;
        if (segment_matches(index + 1))
            best = index + 1 + anchor.size() + 1;
    }
    return std::string(path.substr(best.value_or(basename)));
}

[[nodiscard]] std::string TimestampNow()
{
    using Clock = std::chrono::system_clock;
    const auto now = Clock::now();
    const auto milliseconds = std::chrono::duration_cast<
        std::chrono::milliseconds>(now.time_since_epoch()).count();
    const std::time_t time = Clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    std::array<char, 32> output{};
    std::snprintf(
        output.data(), output.size(), "%02d:%02d:%02d.%03lld",
        local.tm_hour, local.tm_min, local.tm_sec,
        static_cast<long long>(milliseconds % 1000));
    return output.data();
}

} // namespace

struct Logger::Impl
{
    struct Record
    {
        Level level = Level::Info;
        std::string timestamp;
        std::size_t thread_id = 0;
        std::string source;
        int line = 0;
        std::string function;
        std::string tags;
        std::string message;
        bool stdout_sink = false;
        bool file_sink = false;
        bool colors = false;
    };

    std::atomic<Level> stdout_level{Level::Info};
    std::atomic<Level> file_level{Level::Off};
    std::atomic<bool> colors{true};
    std::atomic<bool> accepting{true};
    std::atomic<std::shared_ptr<const std::string>> anchor{
        std::make_shared<const std::string>("SAVORRuntime")};

    std::mutex queue_mutex;
    std::condition_variable queue_changed;
    std::condition_variable queue_drained;
    std::deque<Record> queue;
    std::uint64_t accepted_records = 0;
    std::uint64_t written_records = 0;
    bool stopping = false;
    std::thread writer;

    std::array<std::atomic<std::uint64_t>, kLevelCount> dropped_busy{};
    std::array<std::atomic<std::uint64_t>, kLevelCount> dropped_full{};
    std::atomic<std::uint64_t> dropped_format{0};
    std::array<std::atomic<std::uint64_t>, kLevelCount> total_dropped_busy{};
    std::array<std::atomic<std::uint64_t>, kLevelCount> total_dropped_full{};
    std::atomic<std::uint64_t> total_dropped_format{0};

    std::mutex sink_mutex;
    std::FILE* file = nullptr;

    Impl()
    {
#ifdef _WIN32
        HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        if (output != INVALID_HANDLE_VALUE)
        {
            DWORD mode = 0;
            if (GetConsoleMode(output, &mode))
                SetConsoleMode(output, mode | 0x0004);
        }
#endif
        Start();
    }

    void Start()
    {
        std::lock_guard lock(queue_mutex);
        if (writer.joinable())
            return;
        stopping = false;
        accepting.store(true, std::memory_order_release);
        writer = std::thread([this] { WriterMain(); });
    }

    [[nodiscard]] bool HasDroppedRecords() const noexcept
    {
        if (dropped_format.load(std::memory_order_relaxed) != 0)
            return true;
        for (std::size_t index = 0; index < kLevelCount; ++index)
        {
            if (dropped_busy[index].load(std::memory_order_relaxed) != 0 ||
                dropped_full[index].load(std::memory_order_relaxed) != 0)
            {
                return true;
            }
        }
        return false;
    }

    void WriteLine(const Record& record)
    {
        std::lock_guard lock(sink_mutex);
        const auto write = [&](std::FILE* sink, bool colored) {
            if (sink == nullptr)
                return;
            if (colored)
                std::fputs(LevelColor(record.level), sink);
            std::fprintf(
                sink, "[%s] [%s] [T%zu] (%s:%d %s) ",
                record.timestamp.c_str(), LevelTag(record.level),
                record.thread_id, record.source.c_str(), record.line,
                record.function.c_str());
            if (!record.tags.empty())
                std::fprintf(sink, "[tag=%s] ", record.tags.c_str());
            std::fputs(record.message.c_str(), sink);
            std::fputc('\n', sink);
            if (colored)
                std::fputs("\x1b[0m", sink);
        };
        if (record.stdout_sink)
            write(stdout, record.colors);
        if (record.file_sink && file != nullptr)
            write(file, false);
    }

    [[nodiscard]] bool WriteDropSummary()
    {
        std::array<std::uint64_t, kLevelCount> busy{};
        std::array<std::uint64_t, kLevelCount> full{};
        std::uint64_t busy_total = 0;
        std::uint64_t full_total = 0;
        for (std::size_t index = 0; index < kLevelCount; ++index)
        {
            busy[index] = dropped_busy[index].exchange(0);
            full[index] = dropped_full[index].exchange(0);
            busy_total += busy[index];
            full_total += full[index];
        }
        const std::uint64_t format = dropped_format.exchange(0);
        const std::uint64_t total = busy_total + full_total + format;
        if (total == 0)
            return false;

        Record summary;
        summary.level = Level::Warn;
        summary.timestamp = TimestampNow();
        summary.thread_id = std::hash<std::thread::id>{}(
            std::this_thread::get_id());
        summary.source = "Utils/Log.cpp";
        summary.function = "WriterMain";
        summary.tags = "logger.loss";
        summary.stdout_sink = Level::Warn >= stdout_level.load();
        summary.file_sink = Level::Warn >= file_level.load();
        summary.colors = colors.load();
        summary.message = "asynchronous logger dropped records: total=" +
            std::to_string(total) + ", queue_busy=" +
            std::to_string(busy_total) + ", queue_full=" +
            std::to_string(full_total) + ", formatting=" +
            std::to_string(format);
        for (std::size_t index = 0;
             index < static_cast<std::size_t>(Level::Off); ++index)
        {
            if (busy[index] == 0 && full[index] == 0)
                continue;
            summary.message += ", ";
            summary.message += LevelTag(static_cast<Level>(index));
            summary.message += "(busy=" + std::to_string(busy[index]) +
                ",full=" + std::to_string(full[index]) + ")";
        }
        WriteLine(summary);
        return true;
    }

    void FlushSinks()
    {
        std::lock_guard lock(sink_mutex);
        std::fflush(stdout);
        if (file != nullptr)
            std::fflush(file);
    }

    void WriterMain()
    {
        set_this_thread_name_utf8("SavorLogWriterV1");
        for (;;)
        {
            std::deque<Record> batch;
            bool should_stop = false;
            {
                std::unique_lock lock(queue_mutex);
                queue_changed.wait_for(
                    lock, kFlushInterval,
                    [this] {
                        return stopping || !queue.empty() || HasDroppedRecords();
                    });
                batch.swap(queue);
                should_stop = stopping && batch.empty();
            }

            bool urgent = false;
            for (const Record& record : batch)
            {
                WriteLine(record);
                urgent = urgent || record.level >= Level::Warn;
            }
            const bool wrote_loss_summary = WriteDropSummary();
            if (!batch.empty() || urgent || wrote_loss_summary || should_stop)
                FlushSinks();

            if (!batch.empty())
            {
                std::lock_guard lock(queue_mutex);
                written_records += batch.size();
                queue_drained.notify_all();
            }
            if (should_stop)
                return;
        }
    }

    void Enqueue(Record record) noexcept
    {
        if (!accepting.load(std::memory_order_acquire))
            return;
        std::unique_lock lock(queue_mutex, std::try_to_lock);
        if (!lock.owns_lock())
        {
            const std::size_t index = LevelIndex(record.level);
            dropped_busy[index].fetch_add(
                1, std::memory_order_relaxed);
            total_dropped_busy[index].fetch_add(
                1, std::memory_order_relaxed);
            queue_changed.notify_one();
            return;
        }
        if (!accepting.load(std::memory_order_relaxed))
            return;
        if (queue.size() >= kQueueCapacity)
        {
            const std::size_t index = LevelIndex(record.level);
            dropped_full[index].fetch_add(
                1, std::memory_order_relaxed);
            total_dropped_full[index].fetch_add(
                1, std::memory_order_relaxed);
            lock.unlock();
            queue_changed.notify_one();
            return;
        }
        queue.push_back(std::move(record));
        ++accepted_records;
        lock.unlock();
        queue_changed.notify_one();
    }

    void Flush()
    {
        std::uint64_t target = 0;
        {
            std::unique_lock lock(queue_mutex);
            if (!writer.joinable())
                return;
            target = accepted_records;
            queue_changed.notify_one();
            queue_drained.wait(lock, [this, target] {
                return written_records >= target || !writer.joinable();
            });
        }
        FlushSinks();
    }

    void Shutdown()
    {
        accepting.store(false, std::memory_order_release);
        {
            std::lock_guard lock(queue_mutex);
            if (!writer.joinable())
            {
                std::lock_guard sink_lock(sink_mutex);
                if (file != nullptr)
                {
                    std::fflush(file);
                    std::fclose(file);
                    file = nullptr;
                }
                return;
            }
            stopping = true;
        }
        queue_changed.notify_one();
        writer.join();
        {
            std::lock_guard lock(sink_mutex);
            std::fflush(stdout);
            if (file != nullptr)
            {
                std::fflush(file);
                std::fclose(file);
                file = nullptr;
            }
        }
    }
};

Logger& Logger::get()
{
    static Logger logger;
    return logger;
}

Logger::Logger() : impl_(std::make_unique<Impl>()) {}

Logger::~Logger()
{
    Shutdown();
}

void Logger::set_stdout_level(Level level)
{
    impl_->stdout_level.store(level, std::memory_order_release);
}

void Logger::set_file_level(Level level)
{
    impl_->file_level.store(level, std::memory_order_release);
}

void Logger::set_levels(Level stdout_level, Level file_level)
{
    set_stdout_level(stdout_level);
    set_file_level(file_level);
}

bool Logger::open_file(const char* path, bool append)
{
    if (path == nullptr || *path == '\0')
        return false;
    impl_->Start();
    impl_->Flush();
    std::lock_guard lock(impl_->sink_mutex);
    if (impl_->file != nullptr)
    {
        std::fflush(impl_->file);
        std::fclose(impl_->file);
        impl_->file = nullptr;
    }
#if defined(_WIN32)
    impl_->file = _fsopen(path, append ? "a" : "w", _SH_DENYWR);
#else
    impl_->file = std::fopen(path, append ? "a" : "w");
#endif
    return impl_->file != nullptr;
}

void Logger::close_file()
{
    impl_->Flush();
    std::lock_guard lock(impl_->sink_mutex);
    if (impl_->file != nullptr)
    {
        std::fflush(impl_->file);
        std::fclose(impl_->file);
        impl_->file = nullptr;
    }
}

void Logger::Flush()
{
    impl_->Flush();
}

void Logger::Shutdown() noexcept
{
    try
    {
        impl_->Shutdown();
    }
    catch (...)
    {
        // Emergency fallback only. Producer and worker teardown paths must not
        // turn a logging failure into process termination.
    }
}

void Logger::enable_colors(bool enabled)
{
    impl_->colors.store(enabled, std::memory_order_release);
}

bool Logger::enabled_stdout(Level level) const noexcept
{
    return impl_->accepting.load(std::memory_order_acquire) &&
        level >= impl_->stdout_level.load(std::memory_order_acquire);
}

bool Logger::enabled_file(Level level) const noexcept
{
    return impl_->accepting.load(std::memory_order_acquire) &&
        level >= impl_->file_level.load(std::memory_order_acquire);
}

bool Logger::enabled_any(Level level) const noexcept
{
    return enabled_stdout(level) || enabled_file(level);
}

LoggerStatistics Logger::statistics() const noexcept
{
    LoggerStatistics result;
    try
    {
        std::lock_guard lock(impl_->queue_mutex);
        result.accepted = impl_->accepted_records;
        result.written = impl_->written_records;
    }
    catch (...)
    {
        return result;
    }
    for (std::size_t index = 0; index < kLevelCount; ++index)
    {
        result.dropped_queue_busy[index] =
            impl_->total_dropped_busy[index].load(std::memory_order_relaxed);
        result.dropped_queue_full[index] =
            impl_->total_dropped_full[index].load(std::memory_order_relaxed);
    }
    result.dropped_formatting =
        impl_->total_dropped_format.load(std::memory_order_relaxed);
    return result;
}

std::string BuildTagCsv(std::initializer_list<const char*> tags)
{
    std::string output;
    for (const char* raw : tags)
    {
        if (raw == nullptr || *raw == '\0')
            continue;
        bool valid = true;
        for (const char* current = raw; *current != '\0'; ++current)
        {
            if (!IsValidTagChar(*current))
            {
                valid = false;
                break;
            }
        }
        if (!valid)
            continue;
        if (!output.empty())
            output.push_back(',');
        output.append(raw);
    }
    return output;
}

void Logger::vlogf_tags(
    Level level,
    const char* file,
    int line,
    const char* function,
    const char* tags,
    const char* format,
    std::va_list args) noexcept
{
    try
    {
        Impl::Record record;
        record.level = level;
        record.timestamp = TimestampNow();
        record.thread_id = std::hash<std::thread::id>{}(
            std::this_thread::get_id());
        const auto anchor = impl_->anchor.load(std::memory_order_acquire);
        record.source = ShortenPath(file, anchor ? *anchor : std::string_view{});
        record.line = line;
        record.function = function != nullptr ? function : "";
        record.tags = tags != nullptr ? tags : "";
        record.message = FormatMessage(format != nullptr ? format : "", args);
        record.stdout_sink =
            level >= impl_->stdout_level.load(std::memory_order_acquire);
        record.file_sink =
            level >= impl_->file_level.load(std::memory_order_acquire);
        record.colors = impl_->colors.load(std::memory_order_acquire);
        impl_->Enqueue(std::move(record));
    }
    catch (...)
    {
        impl_->dropped_format.fetch_add(1, std::memory_order_relaxed);
        impl_->total_dropped_format.fetch_add(1, std::memory_order_relaxed);
        impl_->queue_changed.notify_one();
    }
}

void Logger::vlogf(
    Level level,
    const char* file,
    int line,
    const char* function,
    const char* format,
    std::va_list args) noexcept
{
    vlogf_tags(level, file, line, function, nullptr, format, args);
}

void Logger::logf(
    Level level,
    const char* file,
    int line,
    const char* function,
    const char* format,
    ...) noexcept
{
    if (!enabled_any(level))
        return;
    std::va_list args;
    va_start(args, format);
    vlogf(level, file, line, function, format, args);
    va_end(args);
}

void Logger::logf_tags(
    Level level,
    const char* file,
    int line,
    const char* function,
    const char* tags,
    const char* format,
    ...) noexcept
{
    if (!enabled_any(level))
        return;
    std::va_list args;
    va_start(args, format);
    vlogf_tags(level, file, line, function, tags, format, args);
    va_end(args);
}

void Logger::set_source_anchor(const char* name)
{
    impl_->anchor.store(
        std::make_shared<const std::string>(
            name != nullptr && *name != '\0' ? name : "source"),
        std::memory_order_release);
}

} // namespace savor::logger
