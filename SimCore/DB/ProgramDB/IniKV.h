// SimCore/DB/ProgramDB/IniKV.h
#pragma once
#include <string>
#include <vector>
#include <algorithm>

struct IniKV {
    std::vector<std::pair<std::string, std::string>> kv;

    void add(std::string k, std::string v) { kv.emplace_back(std::move(k), std::move(v)); }
    std::string to_string_sorted() const {
        std::vector<std::pair<std::string, std::string>> c = kv;
        std::sort(c.begin(), c.end(), [](auto& a, auto& b) { return a.first < b.first; });
        std::string s;
        for (auto& e : c) { s.append(e.first); s.push_back('='); s.append(e.second); s.push_back('\n'); }
        return s;
    }
    static IniKV parse(const std::string& text) {
        std::vector<std::pair<std::string, std::string>> out;
        size_t i = 0, n = text.size();
        while (i < n) {
            size_t j = text.find('\n', i);
            size_t e = (j == std::string::npos) ? n : j;
            if (e > i) {
                size_t eq = text.find('=', i);
                if (eq != std::string::npos && eq < e) {
                    out.emplace_back(text.substr(i, eq - i), text.substr(eq + 1, e - eq - 1));
                }
            }
            if (j == std::string::npos) break; else i = j + 1;
        }
        return { .kv=out };
    }
    
    bool has(const std::string& key) const {
        for (auto& p : kv) if (p.first == key) return true;
        return false;
    }

    std::string get(const std::string& key, const std::string& def = {}) const {
        for (auto& p : kv) if (p.first == key) return p.second;
        return def;
    }

    int64_t get_i64(const std::string& key, int64_t def = 0) const {
        for (auto& p : kv) if (p.first == key) try { return std::stoll(p.second); }
        catch (...) { return def; }
        return def;
    }

    uint32_t get_u32(const std::string& key, uint32_t def = 0) const {
        for (auto& p : kv) if (p.first == key) try { return static_cast<uint32_t>(std::stoul(p.second)); }
        catch (...) { return def; }
        return def;
    }

    bool get_bool(const std::string& key, bool def = false) const {
        for (auto& p : kv) if (p.first == key) {
            const auto& v = p.second;
            if (v == "1" || v == "true" || v == "TRUE") return true;
            if (v == "0" || v == "false" || v == "FALSE") return false;
            return def;
        }
        return def;
    }

    std::vector<std::string> get_list(const std::string& key) const {
        std::vector<std::string> out;
        for (auto& p : kv) if (p.first == key) {
            const std::string& v = p.second;
            std::string cur;
            for (char c : v) {
                if (c == ',') {
                    size_t a = 0; while (a < cur.size() && isspace(static_cast<unsigned char>(cur[a]))) ++a;
                    size_t b = cur.size(); while (b > a && isspace(static_cast<unsigned char>(cur[b - 1]))) --b;
                    if (b > a) out.emplace_back(cur.substr(a, b - a));
                    cur.clear();
                }
                else {
                    cur.push_back(c);
                }
            }
            if (!cur.empty()) {
                size_t a = 0; while (a < cur.size() && isspace(static_cast<unsigned char>(cur[a]))) ++a;
                size_t b = cur.size(); while (b > a && isspace(static_cast<unsigned char>(cur[b - 1]))) --b;
                if (b > a) out.emplace_back(cur.substr(a, b - a));
            }
            break;
        }
        return out;
    }

    void set_list(const std::string& key, const std::vector<std::string>& xs) {
        std::string joined;
        for (size_t i = 0; i < xs.size(); ++i) {
            if (i) joined.push_back(',');
            joined.append(xs[i]);
        }
        add(key, joined);
    }
};
