#pragma once
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <atomic>

namespace simcore::logger {

    enum class Level : int { Debug = 0, Trace, Info, Warn, Error, Fatal, Off };

    class Logger {
    public:
        static Logger& get();

        void set_stdout_level(Level);
        void set_file_level(Level);
        void set_levels(Level stdout_lv, Level file_lv);
        bool open_file(const char* path, bool append = false);
        void close_file();
        void enable_colors(bool on);

        // printf-style core
        void logf(Level lv, const char* file, int line, const char* func,
            const char* fmt, ...) noexcept;

        // fast checks for macros
        bool enabled_stdout(Level lv) const noexcept { return lv >= stdout_level_.load(); }
        bool enabled_file(Level lv) const noexcept { return lv >= file_level_.load(); }
        bool enabled_any(Level lv) const noexcept { return enabled_stdout(lv) || enabled_file(lv); }

        void set_source_anchor(const char* name); // default "source"

    private:
        Logger();
        ~Logger();
        void vlogf(Level, const char*, int, const char*, const char*, std::va_list) noexcept;

        std::atomic<Level> stdout_level_{ Level::Info };
        std::atomic<Level> file_level_{ Level::Off };
        std::FILE* file_ = nullptr;
        bool colors_ = true;
        std::mutex m_;

        std::string anchor_ = "SOASimulator";
        const char* shorten_path_(const char* full) noexcept;
    };

    // Internal macros for SimCore sources
#define SCLOGT(fmt, ...) do{ auto& L=::simcore::logger::Logger::get(); \
 if (L.enabled_any(::simcore::logger::Level::Trace)) L.logf(::simcore::logger::Level::Trace, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__); }while(0)
#define SCLOGD(fmt, ...) do{ auto& L=::simcore::logger::Logger::get(); \
 if (L.enabled_any(::simcore::logger::Level::Debug)) L.logf(::simcore::logger::Level::Debug, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__); }while(0)
#define SCLOGI(fmt, ...) do{ auto& L=::simcore::logger::Logger::get(); \
 if (L.enabled_any(::simcore::logger::Level::Info )) L.logf(::simcore::logger::Level::Info , __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__); }while(0)
#define SCLOGW(fmt, ...) do{ auto& L=::simcore::logger::Logger::get(); \
 if (L.enabled_any(::simcore::logger::Level::Warn )) L.logf(::simcore::logger::Level::Warn , __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__); }while(0)
#define SCLOGE(fmt, ...) do{ auto& L=::simcore::logger::Logger::get(); \
 if (L.enabled_any(::simcore::logger::Level::Error)) L.logf(::simcore::logger::Level::Error, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__); }while(0)
#define SCLOGF(fmt, ...) do{ auto& L=::simcore::logger::Logger::get(); \
 if (L.enabled_any(::simcore::logger::Level::Fatal)) L.logf(::simcore::logger::Level::Fatal, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__); }while(0)

} // namespace simcore::log
