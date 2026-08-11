#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../Breakpoints/BpRegistry.h"
#include "../Script/CtxRegistry.h"
#include "../../Core/Memory/Soa/SoaAddrRegistry.h"

namespace savor::symbols {

enum class ContextValueType {
    U8,
    U16,
    U32,
    F32,
    F64,
    String,
    GCInputFrame,
    BattlePath,
};

std::string_view ToString(ContextValueType type);
std::optional<ContextValueType> ParseContextValueType(std::string_view value);

struct ContextSymbol {
    std::string stable_id;
    std::string name;
    ContextValueType type = ContextValueType::U32;
    savor::context::key::KeyId key = 0;
    bool builtin = false;
};

struct AddressSymbol {
    std::string stable_id;
    std::string name;
    addr::Region region = addr::Region::MEM1;
    uint32_t base = 0;
    std::optional<uint8_t> width;
    std::optional<ContextValueType> value_type;
    uint16_t key = 0;
    bool builtin = false;
};

struct BreakpointSymbol {
    std::string stable_id;
    std::string name;
    std::string address_id;
    uint32_t pc = 0;
    BPKey key = 0;
    bool enabled = true;
    bool builtin = false;
    BreakpointVisibility visibility = BreakpointVisibility::PlayerVisible;
    BreakpointOwner owner = BreakpointOwner::Shared;
};

class RuntimeSymbolRegistry {
public:
    static constexpr uint16_t CustomContextKeyMin = 0x8000;
    static constexpr uint16_t CustomContextKeyMax = 0xEFFF;
    static constexpr uint16_t CustomAddressKeyMin = 0x8000;
    static constexpr uint16_t CustomAddressKeyMax = 0xEFFF;
    static constexpr uint16_t CustomBreakpointKeyMin = 0x8000;
    static constexpr uint16_t CustomBreakpointKeyMax = 0xEFFF;

    static RuntimeSymbolRegistry BuiltIns();

    bool AddContextSymbol(ContextSymbol symbol, std::string* error_out = nullptr);
    bool AddAddressSymbol(AddressSymbol symbol, std::string* error_out = nullptr);
    bool AddBreakpointSymbol(BreakpointSymbol symbol, std::string* error_out = nullptr);

    const ContextSymbol* FindContext(std::string_view stable_id) const;
    const ContextSymbol* FindContext(savor::context::key::KeyId key) const;
    const AddressSymbol* FindAddress(std::string_view stable_id) const;
    const AddressSymbol* FindAddress(uint16_t key) const;
    const BreakpointSymbol* FindBreakpoint(std::string_view stable_id) const;
    const BreakpointSymbol* FindBreakpoint(BPKey key) const;
    const BreakpointSymbol* MatchBreakpointPc(uint32_t pc) const;

    const std::vector<ContextSymbol>& Contexts() const { return contexts_; }
    const std::vector<AddressSymbol>& Addresses() const { return addresses_; }
    const std::vector<BreakpointSymbol>& Breakpoints() const { return breakpoints_; }

    BreakpointMap BuildBreakpointMap() const;
private:
    bool ReserveStableId(std::string_view id, std::string* error_out);
    savor::context::key::KeyId NextCustomContextKey() const;
    uint16_t NextCustomAddressKey() const;
    BPKey NextCustomBreakpointKey() const;

    std::vector<ContextSymbol> contexts_;
    std::vector<AddressSymbol> addresses_;
    std::vector<BreakpointSymbol> breakpoints_;
    std::unordered_map<std::string, size_t> stable_id_index_;
    std::unordered_map<savor::context::key::KeyId, size_t> context_key_index_;
    std::unordered_map<uint16_t, size_t> address_key_index_;
    std::unordered_map<BPKey, size_t> breakpoint_key_index_;
};

} // namespace savor::symbols
