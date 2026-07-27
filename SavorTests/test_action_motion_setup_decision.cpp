#include "../SavorPredict/ActionMotionSetupDecisionModel.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cstdint>

namespace {

using namespace savor::predict;

CombatantStdActionRow row(
    std::int16_t mode,
    std::uint32_t flags = 0) {
    return CombatantStdActionRow{
        .index = 3,
        .action_id = mode,
        .row_type = 1,
        .callback_index = 8,
        .callback_ordinal = 12,
        .flags = flags,
        .transition_gate_divisor_bits = 0x40a00000u,
    };
}

ActionMotionSetupInput base_input(std::int16_t mode) {
    return ActionMotionSetupInput{
        .actor_slot = 1,
        .target_slot = 4,
        .instruction_mode = mode,
        .turn_phase = 4,
        .selected_row = row(mode),
        .instruction_flags_0xec = 0,
        .instruction_flags_0xf0 = 0,
        .combatant_status_flag_0x400_set = false,
        .current_position =
            BattleFrameVec3{.x = 15.0f, .y = 0.0f, .z = 15.0f},
        .own_pos_holder =
            BattleFrameVec3{.x = 15.0f, .y = 0.0f, .z = 30.0f},
        .target_current_position =
            BattleFrameVec3{.x = -15.0f, .y = 0.0f, .z = -45.0f},
        .secondary_target_current_position =
            BattleFrameVec3{.x = 0.0f, .y = 0.0f, .z = 45.0f},
        .slot_zero_current_position =
            BattleFrameVec3{.x = -15.0f, .y = 0.0f, .z = 0.0f},
        .current_facing_angle = 0x8000u,
        .turn_speed_degrees =
            std::bit_cast<float>(std::uint32_t{0x41B0CCC4u}),
        .motion_base_speed = 2.55f,
        .motion_alt_speed = 0.45f,
    };
}

const ActionMotionSetupOperation* find_operation(
    const ActionMotionSetupResult& result,
    ActionMotionSetupOperationKind kind,
    ActionMotionSetupOperationStage stage =
        ActionMotionSetupOperationStage::Setup) {
    const auto found = std::find_if(
        result.operations.begin(),
        result.operations.end(),
        [kind, stage](const ActionMotionSetupOperation& operation) {
            return operation.kind == kind && operation.stage == stage;
        });
    return found == result.operations.end() ? nullptr : &*found;
}

TEST(SavorPredictActionMotionSetupDecision, ModesFourFiveAndEightUseTargetCurrentPosition) {
    for (const auto mode : {std::int16_t{4}, std::int16_t{5}, std::int16_t{8}}) {
        const auto result = model_action_motion_setup_8001fabc(base_input(mode));
        ASSERT_TRUE(result.target.target.has_value());
        EXPECT_EQ(
            result.target.source,
            ActionMotionTargetSource::TargetCombatantCurrentPosition);
        EXPECT_FLOAT_EQ(result.target.target->x, -15.0f);
        EXPECT_FLOAT_EQ(result.target.target->z, -45.0f);
    }
}

TEST(SavorPredictActionMotionSetupDecision, MissingTargetUsesValidatedSlotZeroFallback) {
    auto input = base_input(5);
    input.target_current_position.reset();

    const auto result = model_action_motion_setup_8001fabc(input);

    ASSERT_TRUE(result.target.target.has_value());
    EXPECT_EQ(
        result.target.source,
        ActionMotionTargetSource::SlotZeroCurrentPositionFallback);
    EXPECT_FLOAT_EQ(result.target.target->x, -15.0f);
    EXPECT_FLOAT_EQ(result.target.target->z, 0.0f);
}

TEST(SavorPredictActionMotionSetupDecision, ModeThirteenDecimalUsesSecondaryTargetField) {
    auto input = base_input(0x0d);

    const auto result = model_action_motion_setup_8001fabc(input);

    ASSERT_TRUE(result.target.target.has_value());
    EXPECT_EQ(
        result.target.source,
        ActionMotionTargetSource::
            SecondaryTargetCombatantCurrentPosition);
    EXPECT_FLOAT_EQ(result.target.target->x, 0.0f);
    EXPECT_FLOAT_EQ(result.target.target->z, 45.0f);
    EXPECT_NE(result.route, ActionMotionSetupRoute::Unknown);
}

TEST(SavorPredictActionMotionSetupDecision, CapturedModeFiveVectorSelectsModeThreeRoute) {
    auto input = base_input(5);
    input.selected_row = row(5, 0x88000000u);

    const auto result = model_action_motion_setup_8001fabc(input);

    EXPECT_EQ(result.status, ActionMotionSetupStatus::Matched);
    EXPECT_EQ(
        result.branch,
        ActionMotionSetupBranch::InstallRotationPlayback);
    EXPECT_EQ(result.route, ActionMotionSetupRoute::Mode3Resolver);
    ASSERT_NE(
        find_operation(
            result,
            ActionMotionSetupOperationKind::PublishTarget),
        nullptr);
    const auto* turn = find_operation(
        result,
        ActionMotionSetupOperationKind::PublishTurnState);
    ASSERT_NE(turn, nullptr);
    EXPECT_EQ(
        std::bit_cast<std::uint32_t>(turn->turn_current_degrees),
        0x43340000u);
    EXPECT_EQ(
        std::bit_cast<std::uint32_t>(turn->turn_target_degrees),
        0x434E9208u);
    EXPECT_EQ(
        std::bit_cast<std::uint32_t>(turn->turn_step_degrees),
        0x41B0CCC4u);
    ASSERT_NE(
        find_operation(
            result,
            ActionMotionSetupOperationKind::SetInstructionFlagsF0,
            ActionMotionSetupOperationStage::AfterResolver),
        nullptr);
}

TEST(SavorPredictActionMotionSetupDecision, ModeSixPublishesOwnPosHolderIncrement) {
    auto input = base_input(6);
    input.current_position =
        BattleFrameVec3{.x = -15.0f, .y = 0.0f, .z = 0.0f};
    input.own_pos_holder =
        BattleFrameVec3{.x = 0.0f, .y = 0.0f, .z = -15.0f};
    input.selected_row = row(6, 0x81200000u);

    const auto result = model_action_motion_setup_8001fabc(input);

    ASSERT_TRUE(result.target.target.has_value());
    EXPECT_EQ(result.target.source, ActionMotionTargetSource::OwnPosHolder);
    const auto* increment = find_operation(
        result,
        ActionMotionSetupOperationKind::PublishMoveIncrement);
    ASSERT_NE(increment, nullptr);
    EXPECT_NEAR(increment->vector.x, 1.803122f, 0.00001f);
    EXPECT_NEAR(increment->vector.z, -1.803122f, 0.00001f);
    EXPECT_EQ(result.selected_motion_speed, 2.55f);
}

TEST(SavorPredictActionMotionSetupDecision, SuppressionPublishesTargetBeforeCurrentModeRoute) {
    auto input = base_input(5);
    input.selected_row = row(5, 0x10000000u);
    input.instruction_flags_0xf0 = 0x00000400u;

    const auto result = model_action_motion_setup_8001fabc(input);

    EXPECT_EQ(
        result.branch,
        ActionMotionSetupBranch::SelectedRowSuppression);
    EXPECT_EQ(result.route, ActionMotionSetupRoute::CurrentModeResolver);
    ASSERT_GE(result.operations.size(), 2u);
    EXPECT_EQ(
        result.operations.front().kind,
        ActionMotionSetupOperationKind::PublishTarget);
    EXPECT_EQ(
        result.operations.back().kind,
        ActionMotionSetupOperationKind::ClearInstructionFlagsF0);
    EXPECT_EQ(
        result.operations.back().stage,
        ActionMotionSetupOperationStage::BeforeResolver);
}

TEST(SavorPredictActionMotionSetupDecision, ReadinessBypassPublishesNoSetupWrites) {
    auto input = base_input(5);
    input.instruction_flags_0xf0 = 0x00000800u;

    const auto result = model_action_motion_setup_8001fabc(input);

    EXPECT_EQ(result.status, ActionMotionSetupStatus::Matched);
    EXPECT_EQ(result.branch, ActionMotionSetupBranch::State8Bypass);
    EXPECT_EQ(result.route, ActionMotionSetupRoute::State8Bypass);
    EXPECT_TRUE(result.operations.empty());
}

TEST(SavorPredictActionMotionSetupDecision, BelowThresholdSnapsFacingAndSetsBit200) {
    auto input = base_input(5);
    input.target_current_position =
        BattleFrameVec3{.x = 15.1f, .y = 0.0f, .z = -15.0f};
    input.turn_speed_degrees = 30.0f;

    const auto result = model_action_motion_setup_8001fabc(input);

    EXPECT_EQ(
        result.branch,
        ActionMotionSetupBranch::BelowRotationThreshold);
    EXPECT_EQ(result.route, ActionMotionSetupRoute::CurrentModeResolver);
    ASSERT_NE(
        find_operation(
            result,
            ActionMotionSetupOperationKind::PublishFacingAngle),
        nullptr);
    const auto* set_f0 = find_operation(
        result,
        ActionMotionSetupOperationKind::SetInstructionFlagsF0);
    ASSERT_NE(set_f0, nullptr);
    EXPECT_EQ(set_f0->flags_mask, 0x00000200u);
}

} // namespace
