#include "SoaAddrProgram.h"
#include "../Soa/SoaAddrRegistry.h"
#include "../../DolphinWrapper.h"
#include "Battle/DerivedBattleBuffer.h"

#include <utility>

using addr::AddrRegistry;
using addr::Region;

namespace addrprog {
namespace {

    static inline bool read_u16(const uint8_t*& p, const uint8_t* e, uint16_t& v) {
        if (p + 2 > e) return false; v = uint16_t(p[0]) | (uint16_t(p[1]) << 8); p += 2; return true;
    }
    static inline bool read_i32(const uint8_t*& p, const uint8_t* e, int32_t& v) {
        if (p + 4 > e) return false; v = int32_t(uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24)); p += 4; return true;
    }
    static inline bool read_u32(const uint8_t*& p, const uint8_t* e, uint32_t& v) {
        if (p + 4 > e) return false; v = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); p += 4; return true;
    }

    static inline bool read_u8(const uint8_t*& p, const uint8_t* e, uint8_t& v) {
        if (p + 1 > e) return false; v = *p++; return true;
    }

    static const char* op_name(const uint8_t op) {
        switch (op) {
        case END: return "END";
        case BASE_KEY: return "BASE_KEY";
        case LOAD_PTR32: return "LOAD_PTR32";
        case ADD_I32: return "ADD_I32";
        case INDEX: return "INDEX";
        case FIELD_OFF: return "FIELD_OFF";
        case BASE_GPR: return "BASE_GPR";
        case BASE_ABS: return "BASE_ABS";
        default: return "UNKNOWN";
        }
    }

    static void append_step(
        EvalResult& result,
        const bool include_trace,
        const uint8_t op,
        const uint32_t before,
        const uint32_t after,
        const uint64_t value = 0,
        const bool has_value = false)
    {
        if (!include_trace) return;
        result.trace.push_back(EvalTraceStep{
            .op = op,
            .op_name = op_name(op),
            .address_before = before,
            .address_after = after,
            .value = value,
            .has_value = has_value,
        });
    }

    static EvalResult fail(std::string error, EvalResult result = {}) {
        result.ok = false;
        result.error = std::move(error);
        return result;
    }

    static bool read_host_value(savor::DolphinWrapper& host, const uint32_t va, const uint8_t width, uint64_t& out_bits)
    {
        switch (width) {
        case 1: { uint8_t v = 0; if (!host.readU8(va, v)) return false; out_bits = v; return true; }
        case 2: { uint16_t v = 0; if (!host.readU16(va, v)) return false; out_bits = v; return true; }
        case 4: { uint32_t v = 0; if (!host.readU32(va, v)) return false; out_bits = v; return true; }
        case 8: { uint64_t v = 0; if (!host.readU64(va, v)) return false; out_bits = v; return true; }
        default: return false;
        }
    }

} // namespace

    ExecResult exec(const uint8_t* blob, size_t blob_size, uint32_t offset,
        savor::DolphinWrapper& host,
        const savor::IDerivedBuffer* derived)
    {
        const auto result = evaluate(blob, blob_size, offset, host, derived);
        return { result.va, result.ok };
    }

    EvalResult evaluate(const uint8_t* blob, size_t blob_size, uint32_t offset,
        savor::DolphinWrapper& host,
        const savor::IDerivedBuffer* derived,
        const RegisterReadFn& read_register,
        const bool include_trace)
    {
        if (blob == nullptr) return fail("null program blob");
        const uint8_t* p = blob + offset;
        const uint8_t* e = blob + blob_size;
        if (offset >= blob_size || p >= e) return fail("program offset out of range");

        uint32_t va = 0;
        addr::Region region = addr::Region::MEM1;
        MemoryDomain domain = MemoryDomain::Host;
        bool have_region = false;
        EvalResult result{};

        for (;;) {
            if (p >= e) return fail("program ended without END", result);
            const uint8_t op = *p++;
            switch (op) {
            case END:
                append_step(result, include_trace, op, va, va);
                result.va = va;
                result.domain = domain;
                result.ok = true;
                return result;

            case BASE_KEY: {
                const uint32_t before = va;
                uint16_t k = 0; if (!read_u16(p, e, k)) return fail("truncated BASE_KEY operand", result);
                auto key = static_cast<addr::AddrKey>(k);
                va = addr::AddrRegistry::base(key);
                region = addr::AddrRegistry::region(key);
                domain = region == addr::Region::DERIVED ? MemoryDomain::Derived : MemoryDomain::Host;
                have_region = true;
                append_step(result, include_trace, op, before, va, k, true);
                break;
            }

            case LOAD_PTR32: {
                if (!have_region) return fail("LOAD_PTR32 before base", result);
                const uint32_t before = va;
                uint32_t tmp = 0;
                switch (region) {
                case addr::Region::MEM1:
                case addr::Region::MEM2:
                    if (!host.readU32(va, tmp)) return fail("host LOAD_PTR32 read failed", result);
                    if (tmp == 0) return fail("LOAD_PTR32 resolved null", result);
                    va = tmp;
                    break;
                case addr::Region::DERIVED: {
                    if (!derived) return fail("derived LOAD_PTR32 without derived buffer", result);
                    uint64_t bits = 0;
                    // requires IDerivedBuffer::read_raw(offset,width,...)
                    if (!derived->read_raw(va, /*width*/4, bits)) return fail("derived LOAD_PTR32 read failed", result);
                    va = static_cast<uint32_t>(bits);
                    if (va == 0) return fail("LOAD_PTR32 resolved null", result);
                    break;
                }
                default: return fail("unsupported LOAD_PTR32 region", result);
                }
                append_step(result, include_trace, op, before, va, va, true);
                break;
            }

            case ADD_I32: {
                const uint32_t before = va;
                int32_t imm = 0; if (!read_i32(p, e, imm)) return fail("truncated ADD_I32 operand", result);
                va += static_cast<uint32_t>(imm);
                append_step(result, include_trace, op, before, va, static_cast<uint32_t>(imm), true);
                break;
            }
            case INDEX: {
                const uint32_t before = va;
                uint16_t n = 0, s = 0; if (!read_u16(p, e, n) || !read_u16(p, e, s)) return fail("truncated INDEX operand", result);
                va += uint32_t(n) * uint32_t(s);
                append_step(result, include_trace, op, before, va, uint32_t(n) * uint32_t(s), true);
                break;
            }
            case FIELD_OFF: {
                const uint32_t before = va;
                uint32_t off = 0; if (!read_u32(p, e, off)) return fail("truncated FIELD_OFF operand", result);
                va += off;
                append_step(result, include_trace, op, before, va, off, true);
                break;
            }
            case BASE_GPR: {
                const uint32_t before = va;
                uint8_t reg = 0; if (!read_u8(p, e, reg)) return fail("truncated BASE_GPR operand", result);
                if (reg > 31) return fail("BASE_GPR register out of range", result);
                if (!read_register) return fail("BASE_GPR without register reader", result);
                if (!read_register(reg, va)) return fail("BASE_GPR register read failed", result);
                region = addr::Region::MEM1;
                domain = MemoryDomain::Host;
                have_region = true;
                append_step(result, include_trace, op, before, va, reg, true);
                break;
            }
            case BASE_ABS: {
                const uint32_t before = va;
                if (!read_u32(p, e, va)) return fail("truncated BASE_ABS operand", result);
                region = addr::Region::MEM1;
                domain = MemoryDomain::Host;
                have_region = true;
                append_step(result, include_trace, op, before, va);
                break;
            }

            default: return fail("unknown addrprog op", result);
            }
        }
    }

    bool read_value(const uint8_t* blob, size_t blob_size, uint32_t offset,
        savor::DolphinWrapper& host,
        const savor::IDerivedBuffer* derived,
        const uint8_t width,
        uint64_t& out_bits,
        EvalResult* eval_out,
        const RegisterReadFn& read_register,
        const bool include_trace)
    {
        auto result = evaluate(blob, blob_size, offset, host, derived, read_register, include_trace);
        if (!result.ok) {
            if (eval_out) *eval_out = std::move(result);
            return false;
        }

        bool read_ok = false;
        switch (result.domain) {
        case MemoryDomain::Host:
            read_ok = read_host_value(host, result.va, width, out_bits);
            break;
        case MemoryDomain::Derived:
            read_ok = derived != nullptr && derived->read_raw(result.va, width, out_bits);
            break;
        default:
            read_ok = false;
            break;
        }
        if (eval_out) *eval_out = std::move(result);
        return read_ok;
    }

} // namespace addrprog
