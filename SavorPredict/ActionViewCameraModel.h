#pragma once

#include "CheckpointTrace.h"
#include "ProgressEventParser.h"

#include <optional>
#include <string_view>
#include <vector>

namespace savor::predict {

enum class ActionViewCameraCheckpointStatus {
    ObservedOnly,
    MatchesExpected,
    MissingMode0eDraws,
    ExtraMode0eDraws,
};

struct ActionViewCameraExpectation {
    int observed_attack_events = 0;
    int expected_mode0e_camera_draws = 0;
    int expected_mode0_rewrite_gate_draws = 0;
    std::string_view expected_owner = "mode0e_action_view_camera";
    std::string_view expected_pc = "80052BF0";
    std::string_view rewrite_gate_owner = "mode0_action_view_camera_fallback";
    std::string_view rewrite_gate_pc = "800513D4";
    std::string_view rejected_fallback_owner = "mode0_action_view_camera_fallback";
    std::string_view rejected_fallback_pc = "800513D4";
};

struct ActionViewCameraCheckpointSummary {
    std::optional<int> expected_mode0e_camera_draws;
    int observed_mode0e_camera_draws = 0;
    int observed_mode0_fallback_draws = 0;
    int observed_attack_hit_draws = 0;
    std::optional<int> first_mode0e_draw_index;
    std::optional<int> first_attack_hit_draw_index;
    int mode0e_draws_before_first_attack_hit = 0;
    int mode0e_draws_after_first_attack_hit = 0;
    ActionViewCameraCheckpointStatus status = ActionViewCameraCheckpointStatus::ObservedOnly;
};

ActionViewCameraExpectation first_battle_action_view_camera_expectation(const ParsedProgressEvents& events);
ActionViewCameraCheckpointSummary summarize_action_view_camera_checkpoints(
    const std::vector<CheckpointEvent>& events,
    std::optional<int> expected_mode0e_camera_draws);
const char* action_view_camera_checkpoint_status_name(ActionViewCameraCheckpointStatus status);
const char* first_battle_action_view_camera_rule_detail();

} // namespace savor::predict
