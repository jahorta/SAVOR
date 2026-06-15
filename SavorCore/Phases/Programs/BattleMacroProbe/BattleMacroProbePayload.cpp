#include "BattleMacroProbePayload.h"

#include "../../../Runner/IPC/Wire.h"
#include "../../../Runner/Script/CtxRegistry.h"
#include "../../../Runner/Script/ScriptProgress.h"
#include "../../../Core/Memory/Soa/SoaAddrRegistry.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <iterator>
#include <sstream>

namespace phase::battle::macroprobe {
namespace {

void put_u32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v >> 16));
    b.push_back(static_cast<std::uint8_t>(v >> 24));
}

bool get_u32(const std::uint8_t*& p, const std::uint8_t* e, std::uint32_t& v) {
    if (p + 4 > e) return false;
    v = static_cast<std::uint32_t>(p[0])
        | (static_cast<std::uint32_t>(p[1]) << 8)
        | (static_cast<std::uint32_t>(p[2]) << 16)
        | (static_cast<std::uint32_t>(p[3]) << 24);
    p += 4;
    return true;
}

savor::GCInputFrame PressA() {
    savor::GCInputFrame f{};
    f.A();
    return f;
}

savor::GCInputFrame PressB() {
    savor::GCInputFrame f{};
    f.B();
    return f;
}

savor::GCInputFrame PressUp() {
    savor::GCInputFrame f{};
    f.DUp();
    return f;
}

savor::GCInputFrame PressDown() {
    savor::GCInputFrame f{};
    f.DDown();
    return f;
}

std::vector<BPKey> MainMenuMoveHigherBps() {
    return {bp::battle::BattleMacroMainMenuMoveHigher, bp::battle::BattleMacroMainMenuMoveHigherAlt};
}

std::vector<BPKey> MainMenuMoveLowerBps() {
    return {bp::battle::BattleMacroMainMenuMoveLower, bp::battle::BattleMacroMainMenuMoveLowerAlt};
}

void AddDirectCommandAcceptSteps(std::vector<MacroStep>& steps) {
    steps.push_back(MacroStep{
        .label = "direct_command_queued",
        .input = PressA(),
        .expected_bps = {bp::battle::BattleMacroDirectCommandQueued},
    });
}

void AddMainMenuMoveSteps(
    std::vector<MacroStep>& steps,
    const char* move_label,
    savor::GCInputFrame input,
    std::vector<BPKey> move_bps) {
    steps.push_back(MacroStep{
        .label = move_label,
        .input = input,
        .expected_bps = std::move(move_bps),
    });
    steps.push_back(MacroStep{
        .label = "main_menu_command_transition_done",
        .input = savor::GCInputFrame{},
        .expected_bps = {bp::battle::BattleMacroCommandTransitionDone},
    });
}

void AddInputReadyGateStep(std::vector<MacroStep>& steps, const char* label) {
    steps.push_back(MacroStep{
        .label = label,
        .input = savor::GCInputFrame{},
        .expected_bps = {bp::battle::BattleMacroInputReadyGate},
    });
}

void AddEnemyTargetReadyStep(std::vector<MacroStep>& steps, const char* label) {
    steps.push_back(MacroStep{
        .label = label,
        .input = savor::GCInputFrame{},
        .expected_bps = {bp::battle::BattleMacroEnemyTargetReady},
    });
}

void AddNeutralFrames(std::vector<MacroStep>& steps, const char* label, std::uint32_t frame_count) {
    if (frame_count == 0) {
        return;
    }
    steps.push_back(MacroStep{
        .label = label,
        .kind = MacroStep::Kind::NeutralFrames,
        .input = savor::GCInputFrame{},
        .frame_count = frame_count,
    });
}

