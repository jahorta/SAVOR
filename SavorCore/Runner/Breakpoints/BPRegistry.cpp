#include "BpRegistry.h"
#include <cctype>

namespace bp {

    static const BPAddr kAll[] = {
    #define ROW(ns, NAME, ID, PC, STR) { static_cast<BPKey>(ID), static_cast<uint32_t>(PC), STR, "builtin.bp." #ns "." STR },
    BP_TABLE_ALL(ROW)
    #undef ROW
    };

    std::span<const BPAddr> BpRegistry::all() {
        return std::span<const BPAddr>(kAll, sizeof(kAll) / sizeof(kAll[0]));
    }

    const BPAddr* BpRegistry::find(BPKey k) {
        for (const auto& r : kAll) if (r.key == k) return &r;
        return nullptr;
    }

    const BPAddr* BpRegistry::find(uint32_t pc)
    {
        std::optional<BPKey> m = match(pc);
        if (!m.has_value()) return nullptr;
        return find(m.value());
    }

    std::optional<BPKey> BpRegistry::match(uint32_t pc) {
        for (const auto& r : kAll) if (r.pc == pc) return r.key;
        return std::nullopt;
    }

    const char* BpRegistry::name(BPKey k) {
        if (auto* r = find(k)) return r->name;
        return "";
    }

    uint32_t BpRegistry::pc(BPKey k) {
        if (auto* r = find(k)) return r->pc;
        return 0u;
    }

    BreakpointMap BpRegistry::as_map() {
        BreakpointMap m;
        m.addrs.reserve(sizeof(kAll) / sizeof(kAll[0]));
        for (const auto& r : kAll) m.addrs.push_back(BPAddr{ r.key, r.pc, r.name, r.stable_id });
        m.start_key = 0;  // leave to caller if they need it
        m.terminal_key = 0;
        return m;
    }

    // --- Minimal loader (kept from BPCore, trimmed to essentials) ---

    static inline std::string trim(std::string s) {
        size_t i = 0, j = s.size();
        while (i < j && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1]))) --j;
        return s.substr(i, j - i);
    }

    static inline std::optional<uint32_t> parse_u32_hex(std::string_view v) {
        // accepts: 0xDEADBEEF or decimal
        uint32_t result = 0;
        if (v.size() >= 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
            for (size_t i = 2; i < v.size(); ++i) {
                char c = v[i];
                uint32_t d =
                    (c >= '0' && c <= '9') ? (c - '0') :
                    (c >= 'a' && c <= 'f') ? (10 + c - 'a') :
                    (c >= 'A' && c <= 'F') ? (10 + c - 'A') : 0xFFFFFFFFu;
                if (d == 0xFFFFFFFFu) return std::nullopt;
                result = (result << 4) | d;
            }
            return result;
        }
        else {
            // decimal
            for (char c : v) {
                if (c < '0' || c>'9') return std::nullopt;
                result = result * 10 + (c - '0');
            }
            return result;
        }
    }

    BreakpointMap load_bpmap_file(const std::string& path, BreakpointMap base)
    {
        std::ifstream f(path);
        if (!f) return base;

        std::string line;
        while (std::getline(f, line)) {
            auto s = trim(line);
            if (s.empty() || s[0] == '#') continue;
            auto eq = s.find('=');
            if (eq == std::string::npos) continue;

            auto key = trim(s.substr(0, eq));
            auto val = trim(s.substr(eq + 1));
            auto parsed = parse_u32_hex(val);
            if (!parsed) continue;

            for (auto& a : base.addrs) {
                if (key == a.name) {
                    a.pc = *parsed;
                    break;
                }
            }
        }
        return base;
    }

} // namespace savor
