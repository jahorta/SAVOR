#include <gtest/gtest.h>

#include <algorithm>
#include <initializer_list>
#include <string>
#include <variant>
#include <vector>

#include "Cli.h"
#include "Core/Input/InputPlan.h"
#include "Core/Input/SoaBattle/ActionTypes.h"
#include "Core/Input/SoaBattle/PlanWriter.h"
#include "Core/Memory/Soa/SoaAddrRegistry.h"
#include "Phases/Programs/BattleMacroProbe/BattleMacroProbePayload.h"
#include "Phases/Programs/BattleMacroProbe/BattleMacroProbeScript.h"
#include "Phases/Programs/BattleTurnRunner/BattleTurnRunnerPayload.h"
#include "Phases/Programs/BattleTurnRunner/BattleTurnRunnerScript.h"
#include "Runner/Breakpoints/BpRegistry.h"
#include "Runner/Script/CtxRegistry.h"
#include "Runner/Script/ScriptProgress.h"

namespace {

using phase::battle::macroprobe::BuildMacroPlanSteps;
using phase::battle::macroprobe::BuildMacroProbePlanSteps;
using phase::battle::macroprobe::BuildMacroPlanStepsFromTurnPlan;
using phase::battle::macroprobe::BuildMacroSteps;
using phase::battle::macroprobe::BuildPlanningContext;
using phase::battle::macroprobe::FakeAttackMemoryGateMode;
using phase::battle::macroprobe::FakeAttackPattern;
using phase::battle::macroprobe::FailureCode;
using phase::battle::macroprobe::MacroCommand;
using phase::battle::macroprobe::MacroMode;
using phase::battle::macroprobe::MacroStep;
using phase::battle::macroprobe::ParseCommandPlanSpec;
using savor::inputmacro::InputMacroActionKind;

const savor::inputmacro::BreakpointWaitAction* BreakpointWait(const MacroStep& step)
{
    return std::get_if<savor::inputmacro::BreakpointWaitAction>(&step.action);
}

const savor::GCInputFrame& StepInput(const MacroStep& step)
{
    static const savor::GCInputFrame neutral{};
    const auto* wait = BreakpointWait(step);
    return wait != nullptr ? wait->input : neutral;
}

const std::vector<BPKey>& ExpectedBreakpoints(const MacroStep& step)
{
    static const std::vector<BPKey> empty;
    const auto* wait = BreakpointWait(step);
    return wait != nullptr ? wait->expected_keys : empty;
}

InputMacroActionKind StepKind(const MacroStep& step)
{
    return savor::inputmacro::ActionKind(step.action);
}

std::uint32_t StepFrameCount(const MacroStep& step)
{
    const auto* action = std::get_if<savor::inputmacro::NeutralFramesAction>(&step.action);
    return action != nullptr ? action->frame_count : 0u;
}

std::uint32_t StepMemoryAddress(const MacroStep& step)
{
    if (const auto* action = std::get_if<savor::inputmacro::CaptureU32BaselineAction>(&step.action)) {
        return action->address;
    }
    if (const auto* action = std::get_if<savor::inputmacro::WaitU32ChangeAction>(&step.action)) {
        return action->address;
    }
    return 0u;
}

std::uint32_t StepMemoryTimeout(const MacroStep& step)
{
    const auto* action = std::get_if<savor::inputmacro::WaitU32ChangeAction>(&step.action);
    return action != nullptr ? action->timeout_ms : 0u;
}

std::uint32_t StepMemoryCycleIndex(const MacroStep& step)
{
    const auto* action = std::get_if<savor::inputmacro::WaitU32ChangeAction>(&step.action);
    return action != nullptr ? action->diagnostic_cycle_index : 0u;
}

bool HoldsInputThroughHit(const MacroStep& step)
{
    const auto* wait = BreakpointWait(step);
    return wait != nullptr && wait->hold_input_through_hit_opcode;
}

bool ParseTestArgs(std::initializer_list<const char*> args, savor::e2e::CliOptions* options, std::string* error)
{
    std::vector<std::string> storage(args.begin(), args.end());
    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (auto& value : storage) {
        argv.push_back(value.data());
    }
    return savor::e2e::ParseArgs(static_cast<int>(argv.size()), argv.data(), options, error);
}

void SetBattleSlot(
    soa::battle::ctx::BattleContext& context,
    std::uint32_t slot,
    bool is_player,
    bool present,
    bool is_alive)
{
    ASSERT_LT(slot, static_cast<std::uint32_t>(soa::battle::ctx::SLOT_COUNT));
    context.slots_[slot].is_player = is_player ? 1 : 0;
    context.slots_[slot].present = present ? 1 : 0;
    context.slots_[slot].is_alive = is_alive ? 1 : 0;
}

soa::battle::ctx::BattleContext MakePlanningContextSource()
{
    soa::battle::ctx::BattleContext context{};
    for (std::uint32_t slot = 0; slot < 4; ++slot) {
        SetBattleSlot(context, slot, true, true, true);
    }
    for (std::uint32_t slot = 4; slot < 12; ++slot) {
        SetBattleSlot(context, slot, false, false, false);
    }
    return context;
}

std::uint32_t CountInput(const std::vector<MacroStep>& steps, std::uint16_t buttons)
{
    return static_cast<std::uint32_t>(std::count_if(
        steps.begin(),
        steps.end(),
        [&](const MacroStep& step) { return StepInput(step).buttons == buttons; }));
}

std::uint32_t CountLabel(const std::vector<MacroStep>& steps, const char* label)
{
    return static_cast<std::uint32_t>(std::count_if(
        steps.begin(),
        steps.end(),
        [&](const MacroStep& step) { return std::string(step.label) == label; }));
}

std::vector<size_t> LabelIndexes(const std::vector<MacroStep>& steps, const char* label)
{
    std::vector<size_t> indexes;
    for (size_t i = 0; i < steps.size(); ++i) {
        if (std::string(steps[i].label) == label) {
            indexes.push_back(i);
        }
    }
    return indexes;
}

void ExpectSingleBreakpointInputGates(const std::vector<MacroStep>& steps)
{
    for (const auto& step : steps) {
        if (StepKind(step) == InputMacroActionKind::BreakpointWait) {
            EXPECT_EQ(ExpectedBreakpoints(step).size(), 1u) << step.label;
        }
    }
}

soa::battle::actions::ActionPlan MakeTurnAction(
    soa::battle::actions::BattleAction macro,
    std::uint8_t actor_slot,
    std::uint8_t target_slot = 0xFF)
{
    soa::battle::actions::ActionPlan action{};
    action.actor_slot = actor_slot;
    action.macro = macro;
    action.params.target_slot = target_slot;
    return action;
}

TEST(BattleMacroProbePlanningContext, ExtractsAliveTargetsAndInventoryRows)
{
    auto source = MakePlanningContextSource();
    SetBattleSlot(source, 4, false, true, false);
    SetBattleSlot(source, 5, false, true, true);
    SetBattleSlot(source, 6, false, false, false);
    SetBattleSlot(source, 7, false, true, true);
    source.state.useable_items[2] = soa::ItemSlot{.item_id = 240, .count = 3};
    source.state.useable_items[5] = soa::ItemSlot{.item_id = 999, .count = 1};
    source.state.useable_items[6] = soa::ItemSlot{.item_id = 241, .count = 0};

    const auto context = BuildPlanningContext(source);

    ASSERT_EQ(context.alive_ally_slots.size(), 4u);
    ASSERT_EQ(context.alive_enemy_slots.size(), 2u);
    EXPECT_EQ(context.alive_enemy_slots[0], 5u);
    EXPECT_EQ(context.alive_enemy_slots[1], 7u);
    EXPECT_FALSE(context.IsAliveEnemySlot(4));
    EXPECT_TRUE(context.IsAliveEnemySlot(5));
    EXPECT_EQ(context.EnemySelectableIndex(5), 0);
    EXPECT_EQ(context.EnemySelectableIndex(7), 1);
    EXPECT_EQ(context.EnemySelectableIndex(8), -1);

    ASSERT_EQ(context.usable_items.size(), 2u);
    EXPECT_EQ(context.usable_items[0].row_index, 2u);
    EXPECT_EQ(context.usable_items[0].item_id, 240u);
    EXPECT_EQ(context.usable_items[0].count, 3u);
    EXPECT_EQ(context.usable_items[0].name, "Sacri Crystal");
    EXPECT_EQ(context.usable_items[1].row_index, 5u);
    EXPECT_EQ(context.usable_items[1].item_id, 999u);
    EXPECT_EQ(context.usable_items[1].count, 1u);
    EXPECT_EQ(context.usable_items[1].name, "item:999");
}

TEST(BattleMacroProbeCompiler, TurnPlanAttackBlockFocusCompilesInActorOrder)
{
    auto source = MakePlanningContextSource();
    SetBattleSlot(source, 4, false, true, true);
    SetBattleSlot(source, 5, false, true, true);
    const auto context = BuildPlanningContext(source);

    soa::battle::actions::TurnPlan turn{};
    turn.commands = {
        MakeTurnAction(soa::battle::actions::BattleAction::Attack, 0, 5),
        MakeTurnAction(soa::battle::actions::BattleAction::Defend, 1),
        MakeTurnAction(soa::battle::actions::BattleAction::Focus, 2),
    };

    soa::battle::actions::MaterializeErr err = soa::battle::actions::MaterializeErr::OK;
    const auto steps = BuildMacroPlanStepsFromTurnPlan(turn, 3, &context, &err);

    ASSERT_EQ(err, soa::battle::actions::MaterializeErr::OK);
    ASSERT_FALSE(steps.empty());
    EXPECT_EQ(StepInput(steps[0]).buttons, savor::GC_A);
    EXPECT_EQ(ExpectedBreakpoints(steps[0]).front(), bp::battle::BattleMacroMainMenuAcceptDispatch);
    EXPECT_EQ(CountLabel(steps, "block_menu_up"), 1u);
    EXPECT_EQ(CountLabel(steps, "focus_menu_down"), 3u);
    EXPECT_EQ(CountLabel(steps, "direct_command_queued"), 2u);
}

TEST(BattleMacroProbeCompiler, TurnPlanFirstFakeAttackUsesFixedFastPattern)
{
    auto source = MakePlanningContextSource();
    SetBattleSlot(source, 4, false, true, true);
    const auto context = BuildPlanningContext(source);

    soa::battle::actions::TurnPlan turn{};
    turn.fake_attack_count = 1;
    turn.commands = {MakeTurnAction(soa::battle::actions::BattleAction::Defend, 0)};

    soa::battle::actions::MaterializeErr err = soa::battle::actions::MaterializeErr::OK;
    const auto steps = BuildMacroPlanStepsFromTurnPlan(turn, 3, &context, &err);

    ASSERT_EQ(err, soa::battle::actions::MaterializeErr::OK);
    ASSERT_EQ(steps.size(), 8u);
    EXPECT_EQ(StepKind(steps[0]), InputMacroActionKind::CaptureU32Baseline);
    EXPECT_EQ(std::string(steps[0].label), "fake_attack_rng_capture");
    EXPECT_EQ(StepInput(steps[1]).buttons, savor::GC_A);
    EXPECT_EQ(ExpectedBreakpoints(steps[1]).front(), bp::battle::BattleMacroMainMenuAcceptDispatch);
    EXPECT_EQ(ExpectedBreakpoints(steps[2]).front(), bp::battle::BattleMacroEnemyTargetReady);
    EXPECT_EQ(StepKind(steps[3]), InputMacroActionKind::WaitU32Change);
    EXPECT_EQ(std::string(steps[3].label), "fake_attack_rng_changed_target");
    EXPECT_EQ(StepInput(steps[4]).buttons, savor::GC_B);
    EXPECT_EQ(ExpectedBreakpoints(steps[4]).front(), bp::battle::BattleMacroInputReadyGate);
    EXPECT_EQ(StepInput(steps[5]).buttons, savor::GC_DU);
    EXPECT_EQ(ExpectedBreakpoints(steps[7]).front(), bp::battle::BattleMacroDirectCommandQueued);
    EXPECT_EQ(CountLabel(steps, "fake_attack_target_neutral_before_b"), 0u);
}

TEST(BattleMacroProbeCompiler, TurnPlanFastFakeAttacksAreInterleavedWithCommands)
{
    auto source = MakePlanningContextSource();
    SetBattleSlot(source, 4, false, true, true);
    const auto context = BuildPlanningContext(source);

    soa::battle::actions::TurnPlan turn{};
    turn.fake_attack_count = 3;
    turn.commands = {
        MakeTurnAction(soa::battle::actions::BattleAction::Defend, 0),
        MakeTurnAction(soa::battle::actions::BattleAction::Focus, 1),
        MakeTurnAction(soa::battle::actions::BattleAction::Defend, 2),
        MakeTurnAction(soa::battle::actions::BattleAction::Focus, 3),
    };

    soa::battle::actions::MaterializeErr err = soa::battle::actions::MaterializeErr::OK;
    const auto steps = BuildMacroPlanStepsFromTurnPlan(turn, 3, &context, &err);

    ASSERT_EQ(err, soa::battle::actions::MaterializeErr::OK);
    EXPECT_EQ(CountLabel(steps, "fake_attack_rng_capture"), 3u);
    EXPECT_EQ(CountLabel(steps, "fake_attack_rng_changed_target"), 3u);
    EXPECT_EQ(CountLabel(steps, "fake_attack_target_neutral_before_b"), 0u);

    const auto captures = LabelIndexes(steps, "fake_attack_rng_capture");
    const auto accepts = LabelIndexes(steps, "direct_command_queued");
    ASSERT_EQ(captures.size(), 3u);
    ASSERT_EQ(accepts.size(), 4u);
    EXPECT_LT(captures[0], accepts[0]);
    EXPECT_LT(accepts[0], captures[1]);
    EXPECT_LT(captures[1], accepts[1]);
    EXPECT_LT(accepts[1], captures[2]);
    EXPECT_LT(captures[2], accepts[2]);
    EXPECT_LT(accepts[2], accepts[3]);
}

TEST(BattleMacroProbeCompiler, TurnPlanExcessFakeAttacksUseMeasuredPrefixBeforeFastCharacterFakes)
{
    auto source = MakePlanningContextSource();
    SetBattleSlot(source, 2, true, true, false);
    SetBattleSlot(source, 3, true, true, false);
    SetBattleSlot(source, 4, false, true, true);
    const auto context = BuildPlanningContext(source);

    ASSERT_EQ(context.alive_ally_slots.size(), 2u);

    soa::battle::actions::TurnPlan turn{};
    turn.fake_attack_count = 3;
    turn.commands = {
        MakeTurnAction(soa::battle::actions::BattleAction::Defend, 0),
        MakeTurnAction(soa::battle::actions::BattleAction::Focus, 1),
    };

    soa::battle::actions::MaterializeErr err = soa::battle::actions::MaterializeErr::OK;
    const auto steps = BuildMacroPlanStepsFromTurnPlan(turn, 3, &context, &err);

    ASSERT_EQ(err, soa::battle::actions::MaterializeErr::OK);
    EXPECT_EQ(CountLabel(steps, "fake_attack_rng_capture"), 3u);
    EXPECT_EQ(CountLabel(steps, "fake_attack_rng_changed_target"), 3u);
    EXPECT_EQ(CountLabel(steps, "fake_attack_target_neutral_before_b"), 2u);

    const auto captures = LabelIndexes(steps, "fake_attack_rng_capture");
    const auto slow_neutrals = LabelIndexes(steps, "fake_attack_target_neutral_before_b");
    const auto accepts = LabelIndexes(steps, "direct_command_queued");
    ASSERT_EQ(captures.size(), 3u);
    ASSERT_EQ(slow_neutrals.size(), 2u);
    ASSERT_EQ(accepts.size(), 2u);
    EXPECT_LT(captures[0], slow_neutrals[0]);
    EXPECT_EQ(StepFrameCount(steps[slow_neutrals[0]]), 7u);
    EXPECT_LT(slow_neutrals[0], captures[1]);
    EXPECT_LT(captures[1], slow_neutrals[1]);
    EXPECT_EQ(StepFrameCount(steps[slow_neutrals[1]]), 7u);
    EXPECT_LT(slow_neutrals[1], captures[2]);
    EXPECT_LT(captures[2], accepts[0]);
    EXPECT_LT(accepts[0], accepts[1]);
}

TEST(BattleMacroProbeCompiler, TurnPlanRejectsUseItem)
{
    auto source = MakePlanningContextSource();
    SetBattleSlot(source, 4, false, true, true);
    const auto context = BuildPlanningContext(source);

    soa::battle::actions::TurnPlan turn{};
    turn.commands = {MakeTurnAction(soa::battle::actions::BattleAction::UseItem, 0, 4)};

    soa::battle::actions::MaterializeErr err = soa::battle::actions::MaterializeErr::OK;
    const auto steps = BuildMacroPlanStepsFromTurnPlan(turn, 3, &context, &err);

    EXPECT_TRUE(steps.empty());
    EXPECT_EQ(err, soa::battle::actions::MaterializeErr::InvalidNavigation);
}

TEST(BattleMacroProbeCompiler, InputGateStepsUseSingleExpectedBreakpoint)
{
    auto source = MakePlanningContextSource();
    SetBattleSlot(source, 4, false, true, true);
    SetBattleSlot(source, 5, false, true, true);
    SetBattleSlot(source, 6, false, true, true);
    const auto context = BuildPlanningContext(source);

    FailureCode failure = FailureCode::Ok;
    ExpectSingleBreakpointInputGates(BuildMacroSteps(MacroMode::Attack, 6, &context, &failure));
    ASSERT_EQ(failure, FailureCode::Ok);
    ExpectSingleBreakpointInputGates(BuildMacroSteps(MacroMode::Focus, 4, &context, &failure));
    ASSERT_EQ(failure, FailureCode::Ok);
    ExpectSingleBreakpointInputGates(BuildMacroSteps(MacroMode::Block, 4, &context, &failure));
    ASSERT_EQ(failure, FailureCode::Ok);

    const std::vector<MacroCommand> commands{
        MacroCommand{.mode = MacroMode::Attack, .target_slot = 6},
        MacroCommand{.mode = MacroMode::Focus, .target_slot = 4},
        MacroCommand{.mode = MacroMode::Block, .target_slot = 4},
    };
    ExpectSingleBreakpointInputGates(BuildMacroProbePlanSteps(
        commands,
        3,
        2,
        FakeAttackPattern{
            .memory_gate_mode = FakeAttackMemoryGateMode::TargetSide,
            .target_neutral_before_b_frames = 7,
            .input_neutral_after_b_frames = 0,
            .memory_timeout_ms = 1000,
        },
        &context,
        &failure));
    ASSERT_EQ(failure, FailureCode::Ok);
}

TEST(BattleMacroProbeCompiler, TurnPlanRejectsDeadExplicitAttackTarget)
{
    auto source = MakePlanningContextSource();
    SetBattleSlot(source, 4, false, true, false);
    SetBattleSlot(source, 5, false, true, true);
    const auto context = BuildPlanningContext(source);

    soa::battle::actions::TurnPlan turn{};
    turn.commands = {MakeTurnAction(soa::battle::actions::BattleAction::Attack, 0, 4)};

    soa::battle::actions::MaterializeErr err = soa::battle::actions::MaterializeErr::OK;
    const auto steps = BuildMacroPlanStepsFromTurnPlan(turn, 3, &context, &err);

    EXPECT_TRUE(steps.empty());
    EXPECT_EQ(err, soa::battle::actions::MaterializeErr::NoValidTarget);
}

TEST(BattleMacroProbeCompiler, FocusCompilesThreeDownsThenAccept)
{
    FailureCode failure = FailureCode::Ok;
    const auto steps = BuildMacroSteps(MacroMode::Focus, 4, &failure);
    ASSERT_EQ(failure, FailureCode::Ok);
    ASSERT_EQ(steps.size(), 7u);
    EXPECT_EQ(StepInput(steps[0]).buttons, savor::GC_DD);
    EXPECT_EQ(StepInput(steps[1]).buttons, 0u);
    EXPECT_EQ(StepInput(steps[2]).buttons, savor::GC_DD);
    EXPECT_EQ(StepInput(steps[3]).buttons, 0u);
    EXPECT_EQ(StepInput(steps[4]).buttons, savor::GC_DD);
    ASSERT_EQ(ExpectedBreakpoints(steps[0]).size(), 1u);
    EXPECT_EQ(ExpectedBreakpoints(steps[0]).front(), bp::battle::BattleMacroMainMenuMoveLower);
    ASSERT_EQ(ExpectedBreakpoints(steps[2]).size(), 1u);
    EXPECT_EQ(ExpectedBreakpoints(steps[2]).front(), bp::battle::BattleMacroMainMenuMoveLower);
    ASSERT_EQ(ExpectedBreakpoints(steps[4]).size(), 1u);
    EXPECT_EQ(ExpectedBreakpoints(steps[4]).front(), bp::battle::BattleMacroMainMenuMoveLower);
    EXPECT_EQ(ExpectedBreakpoints(steps[1]).front(), bp::battle::BattleMacroCommandTransitionDone);
    EXPECT_EQ(StepInput(steps[6]).buttons, savor::GC_A);
    ASSERT_EQ(ExpectedBreakpoints(steps[6]).size(), 1u);
    EXPECT_EQ(ExpectedBreakpoints(steps[6]).front(), bp::battle::BattleMacroDirectCommandQueued);
}

TEST(BattleMacroProbeCompiler, BlockCompilesUpThenAccept)
{
    FailureCode failure = FailureCode::Ok;
    const auto steps = BuildMacroSteps(MacroMode::Block, 4, &failure);
    ASSERT_EQ(failure, FailureCode::Ok);
    ASSERT_EQ(steps.size(), 3u);
    EXPECT_EQ(StepInput(steps[0]).buttons, savor::GC_DU);
    ASSERT_EQ(ExpectedBreakpoints(steps[0]).size(), 1u);
    EXPECT_EQ(ExpectedBreakpoints(steps[0]).front(), bp::battle::BattleMacroMainMenuMoveHigher);
    EXPECT_EQ(StepInput(steps[1]).buttons, 0u);
    EXPECT_EQ(ExpectedBreakpoints(steps[1]).front(), bp::battle::BattleMacroCommandTransitionDone);
    EXPECT_EQ(StepInput(steps[2]).buttons, savor::GC_A);
    ASSERT_EQ(ExpectedBreakpoints(steps[2]).size(), 1u);
    EXPECT_EQ(ExpectedBreakpoints(steps[2]).front(), bp::battle::BattleMacroDirectCommandQueued);
}

TEST(BattleMacroProbeCompiler, AttackCompilesTargetCursorMoves)
{
    FailureCode failure = FailureCode::Ok;
    const auto steps = BuildMacroSteps(MacroMode::Attack, 6, &failure);
    ASSERT_EQ(failure, FailureCode::Ok);
    ASSERT_EQ(steps.size(), 7u);
    EXPECT_EQ(StepInput(steps[0]).buttons, savor::GC_A);
    EXPECT_EQ(ExpectedBreakpoints(steps[0]).front(), bp::battle::BattleMacroMainMenuAcceptDispatch);
    EXPECT_EQ(StepInput(steps[1]).buttons, 0u);
    EXPECT_EQ(ExpectedBreakpoints(steps[1]).front(), bp::battle::BattleMacroEnemyTargetReady);
    EXPECT_EQ(StepInput(steps[2]).buttons, 0u);
    EXPECT_EQ(ExpectedBreakpoints(steps[2]).front(), bp::battle::BattleMacroEnemyTargetReady);
    EXPECT_EQ(StepInput(steps[3]).buttons, savor::GC_DD);
    EXPECT_EQ(ExpectedBreakpoints(steps[3]).front(), bp::battle::BattleMacroEnemyTargetMoveDownAccepted);
    EXPECT_EQ(StepInput(steps[4]).buttons, 0u);
    EXPECT_EQ(ExpectedBreakpoints(steps[4]).front(), bp::battle::BattleMacroEnemyTargetReady);
    EXPECT_EQ(StepInput(steps[5]).buttons, savor::GC_DD);
    EXPECT_EQ(ExpectedBreakpoints(steps[5]).front(), bp::battle::BattleMacroEnemyTargetMoveDownAccepted);
    EXPECT_EQ(StepInput(steps[6]).buttons, savor::GC_A);
    ASSERT_EQ(ExpectedBreakpoints(steps[6]).size(), 1u);
    EXPECT_EQ(ExpectedBreakpoints(steps[6]).front(), bp::battle::BattleMacroEnemyTargetFinalized);
    EXPECT_TRUE(HoldsInputThroughHit(steps[6]));
}

TEST(BattleMacroProbeCompiler, AttackWithoutTargetMovementGetsSecondEnemyReadyWait)
{
    FailureCode failure = FailureCode::Ok;
    const auto steps = BuildMacroSteps(MacroMode::Attack, 4, &failure);
    ASSERT_EQ(failure, FailureCode::Ok);
    ASSERT_EQ(steps.size(), 4u);
    EXPECT_EQ(ExpectedBreakpoints(steps[0]).front(), bp::battle::BattleMacroMainMenuAcceptDispatch);
    EXPECT_EQ(ExpectedBreakpoints(steps[1]).front(), bp::battle::BattleMacroEnemyTargetReady);
    EXPECT_EQ(ExpectedBreakpoints(steps[2]).front(), bp::battle::BattleMacroEnemyTargetReady);
    EXPECT_EQ(StepInput(steps[3]).buttons, savor::GC_A);
    EXPECT_EQ(ExpectedBreakpoints(steps[3]).front(), bp::battle::BattleMacroEnemyTargetFinalized);
}

TEST(BattleMacroProbeCompiler, AttackRejectsInvalidTargetSlot)
{
    FailureCode failure = FailureCode::Ok;
    const auto steps = BuildMacroSteps(MacroMode::Attack, 12, &failure);
    EXPECT_TRUE(steps.empty());
    EXPECT_EQ(failure, FailureCode::InvalidTarget);
}

TEST(BattleMacroProbeCompiler, AttackUsesLiveEnemySelectorOrder)
{
    auto source = MakePlanningContextSource();
    SetBattleSlot(source, 4, false, true, true);
    SetBattleSlot(source, 5, false, true, true);
    SetBattleSlot(source, 6, false, true, true);
    const auto context = BuildPlanningContext(source);

    FailureCode failure = FailureCode::Ok;
    const auto steps = BuildMacroSteps(MacroMode::Attack, 6, &context, &failure);

    ASSERT_EQ(failure, FailureCode::Ok);
    ASSERT_EQ(steps.size(), 7u);
    EXPECT_EQ(CountInput(steps, savor::GC_DD), 2u);
    EXPECT_EQ(ExpectedBreakpoints(steps[3]).front(), bp::battle::BattleMacroEnemyTargetMoveDownAccepted);
    EXPECT_EQ(ExpectedBreakpoints(steps[4]).front(), bp::battle::BattleMacroEnemyTargetReady);
    EXPECT_EQ(ExpectedBreakpoints(steps[5]).front(), bp::battle::BattleMacroEnemyTargetMoveDownAccepted);
}

TEST(BattleMacroProbeCompiler, AttackFailsWhenExplicitTargetIsDead)
{
    auto source = MakePlanningContextSource();
    SetBattleSlot(source, 4, false, true, false);
    SetBattleSlot(source, 5, false, true, true);
    const auto context = BuildPlanningContext(source);

    FailureCode failure = FailureCode::Ok;
    const auto steps = BuildMacroSteps(MacroMode::Attack, 4, &context, &failure);

    EXPECT_TRUE(steps.empty());
    EXPECT_EQ(failure, FailureCode::InvalidTarget);
}

TEST(BattleMacroProbeCompiler, AttackTargetsFirstLiveEnemyWithoutCursorMoves)
{
    auto source = MakePlanningContextSource();
    SetBattleSlot(source, 4, false, true, false);
    SetBattleSlot(source, 5, false, true, true);
    SetBattleSlot(source, 6, false, true, true);
    const auto context = BuildPlanningContext(source);

    FailureCode failure = FailureCode::Ok;
    const auto steps = BuildMacroSteps(MacroMode::Attack, 5, &context, &failure);

    ASSERT_EQ(failure, FailureCode::Ok);
    ASSERT_EQ(steps.size(), 4u);
    EXPECT_EQ(CountInput(steps, savor::GC_DD), 0u);
    EXPECT_EQ(ExpectedBreakpoints(steps[3]).front(), bp::battle::BattleMacroEnemyTargetFinalized);
}

TEST(BattleMacroProbeCompiler, BlockBlockPlanAddsNeutralCharacterTransition)
{
    FailureCode failure = FailureCode::Ok;
    const auto steps = BuildMacroPlanSteps(
        {
            MacroCommand{.mode = MacroMode::Block, .target_slot = 4},
            MacroCommand{.mode = MacroMode::Block, .target_slot = 4},
        },
        3,
        &failure);

    ASSERT_EQ(failure, FailureCode::Ok);
    ASSERT_EQ(steps.size(), 8u);
    EXPECT_EQ(StepInput(steps[0]).buttons, savor::GC_DU);
    EXPECT_EQ(ExpectedBreakpoints(steps[2]).front(), bp::battle::BattleMacroDirectCommandQueued);
    EXPECT_EQ(ExpectedBreakpoints(steps[3]).front(), bp::battle::BattleMacroInputReadyGate);
    EXPECT_EQ(StepKind(steps[4]), InputMacroActionKind::NeutralFrames);
    EXPECT_EQ(StepFrameCount(steps[4]), 3u);
    EXPECT_EQ(StepInput(steps[5]).buttons, savor::GC_DU);
    EXPECT_EQ(ExpectedBreakpoints(steps[7]).front(), bp::battle::BattleMacroDirectCommandQueued);
}

TEST(BattleMacroProbeCompiler, AttackThenBlockPlanPreservesAttackTargetMovement)
{
    FailureCode failure = FailureCode::Ok;
    const auto steps = BuildMacroPlanSteps(
        {
            MacroCommand{.mode = MacroMode::Attack, .target_slot = 6},
            MacroCommand{.mode = MacroMode::Block, .target_slot = 4},
        },
        3,
        &failure);

    ASSERT_EQ(failure, FailureCode::Ok);
    ASSERT_EQ(steps.size(), 12u);
    EXPECT_EQ(ExpectedBreakpoints(steps[0]).front(), bp::battle::BattleMacroMainMenuAcceptDispatch);
    EXPECT_EQ(ExpectedBreakpoints(steps[1]).front(), bp::battle::BattleMacroEnemyTargetReady);
    EXPECT_EQ(ExpectedBreakpoints(steps[2]).front(), bp::battle::BattleMacroEnemyTargetReady);
    EXPECT_EQ(StepInput(steps[3]).buttons, savor::GC_DD);
    EXPECT_EQ(ExpectedBreakpoints(steps[3]).front(), bp::battle::BattleMacroEnemyTargetMoveDownAccepted);
    EXPECT_EQ(StepInput(steps[4]).buttons, 0u);
    EXPECT_EQ(ExpectedBreakpoints(steps[4]).front(), bp::battle::BattleMacroEnemyTargetReady);
    EXPECT_EQ(StepInput(steps[5]).buttons, savor::GC_DD);
    EXPECT_EQ(ExpectedBreakpoints(steps[5]).front(), bp::battle::BattleMacroEnemyTargetMoveDownAccepted);
    EXPECT_EQ(ExpectedBreakpoints(steps[6]).front(), bp::battle::BattleMacroEnemyTargetFinalized);
    EXPECT_EQ(ExpectedBreakpoints(steps[7]).front(), bp::battle::BattleMacroInputReadyGate);
    EXPECT_EQ(StepKind(steps[8]), InputMacroActionKind::NeutralFrames);
    EXPECT_EQ(StepInput(steps[9]).buttons, savor::GC_DU);
    EXPECT_EQ(ExpectedBreakpoints(steps[11]).front(), bp::battle::BattleMacroDirectCommandQueued);
}

TEST(BattleMacroProbeCompiler, FakeAttackOneCompilesMemoryGateCycle)
{
    FailureCode failure = FailureCode::Ok;
    const auto steps = BuildMacroProbePlanSteps(
        {MacroCommand{.mode = MacroMode::Block, .target_slot = 4}},
        3,
        1,
        nullptr,
        &failure);

    ASSERT_EQ(failure, FailureCode::Ok);
    ASSERT_EQ(steps.size(), 9u);
    EXPECT_EQ(StepKind(steps[0]), InputMacroActionKind::CaptureU32Baseline);
    EXPECT_EQ(StepMemoryAddress(steps[0]), addr::AddrRegistry::base(addr::core::RNG_SEED));
    EXPECT_EQ(std::string(steps[0].label), "fake_attack_rng_capture");
    EXPECT_EQ(StepInput(steps[1]).buttons, savor::GC_A);
    EXPECT_EQ(ExpectedBreakpoints(steps[1]).front(), bp::battle::BattleMacroMainMenuAcceptDispatch);
    EXPECT_EQ(ExpectedBreakpoints(steps[2]).front(), bp::battle::BattleMacroEnemyTargetReady);
    EXPECT_EQ(StepKind(steps[3]), InputMacroActionKind::WaitU32Change);
    EXPECT_EQ(std::string(steps[3].label), "fake_attack_rng_changed_target");
    EXPECT_EQ(StepMemoryAddress(steps[3]), addr::AddrRegistry::base(addr::core::RNG_SEED));
    EXPECT_EQ(StepMemoryTimeout(steps[3]), 1000u);
    EXPECT_EQ(StepMemoryCycleIndex(steps[3]), 0u);
    EXPECT_EQ(StepInput(steps[4]).buttons, savor::GC_B);
    EXPECT_EQ(ExpectedBreakpoints(steps[4]).front(), bp::battle::BattleMacroInputReadyGate);
    EXPECT_EQ(StepKind(steps[5]), InputMacroActionKind::NeutralFrames);
    EXPECT_EQ(StepFrameCount(steps[5]), 20u);
    EXPECT_EQ(StepInput(steps[6]).buttons, savor::GC_DU);
}

TEST(BattleMacroProbeCompiler, FakeAttackTwoEmitsIndependentCaptureChangeCycles)
{
    FailureCode failure = FailureCode::Ok;
    const auto steps = BuildMacroProbePlanSteps(
        {MacroCommand{.mode = MacroMode::Block, .target_slot = 4}},
        3,
        2,
        nullptr,
        &failure);

    ASSERT_EQ(failure, FailureCode::Ok);
    ASSERT_EQ(steps.size(), 15u);
    EXPECT_EQ(CountLabel(steps, "fake_attack_rng_capture"), 2u);
    EXPECT_EQ(CountLabel(steps, "fake_attack_rng_changed_target"), 2u);
    EXPECT_EQ(StepKind(steps[0]), InputMacroActionKind::CaptureU32Baseline);
    EXPECT_EQ(StepKind(steps[3]), InputMacroActionKind::WaitU32Change);
    EXPECT_EQ(StepKind(steps[5]), InputMacroActionKind::NeutralFrames);
    EXPECT_EQ(StepKind(steps[6]), InputMacroActionKind::CaptureU32Baseline);
    EXPECT_EQ(StepKind(steps[9]), InputMacroActionKind::WaitU32Change);
    EXPECT_EQ(StepKind(steps[11]), InputMacroActionKind::NeutralFrames);
}

TEST(BattleMacroProbeCompiler, FakeAttackSweepBuilderUsesCandidateFirstAndFixedFinal)
{
    FailureCode failure = FailureCode::Ok;
    const auto steps = BuildMacroProbePlanSteps(
        {MacroCommand{.mode = MacroMode::Block, .target_slot = 4}},
        3,
        3,
        FakeAttackPattern{
            .memory_gate_mode = FakeAttackMemoryGateMode::TargetSide,
            .target_neutral_before_b_frames = 7,
            .input_neutral_after_b_frames = 0,
            .memory_timeout_ms = 1000,
        },
        FakeAttackPattern{
            .memory_gate_mode = FakeAttackMemoryGateMode::TargetSide,
            .target_neutral_before_b_frames = 7,
            .input_neutral_after_b_frames = 0,
            .memory_timeout_ms = 1000,
        },
        FakeAttackPattern{
            .memory_gate_mode = FakeAttackMemoryGateMode::TargetSide,
            .target_neutral_before_b_frames = 0,
            .input_neutral_after_b_frames = 0,
            .memory_timeout_ms = 1000,
        },
        nullptr,
        &failure);

    ASSERT_EQ(failure, FailureCode::Ok);
    ASSERT_EQ(steps.size(), 20u);
    EXPECT_EQ(CountLabel(steps, "fake_attack_rng_capture"), 3u);
    EXPECT_EQ(CountLabel(steps, "fake_attack_rng_changed_target"), 3u);
    EXPECT_EQ(CountLabel(steps, "fake_attack_target_neutral_before_b"), 2u);
    EXPECT_EQ(StepMemoryCycleIndex(steps[3]), 0u);
    EXPECT_EQ(StepMemoryCycleIndex(steps[9]), 1u);
    EXPECT_EQ(StepMemoryCycleIndex(steps[15]), 2u);
    EXPECT_EQ(std::string(steps[4].label), "fake_attack_target_neutral_before_b");
    EXPECT_EQ(StepFrameCount(steps[4]), 7u);
    EXPECT_EQ(std::string(steps[10].label), "fake_attack_target_neutral_before_b");
    EXPECT_EQ(StepFrameCount(steps[10]), 7u);
    EXPECT_EQ(StepInput(steps[16]).buttons, savor::GC_B);
    EXPECT_EQ(ExpectedBreakpoints(steps[16]).front(), bp::battle::BattleMacroInputReadyGate);
}

TEST(BattleMacroProbeCompiler, FakeAttackInputSidePatternCompilesMemoryGateAfterInputReady)
{
    FailureCode failure = FailureCode::Ok;
    const auto steps = BuildMacroProbePlanSteps(
        {MacroCommand{.mode = MacroMode::Block, .target_slot = 4}},
        3,
        1,
        FakeAttackPattern{
            .memory_gate_mode = FakeAttackMemoryGateMode::InputSide,
            .target_neutral_before_b_frames = 2,
            .input_neutral_after_b_frames = 4,
            .memory_timeout_ms = 777,
        },
        nullptr,
        &failure);

    ASSERT_EQ(failure, FailureCode::Ok);
    ASSERT_EQ(steps.size(), 10u);
    EXPECT_EQ(StepKind(steps[3]), InputMacroActionKind::NeutralFrames);
    EXPECT_EQ(StepFrameCount(steps[3]), 2u);
    EXPECT_EQ(StepInput(steps[4]).buttons, savor::GC_B);
    EXPECT_EQ(ExpectedBreakpoints(steps[4]).front(), bp::battle::BattleMacroInputReadyGate);
    EXPECT_EQ(StepKind(steps[5]), InputMacroActionKind::NeutralFrames);
    EXPECT_EQ(StepFrameCount(steps[5]), 4u);
    EXPECT_EQ(StepKind(steps[6]), InputMacroActionKind::WaitU32Change);
    EXPECT_EQ(std::string(steps[6].label), "fake_attack_rng_changed_input");
    EXPECT_EQ(StepMemoryTimeout(steps[6]), 777u);
}

TEST(BattleMacroProbeCompiler, ParsesPlanSpec)
{
    std::vector<MacroCommand> commands;
    std::string error;
    ASSERT_TRUE(ParseCommandPlanSpec("block,focus", &commands, &error)) << error;
    ASSERT_EQ(commands.size(), 2u);
    EXPECT_EQ(commands[0].mode, MacroMode::Block);
    EXPECT_EQ(commands[1].mode, MacroMode::Focus);

    ASSERT_TRUE(ParseCommandPlanSpec("attack:6,block", &commands, &error)) << error;
    ASSERT_EQ(commands.size(), 2u);
    EXPECT_EQ(commands[0].mode, MacroMode::Attack);
    EXPECT_EQ(commands[0].target_slot, 6u);
    EXPECT_EQ(commands[1].mode, MacroMode::Block);
}

TEST(BattleMacroProbeCompiler, RejectsInvalidPlanSpec)
{
    std::vector<MacroCommand> commands;
    std::string error;
    EXPECT_FALSE(ParseCommandPlanSpec("", &commands, &error));
    EXPECT_FALSE(ParseCommandPlanSpec("run", &commands, &error));
    EXPECT_FALSE(ParseCommandPlanSpec("attack:12", &commands, &error));
    EXPECT_FALSE(ParseCommandPlanSpec("block:4", &commands, &error));
}

TEST(BattleMacroProbePayload, DecodeEnablesBattleProgress)
{
    std::vector<std::uint8_t> payload;
    ASSERT_TRUE(phase::battle::macroprobe::encode_payload(
        phase::battle::macroprobe::EncodeSpec{
            .commands = {
                MacroCommand{.mode = MacroMode::Attack, .target_slot = 5},
                MacroCommand{.mode = MacroMode::Block, .target_slot = 4},
            },
            .transition_neutral_frames = 3,
            .step_timeout_ms = 10000,
            .vi_stall_ms = 5000,
            .observation_tail_ms = 17000,
            .fake_attack_count = 2,
            .fake_attack_pattern = FakeAttackPattern{
                .memory_gate_mode = FakeAttackMemoryGateMode::InputSide,
                .target_neutral_before_b_frames = 2,
                .input_neutral_after_b_frames = 4,
                .memory_timeout_ms = 777,
            },
            .use_mixed_fake_attack_patterns = true,
            .first_fake_attack_pattern = FakeAttackPattern{
                .memory_gate_mode = FakeAttackMemoryGateMode::TargetSide,
                .target_neutral_before_b_frames = 0,
                .input_neutral_after_b_frames = 0,
                .memory_timeout_ms = 1000,
            },
            .repeat_fake_attack_pattern = FakeAttackPattern{
                .memory_gate_mode = FakeAttackMemoryGateMode::InputSide,
                .target_neutral_before_b_frames = 2,
                .input_neutral_after_b_frames = 4,
                .memory_timeout_ms = 777,
            },
            .use_final_fake_attack_pattern = true,
            .final_fake_attack_pattern = FakeAttackPattern{
                .memory_gate_mode = FakeAttackMemoryGateMode::TargetSide,
                .target_neutral_before_b_frames = 0,
                .input_neutral_after_b_frames = 0,
                .memory_timeout_ms = 555,
            },
        },
        payload));

    savor::PSContext ctx;
    ASSERT_TRUE(phase::battle::macroprobe::decode_payload(payload, ctx));

    std::uint32_t progress_flags = 0;
    ASSERT_TRUE(ctx.get(savor::context::key::core::PROGRESS_CORE_FLAGS, progress_flags));
    EXPECT_NE(progress_flags & static_cast<std::uint32_t>(CoreProgressFlags::BattleProgress), 0u);
    EXPECT_NE(progress_flags & static_cast<std::uint32_t>(CoreProgressFlags::DontRecordHeartbeat), 0u);
    std::uint32_t run_poll_ms = 0;
    ASSERT_TRUE(ctx.get(savor::context::key::core::RUN_POLL_MS, run_poll_ms));
    EXPECT_EQ(run_poll_ms, 10u);
    std::uint32_t observation_tail_ms = 0;
    ASSERT_TRUE(ctx.get(savor::context::key::battle::MACRO_OBSERVATION_TAIL_MS, observation_tail_ms));
    EXPECT_EQ(observation_tail_ms, 17000u);
    std::uint32_t fake_attack_count = 0;
    ASSERT_TRUE(ctx.get(savor::context::key::battle::FAKE_ATTACK_COUNT_THIS_TURN, fake_attack_count));
    EXPECT_EQ(fake_attack_count, 2u);
    std::uint32_t memory_addr = 1;
    ASSERT_TRUE(ctx.get(savor::context::key::battle::MACRO_MEMORY_ADDR, memory_addr));
    EXPECT_EQ(memory_addr, 0u);
    std::uint32_t gate_mode = 0;
    std::uint32_t target_neutral = 0;
    std::uint32_t input_neutral = 0;
    std::uint32_t memory_timeout = 0;
    std::uint32_t use_mixed = 0;
    std::uint32_t first_gate_mode = 0;
    std::uint32_t first_target_neutral = 99;
    std::uint32_t first_input_neutral = 99;
    std::uint32_t first_memory_timeout = 0;
    std::uint32_t use_final = 0;
    std::uint32_t final_gate_mode = 0;
    std::uint32_t final_target_neutral = 99;
    std::uint32_t final_input_neutral = 99;
    std::uint32_t final_memory_timeout = 0;
    ASSERT_TRUE(ctx.get(savor::context::key::battle::MACRO_FAKE_MEMORY_GATE_MODE, gate_mode));
    ASSERT_TRUE(ctx.get(savor::context::key::battle::MACRO_FAKE_TARGET_NEUTRAL_FRAMES, target_neutral));
    ASSERT_TRUE(ctx.get(savor::context::key::battle::MACRO_FAKE_INPUT_NEUTRAL_FRAMES, input_neutral));
    ASSERT_TRUE(ctx.get(savor::context::key::battle::MACRO_FAKE_MEMORY_TIMEOUT_MS, memory_timeout));
    ASSERT_TRUE(ctx.get(savor::context::key::battle::MACRO_FAKE_USE_MIXED_PATTERNS, use_mixed));
    ASSERT_TRUE(ctx.get(savor::context::key::battle::MACRO_FAKE_FIRST_MEMORY_GATE_MODE, first_gate_mode));
    ASSERT_TRUE(ctx.get(savor::context::key::battle::MACRO_FAKE_FIRST_TARGET_NEUTRAL_FRAMES, first_target_neutral));
    ASSERT_TRUE(ctx.get(savor::context::key::battle::MACRO_FAKE_FIRST_INPUT_NEUTRAL_FRAMES, first_input_neutral));
    ASSERT_TRUE(ctx.get(savor::context::key::battle::MACRO_FAKE_FIRST_MEMORY_TIMEOUT_MS, first_memory_timeout));
    ASSERT_TRUE(ctx.get(savor::context::key::battle::MACRO_FAKE_USE_FINAL_PATTERN, use_final));
    ASSERT_TRUE(ctx.get(savor::context::key::battle::MACRO_FAKE_FINAL_MEMORY_GATE_MODE, final_gate_mode));
    ASSERT_TRUE(ctx.get(savor::context::key::battle::MACRO_FAKE_FINAL_TARGET_NEUTRAL_FRAMES, final_target_neutral));
    ASSERT_TRUE(ctx.get(savor::context::key::battle::MACRO_FAKE_FINAL_INPUT_NEUTRAL_FRAMES, final_input_neutral));
    ASSERT_TRUE(ctx.get(savor::context::key::battle::MACRO_FAKE_FINAL_MEMORY_TIMEOUT_MS, final_memory_timeout));
    EXPECT_EQ(gate_mode, static_cast<std::uint32_t>(FakeAttackMemoryGateMode::InputSide));
    EXPECT_EQ(target_neutral, 2u);
    EXPECT_EQ(input_neutral, 4u);
    EXPECT_EQ(memory_timeout, 777u);
    EXPECT_EQ(use_mixed, 1u);
    EXPECT_EQ(first_gate_mode, static_cast<std::uint32_t>(FakeAttackMemoryGateMode::TargetSide));
    EXPECT_EQ(first_target_neutral, 0u);
    EXPECT_EQ(first_input_neutral, 0u);
    EXPECT_EQ(first_memory_timeout, 1000u);
    EXPECT_EQ(use_final, 1u);
    EXPECT_EQ(final_gate_mode, static_cast<std::uint32_t>(FakeAttackMemoryGateMode::TargetSide));
    EXPECT_EQ(final_target_neutral, 0u);
    EXPECT_EQ(final_input_neutral, 0u);
    EXPECT_EQ(final_memory_timeout, 555u);
}

TEST(BattleTurnRunnerPayload, DecodeInitializesMacroDefaults)
{
    phase::battle::turnrunner::EncodeSpec spec{};
    spec.run_ms = 120000;
    spec.vi_stall_ms = 5000;
    spec.current_turn = 1;
    spec.max_turn = 1;
    spec.turn_plan.commands = {
        MakeTurnAction(soa::battle::actions::BattleAction::Defend, 0),
        MakeTurnAction(soa::battle::actions::BattleAction::Focus, 1),
    };

    std::vector<std::uint8_t> payload;
    ASSERT_TRUE(phase::battle::turnrunner::encode_payload(spec, payload));

    savor::PSContext ctx;
    ASSERT_TRUE(phase::battle::turnrunner::decode_payload(payload, ctx));

    std::uint32_t run_poll_ms = 0;
    std::uint32_t transition_frames = 0;
    std::uint32_t macro_result = 0;
    std::uint32_t macro_failure = 99;
    std::uint32_t macro_step_count = 99;
    std::uint32_t gate_mode = 0;
    std::uint32_t target_neutral = 0;
    std::uint32_t input_neutral = 99;
    std::uint32_t memory_timeout = 0;
    ASSERT_TRUE(ctx.get(savor::context::key::core::RUN_POLL_MS, run_poll_ms));
    ASSERT_TRUE(ctx.get(savor::context::key::battle::MACRO_TRANSITION_NEUTRAL_FRAMES, transition_frames));
    ASSERT_TRUE(ctx.get(savor::context::key::battle::MACRO_RESULT, macro_result));
    ASSERT_TRUE(ctx.get(savor::context::key::battle::MACRO_FAILURE_CODE, macro_failure));
    ASSERT_TRUE(ctx.get(savor::context::key::battle::MACRO_STEP_COUNT, macro_step_count));
    ASSERT_TRUE(ctx.get(savor::context::key::battle::MACRO_FAKE_MEMORY_GATE_MODE, gate_mode));
    ASSERT_TRUE(ctx.get(savor::context::key::battle::MACRO_FAKE_TARGET_NEUTRAL_FRAMES, target_neutral));
    ASSERT_TRUE(ctx.get(savor::context::key::battle::MACRO_FAKE_INPUT_NEUTRAL_FRAMES, input_neutral));
    ASSERT_TRUE(ctx.get(savor::context::key::battle::MACRO_FAKE_MEMORY_TIMEOUT_MS, memory_timeout));
    EXPECT_EQ(run_poll_ms, 10u);
    EXPECT_EQ(transition_frames, 3u);
    EXPECT_EQ(macro_result, 1u);
    EXPECT_EQ(macro_failure, 0u);
    EXPECT_EQ(macro_step_count, 0u);
    EXPECT_EQ(gate_mode, static_cast<std::uint32_t>(FakeAttackMemoryGateMode::TargetSide));
    EXPECT_EQ(target_neutral, 7u);
    EXPECT_EQ(input_neutral, 0u);
    EXPECT_EQ(memory_timeout, 1000u);
}

TEST(BattleMacroProbeProgram, ArmsInputReadyStartGate)
{
    const auto program = phase::battle::macroprobe::MakeBattleMacroProbeProgram();
    const auto has_canonical = [&](BPKey key) {
        return std::find(program.canonical_bp_keys.begin(), program.canonical_bp_keys.end(), key)
            != program.canonical_bp_keys.end();
    };
    const auto has_gated = [&](BPKey key) {
        return std::find(program.gated_bp_keys.begin(), program.gated_bp_keys.end(), key)
            != program.gated_bp_keys.end();
    };

    EXPECT_TRUE(has_canonical(bp::battle::TurnInputs));
    EXPECT_TRUE(has_canonical(bp::battle::TurnIsReady));
    EXPECT_FALSE(has_canonical(bp::battle::BattleMacroInputReadyGate));
    EXPECT_TRUE(has_gated(bp::battle::BattleMacroInputReadyGate));
    EXPECT_TRUE(has_gated(bp::battle::BattleMacroMainMenuAcceptDispatch));
    EXPECT_TRUE(has_gated(bp::battle::BattleMacroMagicReady));
    EXPECT_TRUE(has_gated(bp::battle::BattleMacroSMoveReady));
    EXPECT_TRUE(has_gated(bp::battle::BattleMacroConditionalRunReady));
    EXPECT_TRUE(has_gated(bp::battle::BattleMacroItemCategoryReady));
    EXPECT_TRUE(has_gated(bp::battle::BattleMacroItemRowListReady));
    EXPECT_TRUE(has_gated(bp::battle::BattleMacroItemDetailReady));
    EXPECT_TRUE(has_gated(bp::battle::BattleMacroEnemyTargetReady));
    EXPECT_TRUE(has_gated(bp::battle::BattleMacroAllyTargetReady));
    EXPECT_TRUE(has_gated(bp::battle::BattleMacroEnemyTargetMoveDownAccepted));
    EXPECT_TRUE(has_gated(bp::battle::BattleMacroEnemyTargetMoveUpAccepted));
    EXPECT_TRUE(has_gated(bp::battle::BattleMacroEnemyTargetFinalized));
    EXPECT_FALSE(has_canonical(bp::battle::BattleMacroMainMenuAcceptDispatch));
    EXPECT_FALSE(has_canonical(bp::battle::BattleMacroDirectCommandQueued));
    EXPECT_FALSE(has_gated(bp::battle::BattleMacroEnemyTargetCursorMoved));
    EXPECT_FALSE(has_gated(bp::battle::BattleMacroEnemyTargetWritten));
    ASSERT_EQ(program.ops.size(), 23u);
    EXPECT_EQ(program.ops[0].code, savor::PSOpCode::ARM_PHASE_BPS_ONCE);
    EXPECT_EQ(program.ops[1].code, savor::PSOpCode::LOAD_SNAPSHOT);
    EXPECT_EQ(program.ops[2].code, savor::PSOpCode::MATERIALIZE_BATTLE_MACRO_STEPS);
    EXPECT_EQ(program.ops[3].code, savor::PSOpCode::GOTO_IF);
    EXPECT_EQ(program.ops[3].jcc.key, savor::context::key::battle::MACRO_FAILURE_CODE);
    EXPECT_EQ(program.ops[3].jcc.cmp, savor::PSCmp::NE);
    EXPECT_EQ(program.ops[3].jcc.imm, 0u);
    EXPECT_EQ(program.ops[4].code, savor::PSOpCode::LABEL);
    EXPECT_EQ(program.ops[5].code, savor::PSOpCode::EXECUTE_BATTLE_MACRO_STEP);
    EXPECT_EQ(program.ops[6].code, savor::PSOpCode::GOTO_IF);
    EXPECT_EQ(program.ops[6].jcc.key, savor::context::key::battle::MACRO_FAILURE_CODE);
    EXPECT_EQ(program.ops[6].jcc.cmp, savor::PSCmp::NE);
    EXPECT_EQ(program.ops[7].code, savor::PSOpCode::GOTO_IF);
    EXPECT_EQ(program.ops[7].jcc.key, savor::context::key::battle::MACRO_RESULT);
    EXPECT_EQ(program.ops[7].jcc.cmp, savor::PSCmp::EQ);
    EXPECT_EQ(program.ops[7].jcc.imm, 0u);
    EXPECT_EQ(program.ops[8].code, savor::PSOpCode::GOTO);
    EXPECT_EQ(program.ops[9].code, savor::PSOpCode::LABEL);
    EXPECT_EQ(program.ops[10].code, savor::PSOpCode::STEP_OPCODE);
    EXPECT_EQ(program.ops[10].imm.v, 1u);
    EXPECT_EQ(program.ops[11].code, savor::PSOpCode::SET_TIMEOUT);
    EXPECT_EQ(program.ops[11].imm.v, 1000u);
    EXPECT_EQ(program.ops[12].code, savor::PSOpCode::RUN_UNTIL_BP);
    EXPECT_EQ(program.ops[13].code, savor::PSOpCode::GOTO_IF);
    EXPECT_EQ(program.ops[13].jcc.key, savor::context::key::core::RUN_HIT_BP_KEY);
    EXPECT_EQ(program.ops[13].jcc.cmp, savor::PSCmp::EQ);
    EXPECT_EQ(program.ops[13].jcc.imm, static_cast<std::uint32_t>(bp::battle::TurnIsReady));
    EXPECT_EQ(program.ops[14].code, savor::PSOpCode::GOTO);
    EXPECT_EQ(program.ops[15].code, savor::PSOpCode::LABEL);
    EXPECT_EQ(program.ops[16].code, savor::PSOpCode::STEP_OPCODE);
    EXPECT_EQ(program.ops[16].imm.v, 1u);
    EXPECT_EQ(program.ops[17].code, savor::PSOpCode::SET_TIMEOUT_FROM);
    EXPECT_EQ(program.ops[17].key.id, savor::context::key::battle::MACRO_OBSERVATION_TAIL_MS);
    EXPECT_EQ(program.ops[18].code, savor::PSOpCode::RUN_UNTIL_BP);
    EXPECT_EQ(program.ops[19].code, savor::PSOpCode::LABEL);
    EXPECT_EQ(program.ops[20].code, savor::PSOpCode::SET_U32);
    EXPECT_EQ(program.ops[20].keyimm.key, savor::context::key::core::DW_RUN_OUTCOME_CODE);
    EXPECT_EQ(program.ops[20].keyimm.imm, 0u);
    EXPECT_EQ(program.ops[21].code, savor::PSOpCode::LABEL);
    EXPECT_EQ(program.ops[22].code, savor::PSOpCode::RETURN_RESULT);
}

TEST(BattleTurnRunnerProgram, UsesMacroLoopInsteadOfRawInputTape)
{
    const auto program = phase::battle::turnrunner::MakeBattleTurnRunnerProgram();
    const auto has_gated = [&](BPKey key) {
        return std::find(program.gated_bp_keys.begin(), program.gated_bp_keys.end(), key)
            != program.gated_bp_keys.end();
    };
    const auto has_op = [&](savor::PSOpCode code) {
        return std::any_of(program.ops.begin(), program.ops.end(), [&](const savor::PSOp& op) {
            return op.code == code;
        });
    };
    const auto has_set_run_poll = [&](std::uint32_t value) {
        return std::any_of(program.ops.begin(), program.ops.end(), [&](const savor::PSOp& op) {
            return op.code == savor::PSOpCode::SET_U32
                && op.keyimm.key == savor::context::key::core::RUN_POLL_MS
                && op.keyimm.imm == value;
        });
    };

    EXPECT_TRUE(has_gated(bp::battle::BattleMacroInputReadyGate));
    EXPECT_TRUE(has_gated(bp::battle::BattleMacroEnemyTargetReady));
    EXPECT_TRUE(has_gated(bp::battle::BattleMacroEnemyTargetFinalized));
    EXPECT_TRUE(has_op(savor::PSOpCode::MATERIALIZE_BATTLE_TURN_MACRO_STEPS));
    EXPECT_TRUE(has_op(savor::PSOpCode::EXECUTE_BATTLE_MACRO_STEP));
    EXPECT_FALSE(has_op(savor::PSOpCode::BUILD_TURN_INPUTPLAN_FROM_BATTLE_PATH));
    EXPECT_FALSE(has_op(savor::PSOpCode::APPLY_BATTLE_INPUTPLAN_FRAMES));
    EXPECT_TRUE(has_op(savor::PSOpCode::CLEAR_MEMORY_WATCHPOINTS));
    EXPECT_TRUE(has_op(savor::PSOpCode::ARM_CAPTURE_MEMORY_WATCHPOINTS));
    EXPECT_TRUE(has_set_run_poll(10u));
    EXPECT_TRUE(has_set_run_poll(0u));
}

TEST(BattleTurnRunnerProgram, StepsPastSeedOverrideBreakpoint)
{
    const auto program = phase::battle::turnrunner::MakeBattleTurnRunnerProgram();
    auto capture_it = std::find_if(program.ops.begin(), program.ops.end(), [](const savor::PSOp& op) {
        return op.code == savor::PSOpCode::CAPTURE_SEED_OVERRIDE;
    });
    ASSERT_NE(capture_it, program.ops.end());
    ASSERT_NE(std::next(capture_it), program.ops.end());
    EXPECT_EQ(std::next(capture_it)->code, savor::PSOpCode::GOTO_IF_KEYS);
    ASSERT_NE(std::next(capture_it, 2), program.ops.end());
    EXPECT_EQ(std::next(capture_it, 2)->code, savor::PSOpCode::STEP_OPCODE);
    EXPECT_EQ(std::next(capture_it, 2)->imm.v, 1u);
}

TEST(BattleMacroProbeCli, BattlePlanDisablesInteractivePrompt)
{
    savor::e2e::CliOptions options;
    std::string error;
    ASSERT_TRUE(ParseTestArgs({
        "SavorE2E",
        "--scenario", "battle_macro_probe",
        "--iso", ".",
        "--dolphin-base-dir", ".",
        "--savestate-file", ".",
        "--battle-plan", "block,focus",
    }, &options, &error)) << error;
    EXPECT_TRUE(options.battle_macro_args_supplied);
    ASSERT_TRUE(options.battle_macro_plan_spec.has_value());
    EXPECT_EQ(*options.battle_macro_plan_spec, "block,focus");
}

TEST(BattleMacroProbeCli, BattleScenarioNamesParse)
{
    savor::e2e::CliOptions options;
    std::string error;
    ASSERT_TRUE(ParseTestArgs({
        "SavorE2E",
        "--scenario", "seedprobe_battle",
        "--iso", ".",
        "--dolphin-base-dir", ".",
        "--savestate-file", ".",
    }, &options, &error)) << error;
    ASSERT_EQ(options.scenarios.size(), 1u);
    EXPECT_EQ(options.scenarios[0], "seedprobe_battle");

    error.clear();
    ASSERT_TRUE(ParseTestArgs({
        "SavorE2E",
        "--scenario", "battle",
        "--iso", ".",
        "--dolphin-base-dir", ".",
        "--savestate-file", ".",
    }, &options, &error)) << error;
    ASSERT_EQ(options.scenarios.size(), 1u);
    EXPECT_EQ(options.scenarios[0], "battle");
}

TEST(BattleMacroProbeCli, BattleMacroLegacyArgDisablesInteractivePrompt)
{
    savor::e2e::CliOptions options;
    std::string error;
    ASSERT_TRUE(ParseTestArgs({
        "SavorE2E",
        "--scenario", "battle_macro_probe",
        "--iso", ".",
        "--dolphin-base-dir", ".",
        "--savestate-file", ".",
        "--battle-macro", "block",
    }, &options, &error)) << error;
    EXPECT_TRUE(options.battle_macro_args_supplied);
    EXPECT_EQ(options.battle_macro_mode, "block");
    EXPECT_FALSE(options.battle_macro_plan_spec.has_value());
}

TEST(BattleMacroProbeCli, InvalidBattlePlanFailsClearly)
{
    savor::e2e::CliOptions options;
    std::string error;
    EXPECT_FALSE(ParseTestArgs({
        "SavorE2E",
        "--scenario", "battle_macro_probe",
        "--iso", ".",
        "--dolphin-base-dir", ".",
        "--savestate-file", ".",
        "--battle-plan", "attack:99",
    }, &options, &error));
    EXPECT_NE(error.find("target slot"), std::string::npos);
}

TEST(BattleMacroProbeCli, BattleFakeAttacksParsesWithoutDisablingInteractivePlan)
{
    savor::e2e::CliOptions options;
    std::string error;
    ASSERT_TRUE(ParseTestArgs({
        "SavorE2E",
        "--scenario", "battle_macro_probe",
        "--iso", ".",
        "--dolphin-base-dir", ".",
        "--savestate-file", ".",
        "--battle-fake-attacks", "2",
    }, &options, &error)) << error;
    EXPECT_EQ(options.battle_macro_fake_attacks.value_or(-1), 2);
    EXPECT_FALSE(options.battle_macro_args_supplied);
    EXPECT_FALSE(options.battle_macro_plan_spec.has_value());
}

TEST(BattleMacroProbeCli, InvalidBattleFakeAttacksFailsClearly)
{
    savor::e2e::CliOptions options;
    std::string error;
    EXPECT_FALSE(ParseTestArgs({
        "SavorE2E",
        "--scenario", "battle_macro_probe",
        "--iso", ".",
        "--dolphin-base-dir", ".",
        "--savestate-file", ".",
        "--battle-fake-attacks", "256",
    }, &options, &error));
    EXPECT_NE(error.find("--battle-fake-attacks"), std::string::npos);
}

TEST(BattleMacroProbeCli, BattleFakeAttackSweepParsesKnobs)
{
    savor::e2e::CliOptions options;
    std::string error;
    ASSERT_TRUE(ParseTestArgs({
        "SavorE2E",
        "--scenario", "battle_macro_probe",
        "--iso", ".",
        "--dolphin-base-dir", ".",
        "--savestate-file", ".",
        "--battle-fake-attack-sweep",
        "--battle-fake-sweep-trials", "2",
        "--battle-fake-sweep-min-target-neutral", "1",
        "--battle-fake-sweep-max-target-neutral", "3",
        "--battle-fake-sweep-min-input-neutral", "2",
        "--battle-fake-sweep-max-input-neutral", "4",
        "--battle-fake-sweep-output", "out.json",
    }, &options, &error)) << error;
    EXPECT_TRUE(options.battle_fake_attack_sweep);
    EXPECT_TRUE(options.battle_macro_args_supplied);
    EXPECT_EQ(options.battle_fake_sweep_trials, 2);
    EXPECT_EQ(options.battle_fake_sweep_min_target_neutral, 1);
    EXPECT_EQ(options.battle_fake_sweep_max_target_neutral, 3);
    EXPECT_EQ(options.battle_fake_sweep_min_input_neutral, 2);
    EXPECT_EQ(options.battle_fake_sweep_max_input_neutral, 4);
    ASSERT_TRUE(options.battle_fake_sweep_output.has_value());
    EXPECT_EQ(options.battle_fake_sweep_output->string(), "out.json");
}

TEST(BattleMacroProbeCli, InvalidBattleFakeAttackSweepKnobsFailClearly)
{
    savor::e2e::CliOptions options;
    std::string error;
    EXPECT_FALSE(ParseTestArgs({
        "SavorE2E",
        "--scenario", "battle_macro_probe",
        "--iso", ".",
        "--dolphin-base-dir", ".",
        "--savestate-file", ".",
        "--battle-fake-attack-sweep",
        "--battle-fake-sweep-trials", "0",
    }, &options, &error));
    EXPECT_NE(error.find("--battle-fake-sweep-trials"), std::string::npos);
}

TEST(BattleMacroProbeCli, InvalidBattleFakeAttackSweepMinMaxFailsClearly)
{
    savor::e2e::CliOptions options;
    std::string error;
    EXPECT_FALSE(ParseTestArgs({
        "SavorE2E",
        "--scenario", "battle_macro_probe",
        "--iso", ".",
        "--dolphin-base-dir", ".",
        "--savestate-file", ".",
        "--battle-fake-attack-sweep",
        "--battle-fake-sweep-min-target-neutral", "8",
        "--battle-fake-sweep-max-target-neutral", "7",
    }, &options, &error));
    EXPECT_NE(error.find("--battle-fake-sweep-min-target-neutral"), std::string::npos);

    error.clear();
    EXPECT_FALSE(ParseTestArgs({
        "SavorE2E",
        "--scenario", "battle_macro_probe",
        "--iso", ".",
        "--dolphin-base-dir", ".",
        "--savestate-file", ".",
        "--battle-fake-attack-sweep",
        "--battle-fake-sweep-min-input-neutral", "3",
        "--battle-fake-sweep-max-input-neutral", "2",
    }, &options, &error));
    EXPECT_NE(error.find("--battle-fake-sweep-min-input-neutral"), std::string::npos);
}

} // namespace
