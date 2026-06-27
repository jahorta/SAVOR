#include "ActionViewPathingTailModel.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <utility>

namespace savor::predict {
namespace {

constexpr float kPi = 3.14159265358979323846f;

void append_step(ActionViewPathingTailResult& result, ActionViewPathingTailStep step) {
    result.total_draws += step.draws_consumed;
    if (step.status == ActionViewPathingTailStatus::MissingInput) {
        result.has_missing_input_steps = true;
    }
    if (step.status == ActionViewPathingTailStatus::Ambiguous) {
        result.has_ambiguous_steps = true;
    }
    if (step.status == ActionViewPathingTailStatus::Unsupported) {
        result.has_unsupported_steps = true;
    }
    result.steps.push_back(std::move(step));
}

float distance_xz(const BattleFrameVec3& a, const BattleFrameVec3& b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

float raw_angle_degrees_xz(const BattleFrameVec3& from, const BattleFrameVec3& to) {
    float degrees = std::atan2(to.z - from.z, to.x - from.x) * 180.0f / kPi;
    if (degrees < 0.0f) {
        degrees += 360.0f;
    }
    return degrees;
}

float raw_abs_angle_delta(float a, float b) {
    const float delta = a - b;
    return delta < 0.0f ? -delta : delta;
}

int first_battle_pathing_tail_scan_count(
    const BattleFrameCombatantState& actor,
    const BattleFrameCombatantState& target) {
    const int dx = std::abs(actor.grid_position.grid_x - target.grid_position.grid_x);
    const int dz = std::abs(actor.grid_position.grid_z - target.grid_position.grid_z);
    return std::max(1, dx + dz + 1);
}

std::string vec_detail(const BattleFrameVec3& v) {
    std::ostringstream out;
    out << "(" << v.x << "," << v.y << "," << v.z << ")";
    return out.str();
}

std::string fun80011694_detail(const Fun80011694Result& scan, int call_index) {
    std::ostringstream out;
    out << "FUN_80011694 call=" << call_index
        << "; accepted_candidates=" << scan.accepted_candidates
        << "; aggregate_score=" << scan.aggregate_score
        << "; selected_slot=" << scan.selected_slot
        << "; fallback_rng_draw=" << (scan.fallback_rng_draw ? 1 : 0);
    if (!scan.detail.empty()) {
        out << "; " << scan.detail;
    }
    return out.str();
}

} // namespace

float angle_short_to_degrees_8006116c(std::uint32_t angle_word) {
    const auto angle = static_cast<std::uint16_t>(angle_word & 0xffffu);
    return static_cast<float>(angle) * 360.0f / 65536.0f;
}

GeometryScorer117ecResult score_geometry_800117ec(const GeometryScorer117ecInput& input) {
    GeometryScorer117ecResult result;
    BattleFrameVec3 base = input.path_base.value_or(BattleFrameVec3{});
    base.y = input.input_reference.y;

    const float direction_x = input.input_reference.x - base.x;
    const float direction_y = input.input_reference.y - base.y;
    const float direction_z = input.input_reference.z - base.z;
    const float candidate_x = input.candidate_position.x - base.x;
    const float candidate_y = input.candidate_position.y - base.y;
    const float candidate_z = input.candidate_position.z - base.z;

    const float direction_len_sq =
        direction_x * direction_x + direction_y * direction_y + direction_z * direction_z;
    float projection_x = base.x;
    float projection_y = base.y;
    float projection_z = base.z;
    if (direction_len_sq > 0.0f) {
        const float t =
            (direction_x * candidate_x + direction_y * candidate_y + direction_z * candidate_z)
            / direction_len_sq;
        projection_x = base.x + direction_x * t;
        projection_y = base.y + direction_y * t;
        projection_z = base.z + direction_z * t;
    }

    const float perp_x = input.candidate_position.x - projection_x;
    const float perp_y = input.candidate_position.y - projection_y;
    const float perp_z = input.candidate_position.z - projection_z;
    const float perp_sq = perp_x * perp_x + perp_y * perp_y + perp_z * perp_z;
    result.perpendicular_distance = perp_sq < 0.025f ? 0.0f : std::sqrt(std::max(0.0f, perp_sq));
    result.distance_base_to_candidate = distance_xz(base, input.candidate_position);
    result.distance_base_to_input = distance_xz(base, input.input_reference);

    const float candidate_angle = raw_angle_degrees_xz(input.candidate_position, base);
    const float input_angle = raw_angle_degrees_xz(input.input_reference, base);
    result.raw_angle_diff_degrees = raw_abs_angle_delta(candidate_angle, input_angle);

    result.accepted =
        result.distance_base_to_candidate <= result.distance_base_to_input
        && result.raw_angle_diff_degrees < 45.0f;
    return result;
}

Fun80011694Result run_fun_80011694(
    const BattleFrameState& frame_state,
    const BattleFrameVec3& input_reference,
    int excluded_slot,
    const BattleFrameVec3& path_base) {
    Fun80011694Result result;
    if (!frame_state.initialized) {
        result.status = ActionViewPathingTailStatus::MissingInput;
        result.detail = "frame state was not initialized";
        return result;
    }

    int best_compare = -1;
    for (const auto& thread : frame_state.packed_thread_order) {
        Fun80011694CandidateResult candidate;
        candidate.slot = thread.slot;
        const auto* combatant = find_frame_combatant(frame_state, thread.slot);
        if (!thread.active || combatant == nullptr || !combatant->present || !combatant->alive) {
            candidate.skipped = true;
            candidate.reason = "null_or_inactive";
            result.candidates.push_back(candidate);
            continue;
        }
        if ((combatant->instruction_flags_0xf0 & 0x80000000u) != 0) {
            candidate.skipped = true;
            candidate.reason = "instruction_flags_0xf0_high_bit";
            result.candidates.push_back(candidate);
            continue;
        }
        if (combatant->slot == excluded_slot) {
            candidate.skipped = true;
            candidate.reason = "excluded_slot";
            result.candidates.push_back(candidate);
            continue;
        }

        const auto geometry = score_geometry_800117ec({
            .input_reference = input_reference,
            .candidate_position = combatant->combatant_position,
            .path_base = path_base,
        });
        if (!geometry.accepted) {
            candidate.reason = "geometry_rejected";
            result.candidates.push_back(candidate);
            continue;
        }

        const float penalty =
            (combatant->instruction_flags_0xec & 0x00200000u) != 0 ? 22.5f : 7.5f;
        candidate.accepted = true;
        candidate.score = geometry.perpendicular_distance - penalty;
        candidate.reason = "accepted";
        result.aggregate_score += candidate.score;
        ++result.accepted_candidates;

        if (combatant->instruction_compare_0x15c >= best_compare) {
            best_compare = combatant->instruction_compare_0x15c;
            result.selected_slot = combatant->slot;
        }
        result.candidates.push_back(candidate);
    }

    result.fallback_rng_draw = result.aggregate_score == 0.0f;
    return result;
}

ActionViewPathingTailResult model_first_battle_action_view_pathing_tail(
    const ActionViewPathingTailInput& input) {
    ActionViewPathingTailResult result;
    if (input.profile_name != "first-battle") {
        append_step(result, {
            .label = "unsupported_profile",
            .status = ActionViewPathingTailStatus::Unsupported,
            .actor_slot = input.actor_slot,
            .target_slot = input.target_slot,
            .detail = "action-view pathing tail v1 supports first-battle only",
        });
        return result;
    }
    if (!input.attack_landed) {
        append_step(result, {
            .label = "skipped_unlanded_attack",
            .status = ActionViewPathingTailStatus::Skipped,
            .actor_slot = input.actor_slot,
            .target_slot = input.target_slot,
            .detail = "v1 only models landed-hit action-view pathing tail draws",
        });
        return result;
    }
    if (input.enemy_event_id.value_or(-1) != 0) {
        append_step(result, {
            .label = "missing_input_enemy_event0_frame_state",
            .status = ActionViewPathingTailStatus::MissingInput,
            .actor_slot = input.actor_slot,
            .target_slot = input.target_slot,
            .detail = "first-battle frame scheduler needs enemy_event_id=0 start positions",
        });
        return result;
    }

    std::optional<BattleFrameState> owned_frame_state;
    const BattleFrameState* frame_state = input.frame_state;
    if (frame_state == nullptr) {
        owned_frame_state = initialize_first_battle_frame_state(*input.enemy_event_id, input.slots);
        frame_state = owned_frame_state ? &*owned_frame_state : nullptr;
    }
    if (frame_state == nullptr) {
        append_step(result, {
            .label = "missing_input_frame_state_init",
            .status = ActionViewPathingTailStatus::MissingInput,
            .actor_slot = input.actor_slot,
            .target_slot = input.target_slot,
            .detail = "failed to initialize first-battle frame state",
        });
        return result;
    }

    const auto* actor = find_frame_combatant(*frame_state, input.actor_slot);
    const auto* target = find_frame_combatant(*frame_state, input.target_slot);
    if (actor == nullptr || target == nullptr || !actor->alive || !target->present) {
        append_step(result, {
            .label = "skipped_missing_actor_or_target",
            .status = ActionViewPathingTailStatus::Skipped,
            .actor_slot = input.actor_slot,
            .target_slot = input.target_slot,
            .detail = "actor or target is unavailable for action-view pathing tail",
        });
        return result;
    }

    append_step(result, {
        .label = "mode1_pathing_record_draw",
        .status = ActionViewPathingTailStatus::Provisional,
        .draws_consumed = 1,
        .actor_slot = input.actor_slot,
        .target_slot = input.target_slot,
        .frame_index = frame_state->frame_index,
        .detail = "FUN_800519f4:80051C00 selected action-view record pathing draw",
    });

    const int scan_count = first_battle_pathing_tail_scan_count(*actor, *target);
    int fallback_draws = 0;
    Fun80011694Result last_scan;

    // The validated first-battle pathing tail runs repeated target-side scans.
    // Use the target reference as the visible input and the same target point as
    // the current path base for the validated zero-score path until broader
    // turn worksheet state is captured for other battles.
    const BattleFrameVec3 target_side_base = target->combatant_position;
    for (int i = 0; i < scan_count; ++i) {
        last_scan = run_fun_80011694(
            *frame_state,
            target->combatant_position,
            input.target_slot,
            target_side_base);
        if (last_scan.fallback_rng_draw) {
            ++fallback_draws;
        }
    }

    append_step(result, {
        .label = "fun_80011694_target_side_fallback",
        .status = ActionViewPathingTailStatus::Provisional,
        .draws_consumed = fallback_draws,
        .actor_slot = input.actor_slot,
        .target_slot = input.target_slot,
        .frame_index = frame_state->frame_index,
        .accepted_candidates = last_scan.accepted_candidates,
        .aggregate_score = last_scan.aggregate_score,
        .detail = fun80011694_detail(last_scan, scan_count)
            + "; repeated_scans=" + std::to_string(scan_count)
            + "; fallback_draws=" + std::to_string(fallback_draws)
            + "; actor_pos=" + vec_detail(actor->combatant_position)
            + "; target_pos=" + vec_detail(target->combatant_position),
    });

    return result;
}

const char* action_view_pathing_tail_status_name(ActionViewPathingTailStatus status) {
    switch (status) {
    case ActionViewPathingTailStatus::Exact:
        return "Exact";
    case ActionViewPathingTailStatus::Provisional:
        return "Provisional";
    case ActionViewPathingTailStatus::Skipped:
        return "Skipped";
    case ActionViewPathingTailStatus::MissingInput:
        return "MissingInput";
    case ActionViewPathingTailStatus::Ambiguous:
        return "Ambiguous";
    case ActionViewPathingTailStatus::Unsupported:
        return "Unsupported";
    }
    return "Unsupported";
}

} // namespace savor::predict
