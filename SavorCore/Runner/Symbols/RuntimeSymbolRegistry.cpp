#include "RuntimeSymbolRegistry.h"

#include <algorithm>
#include <sstream>

namespace savor::symbols {

namespace {

std::string DomainName(BPKey key)
{
    switch (bp::domain_of(key)) {
    case bp::BPDomain::PreBattle: return "prebattle";
    case bp::BPDomain::Battle: return "battle";
    case bp::BPDomain::Overworld: return "overworld";
    default: return "unknown";
    }
}

bool IsCustomId(std::string_view id)
{
    return id.rfind("user.", 0) == 0;
}

bool IsBuiltinId(std::string_view id)
{
    return id.rfind("builtin.", 0) == 0;
}

bool ValidateStableId(std::string_view id, std::string_view expected_prefix, std::string* error_out)
{
    if (id.empty()) {
        if (error_out) *error_out = "symbol id is empty";
        return false;
    }
    if (id.rfind(expected_prefix, 0) != 0) {
        if (error_out) *error_out = "symbol id must start with " + std::string(expected_prefix);
        return false;
    }
    if (IsBuiltinId(id)) {
        if (error_out) *error_out = "built-in symbol overrides are not supported";
        return false;
    }
    return true;
}

PSOp LowerReadOp(SymbolicOp::Kind kind, uint32_t address, savor::context::key::KeyId dst)
{
    switch (kind) {
    case SymbolicOp::Kind::ReadU8: return OpReadU8(address, dst);
    case SymbolicOp::Kind::ReadU16: return OpReadU16(address, dst);
    case SymbolicOp::Kind::ReadU32: return OpReadU32(address, dst);
    case SymbolicOp::Kind::ReadF32: return OpReadF32(address, dst);
    case SymbolicOp::Kind::ReadF64: return OpReadF64(address, dst);
    default: return {};
    }
}

} // namespace

std::string_view ToString(ContextValueType type)
{
    switch (type) {
    case ContextValueType::U8: return "u8";
    case ContextValueType::U16: return "u16";
    case ContextValueType::U32: return "u32";
    case ContextValueType::F32: return "f32";
    case ContextValueType::F64: return "f64";
    case ContextValueType::String: return "string";
    case ContextValueType::GCInputFrame: return "gc_input_frame";
    case ContextValueType::BattlePath: return "battle_path";
    }
    return "";
}

std::optional<ContextValueType> ParseContextValueType(std::string_view value)
{
    if (value == "u8") return ContextValueType::U8;
    if (value == "u16") return ContextValueType::U16;
    if (value == "u32") return ContextValueType::U32;
    if (value == "f32") return ContextValueType::F32;
    if (value == "f64") return ContextValueType::F64;
    if (value == "string") return ContextValueType::String;
    if (value == "gc_input_frame") return ContextValueType::GCInputFrame;
    if (value == "battle_path") return ContextValueType::BattlePath;
    return std::nullopt;
}

RuntimeSymbolRegistry RuntimeSymbolRegistry::BuiltIns()
{
    RuntimeSymbolRegistry registry;

    size_t count = 0;
    const auto* keys = savor::context::key::CtxRegistry::all_keys(count);
    for (size_t i = 0; i < count; ++i) {
        ContextSymbol symbol;
        symbol.stable_id = "builtin.ctx." + std::string(keys[i].name);
        symbol.name = std::string(keys[i].name);
        symbol.type = ContextValueType::U32;
        symbol.key = keys[i].id;
        symbol.builtin = true;
        registry.AddContextSymbol(std::move(symbol));
    }

    for (const auto& rec : addr::AddrRegistry::all()) {
        AddressSymbol symbol;
        symbol.stable_id = "builtin.addr." + std::string(rec.name);
        symbol.name = rec.name;
        symbol.region = rec.spec.region;
        symbol.base = rec.spec.base;
        symbol.key = static_cast<uint16_t>(rec.key);
        symbol.builtin = true;
        registry.AddAddressSymbol(std::move(symbol));
    }

    for (const auto& bp : bp::BpRegistry::all()) {
        BreakpointSymbol symbol;
        symbol.stable_id = "builtin.bp." + DomainName(bp.key) + "." + bp.name;
        symbol.name = bp.name;
        symbol.pc = bp.pc;
        symbol.key = bp.key;
        symbol.enabled = true;
        symbol.builtin = true;
        registry.AddBreakpointSymbol(std::move(symbol));
    }

    return registry;
}

bool RuntimeSymbolRegistry::ReserveStableId(std::string_view id, std::string* error_out)
{
    if (stable_id_index_.find(std::string(id)) != stable_id_index_.end()) {
        if (error_out) *error_out = "duplicate symbol id: " + std::string(id);
        return false;
    }
    return true;
}

bool RuntimeSymbolRegistry::AddContextSymbol(ContextSymbol symbol, std::string* error_out)
{
    if (!symbol.builtin && !ValidateStableId(symbol.stable_id, "user.ctx.", error_out)) return false;
    if (!ReserveStableId(symbol.stable_id, error_out)) return false;
    if (symbol.name.empty()) symbol.name = symbol.stable_id;
    if (!symbol.builtin && symbol.key == 0) symbol.key = NextCustomContextKey();
    if (context_key_index_.find(symbol.key) != context_key_index_.end()) {
        if (error_out) *error_out = "duplicate context key id";
        return false;
    }
    const size_t index = contexts_.size();
    context_key_index_[symbol.key] = index;
    stable_id_index_[symbol.stable_id] = index;
    contexts_.push_back(std::move(symbol));
    return true;
}

bool RuntimeSymbolRegistry::AddAddressSymbol(AddressSymbol symbol, std::string* error_out)
{
    if (!symbol.builtin && !ValidateStableId(symbol.stable_id, "user.addr.", error_out)) return false;
    if (!ReserveStableId(symbol.stable_id, error_out)) return false;
    if (symbol.name.empty()) symbol.name = symbol.stable_id;
    if (!symbol.builtin && symbol.key == 0) symbol.key = NextCustomAddressKey();
    if (address_key_index_.find(symbol.key) != address_key_index_.end()) {
        if (error_out) *error_out = "duplicate address key id";
        return false;
    }
    const size_t index = addresses_.size();
    address_key_index_[symbol.key] = index;
    stable_id_index_[symbol.stable_id] = index;
    addresses_.push_back(std::move(symbol));
    return true;
}

bool RuntimeSymbolRegistry::AddBreakpointSymbol(BreakpointSymbol symbol, std::string* error_out)
{
    if (!symbol.builtin && !ValidateStableId(symbol.stable_id, "user.bp.", error_out)) return false;
    if (!ReserveStableId(symbol.stable_id, error_out)) return false;
    if (symbol.name.empty()) symbol.name = symbol.stable_id;
    if (!symbol.builtin) {
        const auto* address = FindAddress(symbol.address_id);
        if (address == nullptr) {
            if (error_out) *error_out = "breakpoint references missing address: " + symbol.address_id;
            return false;
        }
        symbol.pc = address->base;
        if (symbol.key == 0) symbol.key = NextCustomBreakpointKey();
    }
    if (breakpoint_key_index_.find(symbol.key) != breakpoint_key_index_.end()) {
        if (error_out) *error_out = "duplicate breakpoint key id";
        return false;
    }
    const size_t index = breakpoints_.size();
    breakpoint_key_index_[symbol.key] = index;
    stable_id_index_[symbol.stable_id] = index;
    breakpoints_.push_back(std::move(symbol));
    return true;
}

const ContextSymbol* RuntimeSymbolRegistry::FindContext(std::string_view stable_id) const
{
    for (const auto& symbol : contexts_) if (symbol.stable_id == stable_id) return &symbol;
    return nullptr;
}

const ContextSymbol* RuntimeSymbolRegistry::FindContext(savor::context::key::KeyId key) const
{
    auto it = context_key_index_.find(key);
    return it == context_key_index_.end() ? nullptr : &contexts_[it->second];
}

const AddressSymbol* RuntimeSymbolRegistry::FindAddress(std::string_view stable_id) const
{
    for (const auto& symbol : addresses_) if (symbol.stable_id == stable_id) return &symbol;
    return nullptr;
}

const AddressSymbol* RuntimeSymbolRegistry::FindAddress(uint16_t key) const
{
    auto it = address_key_index_.find(key);
    return it == address_key_index_.end() ? nullptr : &addresses_[it->second];
}

const BreakpointSymbol* RuntimeSymbolRegistry::FindBreakpoint(std::string_view stable_id) const
{
    for (const auto& symbol : breakpoints_) if (symbol.stable_id == stable_id) return &symbol;
    return nullptr;
}

const BreakpointSymbol* RuntimeSymbolRegistry::FindBreakpoint(BPKey key) const
{
    auto it = breakpoint_key_index_.find(key);
    return it == breakpoint_key_index_.end() ? nullptr : &breakpoints_[it->second];
}

const BreakpointSymbol* RuntimeSymbolRegistry::MatchBreakpointPc(uint32_t pc) const
{
    for (const auto& symbol : breakpoints_) {
        if (symbol.enabled && symbol.pc == pc) return &symbol;
    }
    return nullptr;
}

BreakpointMap RuntimeSymbolRegistry::BuildBreakpointMap() const
{
    BreakpointMap map;
    for (const auto& symbol : breakpoints_) {
        if (!symbol.enabled) continue;
        map.addrs.push_back(BPAddr{ symbol.key, symbol.pc, symbol.name.c_str(), symbol.stable_id.c_str() });
    }
    return map;
}

bool RuntimeSymbolRegistry::LowerSymbolicPhaseScript(
    const SymbolicPhaseScript& symbolic,
    PhaseScript& out,
    std::string* error_out) const
{
    out = PhaseScript{};
    for (const auto& id : symbolic.canonical_breakpoint_ids) {
        const auto* bp = FindBreakpoint(id);
        if (bp == nullptr) {
            if (error_out) *error_out = "unknown breakpoint symbol: " + id;
            return false;
        }
        out.canonical_bp_keys.push_back(bp->key);
    }

    for (const auto& op : symbolic.ops) {
        switch (op.kind) {
        case SymbolicOp::Kind::Label:
            out.ops.push_back(OpLabel(op.label));
            break;
        case SymbolicOp::Kind::Goto:
            out.ops.push_back(OpGoto(op.label));
            break;
        case SymbolicOp::Kind::RunUntilBp:
            out.ops.push_back(OpRunUntilBp());
            break;
        case SymbolicOp::Kind::GotoIf: {
            const auto* left = FindContext(op.left_key_id);
            if (left == nullptr) {
                if (error_out) *error_out = "unknown context symbol: " + op.left_key_id;
                return false;
            }
            out.ops.push_back(OpGotoIf(left->key, op.cmp, op.imm, op.label));
            break;
        }
        case SymbolicOp::Kind::GotoIfKeys: {
            const auto* left = FindContext(op.left_key_id);
            const auto* right = FindContext(op.right_key_id);
            if (left == nullptr || right == nullptr) {
                if (error_out) *error_out = "unknown context symbol in key comparison";
                return false;
            }
            out.ops.push_back(OpGotoIfKeys(left->key, op.cmp, right->key, op.label));
            break;
        }
        case SymbolicOp::Kind::ReturnResult: {
            const auto* key = FindContext(op.left_key_id);
            if (key == nullptr) {
                if (error_out) *error_out = "unknown result context symbol: " + op.left_key_id;
                return false;
            }
            out.ops.push_back(OpReturnResult(key->key, op.imm));
            break;
        }
        case SymbolicOp::Kind::EmitResult: {
            const auto* key = FindContext(op.left_key_id);
            if (key == nullptr) {
                if (error_out) *error_out = "unknown emit context symbol: " + op.left_key_id;
                return false;
            }
            out.ops.push_back(OpEmitResult(key->key));
            break;
        }
        case SymbolicOp::Kind::SetU32:
        case SymbolicOp::Kind::AddU32: {
            const auto* key = FindContext(op.left_key_id);
            if (key == nullptr) {
                if (error_out) *error_out = "unknown context symbol: " + op.left_key_id;
                return false;
            }
            out.ops.push_back(op.kind == SymbolicOp::Kind::SetU32
                ? OpSetU32(key->key, op.imm)
                : OpAddU32(key->key, op.imm));
            break;
        }
        case SymbolicOp::Kind::ReadU8:
        case SymbolicOp::Kind::ReadU16:
        case SymbolicOp::Kind::ReadU32:
        case SymbolicOp::Kind::ReadF32:
        case SymbolicOp::Kind::ReadF64: {
            const auto* address = FindAddress(op.address_id);
            const auto* dst = FindContext(op.left_key_id);
            if (address == nullptr || dst == nullptr) {
                if (error_out) *error_out = "unknown address or destination context symbol";
                return false;
            }
            out.ops.push_back(LowerReadOp(op.kind, address->base, dst->key));
            break;
        }
        }
    }
    return true;
}

savor::context::key::KeyId RuntimeSymbolRegistry::NextCustomContextKey() const
{
    for (uint16_t k = CustomContextKeyMin; k <= CustomContextKeyMax; ++k) {
        if (context_key_index_.find(k) == context_key_index_.end()) return k;
    }
    return 0;
}

uint16_t RuntimeSymbolRegistry::NextCustomAddressKey() const
{
    for (uint16_t k = CustomAddressKeyMin; k <= CustomAddressKeyMax; ++k) {
        if (address_key_index_.find(k) == address_key_index_.end()) return k;
    }
    return 0;
}

BPKey RuntimeSymbolRegistry::NextCustomBreakpointKey() const
{
    for (uint16_t k = CustomBreakpointKeyMin; k <= CustomBreakpointKeyMax; ++k) {
        if (breakpoint_key_index_.find(k) == breakpoint_key_index_.end()) return k;
    }
    return 0;
}

} // namespace savor::symbols
