#pragma once

#include <cstdint>
#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "../../../Core/Memory/Soa/Battle/BattleContext.h"
#include "../../../Core/Input/SoaBattle/ActionTypes.h"
#include "../../../Core/Input/SoaBattle/PlanWriter.h"
#include "../../../Runner/Breakpoints/BpRegistry.h"
#include "../../../Runner/InputMacro/InputMacroPlan.h"
#include "../../../Runner/Script/PhaseScriptProgram.h"

namespace phase::battle::macroprobe {

static constexpr int PayloadVersion = 7;

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
    MemoryReadFailed = 7,
    CaptureOnlyHitLimit = 8,
};

// Compatibility name retained while battle-macro compiler callers migrate to
// the context-free input-macro subsystem. This is intentionally an alias, not
// a second runtime representation.
using MacroStep = savor::inputmacro::InputMacroStep;

struct MacroCommand {
    MacroMode mode{MacroMode::Attack};
    std::uint32_t target_slot{4};
};

enum class FakeAttackMemoryGateMode : std::uint32_t {
    TargetSide = 1,
    InputSide = 2,
    Both = 3,
};

struct FakeAttackPattern {
    FakeAttackMemoryGateMode memory_gate_mode{FakeAttackMemoryGateMode::TargetSide};
    std::uint32_t target_neutral_before_b_frames{0};
    std::uint32_t input_neutral_after_b_frames{20};
    std::uint32_t memory_timeout_ms{1000};
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
    std::uint32_t observation_tail_ms{10000};
    std::uint32_t fake_attack_count{0};
    FakeAttackPattern fake_attack_pattern{};
    bool use_mixed_fake_attack_patterns{false};
    FakeAttackPattern first_fake_attack_pattern{};
    FakeAttackPattern repeat_fake_attack_pattern{};
    bool use_final_fake_attack_pattern{false};
    FakeAttackPattern final_fake_attack_pattern{};
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
std::vector<MacroStep> BuildMacroProbePlanSteps(
    const std::vector<MacroCommand>& commands,
    std::uint32_t transition_neutral_frames,
    std::uint32_t fake_attack_count,
    const BattleMacroPlanningContext* planning_context,
    FailureCode* failure_out);
std::vector<MacroStep> BuildMacroProbePlanSteps(
    const std::vector<MacroCommand>& commands,
    std::uint32_t transition_neutral_frames,
    std::uint32_t fake_attack_count,
    const FakeAttackPattern& fake_attack_pattern,
    const BattleMacroPlanningContext* planning_context,
    FailureCode* failure_out);
std::vector<MacroStep> BuildMacroProbePlanSteps(
    const std::vector<MacroCommand>& commands,
    std::uint32_t transition_neutral_frames,
    std::uint32_t fake_attack_count,
    const FakeAttackPattern& first_fake_attack_pattern,
    const FakeAttackPattern& repeat_fake_attack_pattern,
    const BattleMacroPlanningContext* planning_context,
    FailureCode* failure_out);
std::vector<MacroStep> BuildMacroProbePlanSteps(
    const std::vector<MacroCommand>& commands,
    std::uint32_t transition_neutral_frames,
    std::uint32_t fake_attack_count,
    const FakeAttackPattern& first_fake_attack_pattern,
    const FakeAttackPattern& repeat_fake_attack_pattern,
    const FakeAttackPattern& final_fake_attack_pattern,
    const BattleMacroPlanningContext* planning_context,
    FailureCode* failure_out);
std::vector<MacroStep> BuildMacroPlanStepsFromTurnPlan(
    const soa::battle::actions::TurnPlan& turn_plan,
    std::uint32_t transition_neutral_frames,
    const BattleMacroPlanningContext* planning_context,
    soa::battle::actions::MaterializeErr* materialize_err_out);

bool encode_payload(const EncodeSpec& spec, std::vector<std::uint8_t>& out);
bool decode_payload(const std::vector<std::uint8_t>& in, savor::PSContext& out_ctx);

} // namespace phase::battle::macroprobe
