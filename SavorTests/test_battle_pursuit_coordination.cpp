#include "../SavorPredict/BattlePursuitCoordinationModel.h"

#include <gtest/gtest.h>

namespace savor::predict {
namespace {

TEST(BattlePursuitCoordinationModel, PeerReadyPublishesCapturedState17Mutation) {
    const auto result = model_battle_pursuit_coordination({
        .owner_state_0x50 = 2,
        .peer_state_0x50 = 1,
        .owner_countdown_0x51 = 0,
        .peer_countdown_0x51 = 0,
    });

    EXPECT_EQ(result.status, BattlePursuitCoordinationStatus::Matched);
    EXPECT_EQ(
        result.branch,
        BattlePursuitCoordinationBranch::PeerReadyPublication);
    EXPECT_TRUE(result.publish_state17);
    EXPECT_EQ(result.owner_state_after_0x50, 0x22);
    EXPECT_EQ(result.peer_state_after_0x50, 0x11);
    EXPECT_EQ(result.owner_countdown_after_0x51, 8);
}

TEST(BattlePursuitCoordinationModel, CoordinatedPairPublishesAtNonpositiveCountdown) {
    const auto result = model_battle_pursuit_coordination({
        .owner_state_0x50 = 2,
        .peer_state_0x50 = 2,
        .owner_countdown_0x51 = 0,
        .peer_countdown_0x51 = 0,
    });

    EXPECT_EQ(result.status, BattlePursuitCoordinationStatus::Matched);
    EXPECT_EQ(
        result.branch,
        BattlePursuitCoordinationBranch::CoordinatedPublication);
    EXPECT_TRUE(result.publish_state17);
    EXPECT_EQ(result.owner_state_after_0x50, 0x22);
    EXPECT_EQ(result.peer_state_after_0x50, 0x12);
    EXPECT_EQ(result.owner_countdown_after_0x51, 8);
}

TEST(BattlePursuitCoordinationModel, CountdownBranchesRemainTypedProvisional) {
    const auto ordered_wait = model_battle_pursuit_coordination({
        .owner_state_0x50 = 2,
        .peer_state_0x50 = 2,
        .owner_countdown_0x51 = 3,
        .peer_countdown_0x51 = 2,
    });
    EXPECT_EQ(
        ordered_wait.branch,
        BattlePursuitCoordinationBranch::CountdownOrderingWait);
    EXPECT_FALSE(ordered_wait.publish_state17);

    const auto decrement = model_battle_pursuit_coordination({
        .owner_state_0x50 = 2,
        .peer_state_0x50 = 2,
        .owner_countdown_0x51 = 2,
        .peer_countdown_0x51 = 3,
    });
    EXPECT_EQ(
        decrement.branch,
        BattlePursuitCoordinationBranch::CountdownDecrement);
    EXPECT_FALSE(decrement.publish_state17);
    EXPECT_EQ(decrement.owner_countdown_after_0x51, 1);
}

TEST(BattlePursuitCoordinationModel, State17PollAcceptsMode4And8AndRejectsOtherModes) {
    for (const std::int16_t mode : {std::int16_t{4}, std::int16_t{8}}) {
        const auto result = model_battle_pursuit_instruction_poll({
            .queued_field9 = 1,
            .instruction_mode = mode,
        });
        ASSERT_TRUE(result.terminal_result.has_value());
        EXPECT_EQ(*result.terminal_result, 1);
        EXPECT_EQ(
            result.reason,
            BattlePursuitInstructionPollReason::QueuedField9ModeMatch);
    }

    const auto mismatch = model_battle_pursuit_instruction_poll({
        .queued_field9 = 1,
        .instruction_mode = 2,
    });
    ASSERT_TRUE(mismatch.terminal_result.has_value());
    EXPECT_EQ(*mismatch.terminal_result, -1);
    EXPECT_EQ(
        mismatch.reason,
        BattlePursuitInstructionPollReason::QueuedField9ModeMismatch);
}

TEST(BattlePursuitCoordinationModel, UnknownReadinessWaitsWithoutInventingCounterProgress) {
    const auto result = model_battle_pursuit_instruction_poll({
        .queued_field9 = 0,
        .instruction_mode = 4,
        .readiness = std::nullopt,
        .turn_phase = 5,
        .fallback_counter = 19,
    });

    EXPECT_FALSE(result.terminal_result.has_value());
    EXPECT_EQ(
        result.reason,
        BattlePursuitInstructionPollReason::ReadinessUnknown);
    EXPECT_EQ(result.fallback_counter_after, 19);
}

TEST(BattlePursuitCoordinationModel, ReadinessAndTurnPhaseGuardFallbackCounter) {
    const auto not_ready = model_battle_pursuit_instruction_poll({
        .queued_field9 = 0,
        .instruction_mode = 4,
        .readiness = false,
        .turn_phase = 5,
        .fallback_counter = 7,
    });
    EXPECT_EQ(
        not_ready.reason,
        BattlePursuitInstructionPollReason::ReadinessFalse);
    EXPECT_EQ(not_ready.fallback_counter_after, 7);

    const auto guarded = model_battle_pursuit_instruction_poll({
        .queued_field9 = 0,
        .instruction_mode = 4,
        .readiness = true,
        .turn_phase = 4,
        .fallback_counter = 7,
    });
    EXPECT_EQ(
        guarded.reason,
        BattlePursuitInstructionPollReason::GuardedTurnPhase);
    EXPECT_EQ(guarded.fallback_counter_after, 7);
}

TEST(BattlePursuitCoordinationModel, StaticForcedReadyThresholdIsExplicitlyProvisional) {
    const auto result = model_battle_pursuit_instruction_poll({
        .queued_field9 = 0,
        .instruction_mode = 4,
        .readiness = true,
        .turn_phase = 5,
        .fallback_counter = 240,
    });

    EXPECT_EQ(
        result.reason,
        BattlePursuitInstructionPollReason::FallbackForcedReady);
    ASSERT_TRUE(result.terminal_result.has_value());
    EXPECT_EQ(*result.terminal_result, 1);
    EXPECT_EQ(result.queued_field9_after, 1);
    EXPECT_EQ(result.fallback_counter_after, 0);
    EXPECT_EQ(result.status, BattlePursuitCoordinationStatus::Provisional);
}

} // namespace
} // namespace savor::predict
