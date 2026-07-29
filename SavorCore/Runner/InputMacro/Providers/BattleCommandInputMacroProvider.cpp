#include "BattleCommandInputMacroProvider.h"

#include "../../../Core/Memory/MemView.h"
#include "../../../Core/Memory/Soa/Battle/BattleContextCodec.h"
#include "../../../Core/Memory/Soa/SoaAddrRegistry.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

namespace phase::battle::macroprobe {
namespace {

savor::GCInputFrame PressA() {
    savor::GCInputFrame frame{};
    frame.A();
    return frame;
}

savor::GCInputFrame PressB() {
    savor::GCInputFrame frame{};
    frame.B();
    return frame;
}

savor::GCInputFrame PressUp() {
    savor::GCInputFrame frame{};
    frame.DUp();
    return frame;
}

savor::GCInputFrame PressDown() {
    savor::GCInputFrame frame{};
    frame.DDown();
    return frame;
}

MacroStep BreakpointStep(
    const char* label,
    savor::GCInputFrame input,
    std::vector<BPKey> expected_keys,
    bool hold_input_through_hit_opcode = false) {
    return MacroStep{
        .label = label,
        .action = savor::inputmacro::BreakpointWaitAction{
            .expected_keys = std::move(expected_keys),
            .input = input,
            .hold_input_through_hit_opcode = hold_input_through_hit_opcode,
        },
    };
}

MacroStep NeutralFramesStep(const char* label, std::uint32_t frame_count) {
    return MacroStep{
        .label = label,
        .action = savor::inputmacro::NeutralFramesAction{
            .frame_count = frame_count,
        },
    };
}

std::string FakeAttackBaselineId(std::uint32_t cycle_index) {
    return "battle.fake_attack.rng." + std::to_string(cycle_index);
}

MacroStep CaptureU32Step(
    const char* label,
    std::uint32_t cycle_index,
    std::uint32_t address) {
    return MacroStep{
        .label = label,
        .action = savor::inputmacro::CaptureU32BaselineAction{
            .baseline_id = FakeAttackBaselineId(cycle_index),
            .address = address,
        },
    };
}

MacroStep WaitU32ChangeStep(
    const char* label,
    std::uint32_t cycle_index,
    std::uint32_t address) {
    return MacroStep{
        .label = label,
        .action = savor::inputmacro::WaitU32ChangeAction{
            .baseline_id = FakeAttackBaselineId(cycle_index),
            .address = address,
            .diagnostic_cycle_index = cycle_index,
        },
    };
}

std::vector<BPKey> MainMenuMoveHigherBps() {
    return {bp::battle::BattleMacroMainMenuMoveHigher};
}

std::vector<BPKey> MainMenuMoveLowerBps() {
    return {bp::battle::BattleMacroMainMenuMoveLower};
}

void AddDirectCommandAcceptSteps(std::vector<MacroStep>& steps) {
    steps.push_back(BreakpointStep(
        "direct_command_queued",
        PressA(),
        {bp::battle::BattleMacroDirectCommandQueued}));
}

void AddMainMenuMoveSteps(
    std::vector<MacroStep>& steps,
    const char* move_label,
    savor::GCInputFrame input,
    std::vector<BPKey> move_bps) {
    steps.push_back(BreakpointStep(move_label, input, std::move(move_bps)));
    steps.push_back(BreakpointStep(
        "main_menu_command_transition_done",
        savor::GCInputFrame{},
        {bp::battle::BattleMacroCommandTransitionDone}));
}

void AddInputReadyGateStep(std::vector<MacroStep>& steps, const char* label) {
    steps.push_back(BreakpointStep(
        label,
        savor::GCInputFrame{},
        {bp::battle::BattleMacroInputReadyGate}));
}

void AddEnemyTargetReadyStep(std::vector<MacroStep>& steps, const char* label) {
    steps.push_back(BreakpointStep(
        label,
        savor::GCInputFrame{},
        {bp::battle::BattleMacroEnemyTargetReady}));
}

void AddNeutralFrames(std::vector<MacroStep>& steps, const char* label, std::uint32_t frame_count) {
    if (frame_count != 0) {
        steps.push_back(NeutralFramesStep(label, frame_count));
    }
}

std::string ItemName(std::uint16_t item_id) {
    if (item_id < soa::text::ItemNames.size()) {
        return std::string(soa::text::get_item_name(item_id));
    }
    return "item:" + std::to_string(item_id);
}

void AddAttackSteps(std::vector<MacroStep>& steps, std::uint32_t target_move_count) {
    steps.push_back(BreakpointStep(
        "attack_accept_main_menu",
        PressA(),
        {bp::battle::BattleMacroMainMenuAcceptDispatch}));
    AddEnemyTargetReadyStep(steps, "attack_enemy_target_ready");
    AddEnemyTargetReadyStep(steps, "attack_enemy_target_ready_confirm");
    for (std::uint32_t i = 0; i < target_move_count; ++i) {
        steps.push_back(BreakpointStep(
            "attack_target_cursor_down",
            PressDown(),
            {bp::battle::BattleMacroEnemyTargetMoveDownAccepted}));
        if (i + 1 < target_move_count) {
            AddEnemyTargetReadyStep(steps, "attack_enemy_target_ready_between_moves");
        }
    }
    steps.push_back(BreakpointStep(
        "attack_target_accept",
        PressA(),
        {bp::battle::BattleMacroEnemyTargetFinalized},
        true));
}

void AddFakeAttackMemoryGate(
    std::vector<MacroStep>& steps,
    const char* label,
    std::uint32_t cycle_index,
    std::uint32_t rng_addr) {
    steps.push_back(WaitU32ChangeStep(label, cycle_index, rng_addr));
}

void AddFakeAttackCycleSteps(
    std::vector<MacroStep>& steps,
    std::uint32_t cycle_index,
    const FakeAttackPattern& pattern) {
    const auto rng_addr = addr::AddrRegistry::base(addr::core::RNG_SEED);
    steps.push_back(CaptureU32Step("fake_attack_rng_capture", cycle_index, rng_addr));
    steps.push_back(BreakpointStep(
        "fake_attack_accept_attack",
        PressA(),
        {bp::battle::BattleMacroMainMenuAcceptDispatch}));
    AddEnemyTargetReadyStep(steps, "fake_attack_enemy_target_ready");
    if (pattern.memory_gate_mode == FakeAttackMemoryGateMode::TargetSide
        || pattern.memory_gate_mode == FakeAttackMemoryGateMode::Both) {
        AddFakeAttackMemoryGate(
            steps,
            "fake_attack_rng_changed_target",
            cycle_index,
            rng_addr);
    }
    AddNeutralFrames(steps, "fake_attack_target_neutral_before_b", pattern.target_neutral_before_b_frames);
    steps.push_back(BreakpointStep(
        "fake_attack_back_to_input_ready",
        PressB(),
        {bp::battle::BattleMacroInputReadyGate}));
    AddNeutralFrames(steps, "fake_attack_input_neutral_after_b", pattern.input_neutral_after_b_frames);
    if (pattern.memory_gate_mode == FakeAttackMemoryGateMode::InputSide
        || pattern.memory_gate_mode == FakeAttackMemoryGateMode::Both) {
        AddFakeAttackMemoryGate(
            steps,
            "fake_attack_rng_changed_input",
            cycle_index,
            rng_addr);
    }
}

} // namespace

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
            context.enemy_slot_to_selectable_index[slot] =
                static_cast<int>(context.alive_enemy_slots.size());
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

std::vector<MacroStep> BuildMacroSteps(
    MacroMode mode,
    std::uint32_t target_slot,
    FailureCode* failure_out) {
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
        auto command_steps = BuildMacroSteps(
            commands[i].mode,
            commands[i].target_slot,
            planning_context,
            &command_failure);
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
            AddNeutralFrames(plan_steps, "character_transition_neutral", transition_neutral_frames);
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
    if (turn_plan.commands.empty()) {
        if (materialize_err_out) *materialize_err_out = MaterializeErr::BadBlob;
        return {};
    }

    std::vector<MacroCommand> commands;
    commands.reserve(turn_plan.commands.size());
    for (const auto& action : turn_plan.commands) {
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

    std::vector<MacroStep> steps;
    const FakeAttackPattern fast_fake_attack_pattern{
        .memory_gate_mode = FakeAttackMemoryGateMode::TargetSide,
        .target_neutral_before_b_frames = 7,
        .input_neutral_after_b_frames = 0,
    };
    const FakeAttackPattern slow_fake_attack_pattern{
        .memory_gate_mode = FakeAttackMemoryGateMode::TargetSide,
        .target_neutral_before_b_frames = 7,
        .input_neutral_after_b_frames = 0,
    };

    const std::uint32_t alive_player_count = planning_context
        ? static_cast<std::uint32_t>(planning_context->alive_ally_slots.size())
        : static_cast<std::uint32_t>(commands.size());
    const std::uint32_t fast_fake_capacity_by_players =
        alive_player_count > 0 ? alive_player_count - 1u : 0u;
    const std::uint32_t fast_fake_capacity = std::min<std::uint32_t>(
        fast_fake_capacity_by_players,
        static_cast<std::uint32_t>(commands.size()));
    (void)fast_fake_capacity;
    const std::uint32_t fast_fake_count = 0;
    const std::uint32_t slow_fake_count = turn_plan.fake_attack_count;

    std::uint32_t fake_cycle_index = 0;
    for (; fake_cycle_index < slow_fake_count; ++fake_cycle_index) {
        AddFakeAttackCycleSteps(steps, fake_cycle_index, slow_fake_attack_pattern);
    }

    for (size_t i = 0; i < commands.size(); ++i) {
        if (i < static_cast<size_t>(fast_fake_count)) {
            AddFakeAttackCycleSteps(steps, fake_cycle_index++, fast_fake_attack_pattern);
        }

        FailureCode command_failure = FailureCode::Ok;
        auto command_steps = BuildMacroSteps(
            commands[i].mode,
            commands[i].target_slot,
            planning_context,
            &command_failure);
        if (command_failure != FailureCode::Ok || command_steps.empty()) {
            if (materialize_err_out) {
                *materialize_err_out = command_failure == FailureCode::InvalidTarget
                    ? MaterializeErr::NoValidTarget
                    : MaterializeErr::InvalidNavigation;
            }
            return {};
        }
        steps.insert(
            steps.end(),
            std::make_move_iterator(command_steps.begin()),
            std::make_move_iterator(command_steps.end()));

        if (i + 1 < commands.size()) {
            AddInputReadyGateStep(steps, "character_transition_input_ready");
            AddNeutralFrames(steps, "character_transition_neutral", transition_neutral_frames);
        }
    }
    return steps;
}

} // namespace phase::battle::macroprobe

namespace savor::inputmacro {
namespace {

namespace Legacy = phase::battle::macroprobe;

constexpr std::array<BPKey, 18> kRequiredBreakpointKeys{
    bp::battle::BattleMacroInputReadyGate,
    bp::battle::BattleMacroMainMenuMoveHigher,
    bp::battle::BattleMacroMainMenuMoveLower,
    bp::battle::BattleMacroCommandTransitionDone,
    bp::battle::BattleMacroMainMenuAcceptDispatch,
    bp::battle::BattleMacroDirectCommandQueued,
    bp::battle::BattleMacroAttackTargetSelectorCreated,
    bp::battle::BattleMacroEnemyTargetMoveDownAccepted,
    bp::battle::BattleMacroEnemyTargetMoveUpAccepted,
    bp::battle::BattleMacroEnemyTargetFinalized,
    bp::battle::BattleMacroMagicReady,
    bp::battle::BattleMacroSMoveReady,
    bp::battle::BattleMacroConditionalRunReady,
    bp::battle::BattleMacroItemCategoryReady,
    bp::battle::BattleMacroItemRowListReady,
    bp::battle::BattleMacroItemDetailReady,
    bp::battle::BattleMacroEnemyTargetReady,
    bp::battle::BattleMacroAllyTargetReady,
};

bool IsRequiredKey(BPKey key) noexcept {
    return std::find(kRequiredBreakpointKeys.begin(), kRequiredBreakpointKeys.end(), key)
        != kRequiredBreakpointKeys.end();
}

BattleCommandInputMacroProvider::PrepareResult WaitFailure(
    BPKey expected_key,
    const BattleCommandProviderWaitResult& wait) {
    BattleCommandInputMacroProvider::PrepareResult result{};
    result.last_expected_key = expected_key;
    result.last_hit_key = wait.hit_key;
    result.last_hit_pc = wait.hit_pc;
    if (!wait.hit) {
        result.failure = Legacy::FailureCode::HostFailure;
        result.diagnostic = "battle command synchronization wait failed";
    } else {
        result.failure = Legacy::FailureCode::UnexpectedBreakpoint;
        result.diagnostic = "unexpected breakpoint while synchronizing battle command input";
    }
    return result;
}

} // namespace

std::span<const BPKey> BattleCommandInputMacroProvider::required_breakpoint_keys() noexcept {
    return kRequiredBreakpointKeys;
}

BattleCommandInputMacroProvider::PrepareResult BattleCommandInputMacroProvider::prepare(
    IBattleCommandInputMacroProviderHost& host,
    const Request& request) const {
    if (host.current_breakpoint_key() != bp::battle::TurnInputs) {
        const std::array expected{bp::battle::TurnInputs};
        const auto wait = host.wait_for_breakpoints(expected);
        if (!wait.hit || wait.hit_key != expected.front()) {
            return WaitFailure(expected.front(), wait);
        }
    }

    const std::array expected{bp::battle::BattleMacroInputReadyGate};
    const auto wait = host.wait_for_breakpoints(expected);
    if (!wait.hit || wait.hit_key != expected.front()) {
        return WaitFailure(expected.front(), wait);
    }

    std::string mem1;
    if (!host.capture_mem1(mem1)) {
        PrepareResult result{};
        result.failure = Legacy::FailureCode::BattleContextUnavailable;
        result.last_expected_key = expected.front();
        result.last_hit_key = wait.hit_key;
        result.last_hit_pc = wait.hit_pc;
        result.diagnostic = "failed to capture MEM1 for battle command planning";
        return result;
    }

    soa::battle::ctx::BattleContext battle_context{};
    const savor::MemView view(
        reinterpret_cast<const std::uint8_t*>(mem1.data()),
        mem1.size());
    if (!soa::battle::ctx::codec::extract_from_mem1(view, battle_context)) {
        PrepareResult result{};
        result.failure = Legacy::FailureCode::BattleContextUnavailable;
        result.last_expected_key = expected.front();
        result.last_hit_key = wait.hit_key;
        result.last_hit_pc = wait.hit_pc;
        result.diagnostic = "failed to decode battle planning context from MEM1";
        return result;
    }

    auto result = compile(request, battle_context);
    result.last_expected_key = expected.front();
    result.last_hit_key = wait.hit_key;
    result.last_hit_pc = wait.hit_pc;
    return result;
}

BattleCommandInputMacroProvider::PrepareResult BattleCommandInputMacroProvider::compile(
    const Request& request,
    const soa::battle::ctx::BattleContext& battle_context) const {
    PrepareResult result{};
    result.planning_context = Legacy::BuildPlanningContext(battle_context);

    std::visit(
        [&](const auto& typed_request) {
            using T = std::decay_t<decltype(typed_request)>;
            if constexpr (std::is_same_v<T, ProbeRequest>) {
                std::vector<Legacy::MacroStep> steps;
                if (typed_request.use_final_fake_attack_pattern) {
                    steps = Legacy::BuildMacroProbePlanSteps(
                        typed_request.commands,
                        typed_request.transition_neutral_frames,
                        typed_request.fake_attack_count,
                        typed_request.use_mixed_fake_attack_patterns
                            ? typed_request.first_fake_attack_pattern
                            : typed_request.fake_attack_pattern,
                        typed_request.fake_attack_pattern,
                        typed_request.final_fake_attack_pattern,
                        &result.planning_context,
                        &result.failure);
                } else if (typed_request.use_mixed_fake_attack_patterns) {
                    steps = Legacy::BuildMacroProbePlanSteps(
                        typed_request.commands,
                        typed_request.transition_neutral_frames,
                        typed_request.fake_attack_count,
                        typed_request.first_fake_attack_pattern,
                        typed_request.fake_attack_pattern,
                        &result.planning_context,
                        &result.failure);
                } else {
                    steps = Legacy::BuildMacroProbePlanSteps(
                        typed_request.commands,
                        typed_request.transition_neutral_frames,
                        typed_request.fake_attack_count,
                        typed_request.fake_attack_pattern,
                        &result.planning_context,
                        &result.failure);
                }
                result.plan.steps = std::move(steps);
            } else {
                result.plan.steps = Legacy::BuildMacroPlanStepsFromTurnPlan(
                    typed_request.turn_plan,
                    typed_request.transition_neutral_frames,
                    &result.planning_context,
                    &result.materialize_error);
                if (result.materialize_error != soa::battle::actions::MaterializeErr::OK) {
                    result.failure =
                        result.materialize_error == soa::battle::actions::MaterializeErr::NoValidTarget
                        ? Legacy::FailureCode::InvalidTarget
                        : Legacy::FailureCode::InvalidMode;
                }
            }
        },
        request);

    if (result.failure == Legacy::FailureCode::Ok && result.plan.steps.empty()) {
        result.failure = Legacy::FailureCode::NoSteps;
    }
    if (result.failure == Legacy::FailureCode::Ok
        && !plan_uses_only_declared_breakpoints(result.plan)) {
        result.plan.steps.clear();
        result.failure = Legacy::FailureCode::InvalidMode;
        result.diagnostic = "battle command compiler emitted an undeclared breakpoint";
    }
    if (result.failure != Legacy::FailureCode::Ok && result.diagnostic.empty()) {
        std::ostringstream diagnostic;
        diagnostic << "battle command plan compilation failed: failure="
                   << Legacy::FailureCodeName(result.failure)
                   << " materialize="
                   << static_cast<std::uint32_t>(result.materialize_error)
                   << " context='"
                   << Legacy::FormatPlanningContext(result.planning_context)
                   << "'";
        result.diagnostic = diagnostic.str();
    }
    return result;
}

bool BattleCommandInputMacroProvider::plan_uses_only_declared_breakpoints(
    const InputMacroPlan& plan) noexcept {
    for (const auto& step : plan.steps) {
        const auto* wait = std::get_if<BreakpointWaitAction>(&step.action);
        if (wait == nullptr) continue;
        if (wait->expected_keys.empty()) return false;
        if (std::any_of(
                wait->expected_keys.begin(),
                wait->expected_keys.end(),
                [](BPKey key) { return !IsRequiredKey(key); })) {
            return false;
        }
    }
    return true;
}

} // namespace savor::inputmacro
