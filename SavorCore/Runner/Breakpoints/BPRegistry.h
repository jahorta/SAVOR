#pragma once
#include <cstdint>
#include <optional>
#include <string_view>
#include <span>
#include <string>
#include <vector>
#include <fstream>

#include "BP.def.h"

// Minimal public types (replaces BPCore.h)
using BPKey = uint16_t;

struct BPAddr
{
    BPKey key;
    uint32_t pc;
    const char* name;
};

struct BreakpointMap
{
    std::vector<BPAddr> addrs;
    BPKey start_key{ 0 };
    BPKey terminal_key{ 0 };

    const BPAddr* find(BPKey k) const {
        for (const auto& a : addrs) if (a.key == k) return &a;
        return nullptr;
    }
    std::optional<BPKey> match(uint32_t pc) const {
        for (const auto& a : addrs) if (a.pc == pc) return a.key;
        return std::nullopt;
    }
};

// Central, X-macro driven dataset
namespace bp {

    enum class BPDomain : uint8_t { PreBattle = 0, Battle = 1, Overworld = 2, Unknown = 255 };

    // If you rely on numeric ranges, keep this; otherwise you can drop it later.
    inline constexpr BPDomain domain_of(BPKey k) {
        if (k < 200) return BPDomain::PreBattle;
        if (k < 500) return BPDomain::Battle;
        if (k >= 500 && k < 1000) return BPDomain::Overworld;
        return BPDomain::Unknown;
    }

    class BPRegistry {
    public:
        static std::span<const BPAddr> all();
        static const BPAddr* find(BPKey k);
        static const BPAddr* find(uint32_t);
        static std::optional<BPKey> match(uint32_t pc);
        static const char* name(BPKey k);
        static uint32_t pc(BPKey k);
        static BreakpointMap as_map();
    };

    // Lightweight override loader (same behavior you were using):
    // Text file with lines of `Name=0xADDR` updates the default map.
    BreakpointMap load_bpmap_file(const std::string& path, BreakpointMap base);

} // namespace savor

// Domain alias namespaces (ergonomic names preserved)
namespace bp::prebattle {
#define ALIAS_ROW(ns, NAME, ID, PC, STR) inline constexpr BPKey NAME = static_cast<BPKey>(ID);
    BP_TABLE_PREBATTLE(ALIAS_ROW)
#undef ALIAS_ROW
}

namespace bp::battle {
#define ALIAS_ROW(ns, NAME, ID, PC, STR) inline constexpr BPKey NAME = static_cast<BPKey>(ID);
    BP_TABLE_BATTLE(ALIAS_ROW)
#undef ALIAS_ROW
}

namespace bp::overworld {
#define ALIAS_ROW(ns, NAME, ID, PC, STR) inline constexpr BPKey NAME = static_cast<BPKey>(ID);
    BP_TABLE_OVERWORLD(ALIAS_ROW)
#undef ALIAS_ROW
}
