#include "ActionViewPathingTailModel.h"

#include "RngCore.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <utility>

namespace savor::predict {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kAngleShortUnitsPerRadian = 10430.37890625f;
constexpr float kVectorComponentSnapThreshold = 0.00001f;

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

float snap_angle_vector_component(float value) {
    if (value < kVectorComponentSnapThreshold
        && -kVectorComponentSnapThreshold < value) {
        return 0.0f;
    }
    return value;
}

std::uint16_t angle_xz_short_8007201c(
    const BattleFrameVec3& from,
    const BattleFrameVec3& to) {
    const float x = snap_angle_vector_component(to.x - from.x);
    const float z = snap_angle_vector_component(to.z - from.z);
    if (x == 0.0f) {
        return z > 0.0f ? 0x8000u : 0u;
    }

    const float radians = static_cast<float>(
        std::atan2(static_cast<double>(x), -static_cast<double>(z)));
    int angle = -static_cast<int>(kAngleShortUnitsPerRadian * radians);
    if (angle > 0x7fff) {
        angle -= 0x10000;
    } else if (angle < -0x8000) {
        angle += 0x10000;
    }
    return static_cast<std::uint16_t>(angle);
}

float raw_angle_degrees_xz(const BattleFrameVec3& from, const BattleFrameVec3& to) {
    return angle_short_to_degrees_8006116c(angle_xz_short_8007201c(from, to));
}

float raw_abs_angle_delta(float a, float b) {
    const float delta = a - b;
    return delta < 0.0f ? -delta : delta;
}

std::string vec_detail(const BattleFrameVec3& v) {
    std::ostringstream out;
    out << "(" << v.x << "," << v.y << "," << v.z << ")";
    return out.str();
}

float distance_xyz(const BattleFrameVec3& a, const BattleFrameVec3& b) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float dz = b.z - a.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float collision_radius_for(const BattleFrameCombatantState& combatant) {
    return (combatant.instruction_flags_0xec & 0x00200000u) != 0 ? 22.5f : 7.5f;
}

BattleFrameVec3 midpoint(const BattleFrameVec3& a, const BattleFrameVec3& b) {
    return BattleFrameVec3{
        .x = (a.x + b.x) * 0.5f,
        .y = (a.y + b.y) * 0.5f,
        .z = (a.z + b.z) * 0.5f,
    };
}

BattleFrameVec3 build_orbit_path_base(
    const BattleFrameVec3& actor_endpoint,
    const BattleFrameVec3& target_endpoint,
    float camera_distance,
    float yaw_degrees) {
    const BattleFrameVec3 center = midpoint(actor_endpoint, target_endpoint);
    const float forward_x = target_endpoint.x - actor_endpoint.x;
    const float forward_z = target_endpoint.z - actor_endpoint.z;
    const float forward_length = std::sqrt(forward_x * forward_x + forward_z * forward_z);
    if (forward_length == 0.0f) {
        return center;
    }

    const float unit_forward_x = forward_x / forward_length;
    const float unit_forward_z = forward_z / forward_length;
    const float unit_right_x = -unit_forward_z;
    const float unit_right_z = unit_forward_x;
    const float yaw_radians = yaw_degrees * kPi / 180.0f;
    const float yaw_sin = std::sin(yaw_radians);
    const float yaw_cos = std::cos(yaw_radians);

    const float endpoint_distance = distance_xyz(actor_endpoint, target_endpoint);
    const float radius_adjustment = 0.5f * (camera_distance - endpoint_distance);
    const float radius = endpoint_distance * 0.5f + radius_adjustment;
    return BattleFrameVec3{
        .x = center.x + radius * (unit_right_x * yaw_sin - unit_forward_x * yaw_cos),
        .y = center.y,
        .z = center.z + radius * (unit_right_z * yaw_sin - unit_forward_z * yaw_cos),
    };
}

} // namespace

float angle_short_to_degrees_8006116c(std::uint32_t angle_word) {
    const auto angle = static_cast<std::uint16_t>(angle_word & 0xffffu);
    return static_cast<float>(360.0 * static_cast<double>(angle) / 65536.0);
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
    for (const auto& combatant_state : frame_state.combatants) {
        Fun80011694CandidateResult candidate;
        candidate.slot = combatant_state.slot;
        const auto* combatant = &combatant_state;
        candidate.candidate_position = combatant->combatant_cur_pos_0x1c;
        candidate.instruction_flags_0xec = combatant->instruction_flags_0xec;
        candidate.instruction_flags_0xf0 = combatant->instruction_flags_0xf0;
        candidate.instruction_compare_known =
            combatant->instruction_compare_known;
        candidate.instruction_compare_0x15c =
            combatant->instruction_compare_0x15c;
        if (!combatant->present || !combatant->alive) {
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
            .candidate_position = combatant->combatant_cur_pos_0x1c,
            .path_base = path_base,
        });
        candidate.geometry = geometry;
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

        if (!combatant->instruction_compare_known) {
            candidate.reason = "accepted_but_instruction_compare_missing";
            result.status = ActionViewPathingTailStatus::MissingInput;
            result.detail = "FUN_80011694 accepted a candidate whose IW+0x15C producer is not modeled";
            result.candidates.push_back(candidate);
            continue;
        }
        if (combatant->instruction_compare_0x15c >= best_compare) {
            best_compare = combatant->instruction_compare_0x15c;
            result.selected_slot = combatant->slot;
        }
        result.candidates.push_back(candidate);
    }

    result.fallback_rng_draw = result.aggregate_score == 0.0f;
    return result;
}

