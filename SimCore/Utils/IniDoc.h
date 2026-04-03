// SimCore/DB/ProgramDB/IniDoc.h
#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <fstream>
#include <sstream>
#include <optional>
#include "IniKV.h"

struct IniDoc {
    struct Section {
        std::string name;
        IniKV kv;
    };

    // ---- Options -------------------------------------------------------------
    struct ParseOptions {
        bool strip_utf8_bom = true; // (1) Strip BOM if present
        bool trim_keys = true; // (2) Trim spaces/tabs around keys and section names
        bool trim_values = true; // (2) Trim spaces/tabs around values
        bool inline_comments = true; // (3) Support trailing ;/# that are preceded by whitespace
    };

    struct ReadOptions {
        bool duplicates_last_wins = true; // (4) Getter semantics (else first-wins)
        bool list_trim_items = true; // (5) Trim items in get_list
    };

    static constexpr const char* GLOBAL = "";

    // Construct empty doc with optional read options.
    explicit IniDoc(ReadOptions ro = {}) : read_opts_(ro) {}

    // Parse text into an IniDoc with parse and read options.
    static IniDoc parse(const std::string& text, const ParseOptions& p = {}, const ReadOptions& r = {}) {
        IniDoc doc(r);
        doc.sections_.clear();
        doc.name_to_index_.clear();
        doc.current_.assign(GLOBAL);

        size_t offset = 0;
        if (p.strip_utf8_bom && text.size() >= 3 &&
            static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB &&
            static_cast<unsigned char>(text[2]) == 0xBF) {
            offset = 3;
        }

        doc.ensure_section(GLOBAL);
        size_t start = offset;
        while (start <= text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(start, end - start);
            if (!line.empty() && line.back() == '\r') line.pop_back(); // handle CRLF
            start = (end == text.size()) ? end + 1 : end + 1;

            if (line.empty()) continue;

            // Whole-line comments
            if (line[0] == ';' || line[0] == '#') continue;

            // Section header
            if (!line.empty() && line.front() == '[') {
                size_t rbr = line.find(']');
                if (rbr != std::string::npos && rbr > 1) {
                    std::string sec = line.substr(1, rbr - 1);
                    if (p.trim_keys) _trim_spaces(sec);
                    doc.ensure_section(sec);
                    doc.current_ = sec;
                    continue;
                }
            }

            // Key = Value
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);

            if (p.inline_comments && !val.empty()) {
                _strip_inline_comment(val);
            }

            if (p.trim_keys)   _trim_spaces(key);
            if (p.trim_values) _trim_spaces(val);

            doc.ensure_section(doc.current_).add(std::move(key), std::move(val));
        }
        return doc;
    }

    // Parse INI text from a file path. Returns std::nullopt on file I/O failure.
    static std::optional<IniDoc> load(const std::string& path, const ParseOptions& p = {}, const ReadOptions& r = {}) {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return std::nullopt;
        std::ostringstream buf;
        buf << in.rdbuf();
        if (!in.good() && !in.eof()) return std::nullopt;
        return parse(buf.str(), p, r);
    }

    // Save INI text to a file path. Returns false on file I/O failure.
    bool save(const std::string& path, bool sorted = false) const {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        const std::string text = sorted ? to_string_sorted() : to_string_preserve_order();
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        return out.good();
    }

    // ---- Emission ------------------------------------------------------------

    // Deterministic: sections sorted by name; keys within section sorted by key.
    std::string to_string_sorted() const {
        std::vector<const Section*> secs;
        secs.reserve(sections_.size());
        for (auto& s : sections_) secs.push_back(&s);
        std::sort(secs.begin(), secs.end(), [](const Section* a, const Section* b) {
            return a->name < b->name;
            });
        std::string out;
        for (auto* s : secs) {
            if (!s->name.empty()) {
                out.push_back('['); out.append(s->name); out.push_back(']'); out.push_back('\n');
            }
            out.append(s->kv.to_string_sorted());
        }
        return out;
    }

    // Preserve section order; emit keys in stored order.
    std::string to_string_preserve_order() const {
        std::string out;
        for (const auto& s : sections_) {
            if (!s.name.empty()) {
                out.push_back('['); out.append(s.name); out.push_back(']'); out.push_back('\n');
            }
            for (const auto& kvp : s.kv.kv) {
                out.append(kvp.first);
                out.push_back('=');
                out.append(kvp.second);
                out.push_back('\n');
            }
        }
        return out;
    }

    std::vector<std::string> to_string_lines_preserve_order() const {
        std::vector<std::string> out;
        for (const auto& s : sections_) {
            if (!s.name.empty()) out.emplace_back('[' + s.name + ']');

            for (const auto& kvp : s.kv.kv) out.emplace_back(kvp.first + '=' + kvp.second);
        }
        return out;
    }

    // ---- Section management --------------------------------------------------

    bool has_section(const std::string& name) const {
        return name_to_index_.find(name) != name_to_index_.end();
    }

    IniKV* find_section(const std::string& name) {
        auto it = name_to_index_.find(name);
        if (it == name_to_index_.end()) return nullptr;
        return &sections_[it->second].kv;
    }

    const IniKV* find_section(const std::string& name) const {
        auto it = name_to_index_.find(name);
        if (it == name_to_index_.end()) return nullptr;
        return &sections_[it->second].kv;
    }

    IniKV& ensure_section(const std::string& name) {
        auto it = name_to_index_.find(name);
        if (it != name_to_index_.end()) return sections_[it->second].kv;
        size_t idx = sections_.size();
        sections_.push_back(Section{ name, IniKV{} });
        name_to_index_[name] = idx;
        return sections_.back().kv;
    }

    void erase_section(const std::string& name) {
        auto it = name_to_index_.find(name);
        if (it == name_to_index_.end()) return;
        size_t idx = it->second;
        sections_.erase(sections_.begin() + static_cast<std::ptrdiff_t>(idx));
        name_to_index_.clear();
        for (size_t i = 0; i < sections_.size(); ++i) name_to_index_[sections_[i].name] = i;
        if (current_ == name) current_.assign(GLOBAL);
    }

    std::vector<std::string> list_sections(bool sorted = true) const {
        std::vector<std::string> out;
        out.reserve(sections_.size());
        for (const auto& s : sections_) out.push_back(s.name);
        if (sorted) std::sort(out.begin(), out.end());
        return out;
    }

    // ---- Scoped key ops (respect ReadOptions) --------------------------------

    bool has(const std::string& section, const std::string& key) const {
        auto* kv = find_section(section);
        if (!kv) return false;
        return kv->has(key);
    }

    std::string get(const std::string& section, const std::string& key, const std::string& def = {}) const {
        auto* kv = find_section(section);
        if (!kv) return def;
        return read_opts_.duplicates_last_wins ? kv->get_last(key, def) : kv->get(key, def);
    }

    int64_t get_i64(const std::string& section, const std::string& key, int64_t def = 0) const {
        auto* kv = find_section(section);
        if (!kv) return def;
        return read_opts_.duplicates_last_wins ? kv->get_i64_last(key, def) : kv->get_i64(key, def);
    }

    uint32_t get_u32(const std::string& section, const std::string& key, uint32_t def = 0) const {
        auto* kv = find_section(section);
        if (!kv) return def;
        return read_opts_.duplicates_last_wins ? kv->get_u32_last(key, def) : kv->get_u32(key, def);
    }

    bool get_bool(const std::string& section, const std::string& key, bool def = false) const {
        auto* kv = find_section(section);
        if (!kv) return def;
        return read_opts_.duplicates_last_wins ? kv->get_bool_last(key, def) : kv->get_bool(key, def);
    }

    std::vector<std::string> get_list(const std::string& section, const std::string& key) const {
        auto* kv = find_section(section);
        if (!kv) return {};
        // list uses first occurrence for source string (consistent with IniKV::get_list),
        // but we can still trim items based on read options.
        return kv->get_list(key, read_opts_.list_trim_items);
    }

    void set(const std::string& section, const std::string& key, const std::string& value) {
        ensure_section(section).set(key, value);
    }

    void set_list(const std::string& section, const std::string& key, const std::vector<std::string>& xs) {
        ensure_section(section).set_list(key, xs);
    }

    void erase_key(const std::string& section, const std::string& key) {
        auto* kv = find_section(section);
        if (!kv) return;
        kv->erase(key);
    }

    const IniKV& section_kv(const std::string& name) const {
        auto* kv = find_section(name);
        if (!kv) throw std::out_of_range("IniDoc: section not found");
        return *kv;
    }

    // Overlay left->right; later sections replace earlier keys.
    IniKV merged(const std::vector<std::string>& ordered_sections) const {
        IniKV out;
        for (const auto& sec_name : ordered_sections) {
            auto* kv = find_section(sec_name);
            if (!kv) continue;
            for (const auto& p : kv->kv) {
                out.set(p.first, p.second); // replace semantics
            }
        }
        return out;
    }

    // Access read options (optional).
    const ReadOptions& read_options() const { return read_opts_; }
    void set_read_options(const ReadOptions& ro) { read_opts_ = ro; }

private:
    std::vector<Section> sections_;
    std::unordered_map<std::string, size_t> name_to_index_;
    std::string current_ = GLOBAL;
    ReadOptions read_opts_{};

    // --- helpers --------------------------------------------------------------
    static inline void _trim_spaces(std::string& s) {
        size_t a = 0; while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) ++a;
        size_t b = s.size(); while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
        if (a == 0 && b == s.size()) return;
        s = s.substr(a, b - a);
    }

    // Remove trailing inline comment if a ';' or '#' occurs and is preceded by whitespace.
    static inline void _strip_inline_comment(std::string& val) {
        for (size_t i = 0; i < val.size(); ++i) {
            char c = val[i];
            if ((c == ';' || c == '#') && i > 0 && (val[i - 1] == ' ' || val[i - 1] == '\t')) {
                // Trim any whitespace before the comment
                size_t end = i;
                while (end > 0 && (val[end - 1] == ' ' || val[end - 1] == '\t')) --end;
                val.resize(end);
                return;
            }
        }
    }
};
