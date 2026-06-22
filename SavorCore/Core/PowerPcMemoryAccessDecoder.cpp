#include "PowerPcMemoryAccessDecoder.h"

#include <optional>

namespace savor::ppc {
namespace {

std::uint8_t ppc_rt(std::uint32_t opcode) { return static_cast<std::uint8_t>((opcode >> 21) & 0x1Fu); }
std::uint8_t ppc_ra(std::uint32_t opcode) { return static_cast<std::uint8_t>((opcode >> 16) & 0x1Fu); }
std::uint8_t ppc_rb(std::uint32_t opcode) { return static_cast<std::uint8_t>((opcode >> 11) & 0x1Fu); }
std::uint32_t ppc_xo(std::uint32_t opcode) { return (opcode >> 1) & 0x3FFu; }
std::int32_t ppc_simm16(std::uint32_t opcode) { return static_cast<std::int16_t>(opcode & 0xFFFFu); }

std::uint64_t mask_store_value(std::uint32_t value, std::uint32_t size)
{
    switch (size) {
    case 1: return value & 0xFFu;
    case 2: return value & 0xFFFFu;
    default: return value;
    }
}

void append_register_role(
    DecodedMemoryAccess& decoded,
    std::uint8_t reg,
    const char* role,
    std::uint32_t value)
{
    for (auto& existing : decoded.gpr_values) {
        if (existing.reg == reg) {
            if (existing.role.find(role) == std::string::npos) {
                if (!existing.role.empty()) existing.role += "+";
                existing.role += role;
            }
            return;
        }
    }
    decoded.gpr_values.push_back(DecodedMemoryRegisterValue{
        .reg = reg,
        .role = role,
        .value = value,
    });
}

bool read_gpr_for_decode(
    DecodedMemoryAccess& decoded,
    const GprReadFn& read_gpr,
    std::uint8_t reg,
    const char* role,
    std::uint32_t& out)
{
    if (!read_gpr || !read_gpr(reg, out)) {
        decoded.unsupported_reason = std::string("failed to read r") + std::to_string(reg);
        return false;
    }
    append_register_role(decoded, reg, role, out);
    return true;
}

void read_memory_value_for_decode(
    DecodedMemoryAccess& decoded,
    const MemoryReadFn& read_memory)
{
    if (!read_memory || !decoded.is_memory_access || decoded.access_size == 0 || decoded.access_size > 8) {
        return;
    }

    std::uint64_t value = 0;
    if (!read_memory(decoded.effective_address, decoded.access_size, value)) {
        return;
    }

    decoded.memory_value_available = true;
    decoded.memory_value = value;
    if (decoded.access == MemoryAccessKind::Read && !decoded.value_available) {
        decoded.value_available = true;
        decoded.value = value;
        decoded.value_source = "memory";
    }
}

struct AccessInfo {
    const char* mnemonic = nullptr;
    MemoryAccessKind access = MemoryAccessKind::Access;
    std::uint32_t size = 0;
    bool integer_store = false;
    bool multiple_store = false;
};

std::optional<AccessInfo> d_form_access_info(std::uint32_t primary)
{
    using Access = MemoryAccessKind;
    switch (primary) {
    case 32: return AccessInfo{ "lwz", Access::Read, 4 };
    case 33: return AccessInfo{ "lwzu", Access::Read, 4 };
    case 34: return AccessInfo{ "lbz", Access::Read, 1 };
    case 35: return AccessInfo{ "lbzu", Access::Read, 1 };
    case 36: return AccessInfo{ "stw", Access::Write, 4, true };
    case 37: return AccessInfo{ "stwu", Access::Write, 4, true };
    case 38: return AccessInfo{ "stb", Access::Write, 1, true };
    case 39: return AccessInfo{ "stbu", Access::Write, 1, true };
    case 40: return AccessInfo{ "lhz", Access::Read, 2 };
    case 41: return AccessInfo{ "lhzu", Access::Read, 2 };
    case 42: return AccessInfo{ "lha", Access::Read, 2 };
    case 43: return AccessInfo{ "lhau", Access::Read, 2 };
    case 44: return AccessInfo{ "sth", Access::Write, 2, true };
    case 45: return AccessInfo{ "sthu", Access::Write, 2, true };
    case 46: return AccessInfo{ "lmw", Access::Read, 4 };
    case 47: return AccessInfo{ "stmw", Access::Write, 4, false, true };
    case 48: return AccessInfo{ "lfs", Access::Read, 4 };
    case 49: return AccessInfo{ "lfsu", Access::Read, 4 };
    case 50: return AccessInfo{ "lfd", Access::Read, 8 };
    case 51: return AccessInfo{ "lfdu", Access::Read, 8 };
    case 52: return AccessInfo{ "stfs", Access::Write, 4 };
    case 53: return AccessInfo{ "stfsu", Access::Write, 4 };
    case 54: return AccessInfo{ "stfd", Access::Write, 8 };
    case 55: return AccessInfo{ "stfdu", Access::Write, 8 };
    default: return std::nullopt;
    }
}

std::optional<AccessInfo> x_form_access_info(std::uint32_t xo)
{
    using Access = MemoryAccessKind;
    switch (xo) {
    case 23: return AccessInfo{ "lwzx", Access::Read, 4 };
    case 55: return AccessInfo{ "lwzux", Access::Read, 4 };
    case 87: return AccessInfo{ "lbzx", Access::Read, 1 };
    case 119: return AccessInfo{ "lbzux", Access::Read, 1 };
    case 151: return AccessInfo{ "stwx", Access::Write, 4, true };
    case 183: return AccessInfo{ "stwux", Access::Write, 4, true };
    case 215: return AccessInfo{ "stbx", Access::Write, 1, true };
    case 247: return AccessInfo{ "stbux", Access::Write, 1, true };
    case 279: return AccessInfo{ "lhzx", Access::Read, 2 };
    case 311: return AccessInfo{ "lhzux", Access::Read, 2 };
    case 343: return AccessInfo{ "lhax", Access::Read, 2 };
    case 375: return AccessInfo{ "lhaux", Access::Read, 2 };
    case 407: return AccessInfo{ "sthx", Access::Write, 2, true };
    case 439: return AccessInfo{ "sthux", Access::Write, 2, true };
    case 535: return AccessInfo{ "lfsx", Access::Read, 4 };
    case 567: return AccessInfo{ "lfsux", Access::Read, 4 };
    case 599: return AccessInfo{ "lfdx", Access::Read, 8 };
    case 631: return AccessInfo{ "lfdux", Access::Read, 8 };
    case 663: return AccessInfo{ "stfsx", Access::Write, 4 };
    case 695: return AccessInfo{ "stfsux", Access::Write, 4 };
    case 727: return AccessInfo{ "stfdx", Access::Write, 8 };
    case 759: return AccessInfo{ "stfdux", Access::Write, 8 };
    default: return std::nullopt;
    }
}

} // namespace

DecodedMemoryAccess DecodeCurrentMemoryAccess(
    std::uint32_t pc,
    std::uint32_t opcode,
    const GprReadFn& read_gpr,
    const MemoryReadFn& read_memory)
{
    DecodedMemoryAccess decoded{};
    decoded.pc = pc;
    decoded.opcode = opcode;
    decoded.decoded = true;

    const std::uint32_t primary = opcode >> 26;
    const std::uint8_t rt = ppc_rt(opcode);
    const std::uint8_t ra = ppc_ra(opcode);

    if (primary == 18u) {
        decoded.mnemonic = (opcode & 1u) ? "bl" : "b";
        decoded.unsupported_reason = "not a memory access";
        return decoded;
    }
    if (primary == 16u) {
        decoded.mnemonic = "bc";
        decoded.unsupported_reason = "not a memory access";
        return decoded;
    }
    if (primary == 19u) {
        decoded.mnemonic = "branch";
        decoded.unsupported_reason = "not a memory access";
        return decoded;
    }

    if (const auto info = d_form_access_info(primary)) {
        decoded.mnemonic = info->mnemonic;
        decoded.is_memory_access = true;
        decoded.access = info->access;
        decoded.access_size = info->multiple_store ? (32u - rt) * 4u : info->size;
        decoded.base_reg = ra;
        decoded.has_base_reg = ra != 0;

        std::uint32_t base = 0;
        if (ra != 0 && !read_gpr_for_decode(decoded, read_gpr, ra, "base", base)) {
            return decoded;
        }

        decoded.effective_address = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(base) + static_cast<std::int64_t>(ppc_simm16(opcode)));

        if (info->integer_store) {
            std::uint32_t value = 0;
            if (!read_gpr_for_decode(decoded, read_gpr, rt, "source", value)) {
                return decoded;
            }
            decoded.value_reg = rt;
            decoded.has_value_reg = true;
            decoded.value_available = true;
            decoded.value = mask_store_value(value, info->size);
            decoded.value_source = "source_gpr";
        }

        read_memory_value_for_decode(decoded, read_memory);
        decoded.supported = true;
        return decoded;
    }