Fun8005174cResult run_fun_8005174c_pathing_scans(
    const BattleFrameState& frame_state,
    int actor_slot,
    int target_slot,
    std::uint32_t rng_seed_before) {
    Fun8005174cResult result;
    const auto* actor = find_frame_combatant(frame_state, actor_slot);
    const auto* target = find_frame_combatant(frame_state, target_slot);
    if (!frame_state.initialized || actor == nullptr || target == nullptr
        || !actor->present || !actor->alive || !target->present) {
        result.status = ActionViewPathingTailStatus::MissingInput;
        result.detail = "FUN_8005174C needs initialized actor and target combatant state";
        return result;
    }

    const BattleFrameVec3 actor_position = actor->combatant_cur_pos_0x1c;
    const BattleFrameVec3 target_position = target->combatant_cur_pos_0x1c;
    const float combatant_distance = distance_xyz(actor_position, target_position);
    if (!(combatant_distance > 0.0f) || !std::isfinite(combatant_distance)) {
        result.status = ActionViewPathingTailStatus::Unsupported;
        result.detail = "FUN_800171F0 endpoint direction is undefined for coincident combatants";
        return result;
    }

    const float unit_x = (target_position.x - actor_position.x) / combatant_distance;
    const float unit_y = (target_position.y - actor_position.y) / combatant_distance;
    const float unit_z = (target_position.z - actor_position.z) / combatant_distance;
    const float actor_radius = collision_radius_for(*actor);
    const float target_radius = collision_radius_for(*target);
    result.actor_endpoint = BattleFrameVec3{
        .x = actor_position.x - unit_x * actor_radius,
        .y = actor_position.y - unit_y * actor_radius,
        .z = actor_position.z - unit_z * actor_radius,
    };
    result.target_endpoint = BattleFrameVec3{
        .x = target_position.x + unit_x * target_radius,
        .y = target_position.y + unit_y * target_radius,
        .z = target_position.z + unit_z * target_radius,
    };
    result.endpoint_distance = distance_xyz(result.actor_endpoint, result.target_endpoint);
    const float camera_distance_adjustment =
        0.6000000238418579f * result.endpoint_distance + 22.5f;
    result.camera_distance = result.endpoint_distance + camera_distance_adjustment;

    const auto setup_draw = draw_rand15(rng_seed_before);
    result.setup_rand = setup_draw.value;
    result.initial_yaw_degrees = 45.0f * static_cast<float>(setup_draw.value % 7u) + 45.0f;
    result.selected_yaw_degrees = result.initial_yaw_degrees;

    float best_score = 0.0f;
    result.scans.reserve(12);
    for (int iteration = 0; iteration < 12; ++iteration) {
        const float yaw = result.initial_yaw_degrees + 10.0f * static_cast<float>(iteration);
        const BattleFrameVec3 path_base = build_orbit_path_base(
            result.actor_endpoint,
            result.target_endpoint,
            result.camera_distance,
            yaw);
        auto actor_scan = run_fun_80011694(
            frame_state,
            actor_position,
            actor_slot,
            path_base);
        auto target_scan = run_fun_80011694(
            frame_state,
            target_position,
            target_slot,
            path_base);
        if (actor_scan.fallback_rng_draw) {
            ++result.actor_fallback_draws;
        }
        if (target_scan.fallback_rng_draw) {
            ++result.target_fallback_draws;
        }
        const float combined_score = actor_scan.aggregate_score + target_scan.aggregate_score;
        if (best_score <= combined_score) {
            best_score = combined_score;
            result.selected_yaw_degrees = yaw;
        }
        result.scans.push_back(Fun8005174cScanIteration{
            .yaw_iteration = iteration,
            .yaw_degrees = yaw,
            .path_base = path_base,
            .actor_scan = std::move(actor_scan),
            .target_scan = std::move(target_scan),
        });
    }

    std::ostringstream detail;
    detail << "FUN_800519F4/FUN_8005174C single-target pathing"
           << "; setup_rand=" << *result.setup_rand
           << "; initial_yaw=" << result.initial_yaw_degrees
           << "; selected_yaw=" << result.selected_yaw_degrees
           << "; actor_endpoint=" << vec_detail(result.actor_endpoint)
           << "; target_endpoint=" << vec_detail(result.target_endpoint)
           << "; endpoint_distance=" << result.endpoint_distance
           << "; camera_distance=" << result.camera_distance
           << "; yaw_iterations=12"
           << "; actor_fallback_draws=" << result.actor_fallback_draws
           << "; target_fallback_draws=" << result.target_fallback_draws;
    result.detail = detail.str();
    return result;
}