std::string TrimAscii(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

std::string LowerAscii(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool ParseTargetSlot(std::string_view value, std::uint32_t* out) {
    const std::string trimmed = TrimAscii(value);
    if (trimmed.empty()) return false;
    std::uint32_t parsed = 0;
    const char* first = trimmed.data();
    const char* last = trimmed.data() + trimmed.size();
    const auto result = std::from_chars(first, last, parsed);
    if (result.ec != std::errc{} || result.ptr != last) return false;
    if (parsed < 4 || parsed > 11) return false;
    if (out) *out = parsed;
    return true;
}

std::string ItemName(std::uint16_t item_id) {
    if (item_id < soa::text::ItemNames.size()) {
        return std::string(soa::text::get_item_name(item_id));
    }
    return "item:" + std::to_string(item_id);
}

void AddAttackSteps(std::vector<MacroStep>& steps, std::uint32_t target_move_count) {
    steps.push_back(MacroStep{
        .label = "attack_accept_main_menu",
        .input = PressA(),
        .expected_bps = {bp::battle::BattleMacroMainMenuAcceptDispatch},
    });
    AddEnemyTargetReadyStep(steps, "attack_enemy_target_ready");
    AddEnemyTargetReadyStep(steps, "attack_enemy_target_ready_confirm");
    for (std::uint32_t i = 0; i < target_move_count; ++i) {
        steps.push_back(MacroStep{
            .label = "attack_target_cursor_down",
            .input = PressDown(),
            .expected_bps = {bp::battle::BattleMacroEnemyTargetMoveDownAccepted},
        });
        if (i + 1 < target_move_count) {
            AddEnemyTargetReadyStep(steps, "attack_enemy_target_ready_between_moves");
        }
    }
    steps.push_back(MacroStep{
        .label = "attack_target_accept",
        .input = PressA(),
        .hold_input_through_hit_opcode = true,
        .expected_bps = {bp::battle::BattleMacroEnemyTargetFinalized},
    });
}

void AddFakeAttackMemoryGate(
    std::vector<MacroStep>& steps,
    const char* label,
    std::uint32_t cycle_index,
    std::uint32_t rng_addr,
    std::uint32_t timeout_ms) {
    steps.push_back(MacroStep{
        .label = label,
        .kind = MacroStep::Kind::WaitMemoryU32Changed,
        .memory_addr = rng_addr,
        .memory_timeout_ms = timeout_ms,
        .memory_cycle_index = cycle_index,
    });
}

void AddFakeAttackCycleSteps(
    std::vector<MacroStep>& steps,
    std::uint32_t cycle_index,
    const FakeAttackPattern& pattern) {
    const auto rng_addr = addr::AddrRegistry::base(addr::core::RNG_SEED);
    steps.push_back(MacroStep{
        .label = "fake_attack_rng_capture",
        .kind = MacroStep::Kind::CaptureMemoryU32,
        .memory_addr = rng_addr,
        .memory_cycle_index = cycle_index,
    });
    steps.push_back(MacroStep{
        .label = "fake_attack_accept_attack",
        .input = PressA(),
        .expected_bps = {bp::battle::BattleMacroMainMenuAcceptDispatch},
    });
    AddEnemyTargetReadyStep(steps, "fake_attack_enemy_target_ready");
    if (pattern.memory_gate_mode == FakeAttackMemoryGateMode::TargetSide
        || pattern.memory_gate_mode == FakeAttackMemoryGateMode::Both) {
        AddFakeAttackMemoryGate(
            steps,
            "fake_attack_rng_changed_target",
            cycle_index,
            rng_addr,
            pattern.memory_timeout_ms);
    }
    AddNeutralFrames(steps, "fake_attack_target_neutral_before_b", pattern.target_neutral_before_b_frames);
    steps.push_back(MacroStep{
        .label = "fake_attack_back_to_input_ready",
        .input = PressB(),
        .expected_bps = {bp::battle::BattleMacroInputReadyGate},
    });
    AddNeutralFrames(steps, "fake_attack_input_neutral_after_b", pattern.input_neutral_after_b_frames);
    if (pattern.memory_gate_mode == FakeAttackMemoryGateMode::InputSide
        || pattern.memory_gate_mode == FakeAttackMemoryGateMode::Both) {
        AddFakeAttackMemoryGate(
            steps,
            "fake_attack_rng_changed_input",
            cycle_index,
            rng_addr,
            pattern.memory_timeout_ms);
    }
}

} // namespace

const char* MacroModeName(MacroMode mode) {
    switch (mode) {
    case MacroMode::Attack: return "attack";
    case MacroMode::Focus: return "focus";
    case MacroMode::Block: return "block";
    default: return "unknown";
    }
}

const char* FailureCodeName(FailureCode code) {
    switch (code) {
    case FailureCode::Ok: return "ok";
    case FailureCode::InvalidMode: return "invalid_mode";
    case FailureCode::InvalidTarget: return "invalid_target";
    case FailureCode::NoSteps: return "no_steps";
    case FailureCode::Timeout: return "timeout";
    case FailureCode::UnexpectedBreakpoint: return "unexpected_breakpoint";
    case FailureCode::BattleContextUnavailable: return "battle_context_unavailable";
    case FailureCode::MemoryReadFailed: return "memory_read_failed";
    default: return "unknown";
    }
}

bool TryParseMacroMode(std::string_view value, MacroMode* out) {
    if (value == "attack") {
        if (out) *out = MacroMode::Attack;
        return true;
    }
    if (value == "focus") {
        if (out) *out = MacroMode::Focus;
        return true;
    }
    if (value == "block" || value == "defend") {
        if (out) *out = MacroMode::Block;
        return true;
    }
    return false;
}

bool BattleMacroPlanningContext::IsAliveEnemySlot(std::uint32_t slot) const {
    return EnemySelectableIndex(slot) >= 0;
}

int BattleMacroPlanningContext::EnemySelectableIndex(std::uint32_t slot) const {
    if (slot >= enemy_slot_to_selectable_index.size()) return -1;
    return enemy_slot_to_selectable_index[slot];
}

BattleMacroPlanningContext BuildPlanningContext(const soa::battle::ctx::BattleContext& battle_context) {
    BattleMacroPlanningContext context{};
    context.enemy_slot_to_selectable_index.fill(-1);

    for (std::uint32_t slot = 0; slot < soa::battle::ctx::SLOT_COUNT; ++slot) {
        const auto& battle_slot = battle_context.slots_[slot];
        if (!battle_slot.present || !battle_slot.is_alive) continue;
        if (slot < 4 && battle_slot.is_player) {
            context.alive_ally_slots.push_back(slot);
        } else if (slot >= 4 && !battle_slot.is_player) {
            context.enemy_slot_to_selectable_index[slot] = static_cast<int>(context.alive_enemy_slots.size());
            context.alive_enemy_slots.push_back(slot);
        }
    }

    for (std::uint32_t row = 0; row < 80; ++row) {
        const auto& item = battle_context.state.useable_items[row];
        if (item.count == 0) continue;
        context.usable_items.push_back(BattleMacroItemRow{
            .row_index = row,
            .item_id = item.item_id,
            .count = item.count,
            .name = ItemName(item.item_id),
        });
    }

    return context;
}

std::string FormatPlanningContext(const BattleMacroPlanningContext& context) {
    std::ostringstream out;
    out << "alive_allies=";
    for (size_t i = 0; i < context.alive_ally_slots.size(); ++i) {
        if (i > 0) out << ',';
        out << context.alive_ally_slots[i];
    }
    out << " alive_enemies=";
    for (size_t i = 0; i < context.alive_enemy_slots.size(); ++i) {
        if (i > 0) out << ',';
        out << context.alive_enemy_slots[i];
    }
    out << " usable_items=";
    for (size_t i = 0; i < context.usable_items.size(); ++i) {
        const auto& item = context.usable_items[i];
        if (i > 0) out << '|';
        out << "row:" << item.row_index
            << ",id:" << item.item_id
            << ",count:" << static_cast<std::uint32_t>(item.count)
            << ",name:" << item.name;
    }
    return out.str();
}

std::string FormatCommandPlanSpec(const std::vector<MacroCommand>& commands) {
    std::ostringstream out;
    for (size_t i = 0; i < commands.size(); ++i) {
        if (i > 0) out << ',';
        out << MacroModeName(commands[i].mode);
        if (commands[i].mode == MacroMode::Attack) {
            out << ':' << commands[i].target_slot;
        }
    }
    return out.str();
}

bool ParseCommandPlanSpec(std::string_view spec, std::vector<MacroCommand>* out, std::string* error_out) {
    std::vector<MacroCommand> commands;
    size_t start = 0;
    while (start <= spec.size()) {
        const size_t comma = spec.find(',', start);
        const size_t end = comma == std::string_view::npos ? spec.size() : comma;
        std::string token = TrimAscii(spec.substr(start, end - start));
        if (token.empty()) {
            if (error_out) *error_out = "empty command in --battle-plan";
            return false;
        }

        std::string mode_text = token;
        std::string target_text;
        const size_t colon = token.find(':');
        if (colon != std::string::npos) {
            mode_text = token.substr(0, colon);
            target_text = token.substr(colon + 1);
        }
        mode_text = LowerAscii(TrimAscii(mode_text));

        MacroMode mode = MacroMode::Attack;
        if (!TryParseMacroMode(mode_text, &mode)) {
            if (error_out) *error_out = "unknown command in --battle-plan: " + mode_text;
            return false;
        }

        MacroCommand command{.mode = mode, .target_slot = 4};
        if (!target_text.empty()) {
            if (mode != MacroMode::Attack) {
                if (error_out) *error_out = "only attack accepts a target slot in --battle-plan";
                return false;
            }
            if (!ParseTargetSlot(target_text, &command.target_slot)) {
                if (error_out) *error_out = "attack target slot must be between 4 and 11 in --battle-plan";
                return false;
            }
        } else if (colon != std::string::npos) {
            if (error_out) *error_out = "missing attack target slot in --battle-plan";
            return false;
        }

        commands.push_back(command);
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }

    if (commands.empty()) {
        if (error_out) *error_out = "--battle-plan must contain at least one command";
        return false;
    }

    if (out) *out = std::move(commands);
    return true;
}

std::string SerializeCommandPlan(const std::vector<MacroCommand>& commands) {
    return FormatCommandPlanSpec(commands);
}

bool DeserializeCommandPlan(std::string_view blob, std::vector<MacroCommand>* out, std::string* error_out) {
    return ParseCommandPlanSpec(blob, out, error_out);
}

std::vector<MacroStep> BuildMacroSteps(MacroMode mode, std::uint32_t target_slot, FailureCode* failure_out) {
    return BuildMacroSteps(mode, target_slot, nullptr, failure_out);
}

std::vector<MacroStep> BuildMacroSteps(
    MacroMode mode,
    std::uint32_t target_slot,
    const BattleMacroPlanningContext* planning_context,
    FailureCode* failure_out) {
    if (failure_out) *failure_out = FailureCode::Ok;
    std::vector<MacroStep> steps;
    switch (mode) {
    case MacroMode::Attack: {
        if (target_slot < 4 || target_slot > 11) {
            if (failure_out) *failure_out = FailureCode::InvalidTarget;
            return {};
        }
        std::uint32_t target_move_count = target_slot - 4;
        if (planning_context != nullptr) {
            const int selectable_index = planning_context->EnemySelectableIndex(target_slot);
            if (selectable_index < 0) {
                if (failure_out) *failure_out = FailureCode::InvalidTarget;
                return {};
            }
            target_move_count = static_cast<std::uint32_t>(selectable_index);
        }
        AddAttackSteps(steps, target_move_count);
        return steps;
    }
    case MacroMode::Focus:
        for (std::uint32_t i = 0; i < 3; ++i) {
            AddMainMenuMoveSteps(steps, "focus_menu_down", PressDown(), MainMenuMoveLowerBps());
        }
        AddDirectCommandAcceptSteps(steps);
        return steps;
    case MacroMode::Block:
        AddMainMenuMoveSteps(steps, "block_menu_up", PressUp(), MainMenuMoveHigherBps());
        AddDirectCommandAcceptSteps(steps);
        return steps;
    default:
        if (failure_out) *failure_out = FailureCode::InvalidMode;
        return {};
    }
}

std::vector<MacroStep> BuildMacroPlanSteps(
    const std::vector<MacroCommand>& commands,
    std::uint32_t transition_neutral_frames,
    FailureCode* failure_out) {
    return BuildMacroPlanSteps(commands, transition_neutral_frames, nullptr, failure_out);
}

std::vector<MacroStep> BuildMacroPlanSteps(
    const std::vector<MacroCommand>& commands,
    std::uint32_t transition_neutral_frames,
    const BattleMacroPlanningContext* planning_context,
    FailureCode* failure_out) {
    if (failure_out) *failure_out = FailureCode::Ok;
    if (commands.empty()) {
        if (failure_out) *failure_out = FailureCode::NoSteps;
        return {};
    }

    std::vector<MacroStep> plan_steps;
    for (size_t i = 0; i < commands.size(); ++i) {
        FailureCode command_failure = FailureCode::Ok;
        auto command_steps = BuildMacroSteps(commands[i].mode, commands[i].target_slot, planning_context, &command_failure);
        if (command_failure != FailureCode::Ok || command_steps.empty()) {
            if (failure_out) *failure_out = command_failure;
            return {};
        }
        plan_steps.insert(
            plan_steps.end(),
            std::make_move_iterator(command_steps.begin()),
            std::make_move_iterator(command_steps.end()));
        if (i + 1 < commands.size()) {
            AddInputReadyGateStep(plan_steps, "character_transition_input_ready");
            if (transition_neutral_frames > 0) {
                plan_steps.push_back(MacroStep{
                    .label = "character_transition_neutral",
                    .kind = MacroStep::Kind::NeutralFrames,
                    .input = savor::GCInputFrame{},
                    .frame_count = transition_neutral_frames,
                });
            }
        }
    }
    return plan_steps;
}

std::vector<MacroStep> BuildMacroProbePlanSteps(
    const std::vector<MacroCommand>& commands,
    std::uint32_t transition_neutral_frames,
    std::uint32_t fake_attack_count,
    const BattleMacroPlanningContext* planning_context,
    FailureCode* failure_out) {
    return BuildMacroProbePlanSteps(
        commands,
        transition_neutral_frames,
        fake_attack_count,
        FakeAttackPattern{},
        planning_context,
        failure_out);
}

std::vector<MacroStep> BuildMacroProbePlanSteps(
    const std::vector<MacroCommand>& commands,
    std::uint32_t transition_neutral_frames,
    std::uint32_t fake_attack_count,
    const FakeAttackPattern& fake_attack_pattern,
    const BattleMacroPlanningContext* planning_context,
    FailureCode* failure_out) {
    return BuildMacroProbePlanSteps(
        commands,
        transition_neutral_frames,
        fake_attack_count,
        fake_attack_pattern,
        fake_attack_pattern,
        planning_context,
        failure_out);
}

std::vector<MacroStep> BuildMacroProbePlanSteps(
    const std::vector<MacroCommand>& commands,
    std::uint32_t transition_neutral_frames,
    std::uint32_t fake_attack_count,
    const FakeAttackPattern& first_fake_attack_pattern,
    const FakeAttackPattern& repeat_fake_attack_pattern,
    const BattleMacroPlanningContext* planning_context,
    FailureCode* failure_out) {
    return BuildMacroProbePlanSteps(
        commands,
        transition_neutral_frames,
        fake_attack_count,
        first_fake_attack_pattern,
        repeat_fake_attack_pattern,
        repeat_fake_attack_pattern,
        planning_context,
        failure_out);
}

std::vector<MacroStep> BuildMacroProbePlanSteps(
    const std::vector<MacroCommand>& commands,
    std::uint32_t transition_neutral_frames,
    std::uint32_t fake_attack_count,
    const FakeAttackPattern& first_fake_attack_pattern,
    const FakeAttackPattern& repeat_fake_attack_pattern,
    const FakeAttackPattern& final_fake_attack_pattern,
    const BattleMacroPlanningContext* planning_context,
    FailureCode* failure_out) {
    if (failure_out) *failure_out = FailureCode::Ok;
    std::vector<MacroStep> steps;
    for (std::uint32_t i = 0; i < fake_attack_count; ++i) {
        const bool is_final_fake_attack = i > 0 && i + 1u == fake_attack_count;
        AddFakeAttackCycleSteps(
            steps,
            i,
            is_final_fake_attack
                ? final_fake_attack_pattern
                : (i == 0 ? first_fake_attack_pattern : repeat_fake_attack_pattern));
    }

    FailureCode command_failure = FailureCode::Ok;
    auto command_steps = BuildMacroPlanSteps(
        commands,
        transition_neutral_frames,
        planning_context,
        &command_failure);
    if (command_failure != FailureCode::Ok || command_steps.empty()) {
        if (failure_out) *failure_out = command_failure;
        return {};
    }
    steps.insert(steps.end(), command_steps.begin(), command_steps.end());
    return steps;
}

std::vector<MacroStep> BuildMacroPlanStepsFromTurnPlan(
    const soa::battle::actions::TurnPlan& turn_plan,
    std::uint32_t transition_neutral_frames,
    const BattleMacroPlanningContext* planning_context,
    soa::battle::actions::MaterializeErr* materialize_err_out) {
    using soa::battle::actions::BattleAction;
    using soa::battle::actions::MaterializeErr;

    if (materialize_err_out) *materialize_err_out = MaterializeErr::OK;
    if (turn_plan.spec.empty()) {
        if (materialize_err_out) *materialize_err_out = MaterializeErr::BadBlob;
        return {};
    }

    std::vector<MacroCommand> commands;
    commands.reserve(turn_plan.spec.size());
    for (const auto& action : turn_plan.spec) {
        switch (action.macro) {
        case BattleAction::Attack:
            if (action.params.target_slot < 4 || action.params.target_slot > 11) {
                if (materialize_err_out) *materialize_err_out = MaterializeErr::NoValidTarget;
                return {};
            }
            commands.push_back(MacroCommand{
                .mode = MacroMode::Attack,
                .target_slot = action.params.target_slot,
            });
            break;
        case BattleAction::Defend:
            commands.push_back(MacroCommand{.mode = MacroMode::Block, .target_slot = 4});
            break;
        case BattleAction::Focus:
            commands.push_back(MacroCommand{.mode = MacroMode::Focus, .target_slot = 4});
            break;
        case BattleAction::FakeAttack:
        case BattleAction::UseItem:
        default:
            if (materialize_err_out) *materialize_err_out = MaterializeErr::InvalidNavigation;
            return {};
        }
    }

    FailureCode failure = FailureCode::Ok;
    auto command_steps = BuildMacroPlanSteps(
        commands,
        transition_neutral_frames,
        planning_context,
        &failure);
    if (failure != FailureCode::Ok || command_steps.empty()) {
        if (materialize_err_out) {
            *materialize_err_out = failure == FailureCode::InvalidTarget
                ? MaterializeErr::NoValidTarget
                : MaterializeErr::InvalidNavigation;
        }
        return {};
    }

    std::vector<MacroStep> steps;
    const FakeAttackPattern first_fake_attack_pattern{
        .memory_gate_mode = FakeAttackMemoryGateMode::TargetSide,
        .target_neutral_before_b_frames = 0,
        .input_neutral_after_b_frames = 0,
        .memory_timeout_ms = 1000,
    };
    const FakeAttackPattern repeat_fake_attack_pattern{
        .memory_gate_mode = FakeAttackMemoryGateMode::TargetSide,
        .target_neutral_before_b_frames = 7,
        .input_neutral_after_b_frames = 0,
        .memory_timeout_ms = 1000,
    };
    for (std::uint32_t i = 0; i < turn_plan.fake_attack_count; ++i) {
        AddFakeAttackCycleSteps(
            steps,
            i,
            i == 0 ? first_fake_attack_pattern : repeat_fake_attack_pattern);
    }
    steps.insert(steps.end(), command_steps.begin(), command_steps.end());
    return steps;
}

bool encode_payload(const EncodeSpec& spec, std::vector<std::uint8_t>& out) {
    out.clear();
    std::vector<MacroCommand> commands = spec.commands;
    if (commands.empty()) {
        commands.push_back(MacroCommand{.mode = spec.mode, .target_slot = spec.target_slot});
    }

    out.push_back(savor::PK_BattleMacroProbe);
    put_u32(out, PayloadVersion);
    put_u32(out, static_cast<std::uint32_t>(commands.size()));
    for (const auto& command : commands) {
        put_u32(out, static_cast<std::uint32_t>(command.mode));
        put_u32(out, command.target_slot);
    }
    put_u32(out, spec.transition_neutral_frames);
    put_u32(out, spec.step_timeout_ms);
    put_u32(out, spec.vi_stall_ms);
    put_u32(out, spec.observation_tail_ms);
    put_u32(out, spec.fake_attack_count);
    put_u32(out, static_cast<std::uint32_t>(spec.fake_attack_pattern.memory_gate_mode));
    put_u32(out, spec.fake_attack_pattern.target_neutral_before_b_frames);
    put_u32(out, spec.fake_attack_pattern.input_neutral_after_b_frames);
    put_u32(out, spec.fake_attack_pattern.memory_timeout_ms);
    put_u32(out, spec.use_mixed_fake_attack_patterns ? 1u : 0u);
    put_u32(out, static_cast<std::uint32_t>(spec.first_fake_attack_pattern.memory_gate_mode));
    put_u32(out, spec.first_fake_attack_pattern.target_neutral_before_b_frames);
    put_u32(out, spec.first_fake_attack_pattern.input_neutral_after_b_frames);
    put_u32(out, spec.first_fake_attack_pattern.memory_timeout_ms);
    put_u32(out, static_cast<std::uint32_t>(spec.repeat_fake_attack_pattern.memory_gate_mode));
    put_u32(out, spec.repeat_fake_attack_pattern.target_neutral_before_b_frames);
    put_u32(out, spec.repeat_fake_attack_pattern.input_neutral_after_b_frames);
    put_u32(out, spec.repeat_fake_attack_pattern.memory_timeout_ms);
    put_u32(out, spec.use_final_fake_attack_pattern ? 1u : 0u);
    put_u32(out, static_cast<std::uint32_t>(spec.final_fake_attack_pattern.memory_gate_mode));
    put_u32(out, spec.final_fake_attack_pattern.target_neutral_before_b_frames);
    put_u32(out, spec.final_fake_attack_pattern.input_neutral_after_b_frames);
    put_u32(out, spec.final_fake_attack_pattern.memory_timeout_ms);
    return true;
}

bool decode_payload(const std::vector<std::uint8_t>& in, savor::PSContext& out_ctx) {
    if (in.size() < 1 + 4 * 6) return false;

    const std::uint8_t* p = in.data();
    const std::uint8_t* e = p + in.size();
    const std::uint8_t tag = *p++;
    if (tag != savor::PK_BattleMacroProbe) return false;

    std::uint32_t version = 0;
    std::uint32_t command_count = 0;
    std::uint32_t transition_neutral_frames = 0;
    std::uint32_t step_timeout_ms = 0;
    std::uint32_t vi_stall_ms = 0;
    std::uint32_t observation_tail_ms = 0;
    std::uint32_t fake_attack_count = 0;
    FakeAttackPattern fake_attack_pattern{};
    bool use_mixed_fake_attack_patterns = false;
    FakeAttackPattern first_fake_attack_pattern{};
    FakeAttackPattern repeat_fake_attack_pattern{};
    bool use_final_fake_attack_pattern = false;
    FakeAttackPattern final_fake_attack_pattern{};
    const auto valid_gate_mode = [](FakeAttackMemoryGateMode mode) {
        return mode == FakeAttackMemoryGateMode::TargetSide
            || mode == FakeAttackMemoryGateMode::InputSide
            || mode == FakeAttackMemoryGateMode::Both;
    };
    const auto get_pattern = [&](FakeAttackPattern* pattern) {
        std::uint32_t raw_gate_mode = 0;
        if (!get_u32(p, e, raw_gate_mode)) return false;
        if (!get_u32(p, e, pattern->target_neutral_before_b_frames)) return false;
        if (!get_u32(p, e, pattern->input_neutral_after_b_frames)) return false;
        if (!get_u32(p, e, pattern->memory_timeout_ms)) return false;
        pattern->memory_gate_mode = static_cast<FakeAttackMemoryGateMode>(raw_gate_mode);
        return valid_gate_mode(pattern->memory_gate_mode);
    };
    if (!get_u32(p, e, version) || (version != PayloadVersion && version != 6 && version != 5 && version != 4 && version != 3)) return false;
    if (!get_u32(p, e, command_count)) return false;
    if (command_count == 0 || command_count > 16) return false;
    std::vector<MacroCommand> commands;
    commands.reserve(command_count);
    for (std::uint32_t i = 0; i < command_count; ++i) {
        std::uint32_t mode = 0;
        std::uint32_t target_slot = 0;
        if (!get_u32(p, e, mode)) return false;
        if (!get_u32(p, e, target_slot)) return false;
        MacroMode parsed_mode = static_cast<MacroMode>(mode);
        if (parsed_mode != MacroMode::Attack && parsed_mode != MacroMode::Focus && parsed_mode != MacroMode::Block) {
            return false;
        }
        if (target_slot < 4 || target_slot > 11) return false;
        commands.push_back(MacroCommand{.mode = parsed_mode, .target_slot = target_slot});
    }
    if (!get_u32(p, e, transition_neutral_frames)) return false;
    if (!get_u32(p, e, step_timeout_ms)) return false;
    if (!get_u32(p, e, vi_stall_ms)) return false;
    if (!get_u32(p, e, observation_tail_ms)) return false;
    if (version >= 4 && !get_u32(p, e, fake_attack_count)) return false;
    if (version >= 5) {
        if (!get_pattern(&fake_attack_pattern)) return false;
    }
    first_fake_attack_pattern = fake_attack_pattern;
    repeat_fake_attack_pattern = fake_attack_pattern;
    final_fake_attack_pattern = fake_attack_pattern;
    if (version >= 6) {
        std::uint32_t raw_use_mixed = 0;
        if (!get_u32(p, e, raw_use_mixed)) return false;
        use_mixed_fake_attack_patterns = raw_use_mixed != 0;
        if (!get_pattern(&first_fake_attack_pattern)) return false;
        if (!get_pattern(&repeat_fake_attack_pattern)) return false;
        final_fake_attack_pattern = repeat_fake_attack_pattern;
    }
    if (version >= 7) {
        std::uint32_t raw_use_final = 0;
        if (!get_u32(p, e, raw_use_final)) return false;
        use_final_fake_attack_pattern = raw_use_final != 0;
        if (!get_pattern(&final_fake_attack_pattern)) return false;
    }
    if (p != e) return false;

    out_ctx[savor::context::key::battle::MACRO_MODE] = static_cast<std::uint32_t>(commands.front().mode);
    out_ctx[savor::context::key::battle::MACRO_TARGET_SLOT] = commands.front().target_slot;
    out_ctx[savor::context::key::battle::MACRO_PLAN_BLOB] = SerializeCommandPlan(commands);
    out_ctx[savor::context::key::battle::MACRO_TRANSITION_NEUTRAL_FRAMES] = transition_neutral_frames;
    out_ctx[savor::context::key::battle::MACRO_OBSERVATION_TAIL_MS] = observation_tail_ms;
    out_ctx[savor::context::key::battle::FAKE_ATTACK_COUNT_THIS_TURN] = fake_attack_count;
    out_ctx[savor::context::key::battle::MACRO_FAKE_MEMORY_GATE_MODE] =
        static_cast<std::uint32_t>(fake_attack_pattern.memory_gate_mode);
    out_ctx[savor::context::key::battle::MACRO_FAKE_TARGET_NEUTRAL_FRAMES] =
        fake_attack_pattern.target_neutral_before_b_frames;
    out_ctx[savor::context::key::battle::MACRO_FAKE_INPUT_NEUTRAL_FRAMES] =
        fake_attack_pattern.input_neutral_after_b_frames;
    out_ctx[savor::context::key::battle::MACRO_FAKE_MEMORY_TIMEOUT_MS] =
        fake_attack_pattern.memory_timeout_ms;
    out_ctx[savor::context::key::battle::MACRO_FAKE_USE_MIXED_PATTERNS] =
        use_mixed_fake_attack_patterns ? 1u : 0u;
    out_ctx[savor::context::key::battle::MACRO_FAKE_FIRST_MEMORY_GATE_MODE] =
        static_cast<std::uint32_t>(first_fake_attack_pattern.memory_gate_mode);
    out_ctx[savor::context::key::battle::MACRO_FAKE_FIRST_TARGET_NEUTRAL_FRAMES] =
        first_fake_attack_pattern.target_neutral_before_b_frames;
    out_ctx[savor::context::key::battle::MACRO_FAKE_FIRST_INPUT_NEUTRAL_FRAMES] =
        first_fake_attack_pattern.input_neutral_after_b_frames;
    out_ctx[savor::context::key::battle::MACRO_FAKE_FIRST_MEMORY_TIMEOUT_MS] =
        first_fake_attack_pattern.memory_timeout_ms;
    out_ctx[savor::context::key::battle::MACRO_FAKE_USE_FINAL_PATTERN] =
        use_final_fake_attack_pattern ? 1u : 0u;
    out_ctx[savor::context::key::battle::MACRO_FAKE_FINAL_MEMORY_GATE_MODE] =
        static_cast<std::uint32_t>(final_fake_attack_pattern.memory_gate_mode);
    out_ctx[savor::context::key::battle::MACRO_FAKE_FINAL_TARGET_NEUTRAL_FRAMES] =
        final_fake_attack_pattern.target_neutral_before_b_frames;
    out_ctx[savor::context::key::battle::MACRO_FAKE_FINAL_INPUT_NEUTRAL_FRAMES] =
        final_fake_attack_pattern.input_neutral_after_b_frames;
    out_ctx[savor::context::key::battle::MACRO_FAKE_FINAL_MEMORY_TIMEOUT_MS] =
        final_fake_attack_pattern.memory_timeout_ms;
    out_ctx[savor::context::key::core::RUN_MS] = step_timeout_ms;
    out_ctx[savor::context::key::core::VI_STALL_MS] = vi_stall_ms;
    out_ctx[savor::context::key::core::RUN_POLL_MS] = 10u;
    savor::progress::ProgressDeets progress{ .poll_rate = 5000 };
    progress.set_flag(CoreProgressFlags::BattleProgress);
    progress.set_flag(CoreProgressFlags::DontRecordHeartbeat);
    out_ctx[savor::context::key::core::PROGRESS_RATE] = progress.poll_rate;
    out_ctx[savor::context::key::core::PROGRESS_CORE_FLAGS] = progress.flags;
    out_ctx[savor::context::key::battle::MACRO_RESULT] = 1u;
    out_ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<std::uint32_t>(FailureCode::NoSteps);
    out_ctx[savor::context::key::battle::MACRO_STEP_COUNT] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_ADDR] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_BASELINE] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_LATEST] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_CHANGED] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_POLL_COUNT] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_ELAPSED_MS] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_GATE_COUNT] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_BASELINE] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_LATEST] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_CHANGED] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_POLL_COUNT] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_ELAPSED_MS] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_BASELINE] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_LATEST] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_CHANGED] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_POLL_COUNT] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_ELAPSED_MS] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_BASELINE] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_LATEST] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_CHANGED] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_POLL_COUNT] = 0u;
    out_ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_ELAPSED_MS] = 0u;
    return true;
}

} // namespace phase::battle::macroprobe
