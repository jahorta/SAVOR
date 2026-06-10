#pragma once
#include <unordered_map>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>

struct DeltaColorizer {
    struct RGB { uint8_t r, g, b; };

    std::vector<std::string> escs;                    // ANSI sequences
    std::vector<RGB> rgbs;                            // parallel RGBs
    std::unordered_map<long long, std::size_t> map;   // delta -> index
    std::vector<long long> seen;                      // deltas seen (dedup at finalize)

    // Positive ramp: blues -> greens -> yellows -> oranges -> reds
    std::vector<int> pos_ramp = {
        33, 39, 45, 51,                // blues / cyans
        50, 48, 46, 82, 118, 154,        // greens
        190, 226,                        // yellows
        220, 214, 208, 202,              // oranges
        196                              // red
    };
    // Negative ramp: purples / magentas (well-spaced)
    std::vector<int> neg_ramp = { 55, 93, 129, 165, 201 };

    double gamma_pos = 0.55;             // bias spacing toward larger positives
    bool finalized = false;

    void begin() {
        finalized = false;
        escs.clear(); rgbs.clear(); map.clear(); seen.clear();
    }

    void ingest(long long d) {
        if (!finalized && d != 0) seen.push_back(d);
    }

    void finalize() {
        if (finalized) return;
        finalized = true;
        if (seen.empty()) return;

        std::sort(seen.begin(), seen.end());
        seen.erase(std::unique(seen.begin(), seen.end()), seen.end());

        // Partition negatives / positives
        auto it_split = std::upper_bound(seen.begin(), seen.end(), -1LL);
        const std::size_t n_neg = std::distance(seen.begin(), it_split);
        const std::size_t n_pos = seen.size() - n_neg;

        escs.reserve(seen.size());
        rgbs.reserve(seen.size());

        // Assign negatives along neg_ramp (linear mapping)
        for (std::size_t i = 0; i < n_neg; ++i) {
            int idx = (n_neg <= 1) ? (int)neg_ramp.size() / 2
                : int(std::round(double(i) / double(n_neg - 1) * (neg_ramp.size() - 1)));
            int code = neg_ramp[(std::size_t)idx];
            map[seen[i]] = escs.size();
            escs.push_back(esc_from_code(code));
            rgbs.push_back(rgb_from_code(code));
        }

        // Assign positives along pos_ramp (gamma skew)
        for (std::size_t j = 0; j < n_pos; ++j) {
            double t = (n_pos <= 1) ? 1.0 : double(j) / double(n_pos - 1);
            double tw = std::pow(t, gamma_pos);
            int idx = int(std::round(tw * (pos_ramp.size() - 1)));
            if (idx < 0) idx = 0;
            if (idx >= (int)pos_ramp.size()) idx = (int)pos_ramp.size() - 1;
            int code = pos_ramp[(std::size_t)idx];
            const long long d = seen[n_neg + j];
            map[d] = escs.size();
            escs.push_back(esc_from_code(code));
            rgbs.push_back(rgb_from_code(code));
        }
    }

    const char* color(long long d) const {
        if (d == 0) return "\x1b[2;37m";
        auto it = map.find(d);
        if (it == map.end()) return "\x1b[38;5;33m"; // fallback: blue
        return escs[it->second].c_str();
    }

    RGB rgb(long long d) const {
        if (d == 0) return RGB{ 160,160,160 };
        auto it = map.find(d);
        if (it == map.end()) return rgb_from_code(33);
        return rgbs[it->second];
    }

    static constexpr const char* reset() { return "\x1b[0m"; }

    // Hex formatter: negatives as "-<nibble>", positives as "<byte>", zero as "00"
    static std::string fmt_delta_hex(long long d) {
        char buf[8];
        if (d == 0) { std::snprintf(buf, sizeof(buf), "%02X", 0); return std::string(buf); }
        if (d < 0) { std::snprintf(buf, sizeof(buf), "-%X", (unsigned)(std::llabs(d) & 0xF)); return std::string(buf); }
        std::snprintf(buf, sizeof(buf), "%02X", (unsigned)(d & 0xFF));
        return std::string(buf);
    }

private:
    static std::string esc_from_code(int code) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "\x1b[38;5;%dm", code);
        return std::string(buf);
    }
    static RGB rgb_from_code(int code) {
        if (code < 16) {
            static const uint8_t base[16][3] = {
                {0,0,0},{205,0,0},{0,205,0},{205,205,0},{0,0,238},{205,0,205},{0,205,205},{229,229,229},
                {127,127,127},{255,0,0},{0,255,0},{255,255,0},{92,92,255},{255,0,255},{0,255,255},{255,255,255}
            };
            return RGB{ base[code][0], base[code][1], base[code][2] };
        }
        if (code >= 232) {
            uint8_t v = uint8_t(8 + 10 * (code - 232));
            return RGB{ v, v, v };
        }
        int c = code - 16, r = c / 36, g = (c % 36) / 6, b = c % 6;
        static const uint8_t lev[6] = { 0, 95, 135, 175, 215, 255 };
        return RGB{ lev[r], lev[g], lev[b] };
    }
};

// header-only, per-thread instance
inline thread_local DeltaColorizer g_delta_color;

inline void color_lut_begin() { g_delta_color.begin(); }
inline void color_lut_ingest(long long d) { g_delta_color.ingest(d); }
inline void color_lut_finalize() { g_delta_color.finalize(); }
inline const char* color_for_delta(long long d) { return g_delta_color.color(d); }
inline DeltaColorizer::RGB rgb_for_delta(long long d) { return g_delta_color.rgb(d); }
inline constexpr const char* color_reset() { return DeltaColorizer::reset(); }
inline std::string fmt_delta_hex(long long d) { return DeltaColorizer::fmt_delta_hex(d); }
