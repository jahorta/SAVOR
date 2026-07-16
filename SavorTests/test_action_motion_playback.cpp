#include "../SavorPredict/ActionMotionPlaybackModel.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

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

    visit = visit_action_motion_playback(runtime);
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

    const auto release = visit_action_motion_playback(runtime);
    EXPECT_EQ(release.kind, ActionMotionPlaybackVisitKind::PublicationReleased);
    EXPECT_TRUE(release.publication_released_this_visit);
}

TEST(SavorPredictActionMotionPlayback, FlaggedShortDurationSubstitutesFive) {
    auto runtime = install_duration(0x3dcccccdU, 0x00040000u);
    EXPECT_TRUE(runtime.substituted_default_duration);
    EXPECT_EQ(runtime.effective_duration_bits, 0x40a00000u);
    EXPECT_EQ(runtime.increment_bits_0x6c, 0x3e4ccccdu);
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
