#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace savor::utils {

    class ProgressBar {
    public:
        struct Options {
            bool use_stdout = true;          // true: print to stdout; false: log snapshots only
            bool log_snapshots = false;      // if true, call LogFn periodically
            double min_redraw_sec = 0.10;    // throttle redraws
            double min_percent_step = 0.50;  // throttle by percent change
            const char* label = "";          // prefix label
            int bar_width = 40;              // visual width of bar
        };

        using Clock = std::chrono::steady_clock;
        using time_point = Clock::time_point;
        using LogFn = void(*)(const char*);  // optional callback for snapshots

        ProgressBar() = default;

        void init(uint64_t total, Options opt = {}, LogFn log_fn = nullptr);
        void start();                        // set t0; prints initial line
        void advanceTo(uint64_t done);       // set absolute done
        void tick(uint64_t n = 1);           // add n to done
        void finish();                       // force final line and newline

        uint64_t total() const { return total_; }
        uint64_t done()  const { return done_.load(std::memory_order_relaxed); }

    private:
        void maybe_redraw(bool force);

        static void format_eta(double seconds, char* out, size_t n);
        static void format_line(char* out, size_t n, const char* label,
            int barw, double pct, uint64_t done, uint64_t total,
            double rate, const char* eta);

        Options opt_{};
        LogFn log_fn_ = nullptr;

        uint64_t total_ = 0;
        std::atomic<uint64_t> done_{ 0 };

        time_point t0_{};
        time_point last_draw_{};
        double last_pct_ = -1.0;

        std::mutex mu_;
        bool started_ = false;
        bool finished_ = false;
    };

} // namespace savor::utils
