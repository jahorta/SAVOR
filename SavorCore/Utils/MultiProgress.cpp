#include "MultiProgress.h"
#include <algorithm>
#include <cstdio>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace savor::utils {

    void MultiProgress::init(const std::vector<MPBarSpec>& specs, Options o) {
        opt_ = o;
        bars_.clear(); bars_.reserve(specs.size());
        for (auto& s : specs) {
            Bar b; b.label = s.label; b.total = std::max<uint64_t>(1, s.total); b.done = 0; b.rate = 0.0;
            bars_.push_back(b);
        }
        started_ = finished_ = false;
        drew_once_ = false;
    }

    void MultiProgress::start() {
        if (started_) return;
        started_ = true;
        t0_ = last_draw_ = std::chrono::steady_clock::now();
        if (opt_.use_vt) vt_ok_ = enable_vt_once();
        maybe_redraw(true);
    }

    void MultiProgress::tick(size_t i, uint64_t n) {
        if (i >= bars_.size()) return;
        auto& b = bars_[i];
        b.done = std::min<uint64_t>(b.done + n, b.total);
        maybe_redraw(false);
    }

    void MultiProgress::advanceTo(size_t i, uint64_t d) {
        if (i >= bars_.size()) return;
        auto& b = bars_[i];
        b.done = std::min<uint64_t>(d, b.total);
        maybe_redraw(false);
    }

    void MultiProgress::setLabel(size_t i, const std::string& s) {
        if (i >= bars_.size()) return;
        bars_[i].label = s;
        maybe_redraw(true);
    }

    void MultiProgress::setTotal(size_t i, uint64_t total) {
        if (i >= bars_.size()) return;
        bars_[i].total = std::max<uint64_t>(1, total);
        if (bars_[i].done > bars_[i].total) bars_[i].done = bars_[i].total;
        maybe_redraw(true);
    }

    void MultiProgress::setSuffix(size_t i, const std::string& s) {
        if (i >= bars_.size()) return;
        bars_[i].suffix = s;
        maybe_redraw(true);
    }

    void MultiProgress::finish() {
        finished_ = true;
        for (auto& b : bars_) b.done = b.total;
        maybe_redraw(true);
        if (opt_.use_stdout) std::fputc('\n', stdout);
    }

    uint64_t MultiProgress::total(size_t i) const { return (i < bars_.size()) ? bars_[i].total : 0; }
    uint64_t MultiProgress::done(size_t i)  const { return (i < bars_.size()) ? bars_[i].done : 0; }

    void MultiProgress::maybe_redraw(bool force) {
        if (!opt_.use_stdout) return;
        if (!started_) return;

        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - last_draw_).count();
        if (!force && dt < opt_.min_redraw_sec) return;
       
        for (auto& b : bars_) 
        {
            
            if (b.done == b.total) continue;
            if (b.done == 1) b.start = std::chrono::steady_clock::now();
            const double elapsed = std::max(1e-9, std::chrono::duration<double>(now-b.start).count());
            if (b.done > 1) b.rate = double(b.done) / elapsed;
        }

        if (vt_ok_) {
            std::fputs("\r", stdout);
            if (drew_once_) {
                for (size_t i = 0; i < bars_.size(); ++i) std::fputs("\x1b[A", stdout); // move up
            }
        }

        char line[256];
        for (size_t i = 0; i < bars_.size(); ++i) {
            auto& b = bars_[i];
            const double pct = 100.0 * double(b.done) / double(b.total);
            format_line(line, sizeof(line),
                b.label.c_str(), opt_.bar_width, pct, b.done, b.total, b.rate,
                b.suffix.empty() ? nullptr : b.suffix.c_str());
            if (vt_ok_) {
                std::fputs("\r", stdout);
                std::fputs(line, stdout);
                std::fputc('\n', stdout);
            }
            else {
                std::fputs(line, stdout);
                std::fputc('\n', stdout);
            }
        }
        std::fflush(stdout);
        last_draw_ = now;
        drew_once_ = true;
    }

    void MultiProgress::format_line(char* out, size_t n, const char* label, int barw,
        double pct, uint64_t done, uint64_t total, double rate, const char* suffix) {

        const int w = std::max(1, std::min(barw, 60));
        const int filled = std::clamp(int((pct / 100.0) * w), 0, w);
        char bar[64];
        for (int i = 0; i < w; ++i) bar[i] = (i < filled) ? '#' : '-';
        bar[w] = 0;

        if (suffix && *suffix) {
            std::snprintf(out, n, "%-10s [%s] %6.2f%%  %llu/%llu  %.1f/s\t%s",
                label ? label : "", bar, pct,
                (unsigned long long)done, (unsigned long long)total, rate,
                suffix);
        }
        else {
            std::snprintf(out, n, "%-10s [%s] %6.2f%%  %llu/%llu  %.1f/s",
                label ? label : "", bar, pct,
                (unsigned long long)done, (unsigned long long)total, rate);
        }
    }

    bool MultiProgress::enable_vt_once() {
#if defined(_WIN32)
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD mode = 0;
        if (!GetConsoleMode(h, &mode)) return false;
        if (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) return true;
        DWORD newmode = mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        return SetConsoleMode(h, newmode) != 0;
#else
        return true;
#endif
    }

} // namespace savor::utils
