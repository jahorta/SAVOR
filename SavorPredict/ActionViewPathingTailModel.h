#pragma once

#include "BattleFrameStateModel.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace savor::predict {

enum class ActionViewPathingTailStatus {
    Exact,
    Provisional,
    Skipped,
    MissingInput,
    Ambiguous,
    Unsupported,
};

struct GeometryScorer117ecInput {
    BattleFrameVec3 input_reference{};
    BattleFrameVec3 candidate_position{};
    std::optional<BattleFrameVec3> path_base;
};

struct GeometryScorer117ecResult {
    bool accepted = false;
    float perpendicular_distance = 0.0f;
    float distance_base_to_candidate = 0.0f;
    float distance_base_to_input = 0.0f;
    float raw_angle_diff_degrees = 0.0f;
};

struct Fun80011694CandidateResult {
    int slot = -1;
    bool skipped = false;
    bool accepted = false;
    float score = 0.0f;
    std::string reason;
};

struct Fun80011694Result {
    ActionViewPathingTailStatus status = ActionViewPathingTailStatus::Exact;
    float aggregate_score = 0.0f;
    int selected_slot = -1;
    int accepted_candidates = 0;
    bool fallback_rng_draw = false;
    std::vector<Fun80011694CandidateResult> candidates;
    std::string detail;
};

struct ActionViewPathingTailInput {
    std::string profile_name = "first-battle";
    int actor_slot = -1;
    int target_slot = -1;
    int combatant_action_mode = 0;
    int combatant_command_parameter = 0;
    bool attack_landed = false;
    bool counter_follow_up = false;
    std::optional<int> enemy_event_id;
    std::vector<MovementSlotState> slots;
    const BattleFrameState* frame_state = nullptr;
};

struct ActionViewPathingTailStep {
    std::string phase = "action_view_pathing_tail";
    std::string label;
    ActionViewPathingTailStatus status = ActionViewPathingTailStatus::Exact;
    int draws_consumed = 0;
    int actor_slot = -1;
    int target_slot = -1;
    int frame_index = 0;
    std::optional<int> accepted_candidates;
    std::optional<float> aggregate_score;
    std::string detail;
};

struct ActionViewPathingTailResult {
    std::vector<ActionViewPathingTailStep> steps;
    int total_draws = 0;
    bool has_missing_input_steps = false;
    bool has_ambiguous_steps = false;
    bool has_unsupported_steps = false;
};

float angle_short_to_degrees_8006116c(std::uint32_t angle_word);
GeometryScorer117ecResult score_geometry_800117ec(const GeometryScorer117ecInput& input);
Fun80011694Result run_fun_80011694(
    const BattleFrameState& frame_state,
    const BattleFrameVec3& input_reference,
    int excluded_slot,
    const BattleFrameVec3& path_base);

ActionViewPathingTailResult model_first_battle_action_view_pathing_tail(
    const ActionViewPathingTailInput& input);

const char* action_view_pathing_tail_status_name(ActionViewPathingTailStatus status);

} // namespace savor::predict
