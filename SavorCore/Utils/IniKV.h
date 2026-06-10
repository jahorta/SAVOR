// SavorCore/DB/ProgramDB/IniKV.h
#pragma once
#include <string>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cctype>

struct IniKV {
    std::vector<std::pair<std::string, std::string>> kv;

    // Append a key=value (does not erase duplicates).
    void add(std::string k, std::string v) { kv.emplace_back(std::move(k), std::move(v)); }

    // Replace semantics: erase all existing entries for key, then add one.
    void set(const std::string& key, const std::string& value) {
        erase(key);
        add(key, value);
    }

    // Erase all entries for key.
    void erase(const std::string& key) {
        kv.erase(std::remove_if(kv.begin(), kv.end(),
            [&](const std::pair<std::string, std::string>& p) { return p.first == key; }),
            kv.end());
    }

    // Deterministic text (keys sorted; multiple entries per key are emitted in original order).
    std::string to_string_sorted() const {
        std::vector<std::pair<std::string, std::string>> c = kv;
        std::stable_sort(c.begin(), c.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::string s;
        s.reserve(c.size() * 16);
        for (const auto& e : c) { s.append(e.first); s.push_back('='); s.append(e.second); s.push_back('\n'); }
        return s;
    }

    // --- Queries (first-wins by default) -------------------------------------

    bool has(const std::string& key) const {
        for (const auto& p : kv) if (p.first == key) return true;
        return false;
    }

    // First occurrence wins.
    std::string get(const std::string& key, const std::string& def = {}) const {
        for (const auto& p : kv) if (p.first == key) return p.second;
        return def;
    }

    // Last occurrence wins (convenience for last-wins semantics).
    std::string get_last(const std::string& key, const std::string& def = {}) const {
        for (size_t i = kv.size(); i-- > 0; ) {
            if (kv[i].first == key) return kv[i].second;
        }
        return def;
    }

    int64_t get_i64(const std::string& key, int64_t def = 0) const {
        for (const auto& p : kv) if (p.first == key) {
            try {
                size_t pos = 0;
                long long v = std::stoll(p.second, &pos, 0);
                if (pos == p.second.size()) return static_cast<int64_t>(v);
            }
            catch (...) {}
            return def;
        }
        return def;
    }

    int64_t get_i64_last(const std::string& key, int64_t def = 0) const {
        for (size_t i = kv.size(); i-- > 0; ) if (kv[i].first == key) {
            try {
                size_t pos = 0;
                long long v = std::stoll(kv[i].second, &pos, 0);
                if (pos == kv[i].second.size()) return static_cast<int64_t>(v);
            }
            catch (...) {}
            return def;
        }
        return def;
    }



    uint64_t get_u64(const std::string& key, uint64_t def = 0) const {
        for (const auto& p : kv) if (p.first == key) {
            try {
                size_t pos = 0;
                long long v = std::stoll(p.second, &pos, 0);
                if (pos == p.second.size()) return static_cast<uint64_t>(v);
            }
            catch (...) {}
            return def;
        }
        return def;
    }

    uint64_t get_u64_last(const std::string& key, uint64_t def = 0) const {
        for (size_t i = kv.size(); i-- > 0; ) if (kv[i].first == key) {
            try {
                size_t pos = 0;
                long long v = std::stoll(kv[i].second, &pos, 0);
                if (pos == kv[i].second.size()) return static_cast<uint64_t>(v);
            }
            catch (...) {}
            return def;
        }
        return def;
    }

    uint32_t get_u32(const std::string& key, uint32_t def = 0) const {
        for (const auto& p : kv) if (p.first == key) {
            try {
                size_t pos = 0;
                unsigned long long v = std::stoull(p.second, &pos, 0);
                if (pos == p.second.size()) return static_cast<uint32_t>(v);
            }
            catch (...) {}
            return def;
        }
        return def;
    }

    uint32_t get_u32_last(const std::string& key, uint32_t def = 0) const {
        for (size_t i = kv.size(); i-- > 0; ) if (kv[i].first == key) {
            try {
                size_t pos = 0;
                unsigned long long v = std::stoull(kv[i].second, &pos, 0);
                if (pos == kv[i].second.size()) return static_cast<uint32_t>(v);
            }
            catch (...) {}
            return def;
        }
        return def;
    }

    uint8_t get_u8(const std::string& key, uint8_t def = 0) const {
        for (const auto& p : kv) if (p.first == key) {
            try {
                size_t pos = 0;
                unsigned long long v = std::stoull(p.second, &pos, 0);
                if (pos == p.second.size()) return static_cast<uint8_t>(v);
            }
            catch (...) {}
            return def;
        }
        return def;
    }

    uint8_t get_u8_last(const std::string& key, uint8_t def = 0) const {
        for (size_t i = kv.size(); i-- > 0; ) if (kv[i].first == key) {
            try {
                size_t pos = 0;
                unsigned long long v = std::stoull(kv[i].second, &pos, 0);
                if (pos == kv[i].second.size()) return static_cast<uint8_t>(v);
            }
            catch (...) {}
            return def;
        }
        return def;
    }

    static inline bool _ieq(const std::string& a, const char* lit) {
        size_t n = a.size();
        size_t m = 0; while (lit[m] != '\0') ++m;
        if (n != m) return false;
        for (size_t i = 0; i < n; ++i) {
            unsigned char ca = static_cast<unsigned char>(a[i]);
            unsigned char cb = static_cast<unsigned char>(lit[i]);
            if (std::tolower(ca) != std::tolower(cb)) return false;
        }
        return true;
    }

    bool get_bool(const std::string& key, bool def = false) const {
        for (const auto& p : kv) if (p.first == key) {
            const auto& v = p.second;
            if (_ieq(v, "1") || _ieq(v, "true") || _ieq(v, "yes") || _ieq(v, "on") || _ieq(v, "y") || _ieq(v, "t")) return true;
            if (_ieq(v, "0") || _ieq(v, "false") || _ieq(v, "no") || _ieq(v, "off") || _ieq(v, "n") || _ieq(v, "f")) return false;
            return def;
        }
        return def;
    }

    bool get_bool_last(const std::string& key, bool def = false) const {
        for (size_t i = kv.size(); i-- > 0; ) if (kv[i].first == key) {
            const auto& v = kv[i].second;
            if (_ieq(v, "1") || _ieq(v, "true") || _ieq(v, "yes") || _ieq(v, "on") || _ieq(v, "y") || _ieq(v, "t")) return true;
            if (_ieq(v, "0") || _ieq(v, "false") || _ieq(v, "no") || _ieq(v, "off") || _ieq(v, "n") || _ieq(v, "f")) return false;
            return def;
        }
        return def;
    }

    // Split comma-separated list. If trim_items=true, trim spaces/tabs around each item.
    std::vector<std::string> get_list(const std::string& key, bool trim_items = false) const {
        std::vector<std::string> out;
        for (const auto& p : kv) if (p.first == key) {
            const std::string& s = p.second;
            size_t i = 0;
            while (i <= s.size()) {
                size_t j = s.find(',', i);
                if (j == std::string::npos) j = s.size();
                std::string item = s.substr(i, j - i);
                if (trim_items) {
                    size_t a = 0; while (a < item.size() && (item[a] == ' ' || item[a] == '\t')) ++a;
                    size_t b = item.size(); while (b > a && (item[b - 1] == ' ' || item[b - 1] == '\t')) --b;
                    item = item.substr(a, b - a);
                }
                out.push_back(std::move(item));
                i = j + 1;
            }
            break; // first occurrence only for list (consistent with get)
        }
        return out;
    }

    void set_list(const std::string& key, const std::vector<std::string>& xs) {
        std::string joined;
        for (size_t i = 0; i < xs.size(); ++i) {
            if (i) joined.push_back(',');
            joined.append(xs[i]);
        }
        set(key, joined);
    }
};
