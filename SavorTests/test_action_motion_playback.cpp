#include "../SavorPredict/ActionMotionPlaybackModel.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace {

using namespace savor::predict;

ActionMotionPlaybackRuntime install_duration(
    std::uint32_t duration_bits,
    std::uint32_t flags_f0 = 0) {
    const auto installed = install_action_motion_playback({
        .action_ordinal = 3,
        .slot = 0,
        .instruction_state_revision = 7,
        .selected_action_row_index = 4,
        .selected_action_row_duration_bits = duration_bits,
        .instruction_flags_0xec = 0x00180000u,
        .instruction_flags_0xf0 = flags_f0,
        .provenance = "captured action-row playback fixture",
    });
    EXPECT_TRUE(installed.installed);
    EXPECT_TRUE(installed.blocks_publication);
    EXPECT_EQ(installed.runtime.status, ActionMotionPlaybackStatus::Matched);
    EXPECT_EQ(installed.runtime.phase, ActionMotionPlaybackPhase::Primed);
    EXPECT_EQ(installed.flags_after, 0x80180000u);
    return installed.runtime;
}

ActionMotionPlaybackVisitInput no_delay_visit_input() {
    return {
        .post_state6_delay = ActionMotionDelayLookupResult{
            .status = ActionMotionDelayStatus::NoMatch,
            .delay = 0,
            .gate_result = false,
            .provenance = "fixture descriptor terminator",
        },
    };
}

ActionMotionDelayTable mode5_delay_table(int delay = 13) {
    std::vector<std::uint8_t> payload(0x12, 0);
    payload[1] = 5;
    payload[3] = 1;
    payload[0x11] = static_cast<std::uint8_t>(delay);
    return {
        .table_known = true,
        .includes_sentinel = true,
        .descriptors = {
            {
                .record_index = 16,
                .location_code = 0x32,
                .combined_type = 0x00030032u,
                .payload_size = static_cast<int>(payload.size()),
                .payload_in_bounds = true,
                .payload_bytes = std::move(payload),
            },
            {
                .record_index = 17,
                .location_code = -1,
            },
        },
    };
}

ActionMotionInstructionGateInput mode5_gate_input() {
    return {
        .current_action_key = 5,
        .current_secondary_key = -1,
        .instruction_flags_0xec = 0,
    };
}

TEST(SavorPredictActionMotionPlayback, DurationFiveMatchesCapturedState6Cadence) {
    auto runtime = install_duration(0x40a00000u);
    EXPECT_EQ(runtime.effective_duration_bits, 0x40a00000u);
    EXPECT_EQ(runtime.increment_bits_0x6c, 0x3e4ccccdu);

    auto visit = visit_action_motion_playback(runtime);
    runtime = visit.runtime;
    EXPECT_EQ(visit.kind, ActionMotionPlaybackVisitKind::InitialRendererAdvance);
    EXPECT_EQ(visit.progress_after, 0x3e4ccccdu);
    EXPECT_FALSE(visit.gate_polled);

    constexpr std::array<std::uint32_t, 4> kFalsePollProgressAfter{
        0x3ecccccdU,
        0x3f19999aU,
        0x3f4ccccdU,
        0x3f800000U,
    };
    for (const auto expected_progress : kFalsePollProgressAfter) {
        visit = visit_action_motion_playback(runtime);
        runtime = visit.runtime;
        EXPECT_EQ(visit.kind, ActionMotionPlaybackVisitKind::State6Deferred);
        ASSERT_TRUE(visit.gate_result.has_value());
        EXPECT_FALSE(*visit.gate_result);
        EXPECT_TRUE(visit.renderer_advanced);
        EXPECT_EQ(visit.progress_after, expected_progress);
        EXPECT_NE(visit.flags_after & 0x80000000u, 0u);
    }

    visit = visit_action_motion_playback(runtime);
    runtime = visit.runtime;
    EXPECT_EQ(visit.kind, ActionMotionPlaybackVisitKind::State6Satisfied);
    ASSERT_TRUE(visit.gate_result.has_value());
    EXPECT_TRUE(*visit.gate_result);
    EXPECT_EQ(visit.progress_after, 0x3f800000u);
    EXPECT_EQ(visit.flags_after & 0x80000000u, 0u);
    EXPECT_TRUE(action_motion_playback_blocks_publication(runtime));

    visit = visit_action_motion_playback(runtime, no_delay_visit_input());
    runtime = visit.runtime;
    EXPECT_EQ(visit.kind, ActionMotionPlaybackVisitKind::PublicationReleased);
    EXPECT_TRUE(visit.publication_released_this_visit);
    EXPECT_EQ(visit.control_state_before, 6);
    EXPECT_EQ(visit.control_state_after, 11);
    EXPECT_FALSE(action_motion_playback_blocks_publication(runtime));
    EXPECT_EQ(runtime.renderer_visits, 5);
    EXPECT_EQ(runtime.state6_polls, 5);
}

