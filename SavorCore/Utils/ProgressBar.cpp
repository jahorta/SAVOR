#include "ProgressBar.h"
#include <cstdio>
#include <algorithm>

namespace savor::utils {

    void ProgressBar::init(uint64_t total, Options opt, LogFn log_fn) {
        std::scoped_lock lk(mu_);
        total_ = std::max<uint64_t>(1, total);
        done_.store(0, std::memory_order_relaxed);
        opt_ = opt;
        log_fn_ = log_fn;
        started_ = false;
        finished_ = false;
        last_pct_ = -1.0;
    }

    void ProgressBar::start() {
        std::scoped_lock lk(mu_);
        if (started_) return;
        started_ = true;
        t0_ = Clock::now();
        last_draw_ = t0_;
        maybe_redraw(true);
    }

    void ProgressBar::advanceTo(uint64_t d) {
        done_.store(std::min(d, total_), std::memory_order_relaxed);
        maybe_redraw(false);
    }

    void ProgressBar::tick(uint64_t n) {
        done_.fetch_add(n, std::memory_order_relaxed);
        maybe_redraw(false);
    }

    void ProgressBar::finish() {
        {
            std::scoped_lock lk(mu_);
            finished_ = true;
            done_.store(total_, std::memory_order_relaxed);
        }
        maybe_redraw(true);
        if (opt_.use_stdout) std::fputc('\n', stdout);
    }

    void ProgressBar::maybe_redraw(bool force) {
        std::scoped_lock lk(mu_);
        if (!started_) return;

        const auto now = Clock::now();
        const uint64_t d = std::min(done_.load(std::memory_order_relaxed), total_);
        const double pct = (100.0 * double(d)) / double(total_);
        const double dt = std::chrono::duration<double>(now - last_draw_).count();
        const bool pct_step = (last_pct_ < 0.0) || (pct - last_pct_ >= opt_.min_percent_step);

        if (!force && !pct_step && dt < opt_.min_redraw_sec) return;

        const double elapsed = std::max(1e-9, std::chrono::duration<double>(now - t0_).count());
        const double rate = double(d) / elapsed;
        const double remain = std::max(0.0, double(total_ - d));
        const double eta_s = (rate > 0.0) ? (remain / rate) : 0.0;

        char eta[32]; format_eta(eta_s, eta, sizeof(eta));
        char line[256];
        format_line(line, sizeof(line), opt_.label, opt_.bar_width, pct, d, total_, rate, eta);

        if (opt_.use_stdout) {
            std::fputs("\r", stdout);
            std::fputs(line, stdout);
            std::fflush(stdout);
        }
        if (opt_.log_snapshots && log_fn_) log_fn_(line);

        last_draw_ = now;
        last_pct_ = pct;
    }

    void ProgressBar::format_eta(double s, char* out, size_t n) {
        int sec = (int)(s + 0.5);
        int h = sec / 3600; sec %= 3600;
        int m = sec / 60; sec %= 60;
        if (h > 0) std::snprintf(out, n, "%dh%02dm%02ds", h, m, sec);
        else if (m > 0) std::snprintf(out, n, "%dm%02ds", m, sec);
        else std::snprintf(out, n, "%ds", sec);
    }

    void ProgressBar::format_line(char* out, size_t n, const char* label, int barw,
        double pct, uint64_t done, uint64_t total,
        double rate, const char* eta) {
        const int filled = std::clamp(int((pct / 100.0) * barw), 0, barw);
        char bar[128];
        const int w = std::min(barw, int(sizeof(bar) - 1));
        for (int i = 0; i < w; ++i) bar[i] = (i < filled) ? '#' : '-';
        bar[w] = 0;
        std::snprintf(out, n, "%s[%s] %6.2f%%  %llu/%llu  %.1f jobs/s  ETA %s",
            (label && *label) ? label : "", bar, pct,
            (unsigned long long)done, (unsigned long long)total, rate, eta);
    }

} // namespace savor::utils
