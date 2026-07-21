#include "PhaseScriptOpcodes.h"

#include <array>
#include <sstream>

namespace savor {
namespace {

constexpr std::array<PSOpMetadata, static_cast<size_t>(PSOpCode::Count)> kOpcodeCatalogue{{
#define SAVOR_PHASE_SCRIPT_OPCODE(ORDINAL, SYMBOL, IDENTIFIER, DISPLAY, ARG_FORMAT, SUPPORT) \
    { PSOpCode::SYMBOL, static_cast<uint8_t>(ORDINAL), IDENTIFIER, DISPLAY, PSOpArgFormat::ARG_FORMAT, PSOpSupport::SUPPORT },
#include "PhaseScriptOpcodeTable.inc"
#undef SAVOR_PHASE_SCRIPT_OPCODE
}};

constexpr bool IsValidCatalogue()
{
    for (size_t index = 0; index < kOpcodeCatalogue.size(); ++index) {
        const auto& metadata = kOpcodeCatalogue[index];
        if (metadata.ordinal != index) return false;
        if (static_cast<size_t>(metadata.code) != index) return false;
        if (metadata.identifier.empty() || metadata.display_name.empty()) return false;
        if (metadata.arg_format >= PSOpArgFormat::Count) return false;
    }
    return true;
}

static_assert(kOpcodeCatalogue.size() == 50);
static_assert(IsValidCatalogue());

std::string CompareName(PSCmp cmp)
{
    switch (cmp) {
    case PSCmp::EQ: return "EQ";
    case PSCmp::NE: return "NE";
    case PSCmp::LT: return "LT";
    case PSCmp::LE: return "LE";
    case PSCmp::GT: return "GT";
    case PSCmp::GE: return "GE";
    default: return "?";
    }
}

std::string KeyDescription(savor::context::key::KeyId key)
{
    const std::string_view name = savor::context::key::name_for_id(key);
    if (!name.empty()) return std::string(name);
    return std::to_string(static_cast<uint32_t>(key));
}

} // namespace

std::span<const PSOpMetadata> get_psop_catalogue() noexcept
{
    return kOpcodeCatalogue;
}

const PSOpMetadata* get_psop_metadata(PSOpCode op) noexcept
{
    const auto ordinal = static_cast<size_t>(op);
    if (ordinal >= kOpcodeCatalogue.size()) return nullptr;
    const auto& metadata = kOpcodeCatalogue[ordinal];
    return metadata.code == op ? &metadata : nullptr;
}

std::string_view get_psop_identifier(PSOpCode op) noexcept
{
    const auto* metadata = get_psop_metadata(op);
    return metadata != nullptr ? metadata->identifier : std::string_view{};
}

std::string get_psop_name(PSOpCode op)
{
    const auto* metadata = get_psop_metadata(op);
    return metadata != nullptr ? std::string(metadata->display_name) : std::string("Unknown Code");
}

std::string get_psop_desc(const PSOp& op)
{
    std::ostringstream args;
    const auto* metadata = get_psop_metadata(op.code);
    if (metadata != nullptr) {
        switch (metadata->arg_format) {
        case PSOpArgFormat::None:
            break;
        case PSOpArgFormat::Key:
            args << "key=" << KeyDescription(op.key.id);
            break;
        case PSOpArgFormat::FrameStep:
            args << "n=" << op.step.n << ", disable_breakpoints=" << op.imm.v;
            break;
        case PSOpArgFormat::BreakpointKey:
            args << "bp_key=" << op.imm.v;
            break;
        case PSOpArgFormat::ImmediateU32:
            args << "ms=" << op.imm.v;
            break;
        case PSOpArgFormat::ReadAddressToKey:
            args << "addr=" << op.rd.addr << ", dst=" << KeyDescription(op.rd.dst);
            break;
        case PSOpArgFormat::WriteAddressFromKey:
            args << "addr=" << op.rd.addr << ", value_key=" << KeyDescription(op.rd.dst);
            break;
        case PSOpArgFormat::Label:
            args << "name=" << op.label.name;
            break;
        case PSOpArgFormat::Goto:
            args << "name=" << op.jmp.name;
            break;
        case PSOpArgFormat::GotoIfImmediate:
            args << "key=" << KeyDescription(op.jcc.key)
                 << ", cmp=" << CompareName(op.jcc.cmp)
                 << ", imm=" << op.jcc.imm
                 << ", name=" << op.jcc.name;
            break;
        case PSOpArgFormat::GotoIfKeys:
            args << "left=" << KeyDescription(op.jcc2.left)
                 << ", cmp=" << CompareName(op.jcc2.cmp)
                 << ", right=" << KeyDescription(op.jcc2.right)
                 << ", name=" << op.jcc2.name;
            break;
        case PSOpArgFormat::KeyImmediate:
            args << "key=" << KeyDescription(op.keyimm.key) << ", imm=" << op.keyimm.imm;
            break;
        case PSOpArgFormat::OpcodeStep:
            args << "disable_breakpoints=" << op.imm.v;
            break;
        case PSOpArgFormat::MemoryWatchpoint:
            args << "id=" << op.memwatch.id
                 << ", addr=" << op.memwatch.address
                 << ", addr_key=" << KeyDescription(op.memwatch.address_key)
                 << ", use_addr_key=" << op.memwatch.use_address_key
                 << ", size=" << op.memwatch.size
                 << ", access=" << static_cast<uint32_t>(op.memwatch.access);
            break;
        case PSOpArgFormat::Count:
            break;
        }
    }
    return get_psop_name(op.code) + ": [" + args.str() + "]";
}

} // namespace savor
