#include "Log.h"
#include <chrono>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif
#include <ctime>
#include <cctype>

namespace simcore::logger{

    static std::string fmt_v(const char* fmt, std::va_list ap) {
        std::va_list ap2; va_copy(ap2, ap);
        const int n = std::vsnprintf(nullptr, 0, fmt, ap2);
        va_end(ap2);
        if (n <= 0) return {};
        std::string s;
        std::vector<char> buf(static_cast<size_t>(n) + 1);
        std::vsnprintf(buf.data(), buf.size(), fmt, ap);
        s.assign(buf.data(), static_cast<size_t>(n));
        return s;
    }

    Logger& Logger::get() {
        static Logger g;
        return g;
    }

    Logger::Logger() {
#ifdef _WIN32
        // enable ANSI colors on recent terminals; ignore errors
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD mode = 0; if (GetConsoleMode(h, &mode)) {
                SetConsoleMode(h, mode | 0x0004 /*ENABLE_VIRTUAL_TERMINAL_PROCESSING*/);
            }
        }
#endif
    }

    Logger::~Logger() { close_file(); }

    void Logger::set_stdout_level(Level lv) { stdout_level_.store(lv); }
    void Logger::set_file_level(Level lv) { file_level_.store(lv); }
    void Logger::set_levels(Level out, Level file) { set_stdout_level(out); set_file_level(file); }
    void Logger::enable_colors(bool on) { colors_ = on; }

    bool Logger::open_file(const char* path, bool append) {
        std::lock_guard<std::mutex> lk(m_);
        if (file_) { std::fclose(file_); file_ = nullptr; }
#if defined(_WIN32)
        file_ = _fsopen(path, append ? "a" : "w", _SH_DENYWR);
#else
        file_ = std::fopen(path, append ? "a" : "w");
#endif
        return file_ != nullptr;
    }

    void Logger::close_file() {
        std::lock_guard<std::mutex> lk(m_);
        if (file_) { std::fflush(file_); std::fclose(file_); file_ = nullptr; }
    }

    const char* level_tag(Level lv) noexcept {
        switch (lv) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO ";
        case Level::Warn:  return "WARN ";
        case Level::Error: return "ERROR";
        case Level::Fatal: return "FATAL";
        default:           return "     ";
        }
    }

    static bool is_valid_tag_char(char c) noexcept {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.';
    }

    std::string BuildTagCsv(std::initializer_list<const char*> tags) {
        std::string out;
        for (const char* raw : tags) {
            if (!raw || !*raw) continue;
            bool valid = true;
            for (const char* p = raw; *p; ++p) {
                if (!is_valid_tag_char(*p)) {
                    valid = false;
                    break;
                }
            }
            if (!valid) continue;
            if (!out.empty()) out.push_back(',');
            out.append(raw);
        }
        return out;
    }

    void Logger::vlogf_tags(Level lv, const char* file, int line, const char* func,
        const char* tags_csv, const char* fmt, std::va_list ap) noexcept {
        // Build the message first (so we don't hold the mutex during heavy formatting)
        std::string msg = fmt_v(fmt, ap);

        // Timestamp
        using clock = std::chrono::system_clock;
        auto now = clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        std::time_t t = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char ts[32];
        std::snprintf(ts, sizeof(ts), "%02d:%02d:%02d.%03lld",
            tm.tm_hour, tm.tm_min, tm.tm_sec, (long long)(ms % 1000));

        auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());

        // Assemble one line
        const char* rel = shorten_path_(file);
        char head[256];
        std::snprintf(head, sizeof(head), "[%s] [%s] [T%zu] (%s:%d %s) ",
            ts, level_tag(lv), size_t(tid), rel, line, func);
        const bool has_tags = tags_csv && *tags_csv;

        std::lock_guard<std::mutex> lk(m_);
        // stdout sink
        if (lv >= stdout_level_.load()) {
            if (colors_) {
                const char* c =
                    (lv == Level::Trace) ? "\x1b[90m" :
                    (lv == Level::Debug) ? "\x1b[36m" :
                    (lv == Level::Info) ? "\x1b[37m" :
                    (lv == Level::Warn) ? "\x1b[33m" :
                    (lv == Level::Error) ? "\x1b[31m" :
                    /*Fatal*/            "\x1b[41;97m";
                std::fputs(c, stdout);
            }
            std::fputs(head, stdout);
            if (has_tags) {
                std::fputs("[tag=", stdout);
                std::fputs(tags_csv, stdout);
                std::fputs("] ", stdout);
            }
            std::fputs(msg.c_str(), stdout);
            std::fputc('\n', stdout);
            if (colors_) std::fputs("\x1b[0m", stdout);
            std::fflush(stdout);
        }
        // file sink
        if (file_ && lv >= file_level_.load()) {
            std::fputs(head, file_);
            if (has_tags) {
                std::fputs("[tag=", file_);
                std::fputs(tags_csv, file_);
                std::fputs("] ", file_);
            }
            std::fputs(msg.c_str(), file_);
            std::fputc('\n', file_);
            std::fflush(file_);
        }
    }

    void Logger::vlogf(Level lv, const char* file, int line, const char* func,
        const char* fmt, std::va_list ap) noexcept {
        vlogf_tags(lv, file, line, func, nullptr, fmt, ap);
    }

    void Logger::logf(Level lv, const char* file, int line, const char* func,
        const char* fmt, ...) noexcept {
        if (!enabled_any(lv)) return;
        std::va_list ap; va_start(ap, fmt);
        vlogf(lv, file, line, func, fmt, ap);
        va_end(ap);
    }

    void Logger::logf_tags(Level lv, const char* file, int line, const char* func,
        const char* tags_csv, const char* fmt, ...) noexcept {
        if (!enabled_any(lv)) return;
        std::va_list ap; va_start(ap, fmt);
        vlogf_tags(lv, file, line, func, tags_csv, fmt, ap);
        va_end(ap);
    }

    void Logger::set_source_anchor(const char* name) {
        std::lock_guard<std::mutex> lk(m_);
        anchor_ = (name && *name) ? name : "source";
    }

    // case-insensitive compare for fixed length
    static inline bool ieq_n(const char* a, const char* b, size_t n) {
        for (size_t i = 0; i < n; i++) {
            unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[i];
            if (!ca || !cb) return false;
            if (ca >= 'A' && ca <= 'Z') ca = ca - 'A' + 'a';
            if (cb >= 'A' && cb <= 'Z') cb = cb - 'A' + 'a';
            if (ca != cb) return false;
        }
        return true;
    }

    const char* Logger::shorten_path_(const char* full) noexcept {
        if (!full) return "";

        const char* last_basename = full;   // fallback: file name after last slash
        const char* best = nullptr;         // pointer to char after "<anchor>/" of the LAST match
        const size_t an = anchor_.size();

        auto lower_eq = [](char a, char b) {
            if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = char(b - 'A' + 'a');
            return a == b;
            };

        // Check initial segment (path may start with "source/...")
        {
            const char* q = full;
            size_t i = 0;
            while (i < an && q[i]) {
                if (!lower_eq(q[i], anchor_[i])) break;
                ++i;
            }
            if (i == an && (q[i] == '/' || q[i] == '\\')) {
                best = q + i + 1; // after "anchor/"
            }
        }

        // Scan segments; remember basename and the LAST anchor match
        for (const char* p = full; *p; ++p) {
            if (*p == '/' || *p == '\\') {
                const char* q = p + 1;
                if (*q) last_basename = q;

                // Compare segment q against anchor_
                size_t i = 0;
                while (i < an && q[i]) {
                    if (!lower_eq(q[i], anchor_[i])) break;
                    ++i;
                }
                if (i == an && (q[i] == '/' || q[i] == '\\')) {
                    best = q + i + 1; // after "anchor/"
                }
            }
        }

        return best ? best : last_basename;
    }

} // namespace logx