TEST(SavorPredictActionMotionPlayback, ShortDurationClampsBeforeFirstPoll) {
    auto runtime = install_duration(0x3dccc954u);
    const auto advanced = visit_action_motion_playback(runtime);
    runtime = advanced.runtime;
    EXPECT_EQ(advanced.kind, ActionMotionPlaybackVisitKind::InitialRendererAdvance);
    EXPECT_EQ(advanced.progress_after, 0x3f800000u);

    const auto gate = visit_action_motion_playback(runtime);
    runtime = gate.runtime;
    ASSERT_TRUE(gate.gate_result.has_value());
    EXPECT_TRUE(*gate.gate_result);
    EXPECT_EQ(gate.kind, ActionMotionPlaybackVisitKind::State6Satisfied);
    EXPECT_EQ(runtime.state6_polls, 1);

    const auto release = visit_action_motion_playback(runtime, no_delay_visit_input());
    EXPECT_EQ(release.kind, ActionMotionPlaybackVisitKind::PublicationReleased);
    EXPECT_TRUE(release.publication_released_this_visit);
}

TEST(SavorPredictActionMotionPlayback, FlaggedShortDurationSubstitutesFive) {
    auto runtime = install_duration(0x3dcccccdU, 0x00040000u);
    EXPECT_TRUE(runtime.substituted_default_duration);
    EXPECT_EQ(runtime.effective_duration_bits, 0x40a00000u);
    EXPECT_EQ(runtime.increment_bits_0x6c, 0x3e4ccccdu);
}

TEST(SavorPredictActionMotionPlayback, DescriptorDelayMatchesModeFiveAndCountsState9Visits) {
    const auto lookup = resolve_action_motion_post_state6_delay(
        mode5_delay_table(), mode5_gate_input());
    EXPECT_EQ(lookup.status, ActionMotionDelayStatus::Matched);
    EXPECT_EQ(lookup.descriptor_record_index, 16);
    ASSERT_TRUE(lookup.delay.has_value());
    EXPECT_EQ(*lookup.delay, 13);
    ASSERT_TRUE(lookup.gate_result.has_value());
    EXPECT_TRUE(*lookup.gate_result);

    auto runtime = install_duration(0x3dccc954u);
    runtime = visit_action_motion_playback(runtime).runtime;
    runtime = visit_action_motion_playback(runtime).runtime;
    ASSERT_EQ(runtime.phase, ActionMotionPlaybackPhase::State6Satisfied);

    auto visit = visit_action_motion_playback(
        runtime, {.post_state6_delay = lookup});
    runtime = visit.runtime;
    EXPECT_EQ(visit.kind, ActionMotionPlaybackVisitKind::PostState6DelayDeferred);
    EXPECT_TRUE(visit.post_state6_delay_lookup_performed);
    EXPECT_EQ(visit.post_state6_delay_before, 13);
    EXPECT_EQ(visit.post_state6_delay_after, 12);
    EXPECT_EQ(visit.control_state_before, 6);
    EXPECT_EQ(visit.control_state_after, 9);
    EXPECT_TRUE(action_motion_playback_blocks_publication(runtime));

    for (int expected_before = 12; expected_before > 0; --expected_before) {
        visit = visit_action_motion_playback(runtime);
        runtime = visit.runtime;
        EXPECT_EQ(visit.kind, ActionMotionPlaybackVisitKind::PostState6DelayDeferred);
        EXPECT_EQ(visit.post_state6_delay_before, expected_before);
        EXPECT_EQ(visit.post_state6_delay_after, expected_before - 1);
        EXPECT_FALSE(visit.publication_released_this_visit);
    }

    visit = visit_action_motion_playback(runtime);
    EXPECT_EQ(visit.kind, ActionMotionPlaybackVisitKind::PublicationReleased);
    EXPECT_TRUE(visit.publication_released_this_visit);
    EXPECT_EQ(visit.control_state_before, 9);
    EXPECT_EQ(visit.control_state_after, 11);
}

