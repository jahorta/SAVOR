#pragma once
#include <cstdint>
#include "Soa/SoaAddrRegistry.h"
#include "../../Core/DolphinWrapper.h"
#include "DerivedBase.h"

namespace savor {

    class DolphinKeyReader {
    public:
        explicit DolphinKeyReader(const savor::DolphinWrapper* host) : host_(host) {}
        bool read_key(addr::AddrKey k, uint8_t width, uint64_t& out_bits) const {
            if (!host_) return false;
            switch (width) {
            case 1: { uint8_t  v = 0;  if (!host_->readByKey(k, v)) return false; out_bits = v; return true; }
            case 2: { uint16_t v = 0;  if (!host_->readByKey(k, v)) return false; out_bits = v; return true; }
            case 4: { uint32_t v = 0;  if (!host_->readByKey(k, v)) return false; out_bits = v; return true; }
            case 8: { uint64_t v = 0;  if (!host_->readByKey(k, v)) return false; out_bits = v; return true; }
            default: return false;
            }
        }
    private:
        const savor::DolphinWrapper* host_{};
    };

    class KeyHostRouter {
    public:
        KeyHostRouter(const DolphinKeyReader* mem1, const IDerivedBuffer* derived)
            : mem1_(*mem1), derived_(*derived) {
            has_derived_ = derived != nullptr;
        }

        bool read(addr::AddrKey k, uint8_t width, uint64_t& out_bits) const {
            const auto& sp = addr::AddrRegistry::spec(k);
            switch (sp.region) {
            case addr::Region::MEM1:    return mem1_.read_key(k, width, out_bits);
            case addr::Region::MEM2:    return mem1_.read_key(k, width, out_bits);
            case addr::Region::DERIVED: return !has_derived_ ? false : derived_.can_serve(k) && derived_.read_key(k, width, out_bits);
            default: return false;
            }
        }

    private:
        const DolphinKeyReader& mem1_;
        const IDerivedBuffer& derived_;
        bool has_derived_{ false };
    };

} // namespace savor
