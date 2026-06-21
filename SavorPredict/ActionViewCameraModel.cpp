#include "ActionViewCameraModel.h"

namespace savor::predict {

namespace {

constexpr std::string_view kMode0eOwner = "mode0e_action_view_camera";
constexpr std::string_view kMode0FallbackOwner = "mode0_action_view_camera_fallback";
constexpr std::string_view kAttackHitOwner = "attack_hit_dodge";

bool owner_is(const CheckpointEvent& event, std::string_view owner) {
    return event.known_rng_owner == owner;
}

ActionViewCameraCheckpointStatus classify_status(
    std::optional<int> expected,
    int observed_mode0e) {
    if (!expected.has_value()) {
        return ActionViewCameraCheckpointStatus::ObservedOnly;
    }
    if (observed_mode0e < *expected) {
        return ActionViewCameraCheckpointStatus::MissingMode0eDraws;
    }
    if (observed_mode0e > *expected) {
        return ActionViewCameraCheckpointStatus::ExtraMode0eDraws;
    }
    return ActionViewCameraCheckpointStatus::MatchesExpected;
}

} // namespace

ActionViewCameraExpectation first_battle_action_view_camera_expectation(const ParsedProgressEvents& events) {
    ActionViewCameraExpectation expectation;
    expectation.observed_attack_events = static_cast<int>(events.attacks.size());
    expectation.expected_mode0e_camera_draws = 0;
    expectation.expected_mode0_rewrite_gate_draws = expectation.observed_attack_events;
    return expectation;
}

ActionViewCameraCheckpointSummary summarize_action_view_camera_checkpoints(
    const std::vector<CheckpointEvent>& events,
    std::optional<int> expected_mode0e_camera_draws) {
    ActionViewCameraCheckpointSummary summary;
    summary.expected_mode0e_camera_draws = expected_mode0e_camera_draws;

    for (const auto& event : events) {
        if (owner_is(event, kMode0eOwner)) {
            ++summary.observed_mode0e_camera_draws;
            if (!summary.first_mode0e_draw_index.has_value() && event.rng_draw_index_before.has_value()) {
                summary.first_mode0e_draw_index = *event.rng_draw_index_before;
            }
        } else if (owner_is(event, kMode0FallbackOwner)) {
            ++summary.observed_mode0_fallback_draws;
        } else if (owner_is(event, kAttackHitOwner)) {
            ++summary.observed_attack_hit_draws;
            if (!summary.first_attack_hit_draw_index.has_value() && event.rng_draw_index_before.has_value()) {
                summary.first_attack_hit_draw_index = *event.rng_draw_index_before;
            }
        }
    }

    if (summary.first_attack_hit_draw_index.has_value()) {
        for (const auto& event : events) {
            if (!owner_is(event, kMode0eOwner) || !event.rng_draw_index_before.has_value()) {
                continue;
            }
            if (*event.rng_draw_index_before < *summary.first_attack_hit_draw_index) {
                ++summary.mode0e_draws_before_first_attack_hit;
            } else {
                ++summary.mode0e_draws_after_first_attack_hit;
            }
        }
    }

    summary.status = classify_status(
        expected_mode0e_camera_draws,
        summary.observed_mode0e_camera_draws);
    return summary;
}

const char* action_view_camera_checkpoint_status_name(ActionViewCameraCheckpointStatus status) {
    switch (status) {
    case ActionViewCameraCheckpointStatus::ObservedOnly:
        return "ObservedOnly";
    case ActionViewCameraCheckpointStatus::MatchesExpected:
        return "MatchesExpected";
    case ActionViewCameraCheckpointStatus::MissingMode0eDraws:
        return "MissingMode0eDraws";
    case ActionViewCameraCheckpointStatus::ExtraMode0eDraws:
        return "ExtraMode0eDraws";
    default:
        return "Unknown";
    }
}

const char* first_battle_action_view_camera_rule_detail() {
    return "first-battle action-view camera draws split between serialized mode-0 rewrite gate "
           "UpdateActionViewRecord_80051264:800513d4 and conditional runtime mode-0xe "
           "FUN_80052b24:80052bf0 draws; mode 0xe is reached only when the mode-0 gate "
           "rewrites the selected 0x0003002a payload mode";
}

} // namespace savor::predict
