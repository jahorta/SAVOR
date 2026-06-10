#include "SoaAddrProgram.h"
#include "../Soa/SoaAddrRegistry.h"
#include "../../DolphinWrapper.h"
#include "Battle/DerivedBattleBuffer.h"

using addr::Registry;
using addr::Region;

namespace addrprog {

    static inline bool read_u16(const uint8_t*& p, const uint8_t* e, uint16_t& v) {
        if (p + 2 > e) return false; v = uint16_t(p[0]) | (uint16_t(p[1]) << 8); p += 2; return true;
    }
    static inline bool read_i32(const uint8_t*& p, const uint8_t* e, int32_t& v) {
        if (p + 4 > e) return false; v = int32_t(uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24)); p += 4; return true;
    }
    static inline bool read_u32(const uint8_t*& p, const uint8_t* e, uint32_t& v) {
        if (p + 4 > e) return false; v = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); p += 4; return true;
    }

    ExecResult exec(const uint8_t* blob, size_t blob_size, uint32_t offset,
        savor::DolphinWrapper& host,
        const savor::IDerivedBuffer* derived)
    {
        const uint8_t* p = blob + offset;
        const uint8_t* e = blob + blob_size;
        if (p >= e) return {};

        uint32_t va = 0;
        addr::Region region = addr::Region::MEM1;
        bool have_region = false;

        for (;;) {
            if (p >= e) return {};
            const uint8_t op = *p++;
            switch (op) {
            case END: return { va, true };

            case BASE_KEY: {
                uint16_t k = 0; if (!read_u16(p, e, k)) return {};
                auto key = static_cast<addr::AddrKey>(k);
                va = addr::Registry::base(key);
                region = addr::Registry::region(key);
                have_region = true;
                break;
            }

            case LOAD_PTR32: {
                if (!have_region) return {}; // must follow a BASE_KEY at some point
                uint32_t tmp = 0;
                switch (region) {
                case addr::Region::MEM1:
                case addr::Region::MEM2:
                    if (!host.readU32(va, tmp)) return {};
                    va = tmp;
                    break;
                case addr::Region::DERIVED: {
                    if (!derived) return {};
                    uint64_t bits = 0;
                    // requires IDerivedBuffer::read_raw(offset,width,...)
                    if (!derived->read_raw(va, /*width*/4, bits)) return {};
                    va = static_cast<uint32_t>(bits);
                    break;
                }
                default: return {};
                }
                break;
            }

            case ADD_I32: { int32_t imm = 0; if (!read_i32(p, e, imm)) return {}; va += static_cast<uint32_t>(imm); break; }
            case INDEX: { uint16_t n = 0, s = 0; if (!read_u16(p, e, n) || !read_u16(p, e, s)) return {}; va += uint32_t(n) * uint32_t(s); break; }
            case FIELD_OFF: { uint32_t off = 0; if (!read_u32(p, e, off)) return {}; va += off; break; }

            default: return {};
            }
        }
    }


} // namespace addrprog
