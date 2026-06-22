#include "PreAiCheckpointModel.h"

#include "PreAiCameraModel.h"

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace savor::predict {

namespace {

constexpr std::string_view kBattleStartCameraOwner = "pre_ai_battle_start_camera";
constexpr std::string_view kTargetingCameraOwner = "pre_ai_attack_targeting_camera";
constexpr std::string_view kSoldierAiActionOwner = "soldier_ai_action_decision";
constexpr std::string_view kFakeAttackOwner = "pre_ai_fake_attack";
constexpr std::string_view kFakeAttackNoRandOwner = "pre_ai_fake_attack_no_rand";

bool owner_is(const CheckpointEvent& event, std::string_view owner) {
    return event.known_rng_owner == owner;
}

std::optional<int> parse_field_int(const CheckpointEvent& event, const char* field_name) {
    const auto found = event.fields.find(field_name);
    if (found == event.fields.end()) {
        return std::nullopt;
    }

    char* end = nullptr;
    const long parsed = std::strtol(found->second.c_str(), &end, 0);
    if (end == found->second.c_str() || *end != '\0') {
        return std::nullopt;
    }
    return static_cast<int>(parsed);
}

std::optional<int> parse_first_field_int(
    const CheckpointEvent& event,
    std::initializer_list<const char*> field_names) {
    for (const auto* field_name : field_names) {
        if (const auto value = parse_field_int(event, field_name); value.has_value()) {
            return value;
        }
    }
    return std::nullopt;
}

bool checkpoint_is(const CheckpointEvent& event, std::initializer_list<std::string_view> names) {
    for (const auto name : names) {
        if (event.checkpoint == name || event.function == name) {
            return true;
        }
    }
    return false;
}

bool is_fake_attack_checkpoint(const CheckpointEvent& event) {
    const auto owner = event.fields.find("owner");
    if (owner != event.fields.end() && owner->second == kFakeAttackOwner) {
        return true;
    }
    return checkpoint_is(event, {
        "fake_attack",
        "pre_ai_fake_attack",
        "fakeAttack",
        "FakeAttack",
    });
}

bool is_fake_attack_no_rand_checkpoint(const CheckpointEvent& event) {
    const auto owner = event.fields.find("owner");
    if (owner != event.fields.end() && owner->second == kFakeAttackNoRandOwner) {
        return true;
    }
    return checkpoint_is(event, {
        "fake_attack_no_rand",
        "fake_attack_skipped_rand",
        "pre_ai_fake_attack_no_rand",
        "pre_ai_fake_attack_skipped_rand",
    });
}

PreAiCheckpointStatus classify_status(const PreAiCheckpointSummary& summary) {
    if (!summary.expectation.has_value()) {
        return PreAiCheckpointStatus::ObservedOnly;
    }

    const auto& expectation = *summary.expectation;
    if (summary.observed_battle_start_camera_draws < expectation.expected_battle_start_camera_draws) {
        return PreAiCheckpointStatus::MissingBattleStartCameraDraw;
    }
    if (summary.observed_battle_start_camera_draws > expectation.expected_battle_start_camera_draws) {
        return PreAiCheckpointStatus::ExtraBattleStartCameraDraws;
    }
    if (summary.observed_fake_attack_draws < expectation.expected_fake_attack_draws
        && summary.observed_skipped_fake_attack_draws > 0) {
        return PreAiCheckpointStatus::SkippedFakeAttackDrawsObserved;
    }
    if (summary.observed_fake_attack_draws < expectation.expected_fake_attack_draws) {
        return PreAiCheckpointStatus::MissingFakeAttackDraws;
    }
    if (summary.observed_fake_attack_draws > expectation.expected_fake_attack_draws) {
        return PreAiCheckpointStatus::ExtraFakeAttackDraws;
    }
    if (summary.observed_targeting_camera_draws < expectation.expected_targeting_camera_draws) {
        return PreAiCheckpointStatus::MissingTargetingCameraDraws;
    }
    if (summary.observed_targeting_camera_draws > expectation.expected_targeting_camera_draws) {
        return PreAiCheckpointStatus::ExtraTargetingCameraDraws;
    }
    if (summary.first_soldier_ai_draw_index.has_value()) {
        if (*summary.first_soldier_ai_draw_index
            != expectation.expected_first_soldier_ai_draw_index_before) {
            return PreAiCheckpointStatus::FirstSoldierAiDrawMismatch;
        }
    } else {
        return PreAiCheckpointStatus::MissingFirstSoldierAiDraw;
    }

    return PreAiCheckpointStatus::MatchesExpected;
}

void record_draw_index_bounds(
    const std::optional<int>& draw_index,
    std::optional<int>& first,
    std::optional<int>& last) {
    if (!draw_index.has_value()) {
        return;
    }
    if (!first.has_value()) {
        first = *draw_index;
    }
    last = *draw_index;
}

void record_min_max(
    int value,
    std::optional<int>& min_value,
    std::optional<int>& max_value) {
    if (!min_value.has_value() || value < *min_value) {
        min_value = value;
    }
    if (!max_value.has_value() || value > *max_value) {
        max_value = value;
    }
}

bool is_fake_attempt_draw(const PreAiCheckpointDraw& draw) {
    const std::string_view owner(draw.owner);
    return owner == kFakeAttackOwner || owner == kFakeAttackNoRandOwner;
}

void record_previous_gap_transition(
    const PreAiCheckpointDraw& previous,
    int& transitions_with_previous_gap,
    std::optional<int>& min_previous_gap,
    std::optional<int>& max_previous_gap) {
    if (!previous.camera_frame_gap.has_value()) {
        return;
    }
    ++transitions_with_previous_gap;
    record_min_max(
        *previous.camera_frame_gap,
        min_previous_gap,
        max_previous_gap);
}

void summarize_fake_attempt_transitions(PreAiCheckpointSummary& summary) {
    std::vector<const PreAiCheckpointDraw*> attempts;
    bool can_sort_by_attempt_index = true;
    std::set<int> seen_attempt_indices;

    for (const auto& draw : summary.draws) {
        if (!is_fake_attempt_draw(draw)) {
            continue;
        }
        attempts.push_back(&draw);
        if (!draw.fake_attack_index.has_value()
            || !seen_attempt_indices.insert(*draw.fake_attack_index).second) {
            can_sort_by_attempt_index = false;
        }
    }

    if (can_sort_by_attempt_index) {
        std::sort(
            attempts.begin(),
            attempts.end(),
            [](const auto* lhs, const auto* rhs) {
                return *lhs->fake_attack_index < *rhs->fake_attack_index;
            });
    }

    for (std::size_t i = 1; i < attempts.size(); ++i) {
        const auto& previous = *attempts[i - 1];
        const auto& current = *attempts[i];
        ++summary.fake_attack_attempt_transitions;

        if (!previous.skipped_rng_draw && current.skipped_rng_draw) {
            ++summary.draw_to_skip_fake_attack_transitions;
            record_previous_gap_transition(
                previous,
                summary.draw_to_skip_transitions_with_previous_frame_gap,
                summary.min_draw_to_skip_previous_camera_frame_gap,
                summary.max_draw_to_skip_previous_camera_frame_gap);
        } else if (previous.skipped_rng_draw && !current.skipped_rng_draw) {
            ++summary.skip_to_draw_fake_attack_transitions;
            record_previous_gap_transition(
                previous,
                summary.skip_to_draw_transitions_with_previous_frame_gap,
                summary.min_skip_to_draw_previous_camera_frame_gap,
                summary.max_skip_to_draw_previous_camera_frame_gap);
        } else if (!previous.skipped_rng_draw && !current.skipped_rng_draw) {
            ++summary.draw_to_draw_fake_attack_transitions;
        } else {
            ++summary.skip_to_skip_fake_attack_transitions;
        }
    }
}

} // namespace

PreAiCheckpointExpectation first_battle_pre_ai_checkpoint_expectation(int fake_attacks) {
    const auto model = model_pre_ai_camera_draws(fake_attacks);

    PreAiCheckpointExpectation expectation;
    expectation.expected_fake_attack_attempts = model.fake_attacks;
    expectation.expected_fake_attack_draws = model.fake_attack_draws;
    expectation.expected_battle_start_camera_draws = 1;
    expectation.expected_targeting_camera_draws =
        model.expected_camera_draws - expectation.expected_battle_start_camera_draws;
    expectation.expected_total_pre_ai_draws = model.expected_total_draws;
    expectation.expected_first_soldier_ai_draw_index_before = model.expected_total_draws;
    return expectation;
}

PreAiCheckpointSummary summarize_pre_ai_checkpoints(
    const std::vector<CheckpointEvent>& events,
    std::optional<int> expected_fake_attacks) {
    PreAiCheckpointSummary summary;
    if (expected_fake_attacks.has_value()) {
        summary.expectation = first_battle_pre_ai_checkpoint_expectation(*expected_fake_attacks);
    }

    for (const auto& event : events) {
        if (owner_is(event, kBattleStartCameraOwner)) {
            ++summary.observed_battle_start_camera_draws;
            ++summary.observed_pre_ai_draws;
            PreAiCheckpointDraw draw;
            draw.owner = std::string(kBattleStartCameraOwner);
            draw.draw_index = event.rng_draw_index_before;
            draw.active_slot = event.active_slot;
            draw.target_slot = event.target_slot;
            if (!summary.first_battle_start_camera_draw_index.has_value()
                && draw.draw_index.has_value()) {
                summary.first_battle_start_camera_draw_index = *draw.draw_index;
            }
            summary.draws.push_back(std::move(draw));
            continue;
        }

        if (owner_is(event, kTargetingCameraOwner)) {
            ++summary.observed_targeting_camera_draws;
            ++summary.observed_pre_ai_draws;
            PreAiCheckpointDraw draw;
            draw.owner = std::string(kTargetingCameraOwner);
            draw.draw_index = event.rng_draw_index_before;
            draw.active_slot = event.active_slot;
            draw.target_slot = event.target_slot.has_value()
                ? event.target_slot
                : parse_first_field_int(event, {"target_slot", "target"});
            if (draw.target_slot.has_value()) {
                ++summary.targeting_draws_with_target_slot;
            }
            record_draw_index_bounds(
                draw.draw_index,
                summary.first_targeting_camera_draw_index,
                summary.last_targeting_camera_draw_index);
            summary.draws.push_back(std::move(draw));
            continue;
        }

        if (is_fake_attack_no_rand_checkpoint(event)) {
            ++summary.observed_fake_attack_attempts;
            ++summary.observed_skipped_fake_attack_draws;
            PreAiCheckpointDraw draw;
            draw.owner = std::string(kFakeAttackNoRandOwner);
            draw.draw_index = event.rng_draw_index_before;
            draw.active_slot = event.active_slot;
            draw.target_slot = event.target_slot;
            draw.fake_attack_index =
                parse_first_field_int(event, {"fake_attack_index", "fake_index", "fake_attack"});
            draw.camera_frame_gap =
                parse_first_field_int(event, {"camera_frame_gap", "target_camera_frame_gap", "a_to_b_frames"});
            draw.skipped_rng_draw = true;
            if (draw.camera_frame_gap.has_value()) {
                ++summary.skipped_fake_attempts_with_frame_gap;
                record_min_max(
                    *draw.camera_frame_gap,
                    summary.min_skipped_fake_attempt_camera_frame_gap,
                    summary.max_skipped_fake_attempt_camera_frame_gap);
            }
            summary.draws.push_back(std::move(draw));
            continue;
        }

        if (is_fake_attack_checkpoint(event)) {
            ++summary.observed_fake_attack_attempts;
            ++summary.observed_fake_attack_draws;
            ++summary.observed_pre_ai_draws;
            PreAiCheckpointDraw draw;
            draw.owner = std::string(kFakeAttackOwner);
            draw.draw_index = event.rng_draw_index_before;
            draw.active_slot = event.active_slot;
            draw.target_slot = event.target_slot;
            draw.fake_attack_index =
                parse_first_field_int(event, {"fake_attack_index", "fake_index", "fake_attack"});
            draw.camera_frame_gap =
                parse_first_field_int(event, {"camera_frame_gap", "target_camera_frame_gap", "a_to_b_frames"});
            if (draw.camera_frame_gap.has_value()) {
                ++summary.fake_attack_draws_with_frame_gap;
                record_min_max(
                    *draw.camera_frame_gap,
                    summary.min_fake_attack_draw_camera_frame_gap,
                    summary.max_fake_attack_draw_camera_frame_gap);
            }
            if (draw.fake_attack_index.has_value()) {
                ++summary.fake_draws_with_fake_attack_index;
            }
            record_draw_index_bounds(
                draw.draw_index,
                summary.first_fake_attack_draw_index,
                summary.last_fake_attack_draw_index);
            summary.draws.push_back(std::move(draw));
            continue;
        }

        if (owner_is(event, kSoldierAiActionOwner)) {
            ++summary.observed_first_soldier_ai_draws;
            if (!summary.first_soldier_ai_draw_index.has_value()) {
                summary.first_soldier_ai_draw_index = event.rng_draw_index_before;
            }
        }
    }

    if (summary.first_soldier_ai_draw_index.has_value()) {
        for (const auto& draw : summary.draws) {
            if (!draw.skipped_rng_draw
                && draw.draw_index.has_value()
                && *draw.draw_index < *summary.first_soldier_ai_draw_index) {
                ++summary.pre_ai_draws_before_first_soldier_ai;
            }
        }
    }

    summarize_fake_attempt_transitions(summary);

    summary.status = classify_status(summary);
    return summary;
}

const char* pre_ai_checkpoint_status_name(PreAiCheckpointStatus status) {
    switch (status) {
    case PreAiCheckpointStatus::ObservedOnly: return "ObservedOnly";
    case PreAiCheckpointStatus::MatchesExpected: return "MatchesExpected";
    case PreAiCheckpointStatus::MissingBattleStartCameraDraw: return "MissingBattleStartCameraDraw";
    case PreAiCheckpointStatus::ExtraBattleStartCameraDraws: return "ExtraBattleStartCameraDraws";
    case PreAiCheckpointStatus::SkippedFakeAttackDrawsObserved: return "SkippedFakeAttackDrawsObserved";
    case PreAiCheckpointStatus::MissingFakeAttackDraws: return "MissingFakeAttackDraws";
    case PreAiCheckpointStatus::ExtraFakeAttackDraws: return "ExtraFakeAttackDraws";
    case PreAiCheckpointStatus::MissingTargetingCameraDraws: return "MissingTargetingCameraDraws";
    case PreAiCheckpointStatus::ExtraTargetingCameraDraws: return "ExtraTargetingCameraDraws";
    case PreAiCheckpointStatus::MissingFirstSoldierAiDraw: return "MissingFirstSoldierAiDraw";
    case PreAiCheckpointStatus::FirstSoldierAiDrawMismatch: return "FirstSoldierAiDrawMismatch";
    default: return "Unknown";
    }
}

const char* first_battle_pre_ai_checkpoint_rule_detail() {
    return "first-battle pre-AI checkpoints should show one 8001413c battle-start camera draw, "
           "fake_attack_count fake draws, two 800608dc targeting-camera draws for the two PCs, "
           "and the first 8008b428 Soldier AI draw at the resulting cursor; "
           "the v1 input macro is expected to enforce one RNG draw per fake attack and no targeting-camera suppression; "
           "target-camera A-to-B frame gaps are recorded as observations for both rand-consuming "
           "and no-rand fake-attack attempts; "
           "consecutive fake attempts are summarized as draw-to-skip, skip-to-draw, draw-to-draw, "
           "and skip-to-skip transitions using attempt index order when present; "
           "the old fake-attack suppression behavior remains a research gap but is outside the fixed v1 macro contract";
}

} // namespace savor::predict
