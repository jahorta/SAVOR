#include <gtest/gtest.h>

#include <algorithm>
#include <initializer_list>
#include <string>
#include <vector>

#include "Cli.h"
#include "Core/Input/InputPlan.h"
#include "Phases/Programs/BattleMacroProbe/BattleMacroProbePayload.h"
#include "Phases/Programs/BattleMacroProbe/BattleMacroProbeScript.h"
#include "Runner/Breakpoints/BpRegistry.h"
#include "Runner/Script/CtxRegistry.h"
#include "Runner/Script/ScriptProgress.h"

namespace {

using phase::battle::macroprobe::BuildMacroPlanSteps;
using phase::battle::macroprobe::BuildMacroSteps;
using phase::battle::macroprobe::BuildPlanningContext;
using phase::battle::macroprobe::FailureCode;
using phase::battle::macroprobe::MacroCommand;
using phase::battle::macroprobe::MacroMode;
using phase::battle::macroprobe::MacroStep;
using phase::battle::macroprobe::ParseCommandPlanSpec;

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
        [&](const MacroStep& step) { return step.input.buttons == buttons; }));
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

TEST(BattleMacroProbeCompiler, FocusCompilesThreeDownsThenAccept)
{
    FailureCode failure = FailureCode::Ok;
    const auto steps = BuildMacroSteps(MacroMode::Focus, 4, &failure);
    ASSERT_EQ(failure, FailureCode::Ok);
    ASSERT_EQ(steps.size(), 7u);
    EXPECT_EQ(steps[0].input.buttons, savor::GC_DD);
    EXPECT_EQ(steps[1].input.buttons, 0u);
    EXPECT_EQ(steps[2].input.buttons, savor::GC_DD);
    EXPECT_EQ(steps[3].input.buttons, 0u);
    EXPECT_EQ(steps[4].input.buttons, savor::GC_DD);
    EXPECT_EQ(steps[0].expected_bps.front(), bp::battle::BattleMacroMainMenuMoveLower);
    EXPECT_EQ(steps[1].expected_bps.front(), bp::battle::BattleMacroCommandTransitionDone);
    EXPECT_EQ(steps[6].input.buttons, savor::GC_A);
    ASSERT_EQ(steps[6].expected_bps.size(), 1u);
    EXPECT_EQ(steps[6].expected_bps.front(), bp::battle::BattleMacroDirectCommandQueued);
}

TEST(BattleMacroProbeCompiler, BlockCompilesUpThenAccept)
{
    FailureCode failure = FailureCode::Ok;
    const auto steps = BuildMacroSteps(MacroMode::Block, 4, &failure);
    ASSERT_EQ(failure, FailureCode::Ok);
    ASSERT_EQ(steps.size(), 3u);
    EXPECT_EQ(steps[0].input.buttons, savor::GC_DU);
    EXPECT_EQ(steps[0].expected_bps.front(), bp::battle::BattleMacroMainMenuMoveHigher);
    EXPECT_EQ(steps[1].input.buttons, 0u);
    EXPECT_EQ(steps[1].expected_bps.front(), bp::battle::BattleMacroCommandTransitionDone);
    EXPECT_EQ(steps[2].input.buttons, savor::GC_A);
    ASSERT_EQ(steps[2].expected_bps.size(), 1u);
    EXPECT_EQ(steps[2].expected_bps.front(), bp::battle::BattleMacroDirectCommandQueued);
}

TEST(BattleMacroProbeCompiler, AttackCompilesTargetCursorMoves)
{
    FailureCode failure = FailureCode::Ok;
    const auto steps = BuildMacroSteps(MacroMode::Attack, 6, &failure);
    ASSERT_EQ(failure, FailureCode::Ok);
    ASSERT_EQ(steps.size(), 7u);
    EXPECT_EQ(steps[0].input.buttons, savor::GC_A);
    EXPECT_EQ(steps[0].expected_bps.front(), bp::battle::BattleMacroMainMenuAcceptDispatch);
    EXPECT_EQ(steps[1].input.buttons, 0u);
    EXPECT_EQ(steps[1].expected_bps.front(), bp::battle::BattleMacroEnemyTargetReady);
    EXPECT_EQ(steps[2].input.buttons, 0u);
    EXPECT_EQ(steps[2].expected_bps.front(), bp::battle::BattleMacroEnemyTargetReady);
    EXPECT_EQ(steps[3].input.buttons, savor::GC_DD);
    EXPECT_EQ(steps[3].expected_bps.front(), bp::battle::BattleMacroEnemyTargetMoveDownAccepted);
    EXPECT_EQ(steps[4].input.buttons, 0u);
    EXPECT_EQ(steps[4].expected_bps.front(), bp::battle::BattleMacroEnemyTargetReady);
    EXPECT_EQ(steps[5].input.buttons, savor::GC_DD);
    EXPECT_EQ(steps[5].expected_bps.front(), bp::battle::BattleMacroEnemyTargetMoveDownAccepted);
    EXPECT_EQ(steps[6].input.buttons, savor::GC_A);
    ASSERT_EQ(steps[6].expected_bps.size(), 1u);
    EXPECT_EQ(steps[6].expected_bps.front(), bp::battle::BattleMacroEnemyTargetFinalized);
    EXPECT_TRUE(steps[6].hold_input_through_hit_opcode);
}

