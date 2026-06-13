#include <gtest/gtest.h>

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
        },
        payload));

    savor::PSContext ctx;
    ASSERT_TRUE(phase::battle::macroprobe::decode_payload(payload, ctx));

    std::uint32_t progress_flags = 0;
    ASSERT_TRUE(ctx.get(savor::context::key::core::PROGRESS_CORE_FLAGS, progress_flags));
    EXPECT_NE(progress_flags & static_cast<std::uint32_t>(CoreProgressFlags::BattleProgress), 0u);
    EXPECT_NE(progress_flags & static_cast<std::uint32_t>(CoreProgressFlags::DontRecordHeartbeat), 0u);
}

TEST(BattleMacroProbeProgram, ArmsInputReadyStartGate)
{
    const auto program = phase::battle::macroprobe::MakeBattleMacroProbeProgram();
    EXPECT_NE(
        std::find(
            program.canonical_bp_keys.begin(),
            program.canonical_bp_keys.end(),
            bp::battle::BattleMacroInputReadyGate),
        program.canonical_bp_keys.end());
    EXPECT_NE(
        std::find(
            program.canonical_bp_keys.begin(),
            program.canonical_bp_keys.end(),
            bp::battle::BattleMacroMainMenuAcceptDispatch),
        program.canonical_bp_keys.end());
    EXPECT_NE(
        std::find(
            program.canonical_bp_keys.begin(),
            program.canonical_bp_keys.end(),
            bp::battle::BattleMacroMagicReady),
        program.canonical_bp_keys.end());
    EXPECT_NE(
        std::find(
            program.canonical_bp_keys.begin(),
            program.canonical_bp_keys.end(),
            bp::battle::BattleMacroSMoveReady),
        program.canonical_bp_keys.end());
    EXPECT_NE(
        std::find(
            program.canonical_bp_keys.begin(),
            program.canonical_bp_keys.end(),
            bp::battle::BattleMacroConditionalRunReady),
        program.canonical_bp_keys.end());
    EXPECT_NE(
        std::find(
            program.canonical_bp_keys.begin(),
            program.canonical_bp_keys.end(),
            bp::battle::BattleMacroItemCategoryReady),
        program.canonical_bp_keys.end());
    EXPECT_NE(
        std::find(
            program.canonical_bp_keys.begin(),
            program.canonical_bp_keys.end(),
            bp::battle::BattleMacroItemRowListReady),
        program.canonical_bp_keys.end());
    EXPECT_NE(
        std::find(
            program.canonical_bp_keys.begin(),
            program.canonical_bp_keys.end(),
            bp::battle::BattleMacroItemDetailReady),
        program.canonical_bp_keys.end());
    EXPECT_NE(
        std::find(
            program.canonical_bp_keys.begin(),
            program.canonical_bp_keys.end(),
            bp::battle::BattleMacroEnemyTargetReady),
        program.canonical_bp_keys.end());
    EXPECT_NE(
        std::find(
            program.canonical_bp_keys.begin(),
            program.canonical_bp_keys.end(),
            bp::battle::BattleMacroAllyTargetReady),
        program.canonical_bp_keys.end());
    EXPECT_NE(
        std::find(
            program.canonical_bp_keys.begin(),
            program.canonical_bp_keys.end(),
            bp::battle::BattleMacroEnemyTargetMoveDownAccepted),
        program.canonical_bp_keys.end());
    EXPECT_NE(
        std::find(
            program.canonical_bp_keys.begin(),
            program.canonical_bp_keys.end(),
            bp::battle::BattleMacroEnemyTargetMoveUpAccepted),
        program.canonical_bp_keys.end());
    EXPECT_EQ(
        std::find(
            program.canonical_bp_keys.begin(),
            program.canonical_bp_keys.end(),
            bp::battle::BattleMacroEnemyTargetCursorMoved),
        program.canonical_bp_keys.end());
    EXPECT_EQ(
        std::find(
            program.canonical_bp_keys.begin(),
            program.canonical_bp_keys.end(),
            bp::battle::BattleMacroEnemyTargetWritten),
        program.canonical_bp_keys.end());
    EXPECT_NE(
        std::find(
            program.canonical_bp_keys.begin(),
            program.canonical_bp_keys.end(),
            bp::battle::BattleMacroEnemyTargetFinalized),
        program.canonical_bp_keys.end());
    ASSERT_GT(program.ops.size(), 6u);
    EXPECT_EQ(program.ops[4].code, savor::PSOpCode::STEP_OPCODE);
    EXPECT_EQ(program.ops[4].imm.v, 1u);
    EXPECT_EQ(program.ops[5].code, savor::PSOpCode::SET_TIMEOUT);
    EXPECT_EQ(program.ops[5].imm.v, 1000u);
    EXPECT_EQ(program.ops[6].code, savor::PSOpCode::RUN_UNTIL_BP);
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
