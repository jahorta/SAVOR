#pragma once

#include "CheckpointTrace.h"

#include <optional>
#include <string>
#include <vector>

namespace savor::predict {

enum class PreAiCheckpointStatus {
    ObservedOnly,
    MatchesExpected,
    MissingBattleStartCameraDraw,
    ExtraBattleStartCameraDraws,
    ShortCameraGapSkippedFakeAttackDraws,
    MissingFakeAttackDraws,
    ExtraFakeAttackDraws,
    MissingTargetingCameraDraws,
    ExtraTargetingCameraDraws,
    MissingFirstSoldierAiDraw,
    FirstSoldierAiDrawMismatch,
};

struct PreAiCheckpointExpectation {
    int expected_fake_attack_attempts = 0;
    int expected_fake_attack_draws = 0;
    int expected_battle_start_camera_draws = 1;
    int expected_targeting_camera_draws = 0;
    int expected_total_pre_ai_draws = 0;
    int expected_first_soldier_ai_draw_index_before = 0;
};

struct PreAiCheckpointDraw {
    std::string owner;
    std::optional<int> draw_index;
    std::optional<int> active_slot;
    std::optional<int> target_slot;
    std::optional<int> fake_attack_index;
    std::optional<int> camera_frame_gap;
    bool skipped_rng_draw = false;
};

struct PreAiCheckpointSummary {
    std::optional<PreAiCheckpointExpectation> expectation;
    int observed_battle_start_camera_draws = 0;
    int observed_targeting_camera_draws = 0;
    int observed_fake_attack_attempts = 0;
    int observed_fake_attack_draws = 0;
    int observed_skipped_fake_attack_draws = 0;
    int skipped_fake_attacks_with_short_camera_gap = 0;
    int skipped_fake_draws_with_frame_gap = 0;
    int observed_pre_ai_draws = 0;
    int observed_first_soldier_ai_draws = 0;
    int targeting_draws_with_target_slot = 0;
    int fake_draws_with_fake_attack_index = 0;
    std::optional<int> first_battle_start_camera_draw_index;
    std::optional<int> first_targeting_camera_draw_index;
    std::optional<int> last_targeting_camera_draw_index;
    std::optional<int> first_fake_attack_draw_index;
    std::optional<int> last_fake_attack_draw_index;
    std::optional<int> first_soldier_ai_draw_index;
    int pre_ai_draws_before_first_soldier_ai = 0;
    std::vector<PreAiCheckpointDraw> draws;
    PreAiCheckpointStatus status = PreAiCheckpointStatus::ObservedOnly;
};

PreAiCheckpointExpectation first_battle_pre_ai_checkpoint_expectation(int fake_attacks);
PreAiCheckpointSummary summarize_pre_ai_checkpoints(
    const std::vector<CheckpointEvent>& events,
    std::optional<int> expected_fake_attacks);
const char* pre_ai_checkpoint_status_name(PreAiCheckpointStatus status);
const char* first_battle_pre_ai_checkpoint_rule_detail();

} // namespace savor::predict