TEST(BattleMacroProbeCompiler, AttackWithoutTargetMovementGetsSecondEnemyReadyWait)
{
    FailureCode failure = FailureCode::Ok;
    const auto steps = BuildMacroSteps(MacroMode::Attack, 4, &failure);
    ASSERT_EQ(failure, FailureCode::Ok);
    ASSERT_EQ(steps.size(), 4u);
    EXPECT_EQ(steps[0].expected_bps.front(), bp::battle::BattleMacroMainMenuAcceptDispatch);
    EXPECT_EQ(steps[1].expected_bps.front(), bp::battle::BattleMacroEnemyTargetReady);
    EXPECT_EQ(steps[2].expected_bps.front(), bp::battle::BattleMacroEnemyTargetReady);
    EXPECT_EQ(steps[3].input.buttons, savor::GC_A);
    EXPECT_EQ(steps[3].expected_bps.front(), bp::battle::BattleMacroEnemyTargetFinalized);
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
    EXPECT_EQ(steps[3].expected_bps.front(), bp::battle::BattleMacroEnemyTargetMoveDownAccepted);
    EXPECT_EQ(steps[4].expected_bps.front(), bp::battle::BattleMacroEnemyTargetReady);
    EXPECT_EQ(steps[5].expected_bps.front(), bp::battle::BattleMacroEnemyTargetMoveDownAccepted);
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
    EXPECT_EQ(steps[3].expected_bps.front(), bp::battle::BattleMacroEnemyTargetFinalized);
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
    EXPECT_EQ(steps[0].input.buttons, savor::GC_DU);
    EXPECT_EQ(steps[2].expected_bps.front(), bp::battle::BattleMacroDirectCommandQueued);
    EXPECT_EQ(steps[3].expected_bps.front(), bp::battle::BattleMacroInputReadyGate);
    EXPECT_EQ(steps[4].kind, MacroStep::Kind::NeutralFrames);
    EXPECT_EQ(steps[4].frame_count, 3u);
    EXPECT_EQ(steps[5].input.buttons, savor::GC_DU);
    EXPECT_EQ(steps[7].expected_bps.front(), bp::battle::BattleMacroDirectCommandQueued);
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
    EXPECT_EQ(steps[0].expected_bps.front(), bp::battle::BattleMacroMainMenuAcceptDispatch);
    EXPECT_EQ(steps[1].expected_bps.front(), bp::battle::BattleMacroEnemyTargetReady);
    EXPECT_EQ(steps[2].expected_bps.front(), bp::battle::BattleMacroEnemyTargetReady);
    EXPECT_EQ(steps[3].input.buttons, savor::GC_DD);
    EXPECT_EQ(steps[3].expected_bps.front(), bp::battle::BattleMacroEnemyTargetMoveDownAccepted);
    EXPECT_EQ(steps[4].input.buttons, 0u);
    EXPECT_EQ(steps[4].expected_bps.front(), bp::battle::BattleMacroEnemyTargetReady);
    EXPECT_EQ(steps[5].input.buttons, savor::GC_DD);
    EXPECT_EQ(steps[5].expected_bps.front(), bp::battle::BattleMacroEnemyTargetMoveDownAccepted);
    EXPECT_EQ(steps[6].expected_bps.front(), bp::battle::BattleMacroEnemyTargetFinalized);
    EXPECT_EQ(steps[7].expected_bps.front(), bp::battle::BattleMacroInputReadyGate);
    EXPECT_EQ(steps[8].kind, MacroStep::Kind::NeutralFrames);
    EXPECT_EQ(steps[9].input.buttons, savor::GC_DU);
    EXPECT_EQ(steps[11].expected_bps.front(), bp::battle::BattleMacroDirectCommandQueued);
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
        },
        payload));

    savor::PSContext ctx;
    ASSERT_TRUE(phase::battle::macroprobe::decode_payload(payload, ctx));

    std::uint32_t progress_flags = 0;
    ASSERT_TRUE(ctx.get(savor::context::key::core::PROGRESS_CORE_FLAGS, progress_flags));
    EXPECT_NE(progress_flags & static_cast<std::uint32_t>(CoreProgressFlags::BattleProgress), 0u);
    EXPECT_NE(progress_flags & static_cast<std::uint32_t>(CoreProgressFlags::DontRecordHeartbeat), 0u);
    std::uint32_t observation_tail_ms = 0;
    ASSERT_TRUE(ctx.get(savor::context::key::battle::MACRO_OBSERVATION_TAIL_MS, observation_tail_ms));
    EXPECT_EQ(observation_tail_ms, 17000u);
}

