#include "BpRegistry.h"
#include <cctype>
#include <iterator>

namespace bp {

    namespace {

    static constexpr BPAddr kAll[] = {
    #define ROW(ns, NAME, ID, PC, STR, VISIBILITY, OWNER) \
        { static_cast<BPKey>(ID), static_cast<uint32_t>(PC), STR, "builtin.bp." #ns "." STR, \
          BreakpointVisibility::VISIBILITY, BreakpointOwner::OWNER },
    BP_TABLE_ALL(ROW)
    #undef ROW
    };

    constexpr bool IsAllowedRecord(const BPAddr& record, BreakpointConsumer consumer)
    {
        if (record.visibility == BreakpointVisibility::PlayerVisible) {
            return true;
        }
        if (consumer == BreakpointConsumer::PhaseControl) {
            return true;
        }
        return consumer == BreakpointConsumer::InteractionControl
            && record.owner == BreakpointOwner::Interaction;
    }

    constexpr bool HasNoInternalPlayerVisiblePcAlias()
    {
        for (size_t i = 0; i < std::size(kAll); ++i) {
            for (size_t j = i + 1; j < std::size(kAll); ++j) {
                if (kAll[i].pc == kAll[j].pc
                    && kAll[i].visibility != kAll[j].visibility) {
                    return false;
                }
            }
        }
        return true;
    }

    static_assert(
        HasNoInternalPlayerVisiblePcAlias(),
        "an internal breakpoint PC must not alias a player-visible breakpoint PC");

    } // namespace

    std::span<const BPAddr> BpRegistry::AllRuntime() {
        return std::span<const BPAddr>(kAll, sizeof(kAll) / sizeof(kAll[0]));
    }

    const BPAddr* BpRegistry::FindRuntime(BPKey key) {
        for (const auto& record : kAll) {
            if (record.key == key) return &record;
        }
        return nullptr;
    }

    const BPAddr* BpRegistry::FindRuntime(uint32_t pc) {
        for (const auto& record : kAll) {
            if (record.pc == pc) return &record;
        }
        return nullptr;
    }

    bool BpRegistry::IsAllowed(BPKey key, BreakpointConsumer consumer) {
        const auto* record = FindRuntime(key);
        return record != nullptr && IsAllowedRecord(*record, consumer);
    }

    bool BpRegistry::IsAllowedPc(uint32_t pc, BreakpointConsumer consumer) {
        bool matched = false;
        for (const auto& record : kAll) {
            if (record.pc != pc) continue;
            matched = true;
            if (!IsAllowedRecord(record, consumer)) return false;
        }
        return matched;
    }

    std::vector<BPAddr> BpRegistry::ForConsumer(BreakpointConsumer consumer) {
        std::vector<BPAddr> out;
        out.reserve(std::size(kAll));
        for (const auto& record : kAll) {
            if (IsAllowedRecord(record, consumer)) out.push_back(record);
        }
        return out;
    }

    BreakpointMap BpRegistry::BuildRuntimeMap() {
        BreakpointMap m;
        m.addrs.reserve(std::size(kAll));
        for (const auto& record : kAll) m.addrs.push_back(record);
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
