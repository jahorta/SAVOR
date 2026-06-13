#pragma once

#include <cstdint>
#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "../../../Core/Memory/Soa/Battle/BattleContext.h"
#include "../../../Runner/Breakpoints/BpRegistry.h"
#include "../../../Runner/Script/PhaseScriptVM.h"

namespace phase::battle::macroprobe {

static constexpr int PayloadVersion = 2;

enum class MacroMode : std::uint32_t {
    Attack = 1,
    Focus = 2,
    Block = 3,
};

enum class FailureCode : std::uint32_t {
    Ok = 0,
    InvalidMode = 1,
    InvalidTarget = 2,
    NoSteps = 3,
    Timeout = 4,
    UnexpectedBreakpoint = 5,
    BattleContextUnavailable = 6,
};

struct MacroStep {
    enum class Kind : std::uint32_t {
        InputGate = 0,
        NeutralFrames = 1,
    };

    const char* label = "";
    Kind kind = Kind::InputGate;
    savor::GCInputFrame input{};
    std::uint32_t frame_count = 0;
    bool hold_input_through_hit_opcode = false;
    std::vector<BPKey> expected_bps;
};

struct MacroCommand {
    MacroMode mode{MacroMode::Attack};
    std::uint32_t target_slot{4};
};

struct BattleMacroItemRow {
    std::uint32_t row_index{0};
    std::uint16_t item_id{0};
    std::uint8_t count{0};
    std::string name;
};

struct BattleMacroPlanningContext {
    std::vector<std::uint32_t> alive_ally_slots;
    std::vector<std::uint32_t> alive_enemy_slots;
    std::array<int, soa::battle::ctx::SLOT_COUNT> enemy_slot_to_selectable_index{};
    std::vector<BattleMacroItemRow> usable_items;

    bool IsAliveEnemySlot(std::uint32_t slot) const;
    int EnemySelectableIndex(std::uint32_t slot) const;
};

struct EncodeSpec {
    MacroMode mode{MacroMode::Attack};
    std::uint32_t target_slot{4};
    std::vector<MacroCommand> commands;
    std::uint32_t transition_neutral_frames{3};
    std::uint32_t step_timeout_ms{5000};
    std::uint32_t vi_stall_ms{5000};
};

const char* MacroModeName(MacroMode mode);
const char* FailureCodeName(FailureCode code);
bool TryParseMacroMode(std::string_view value, MacroMode* out);
std::string FormatCommandPlanSpec(const std::vector<MacroCommand>& commands);
BattleMacroPlanningContext BuildPlanningContext(const soa::battle::ctx::BattleContext& battle_context);
std::string FormatPlanningContext(const BattleMacroPlanningContext& context);
bool ParseCommandPlanSpec(std::string_view spec, std::vector<MacroCommand>* out, std::string* error_out = nullptr);
std::string SerializeCommandPlan(const std::vector<MacroCommand>& commands);
bool DeserializeCommandPlan(std::string_view blob, std::vector<MacroCommand>* out, std::string* error_out = nullptr);
std::vector<MacroStep> BuildMacroSteps(MacroMode mode, std::uint32_t target_slot, FailureCode* failure_out);
std::vector<MacroStep> BuildMacroSteps(
    MacroMode mode,
    std::uint32_t target_slot,
    const BattleMacroPlanningContext* planning_context,
    FailureCode* failure_out);
std::vector<MacroStep> BuildMacroPlanSteps(
    const std::vector<MacroCommand>& commands,
    std::uint32_t transition_neutral_frames,
    FailureCode* failure_out);
std::vector<MacroStep> BuildMacroPlanSteps(
    const std::vector<MacroCommand>& commands,
    std::uint32_t transition_neutral_frames,
    const BattleMacroPlanningContext* planning_context,
    FailureCode* failure_out);

bool encode_payload(const EncodeSpec& spec, std::vector<std::uint8_t>& out);
bool decode_payload(const std::vector<std::uint8_t>& in, savor::PSContext& out_ctx);

} // namespace phase::battle::macroprobe
