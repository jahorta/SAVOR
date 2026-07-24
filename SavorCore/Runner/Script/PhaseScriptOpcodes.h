#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "../Breakpoints/BpRegistry.h"
#include "CtxRegistry.h"

namespace savor {

enum class PSOpArgFormat : uint8_t {
    None = 0,
    Key,
    FrameStep,
    BreakpointKey,
    ImmediateU32,
    ReadAddressToKey,
    WriteAddressFromKey,
    Label,
    Goto,
    GotoIfImmediate,
    GotoIfKeys,
    KeyImmediate,
    OpcodeStep,
    MemoryWatchpoint,
    Count,
};

enum class PSOpSupport : uint8_t {
    Unsupported = 0,
    Supported = 1,
};

enum class PSOpCode : uint8_t {
#define SAVOR_PHASE_SCRIPT_OPCODE(ORDINAL, SYMBOL, IDENTIFIER, DISPLAY, ARG_FORMAT, SUPPORT) SYMBOL = ORDINAL,
#include "PhaseScriptOpcodeTable.inc"
#undef SAVOR_PHASE_SCRIPT_OPCODE
    MATERIALIZE_BATTLE_END_RESULTS_MACRO_STEPS =
        MATERIALIZE_BATTLE_RESULTS_SCREEN_MACRO_STEPS,
    Count = 52,
};

static_assert(static_cast<uint8_t>(PSOpCode::START_DETERMINISTIC_RUN) == 11);
static_assert(static_cast<uint8_t>(PSOpCode::GC_SLOT_A_SET_FROM) == 21);
static_assert(static_cast<uint8_t>(PSOpCode::GET_NAVIGATION_CONTEXT) == 50);
static_assert(static_cast<uint8_t>(PSOpCode::RECORD_CURRENT_PC_TO) == 51);

struct PSOpMetadata {
    PSOpCode code{};
    uint8_t ordinal = 0;
    std::string_view identifier;
    std::string_view display_name;
    PSOpArgFormat arg_format = PSOpArgFormat::None;
    PSOpSupport support = PSOpSupport::Unsupported;
};

enum class PSCmp : uint8_t { EQ, NE, LT, LE, GT, GE };

enum class PSMemoryWatchpointAccess : uint32_t {
    Read = 1,
    Write = 2,
    Access = 3,
};

struct PSArg_Read { uint32_t addr; savor::context::key::KeyId dst; };
struct PSArg_Step { uint32_t n; };
struct PSArg_Path { std::string path; };
struct PSArg_ID6 { char id[6]{}; };
struct PSArg_Key { savor::context::key::KeyId id; };

struct PSArg_Label { std::string name; };
struct PSArg_Goto { std::string name; };
struct PSArg_GotoIf {
    savor::context::key::KeyId key;
    PSCmp cmp;
    uint32_t imm;
    std::string name;
};
struct PSArg_GotoIfKeys {
    savor::context::key::KeyId left;
    PSCmp cmp;
    savor::context::key::KeyId right;
    std::string name;
};
struct PSArg_Plan { uint32_t id; };
struct PSArg_ImmU32 { uint32_t v; };
struct PSArg_KeyImm { savor::context::key::KeyId key; uint32_t imm; };
struct PSArg_MemoryWatchpoint {
    uint32_t id = 0;
    uint32_t address = 0;
    savor::context::key::KeyId address_key = 0;
    uint32_t use_address_key = 0;
    uint32_t size = 0;
    PSMemoryWatchpointAccess access = PSMemoryWatchpointAccess::Write;
};

// Only the operand selected by code is semantically active. The explicit
// members retain the existing aggregate layout and builder call surface.
struct PSOp {
    PSOpCode code{};
    PSArg_Read rd{};
    PSArg_Step step{};
    PSArg_Key key{};
    PSArg_Label label{};
    PSArg_Goto jmp{};
    PSArg_GotoIf jcc{};
    PSArg_GotoIfKeys jcc2{};
    PSArg_Plan plan{};
    PSArg_ImmU32 imm{};
    PSArg_KeyImm keyimm{};
    PSArg_MemoryWatchpoint memwatch{};
};

[[nodiscard]] std::span<const PSOpMetadata> get_psop_catalogue() noexcept;
[[nodiscard]] const PSOpMetadata* get_psop_metadata(PSOpCode op) noexcept;
[[nodiscard]] std::string_view get_psop_identifier(PSOpCode op) noexcept;
[[nodiscard]] std::string get_psop_name(PSOpCode op);
[[nodiscard]] std::string get_psop_desc(const PSOp& op);

inline PSOp OpLabel(const std::string& value) { PSOp op; op.code = PSOpCode::LABEL; op.label.name = value; return op; }
inline PSOp OpGoto(const std::string& value) { PSOp op; op.code = PSOpCode::GOTO; op.jmp.name = value; return op; }
inline PSOp OpGotoIf(savor::context::key::KeyId key, PSCmp cmp, uint32_t value, const std::string& label) { PSOp op; op.code = PSOpCode::GOTO_IF; op.jcc = { key, cmp, value, label }; return op; }
inline PSOp OpGotoIfKeys(savor::context::key::KeyId left, PSCmp cmp, savor::context::key::KeyId right, const std::string& label) { PSOp op; op.code = PSOpCode::GOTO_IF_KEYS; op.jcc2 = { left, cmp, right, label }; return op; }
inline PSOp OpReturnResult(savor::context::key::KeyId key, uint32_t code) { PSOp op; op.code = PSOpCode::RETURN_RESULT; op.keyimm = { key, code }; return op; }
inline PSOp OpCapturePredBaselines() { PSOp op; op.code = PSOpCode::CAPTURE_PRED_BASELINES; return op; }
inline PSOp OpArmBpsFromPredTable() { PSOp op; op.code = PSOpCode::ARM_BPS_FROM_PRED_TABLE; return op; }
inline PSOp OpEvalPredicatesAtHitBP() { PSOp op; op.code = PSOpCode::EVAL_PREDICATES_AT_HIT_BP; return op; }
inline PSOp OpSetU32(savor::context::key::KeyId key, uint32_t value) { PSOp op; op.code = PSOpCode::SET_U32; op.keyimm = { key, value }; return op; }
inline PSOp OpAddU32(savor::context::key::KeyId key, uint32_t value) { PSOp op; op.code = PSOpCode::ADD_U32; op.keyimm = { key, value }; return op; }
inline PSOp OpApplyPlanFrameFrom(savor::context::key::KeyId key) { PSOp op; op.code = PSOpCode::APPLY_BATTLE_INPUTPLAN_FRAMES; op.key = { key }; return op; }
inline PSOp OpBuildTurnInputFromActions() { PSOp op; op.code = PSOpCode::BUILD_TURN_INPUTPLAN_FROM_BATTLE_PATH; return op; }
inline PSOp OpMaterializeBattleMacroSteps() { PSOp op; op.code = PSOpCode::MATERIALIZE_BATTLE_MACRO_STEPS; return op; }
inline PSOp OpMaterializeBattleTurnMacroSteps() { PSOp op; op.code = PSOpCode::MATERIALIZE_BATTLE_TURN_MACRO_STEPS; return op; }
inline PSOp OpExecuteBattleMacroStep() { PSOp op; op.code = PSOpCode::EXECUTE_BATTLE_MACRO_STEP; return op; }
// Ordinal 40 is the generic input-macro execution adapter. Keep the legacy
// builder above for existing battle-command programs while allowing new macro
// providers to use a provider-neutral name.
inline PSOp OpExecuteInputMacroStep() { return OpExecuteBattleMacroStep(); }
inline PSOp OpMaterializeBattleResultsScreenMacroSteps() { PSOp op; op.code = PSOpCode::MATERIALIZE_BATTLE_RESULTS_SCREEN_MACRO_STEPS; return op; }
inline PSOp OpMaterializeBattleEndResultsMacroSteps() { return OpMaterializeBattleResultsScreenMacroSteps(); }
inline PSOp OpMaterializeBattleCompletionMacroSteps() { PSOp op; op.code = PSOpCode::MATERIALIZE_BATTLE_COMPLETION_MACRO_STEPS; return op; }
inline PSOp OpRecordTasInputSample() { PSOp op; op.code = PSOpCode::RECORD_TAS_INPUT_SAMPLE; return op; }

inline PSOp OpStepFrames(uint32_t frame_count, bool disable_breakpoints = false) { PSOp op; op.code = PSOpCode::STEP_FRAMES; op.step = { frame_count }; op.imm = { disable_breakpoints ? 1u : 0u }; return op; }
inline PSOp OpStepOpcode(bool disable_breakpoints = false) { PSOp op; op.code = PSOpCode::STEP_OPCODE; op.imm = { disable_breakpoints ? 1u : 0u }; return op; }
inline PSOp OpArmMemoryWatchpoint(uint32_t id, uint32_t address, uint32_t size, PSMemoryWatchpointAccess access) { PSOp op; op.code = PSOpCode::ARM_MEMORY_WATCHPOINT; op.memwatch = { id, address, 0, 0, size, access }; return op; }
inline PSOp OpArmMemoryWatchpointFromKey(uint32_t id, savor::context::key::KeyId address_key, uint32_t size, PSMemoryWatchpointAccess access) { PSOp op; op.code = PSOpCode::ARM_MEMORY_WATCHPOINT; op.memwatch = { id, 0, address_key, 1, size, access }; return op; }
inline PSOp OpClearMemoryWatchpoints() { PSOp op; op.code = PSOpCode::CLEAR_MEMORY_WATCHPOINTS; return op; }
inline PSOp OpArmCaptureMemoryWatchpoints() { PSOp op; op.code = PSOpCode::ARM_CAPTURE_MEMORY_WATCHPOINTS; return op; }

inline PSOp OpApplyInputFrom(savor::context::key::KeyId key) { PSOp op; op.code = PSOpCode::APPLY_INPUT_FROM; op.key.id = key; return op; }
inline PSOp OpSetTimeoutFromKey(savor::context::key::KeyId key) { PSOp op; op.code = PSOpCode::SET_TIMEOUT_FROM; op.key.id = key; return op; }
inline PSOp OpSetTimeoutToMS(uint32_t ms) { PSOp op; op.code = PSOpCode::SET_TIMEOUT; op.imm.v = ms; return op; }
inline PSOp OpMoviePlayFrom(savor::context::key::KeyId key) { PSOp op; op.code = PSOpCode::MOVIE_PLAY_FROM; op.key.id = key; return op; }
inline PSOp OpSaveSavestateFrom(savor::context::key::KeyId key) { PSOp op; op.code = PSOpCode::SAVE_SAVESTATE_FROM; op.key.id = key; return op; }
inline PSOp OpRequireDiscGameIdFrom(savor::context::key::KeyId key) { PSOp op; op.code = PSOpCode::REQUIRE_DISC_GAMEID_FROM; op.key.id = key; return op; }

inline PSOp OpReadU8(uint32_t address, savor::context::key::KeyId dst) { PSOp op; op.code = PSOpCode::READ_U8; op.rd = { address, dst }; return op; }
inline PSOp OpReadU16(uint32_t address, savor::context::key::KeyId dst) { PSOp op; op.code = PSOpCode::READ_U16; op.rd = { address, dst }; return op; }
inline PSOp OpReadU32(uint32_t address, savor::context::key::KeyId dst) { PSOp op; op.code = PSOpCode::READ_U32; op.rd = { address, dst }; return op; }
inline PSOp OpWriteU32(uint32_t address, savor::context::key::KeyId value_key) { PSOp op; op.code = PSOpCode::WRITE_U32; op.rd = { address, value_key }; return op; }
inline PSOp OpReadF32(uint32_t address, savor::context::key::KeyId dst) { PSOp op; op.code = PSOpCode::READ_F32; op.rd = { address, dst }; return op; }
inline PSOp OpReadF64(uint32_t address, savor::context::key::KeyId dst) { PSOp op; op.code = PSOpCode::READ_F64; op.rd = { address, dst }; return op; }
inline PSOp OpGetBattleContext() { PSOp op; op.code = PSOpCode::GET_BATTLE_CONTEXT; return op; }
inline PSOp OpGetNavigationContext() { PSOp op; op.code = PSOpCode::GET_NAVIGATION_CONTEXT; return op; }
inline PSOp OpEmitResult(savor::context::key::KeyId key) { PSOp op; op.code = PSOpCode::EMIT_RESULT; op.key.id = key; return op; }

inline PSOp OpMovieStop() { PSOp op; op.code = PSOpCode::MOVIE_STOP; return op; }
inline PSOp OpArmPhaseBps() { PSOp op; op.code = PSOpCode::ARM_PHASE_BPS_ONCE; return op; }
inline PSOp OpLoadSnapshot() { PSOp op; op.code = PSOpCode::LOAD_SNAPSHOT; return op; }
inline PSOp OpCaptureSnapshot() { PSOp op; op.code = PSOpCode::CAPTURE_SNAPSHOT; return op; }
inline PSOp OpRunUntilBp() { PSOp op; op.code = PSOpCode::RUN_UNTIL_BP; return op; }
inline PSOp OpRunUntilBpKey(BPKey key) { PSOp op; op.code = PSOpCode::RUN_UNTIL_BP_KEY; op.imm.v = static_cast<uint32_t>(key); return op; }
inline PSOp OpRunUntilDebugStop() { PSOp op; op.code = PSOpCode::RUN_UNTIL_DEBUG_STOP; return op; }
inline PSOp OpCaptureSeedOverride() { PSOp op; op.code = PSOpCode::CAPTURE_SEED_OVERRIDE; return op; }
inline PSOp OpRecordCurrentBp() { PSOp op; op.code = PSOpCode::RECORD_CURRENT_BP; return op; }
inline PSOp OpRecordCurrentPcTo(savor::context::key::KeyId key) { PSOp op; op.code = PSOpCode::RECORD_CURRENT_PC_TO; op.key.id = key; return op; }
inline PSOp OpStartDeterministicRun() { PSOp op; op.code = PSOpCode::START_DETERMINISTIC_RUN; return op; }
inline PSOp OpEndDeterministicRun() { PSOp op; op.code = PSOpCode::END_DETERMINISTIC_RUN; return op; }
inline PSOp OpRebootCore() { PSOp op; op.code = PSOpCode::REBOOT_CORE; return op; }

} // namespace savor
