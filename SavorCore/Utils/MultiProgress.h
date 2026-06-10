#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <chrono>

namespace savor::utils {

    struct MPBarSpec {
        std::string label;
        uint64_t total = 0;
    };

    class MultiProgress {
    public:
        struct Options {
            bool use_stdout = true;
            bool use_vt = true;
            double min_redraw_sec = 0.10;
            int bar_width = 40;
        };

        void init(const std::vector<MPBarSpec>& specs, Options o = {});
        void start();                      // prints initial block
        void tick(size_t i, uint64_t n = 1); // bars[i] += n
        void advanceTo(size_t i, uint64_t done);
        void finish();                     // force final draw + newline gap

        uint64_t total(size_t i) const;
        uint64_t done(size_t i) const;
        void setLabel(size_t i, const std::string& s);
        void setTotal(size_t i, uint64_t total);
        void setSuffix(size_t i, const std::string& s);

    private:
        void maybe_redraw(bool force);
        static void format_line(char* out, size_t n, const char* label, int barw,
            double pct, uint64_t done, uint64_t total, double rate,
            const char* suffix);
        bool enable_vt_once();

        struct Bar {
            std::string label;
            uint64_t total = 1;
            uint64_t done = 0;
            double rate = 0.0;
            std::chrono::steady_clock::time_point start{};
            std::string suffix;
        };

        Options opt_{};
        std::vector<Bar> bars_;
        std::chrono::steady_clock::time_point t0_{}, last_draw_{};
        bool started_ = false;
        bool finished_ = false;
        bool vt_ok_ = false;
        bool drew_once_ = false;
    };

} // namespace savor::utils