TEST(BattleMacroProbeProgram, ArmsInputReadyStartGate)
{
    const auto program = phase::battle::macroprobe::MakeBattleMacroProbeProgram();
    const auto has_canonical = [&](BPKey key) {
        return std::find(program.canonical_bp_keys.begin(), program.canonical_bp_keys.end(), key)
            != program.canonical_bp_keys.end();
    };
    const auto has_reserved = [&](BPKey key) {
        return std::find(program.reserved_bp_keys.begin(), program.reserved_bp_keys.end(), key)
            != program.reserved_bp_keys.end();
    };

    EXPECT_TRUE(has_canonical(bp::battle::TurnInputs));
    EXPECT_TRUE(has_canonical(bp::battle::TurnIsReady));
    EXPECT_FALSE(has_canonical(bp::battle::BattleMacroInputReadyGate));
    EXPECT_TRUE(has_reserved(bp::battle::BattleMacroInputReadyGate));
    EXPECT_TRUE(has_reserved(bp::battle::BattleMacroMainMenuAcceptDispatch));
    EXPECT_TRUE(has_reserved(bp::battle::BattleMacroMagicReady));
    EXPECT_TRUE(has_reserved(bp::battle::BattleMacroSMoveReady));
    EXPECT_TRUE(has_reserved(bp::battle::BattleMacroConditionalRunReady));
    EXPECT_TRUE(has_reserved(bp::battle::BattleMacroItemCategoryReady));
    EXPECT_TRUE(has_reserved(bp::battle::BattleMacroItemRowListReady));
    EXPECT_TRUE(has_reserved(bp::battle::BattleMacroItemDetailReady));
    EXPECT_TRUE(has_reserved(bp::battle::BattleMacroEnemyTargetReady));
    EXPECT_TRUE(has_reserved(bp::battle::BattleMacroAllyTargetReady));
    EXPECT_TRUE(has_reserved(bp::battle::BattleMacroEnemyTargetMoveDownAccepted));
    EXPECT_TRUE(has_reserved(bp::battle::BattleMacroEnemyTargetMoveUpAccepted));
    EXPECT_TRUE(has_reserved(bp::battle::BattleMacroEnemyTargetFinalized));
    EXPECT_FALSE(has_canonical(bp::battle::BattleMacroMainMenuAcceptDispatch));
    EXPECT_FALSE(has_canonical(bp::battle::BattleMacroDirectCommandQueued));
    EXPECT_FALSE(has_reserved(bp::battle::BattleMacroEnemyTargetCursorMoved));
    EXPECT_FALSE(has_reserved(bp::battle::BattleMacroEnemyTargetWritten));
    ASSERT_GT(program.ops.size(), 15u);
    EXPECT_EQ(program.ops[4].code, savor::PSOpCode::STEP_OPCODE);
    EXPECT_EQ(program.ops[4].imm.v, 1u);
    EXPECT_EQ(program.ops[5].code, savor::PSOpCode::SET_TIMEOUT);
    EXPECT_EQ(program.ops[5].imm.v, 1000u);
    EXPECT_EQ(program.ops[6].code, savor::PSOpCode::RUN_UNTIL_BP);
    EXPECT_EQ(program.ops[7].code, savor::PSOpCode::GOTO_IF);
    EXPECT_EQ(program.ops[7].jcc.key, savor::context::key::core::RUN_HIT_BP_KEY);
    EXPECT_EQ(program.ops[7].jcc.cmp, savor::PSCmp::EQ);
    EXPECT_EQ(program.ops[7].jcc.imm, static_cast<std::uint32_t>(bp::battle::TurnIsReady));
    EXPECT_EQ(program.ops[8].code, savor::PSOpCode::GOTO);
    EXPECT_EQ(program.ops[9].code, savor::PSOpCode::LABEL);
    EXPECT_EQ(program.ops[10].code, savor::PSOpCode::STEP_OPCODE);
    EXPECT_EQ(program.ops[10].imm.v, 1u);
    EXPECT_EQ(program.ops[11].code, savor::PSOpCode::SET_TIMEOUT_FROM);
    EXPECT_EQ(program.ops[11].key.id, savor::context::key::battle::MACRO_OBSERVATION_TAIL_MS);
    EXPECT_EQ(program.ops[12].code, savor::PSOpCode::RUN_UNTIL_BP);
    EXPECT_EQ(program.ops[13].code, savor::PSOpCode::LABEL);
    EXPECT_EQ(program.ops[14].code, savor::PSOpCode::SET_U32);
    EXPECT_EQ(program.ops[14].keyimm.key, savor::context::key::core::DW_RUN_OUTCOME_CODE);
    EXPECT_EQ(program.ops[14].keyimm.imm, 0u);
    EXPECT_EQ(program.ops[15].code, savor::PSOpCode::LABEL);
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

} // namespace
