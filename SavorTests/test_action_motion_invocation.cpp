#include "../SavorPredict/ActionMotionInvocationModel.h"

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <vector>

namespace {

using namespace savor::predict;

std::vector<CombatantStdActionRow> action_rows() {
    return {
        {.index = 0, .action_id = 2, .row_type = 1,
         .callback_index = 8, .callback_ordinal = 10,
         .transition_gate_divisor_bits = 0x40a00000u},
        {.index = 1, .action_id = 3, .row_type = 1,
         .callback_index = 8, .callback_ordinal = 11,
         .transition_gate_divisor_bits = 0x40a00000u},
        {.index = 2, .action_id = 4, .row_type = 1,
         .callback_index = 8, .callback_ordinal = 12,
         .transition_gate_divisor_bits = 0x40a00000u},
        {.index = 3, .action_id = 5, .row_type = 1,
         .callback_index = 8, .callback_ordinal = 13,
         .transition_gate_divisor_bits = 0x40a00000u},
        {.index = 4, .action_id = 7, .row_type = 1,
         .callback_index = 13, .callback_ordinal = 14,
         .transition_gate_divisor_bits = 0x40a00000u},
        {.index = 5, .action_id = 11, .row_type = 1,
         .callback_index = 10, .callback_ordinal = 15,
         .transition_gate_divisor_bits = 0x40a00000u},
        {.index = 6, .action_id = 0x15, .row_type = 1,
         .callback_index = 8, .callback_ordinal = 16,
         .transition_gate_divisor_bits = 0x40a00000u},
        {.index = 7, .action_id = 0x18, .row_type = 1,
         .callback_index = 8, .callback_ordinal = 17,
         .secondary_key = 9,
         .transition_gate_divisor_bits = 0x40a00000u},
        {.index = 8, .action_id = 6, .row_type = 1,
         .callback_index = 8, .callback_ordinal = 18,
         .transition_gate_divisor_bits = 0},
        {.index = 9, .action_id = -1, .row_type = 3},
        {.index = 10, .action_id = 5, .row_type = 1,
         .callback_index = 8, .callback_ordinal = 99,
         .transition_gate_divisor_bits = 0x40a00000u},
    };
}

ActionMotionRowResolverRequest exact_request(std::int16_t mode) {
    return {
        .requested_mode = mode,
        .selection_blocked = false,
        .current_motion_resource_present = false,
        .instruction_flags_0xf0 = 0u,
    };
}

ActionMotionInvocationRequest exact_invocation(
    ActionMotionPersistentCallbackFamily family,
    int state,
    std::int16_t mode) {
    return {
        .callback_family = family,
        .callback_state = state,
        .instruction_mode = mode,
        .instruction_flags_0xf0 = 0,
        .callback_ready = true,
        .basic_uses_mode_3_route = false,
        .rotation_complete = true,
        .selection_blocked = false,
        .current_motion_resource_present = false,
    };
}

TEST(SavorPredictActionMotionInvocation, ResolverSelectsInstallEligibleRow) {
    const auto resolved = resolve_action_motion_row_8001ecb4(
        action_rows(), exact_request(5));
    EXPECT_EQ(resolved.status, ActionMotionInvocationStatus::Matched);
    EXPECT_EQ(resolved.code, ActionMotionResolverCode::InstallPlayback);
    ASSERT_TRUE(resolved.row.has_value());
    EXPECT_EQ(resolved.row->index, 3);
    EXPECT_EQ(resolved.resolved_motion_id, 13);
}

TEST(SavorPredictActionMotionInvocation, ResolverNormalizesMode16To15) {
    const auto resolved = resolve_action_motion_row_8001ecb4(
        action_rows(), exact_request(0x16));
    EXPECT_EQ(resolved.code, ActionMotionResolverCode::InstallPlayback);
    EXPECT_EQ(resolved.normalized_mode, 0x15);
    ASSERT_TRUE(resolved.row.has_value());
    EXPECT_EQ(resolved.row->index, 6);
}

TEST(SavorPredictActionMotionInvocation, ResolverStopsAtType3Terminator) {
    const auto resolved = resolve_action_motion_row_8001ecb4(
        action_rows(), exact_request(5));
    ASSERT_TRUE(resolved.row.has_value());
    EXPECT_EQ(resolved.row->index, 3);
    EXPECT_NE(resolved.row->callback_ordinal, 99);
}

TEST(SavorPredictActionMotionInvocation, ResolverCurrentMotionAndBlockedRowsReturnZero) {
    auto current = exact_request(5);
    current.current_motion_resource_present = true;
    current.current_motion_id = 13;
    const auto same = resolve_action_motion_row_8001ecb4(action_rows(), current);
    EXPECT_EQ(same.code, ActionMotionResolverCode::NoChange);

    auto blocked = exact_request(5);
    blocked.selection_blocked = true;
    const auto gate = resolve_action_motion_row_8001ecb4(action_rows(), blocked);
    EXPECT_EQ(gate.code, ActionMotionResolverCode::NoChange);
}

TEST(SavorPredictActionMotionInvocation, ResolverZeroDurationReturnsLoadCode) {
    const auto resolved = resolve_action_motion_row_8001ecb4(
        action_rows(), exact_request(6));
    EXPECT_EQ(resolved.status, ActionMotionInvocationStatus::Matched);
    EXPECT_EQ(resolved.code, ActionMotionResolverCode::LoadWithoutPlayback);
}

TEST(SavorPredictActionMotionInvocation, ResolverRequiresSecondaryKey) {
    const auto missing = resolve_action_motion_row_8001ecb4(
        action_rows(), exact_request(0x18));
    EXPECT_EQ(missing.status, ActionMotionInvocationStatus::MissingInput);

    auto present = exact_request(0x18);
    present.secondary_key = 9;
    const auto resolved = resolve_action_motion_row_8001ecb4(
        action_rows(), present);
    EXPECT_EQ(resolved.code, ActionMotionResolverCode::InstallPlayback);
    ASSERT_TRUE(resolved.row.has_value());
    EXPECT_EQ(resolved.row->index, 7);
}

TEST(SavorPredictActionMotionInvocation, BasicState3InstallsOnlyForResultTwo) {
    const auto installed = resolve_action_motion_invocation(
        action_rows(),
        exact_invocation(
            ActionMotionPersistentCallbackFamily::ActionMotionBasic_8001B1B0,
            3,
            5));
    EXPECT_EQ(installed.status, ActionMotionInvocationStatus::Matched);
    EXPECT_EQ(
        installed.decision,
        ActionMotionInvocationDecisionKind::InstallPlayback);
    EXPECT_EQ(installed.callback_state_after, 6);
    EXPECT_EQ(
        installed.playback_continuation,
        ActionMotionPlaybackContinuation::State6PostDelayTo11);

    auto load_request = exact_invocation(
        ActionMotionPersistentCallbackFamily::ActionMotionBasic_8001B1B0,
        3,
        6);
    const auto loaded = resolve_action_motion_invocation(
        action_rows(), load_request);
    EXPECT_EQ(loaded.resolver.code, ActionMotionResolverCode::LoadWithoutPlayback);
    EXPECT_EQ(
        loaded.decision,
        ActionMotionInvocationDecisionKind::LoadSelected);
    EXPECT_EQ(loaded.callback_state_after, 8);
}

TEST(SavorPredictActionMotionInvocation, BasicMode3RouteUsesState5Continuation) {
    auto request = exact_invocation(
        ActionMotionPersistentCallbackFamily::ActionMotionBasic_8001B1B0,
        3,
        5);
    request.basic_uses_mode_3_route = true;
    const auto result = resolve_action_motion_invocation(action_rows(), request);
    EXPECT_EQ(result.decision, ActionMotionInvocationDecisionKind::InstallPlayback);
    EXPECT_EQ(result.callback_state_after, 5);
    EXPECT_EQ(
        result.playback_continuation,
        ActionMotionPlaybackContinuation::State5LoadLookedUpTo4);
}

TEST(SavorPredictActionMotionInvocation, BasicState11ResultTwoLoadsWithoutInstalling) {
    auto request = exact_invocation(
        ActionMotionPersistentCallbackFamily::ActionMotionBasic_8001B1B0,
        11,
        5);
    request.post_motion_result = 2;
    const auto result = resolve_action_motion_invocation(action_rows(), request);
    EXPECT_EQ(result.resolver.code, ActionMotionResolverCode::InstallPlayback);
    EXPECT_EQ(result.decision, ActionMotionInvocationDecisionKind::LoadLookedUp);
    EXPECT_EQ(result.callback_state_after, 12);
}

TEST(SavorPredictActionMotionInvocation, BasicState8DelayReachesState11InPpcOrder) {
    auto state8 = exact_invocation(
        ActionMotionPersistentCallbackFamily::ActionMotionBasic_8001B1B0,
        8,
        5);
    state8.state8_descriptor_delay = 2;
    auto result = resolve_action_motion_invocation(action_rows(), state8);
    EXPECT_EQ(result.decision, ActionMotionInvocationDecisionKind::Wait);
    EXPECT_EQ(result.callback_state_after, 9);
    EXPECT_EQ(result.state8_delay_remaining, 1);

    auto state9 = state8;
    state9.callback_state = 9;
    state9.state8_descriptor_delay.reset();
    state9.state8_delay_remaining = result.state8_delay_remaining;
    result = resolve_action_motion_invocation(action_rows(), state9);
    EXPECT_EQ(result.callback_state_after, 9);
    EXPECT_EQ(result.state8_delay_remaining, 0);

    state9.state8_delay_remaining = result.state8_delay_remaining;
    result = resolve_action_motion_invocation(action_rows(), state9);
    EXPECT_EQ(result.callback_state_after, 11);
    EXPECT_EQ(result.state8_delay_remaining, 0);
}

TEST(SavorPredictActionMotionInvocation, SpecialState1ResultTwoLoadsWithoutInstalling) {
    auto request = exact_invocation(
        ActionMotionPersistentCallbackFamily::ActionMotionSpecial_8001A4F0,
        1,
        11);
    request.post_motion_result = 2;
    const auto result = resolve_action_motion_invocation(action_rows(), request);
    EXPECT_EQ(result.resolver.code, ActionMotionResolverCode::InstallPlayback);
    EXPECT_EQ(result.decision, ActionMotionInvocationDecisionKind::LoadLookedUp);
    EXPECT_EQ(result.callback_state_after, 4);
}

TEST(SavorPredictActionMotionInvocation, InitialSyncCallbackDoesNotInstallPlayback) {
    const auto result = resolve_action_motion_invocation(
        action_rows(),
        exact_invocation(
            ActionMotionPersistentCallbackFamily::ActionMotionSync_8001AB60,
            0,
            1));
    EXPECT_EQ(result.status, ActionMotionInvocationStatus::Provisional);
    EXPECT_EQ(result.decision, ActionMotionInvocationDecisionKind::Wait);
    EXPECT_FALSE(result.resolver_called);
    EXPECT_FALSE(result.operation_row.has_value());
    EXPECT_EQ(result.callback_state_after, 0);
}

TEST(SavorPredictActionMotionInvocation, SpecialState9AndBasicState15Install) {
    const auto special = resolve_action_motion_invocation(
        action_rows(),
        exact_invocation(
            ActionMotionPersistentCallbackFamily::ActionMotionSpecial_8001A4F0,
            9,
            11));
    EXPECT_EQ(special.decision, ActionMotionInvocationDecisionKind::InstallPlayback);
    EXPECT_EQ(special.callback_state_after, 10);

    const auto basic = resolve_action_motion_invocation(
        action_rows(),
        exact_invocation(
            ActionMotionPersistentCallbackFamily::ActionMotionBasic_8001B1B0,
            15,
            5));
    EXPECT_EQ(basic.decision, ActionMotionInvocationDecisionKind::InstallPlayback);
    EXPECT_EQ(basic.callback_state_after, 7);
}

TEST(SavorPredictActionMotionInvocation, CallbackIndexMappingUsesExecutableTable) {
    EXPECT_EQ(
        action_motion_callback_family_for_index(8),
        ActionMotionPersistentCallbackFamily::ActionMotionBasic_8001B1B0);
    EXPECT_EQ(
        action_motion_callback_family_for_index(10),
        ActionMotionPersistentCallbackFamily::ActionMotionSpecial_8001A4F0);
    EXPECT_EQ(
        action_motion_callback_family_for_index(13),
        ActionMotionPersistentCallbackFamily::ActionMotionRanged_80019F0C);
    EXPECT_EQ(
        action_motion_callback_family_for_index(99),
        ActionMotionPersistentCallbackFamily::Unknown);
}

} // namespace