TEST(SavorPredictActionMotionPlayback, SpecialGateWithoutAlternatePairIsMissingInput) {
    auto input = mode5_gate_input();
    input.current_action_key = 0x0b;
    auto table = mode5_delay_table();
    table.descriptors.front().payload_bytes[3] = 2;

    const auto lookup = resolve_action_motion_post_state6_delay(table, input);
    EXPECT_EQ(lookup.status, ActionMotionDelayStatus::MissingInput);
}

TEST(SavorPredictActionMotionPlayback, MissingPostState6DelayHoldsPublication) {
    auto runtime = install_duration(0x3dccc954u);
    runtime = visit_action_motion_playback(runtime).runtime;
    runtime = visit_action_motion_playback(runtime).runtime;
    ASSERT_EQ(runtime.phase, ActionMotionPlaybackPhase::State6Satisfied);

    const auto unavailable = visit_action_motion_playback(runtime);
    EXPECT_EQ(
        unavailable.kind,
        ActionMotionPlaybackVisitKind::PostState6DelayUnavailable);
    EXPECT_EQ(unavailable.runtime.status, ActionMotionPlaybackStatus::MissingInput);
    EXPECT_EQ(unavailable.runtime.phase, ActionMotionPlaybackPhase::Unsupported);
    EXPECT_TRUE(action_motion_playback_blocks_publication(unavailable.runtime));
    EXPECT_FALSE(unavailable.publication_released_this_visit);
}

TEST(SavorPredictActionMotionPlayback, DescriptorTerminatorProducesExactZeroDelay) {
    const auto lookup = resolve_action_motion_post_state6_delay(
        ActionMotionDelayTable{
            .table_known = true,
            .includes_sentinel = true,
            .descriptors = {{.record_index = 0, .location_code = -1}},
        },
        mode5_gate_input());
    EXPECT_EQ(lookup.status, ActionMotionDelayStatus::NoMatch);
    ASSERT_TRUE(lookup.delay.has_value());
    EXPECT_EQ(*lookup.delay, 0);
}

TEST(SavorPredictActionMotionPlayback, MissingOrInvalidRowsRemainUngated) {
    const auto missing = install_action_motion_playback({
        .action_ordinal = 1,
        .slot = 0,
        .instruction_state_revision = 2,
        .selected_action_row_index = 4,
    });
    EXPECT_FALSE(missing.installed);
    EXPECT_FALSE(missing.blocks_publication);
    EXPECT_EQ(missing.runtime.status, ActionMotionPlaybackStatus::MissingInput);
    EXPECT_EQ(missing.runtime.phase, ActionMotionPlaybackPhase::Unsupported);

    const auto zero = install_action_motion_playback({
        .action_ordinal = 1,
        .slot = 0,
        .instruction_state_revision = 2,
        .selected_action_row_index = 4,
        .selected_action_row_duration_bits = 0u,
    });
    EXPECT_FALSE(zero.installed);
    EXPECT_FALSE(zero.blocks_publication);
    EXPECT_EQ(zero.runtime.status, ActionMotionPlaybackStatus::Unsupported);
}

} // namespace
