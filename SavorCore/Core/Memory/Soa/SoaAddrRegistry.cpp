#include "SoaAddrRegistry.h"

namespace addr {

    // Build the master table in one pass.
        static constexpr AddrRec kAll[] = {
          #define ROW(dom, NAME, R, B) \
            { AddrKey::dom##_##NAME, \
              DolphinAddr{ Region::R, static_cast<uint32_t>(B)}, \
              #dom "." #NAME \
            },
          ADDR_TABLE_ALL(ROW)
          #undef ROW
    };

    std::span<const AddrRec> AddrRegistry::all() {
        return std::span<const AddrRec>(kAll, sizeof(kAll) / sizeof(kAll[0]));
    }

    const AddrRec* AddrRegistry::find(AddrKey k) {
        for (const auto& r : kAll) if (r.key == k) return &r;
        return nullptr;
    }

    const DolphinAddr& AddrRegistry::spec(AddrKey k) {
        const auto* r = find(k);
        return r ? r->spec : kAll[0].spec; // table is non-empty
    }

    const uint32_t AddrRegistry::base(AddrKey k)
    {
        return spec(k).base;
    }

    const Region AddrRegistry::region(AddrKey k)
    {
        return spec(k).region;
    }

    const char* AddrRegistry::name(AddrKey k) {
        const auto* r = find(k);
        return r ? r->name : "";
    }

    const bool AddrRegistry::exists(AddrKey k) {
        return find(k) != nullptr;
    }

} // namespace addr
