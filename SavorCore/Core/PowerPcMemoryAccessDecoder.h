#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace savor::ppc {

enum class MemoryAccessKind : std::uint32_t {
    Read = 1,
    Write = 2,
    Access = 3,
};

struct DecodedMemoryRegisterValue {
    std::uint8_t reg = 0;
    std::string role;
    std::uint32_t value = 0;
};

struct DecodedMemoryAccess {
    std::uint32_t pc = 0;
    std::uint32_t opcode = 0;
    bool decoded = false;
    bool is_memory_access = false;
    bool supported = false;
    std::string mnemonic;
    std::string unsupported_reason;
    MemoryAccessKind access = MemoryAccessKind::Access;
    std::uint32_t effective_address = 0;
    std::uint32_t access_size = 0;
    std::uint8_t base_reg = 0;
    bool has_base_reg = false;
    std::uint8_t index_reg = 0;
    bool has_index_reg = false;
    std::uint8_t value_reg = 0;
    bool has_value_reg = false;
    bool value_available = false;
    std::uint64_t value = 0;
    std::string value_source;
    bool memory_value_available = false;
    std::uint64_t memory_value = 0;
    std::vector<DecodedMemoryRegisterValue> gpr_values;
};

using GprReadFn = std::function<bool(std::uint8_t, std::uint32_t&)>;
using MemoryReadFn = std::function<bool(std::uint32_t, std::uint32_t, std::uint64_t&)>;

DecodedMemoryAccess DecodeCurrentMemoryAccess(
    std::uint32_t pc,
    std::uint32_t opcode,
    const GprReadFn& read_gpr,
    const MemoryReadFn& read_memory = {});

} // namespace savor::ppc