ActionViewPathingTailResult model_first_battle_action_view_pathing_tail(
    const ActionViewPathingTailInput& input) {
    ActionViewPathingTailResult result;
    if (!is_first_battle_soldiers_profile_name(input.profile_name)) {
        append_step(result, {
            .label = "unsupported_profile",
            .status = ActionViewPathingTailStatus::Unsupported,
            .actor_slot = input.actor_slot,
            .target_slot = input.target_slot,
            .detail = "action-view pathing tail v1 supports first-battle only",
        });
        return result;
    }
    const BattleFrameState* frame_state = input.frame_state;
    if (frame_state == nullptr) {
        append_step(result, {
            .label = "missing_input_frame_state_init",
            .status = ActionViewPathingTailStatus::MissingInput,
            .actor_slot = input.actor_slot,
            .target_slot = input.target_slot,
            .detail = "producer-derived frame state is required for pathing",
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

    if (!input.rng_seed_before.has_value()) {
        append_step(result, {
            .label = "missing_input_pathing_rng_seed",
            .status = ActionViewPathingTailStatus::MissingInput,
            .actor_slot = input.actor_slot,
            .target_slot = input.target_slot,
            .frame_index = frame_state->frame_index,
            .detail = "mode-1 pathing needs the pre-FUN_800519F4 seed to derive its initial yaw",
        });
        return result;
    }

    const auto scan = run_fun_8005174c_pathing_scans(
        *frame_state,
        input.actor_slot,
        input.target_slot,
        *input.rng_seed_before);
    if (scan.status == ActionViewPathingTailStatus::MissingInput
        || scan.status == ActionViewPathingTailStatus::Unsupported) {
        append_step(result, {
            .label = "fun_8005174c_pathing_scan_unavailable",
            .status = scan.status,
            .actor_slot = input.actor_slot,
            .target_slot = input.target_slot,
            .frame_index = frame_state->frame_index,
            .detail = scan.detail,
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
        .detail = "FUN_800519F4:80051BB0 selected initial yaw"
            "; rand=" + std::to_string(scan.setup_rand.value_or(0))
            + "; initial_yaw=" + std::to_string(scan.initial_yaw_degrees),
    });

    append_step(result, {
        .label = "fun_8005174c_actor_target_fallbacks",
        .status = scan.status,
        .draws_consumed = scan.actor_fallback_draws + scan.target_fallback_draws,
        .actor_slot = input.actor_slot,
        .target_slot = input.target_slot,
        .frame_index = frame_state->frame_index,
        .actor_side_fallback_draws = scan.actor_fallback_draws,
        .target_side_fallback_draws = scan.target_fallback_draws,
        .detail = scan.detail,
    });

    if (input.emit_causal_diagnostics) {
        const auto append_side = [&result](
            const Fun8005174cScanIteration& iteration,
            std::string side,
            int excluded_slot,
            const BattleFrameVec3& input_reference,
            const Fun80011694Result& side_scan) {
            result.scan_diagnostics.push_back({
                .yaw_iteration = iteration.yaw_iteration,
                .yaw_degrees = iteration.yaw_degrees,
                .side = side,
                .excluded_slot = excluded_slot,
                .input_reference = input_reference,
                .path_base = iteration.path_base,
                .accepted_candidates = side_scan.accepted_candidates,
                .aggregate_score = side_scan.aggregate_score,
                .selected_slot = side_scan.selected_slot,
                .fallback_rng_draw = side_scan.fallback_rng_draw,
                .status = side_scan.status,
            });
            for (const auto& candidate : side_scan.candidates) {
                result.candidate_diagnostics.push_back({
                    .yaw_iteration = iteration.yaw_iteration,
                    .yaw_degrees = iteration.yaw_degrees,
                    .side = side,
                    .excluded_slot = excluded_slot,
                    .candidate_slot = candidate.slot,
                    .input_reference = input_reference,
                    .path_base = iteration.path_base,
                    .candidate_position = candidate.candidate_position,
                    .instruction_flags_0xec =
                        candidate.instruction_flags_0xec,
                    .instruction_flags_0xf0 =
                        candidate.instruction_flags_0xf0,
                    .instruction_compare_known =
                        candidate.instruction_compare_known,
                    .instruction_compare_0x15c =
                        candidate.instruction_compare_0x15c,
                    .skipped = candidate.skipped,
                    .accepted = candidate.accepted,
                    .score = candidate.score,
                    .geometry = candidate.geometry,
                    .reason = candidate.reason,
                });
            }
        };
        for (const auto& iteration : scan.scans) {
            append_side(
                iteration,
                "actor",
                input.actor_slot,
                actor->combatant_cur_pos_0x1c,
                iteration.actor_scan);
            append_side(
                iteration,
                "target",
                input.target_slot,
                target->combatant_cur_pos_0x1c,
                iteration.target_scan);
        }
    }

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
