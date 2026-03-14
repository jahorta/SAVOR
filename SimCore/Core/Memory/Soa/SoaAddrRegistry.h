#pragma once
#include <cstdint>
#include <cstddef>
#include <optional>
#include <span>
#include "SoaAddr.def.h"
#include "Battle/DerivedBattleBuffer.addr.h"

namespace addr {

    enum class Region : uint8_t { MEM1 = 1, MEM2 = 2, DERIVED = 3 };

    struct DolphinAddr {
        Region     region;
        uint32_t   base;
    };

#define ADDR_TABLE_ALL(X)       \
  ADDR_TABLE_CORE(X)            \
  ADDR_TABLE_BATTLE(X)          \
  ADDR_TABLE_DERIVED_BATTLE(X)

    enum class AddrKey : uint16_t {
#define MK_ENUM(dom, NAME, R, B) dom##_##NAME,
        ADDR_TABLE_ALL(MK_ENUM)
#undef MK_ENUM
    };

    struct AddrRec {
        AddrKey     key;
        DolphinAddr spec;
        const char* name;        // "dom.NAME"
    };

    class Registry {
    public:
        static std::span<const AddrRec> all();
        static const AddrRec* find(AddrKey k);
        static const DolphinAddr& spec(AddrKey k);
        static const uint32_t base(AddrKey k);
        static const Region region(AddrKey k);
        static const char* name(AddrKey k);
        static const bool exists(AddrKey k);
    };

    // ergonomic aliases: addr::core::X, addr::battle::Y, addr::derived::Z
    namespace core {
#define MK_ALIAS(dom, NAME, R, B) inline constexpr AddrKey NAME = AddrKey::dom##_##NAME;
        ADDR_TABLE_CORE(MK_ALIAS)
#undef MK_ALIAS
    }

    namespace battle {
#define MK_ALIAS(dom, NAME, R, B) inline constexpr AddrKey NAME = AddrKey::dom##_##NAME;
        ADDR_TABLE_BATTLE(MK_ALIAS)
#undef MK_ALIAS
    }

    namespace derived::battle {
#define MK_ALIAS(dom, NAME, R, B) inline constexpr AddrKey NAME = AddrKey::dom##_##NAME;
        ADDR_TABLE_DERIVED_BATTLE(MK_ALIAS)
#undef MK_ALIAS
    }


} // namespace addr