    if (primary == 31u) {
        const auto info = x_form_access_info(ppc_xo(opcode));
        if (!info.has_value()) {
            decoded.mnemonic = "opcode31";
            decoded.unsupported_reason = "unsupported opcode31 form";
            return decoded;
        }

        const std::uint8_t rb = ppc_rb(opcode);
        decoded.mnemonic = info->mnemonic;
        decoded.is_memory_access = true;
        decoded.access = info->access;
        decoded.access_size = info->size;
        decoded.base_reg = ra;
        decoded.has_base_reg = ra != 0;
        decoded.index_reg = rb;
        decoded.has_index_reg = true;

        std::uint32_t base = 0;
        std::uint32_t index = 0;
        if (ra != 0 && !read_gpr_for_decode(decoded, read_gpr, ra, "base", base)) {
            return decoded;
        }
        if (!read_gpr_for_decode(decoded, read_gpr, rb, "index", index)) {
            return decoded;
        }
        decoded.effective_address = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(index));

        if (info->integer_store) {
            std::uint32_t value = 0;
            if (!read_gpr_for_decode(decoded, read_gpr, rt, "source", value)) {
                return decoded;
            }
            decoded.value_reg = rt;
            decoded.has_value_reg = true;
            decoded.value_available = true;
            decoded.value = mask_store_value(value, info->size);
            decoded.value_source = "source_gpr";
        }

        read_memory_value_for_decode(decoded, read_memory);
        decoded.supported = true;
        return decoded;
    }

    decoded.mnemonic = "unknown";
    decoded.unsupported_reason = "unsupported opcode";
    return decoded;
}

} // namespace savor::ppc
