#include "EffectCheckpointModel.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace savor::predict {

namespace {

constexpr std::string_view kPositionBinaryOwner = "combat_effect_spawn_position_binary";
constexpr std::string_view kPositionFourWayOwner = "combat_effect_spawn_position_four_way";
constexpr std::string_view kScaleXOwner = "combat_effect_spawn_scale_x";
constexpr std::string_view kScaleYOwner = "combat_effect_spawn_scale_y";
constexpr std::string_view kScaleZOwner = "combat_effect_spawn_scale_z";
constexpr std::string_view kVariantIndexOwner = "combat_effect_spawn_variant_index";
constexpr std::string_view kAxisAssignmentOwner = "combat_effect_spawn_axis_assignment";

constexpr std::array<std::string_view, 7> kCombatEffectOwners = {
    kPositionBinaryOwner,
    kPositionFourWayOwner,
    kScaleXOwner,
    kScaleYOwner,
    kScaleZOwner,
    kVariantIndexOwner,
    kAxisAssignmentOwner,
};

constexpr std::array<std::string_view, 7> kEmitterSpawnOwners = {
    "effect_emitter_spawn_variant",
    "effect_emitter_spawn_offset_x",
    "effect_emitter_spawn_offset_y",
    "effect_emitter_spawn_scale_a",
    "effect_emitter_spawn_scale_b",
    "effect_emitter_spawn_scale_c",
    "effect_emitter_axis_variant",
};

constexpr std::array<std::string_view, 4> kParticleTickOwners = {
    "effect_particle_motion_gate_x",
    "effect_particle_motion_offset_x",
    "effect_particle_motion_gate_z",
    "effect_particle_motion_offset_z",
};

bool owner_is(const CheckpointEvent& event, std::string_view owner) {
    return event.known_rng_owner == owner;
}

bool owner_in(const CheckpointEvent& event, const auto& owners) {
    return std::find(owners.begin(), owners.end(), event.known_rng_owner) != owners.end();
}

bool has_field(const CheckpointEvent& event, std::string_view field_name) {
    return event.fields.find(std::string(field_name)) != event.fields.end();
}

std::optional<std::string> parse_string_field(const CheckpointEvent& event, std::string_view field_name) {
    const auto found = event.fields.find(std::string(field_name));
    if (found == event.fields.end() || found->second.empty()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<int> parse_int_field(const CheckpointEvent& event, std::string_view field_name) {
    const auto found = event.fields.find(std::string(field_name));
    if (found == event.fields.end()) {
        return std::nullopt;
    }

    const auto& value = found->second;
    char* end = nullptr;
    const int base = (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0) ? 16 : 10;
    const long parsed = std::strtol(value.c_str(), &end, base);
    if (end == value.c_str() || *end != '\0') {
        return std::nullopt;
    }
    if (parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(parsed);
}

bool is_effect_record_copy_checkpoint(const CheckpointEvent& event) {
    return event.pc == "8003BB24"
        || event.checkpoint == "effect_record_copy_complete";
}

struct CombatEffectBufferStats {
    std::string key;
    std::optional<int> first_draw_index;
    std::optional<int> source_key;
    int loop_count = -1;
    int binary_position_draws = 0;
    int four_way_position_draws = 0;
    int scale_x_draws = 0;
    int scale_y_draws = 0;
    int scale_z_draws = 0;
    int variant_index_draws = 0;
    int axis_assignment_draws = 0;
};

struct FirstBattleEffectPairKey {
    int source_key = -1;
    int first_loop_count = 0;
    int second_loop_count = 0;

    bool operator<(const FirstBattleEffectPairKey& other) const {
        if (source_key != other.source_key) {
            return source_key < other.source_key;
        }
        if (first_loop_count != other.first_loop_count) {
            return first_loop_count < other.first_loop_count;
        }
        return second_loop_count < other.second_loop_count;
    }
};

void observe_combat_effect_buffer_draw(CombatEffectBufferStats& stats, const CheckpointEvent& event) {
    if (!stats.first_draw_index.has_value()
        || (event.rng_draw_index_before.has_value()
            && *event.rng_draw_index_before < *stats.first_draw_index)) {
        stats.first_draw_index = event.rng_draw_index_before;
    }

    if (const auto loop_count = parse_int_field(event, "effect_loop_count_0x5c")) {
        stats.loop_count = *loop_count;
    }
    if (const auto source_key = parse_int_field(event, "effect_source_key_0x28")) {
        stats.source_key = *source_key;
    }

    if (owner_is(event, kPositionBinaryOwner)) {
        ++stats.binary_position_draws;
    } else if (owner_is(event, kPositionFourWayOwner)) {
        ++stats.four_way_position_draws;
    } else if (owner_is(event, kScaleXOwner)) {
        ++stats.scale_x_draws;
    } else if (owner_is(event, kScaleYOwner)) {
        ++stats.scale_y_draws;
    } else if (owner_is(event, kScaleZOwner)) {
        ++stats.scale_z_draws;
    } else if (owner_is(event, kVariantIndexOwner)) {
        ++stats.variant_index_draws;
    } else if (owner_is(event, kAxisAssignmentOwner)) {
        ++stats.axis_assignment_draws;
    }
}

bool is_complete_binary_variant_buffer(const CombatEffectBufferStats& stats) {
    return stats.loop_count > 0
        && stats.binary_position_draws == stats.loop_count
        && stats.four_way_position_draws == 0
        && stats.scale_x_draws == stats.loop_count
        && stats.scale_y_draws == stats.loop_count
        && stats.scale_z_draws == stats.loop_count
        && stats.variant_index_draws == stats.loop_count
        && stats.axis_assignment_draws == 0;
}

std::optional<int> expected_first_battle_second_loop_count_for_source_key(int source_key) {
    if (source_key == 4 || source_key == 5) {
        return 6;
    }
    if (source_key == 8) {
        return 4;
    }
    return std::nullopt;
}

bool source_keys_match(const CombatEffectBufferStats& current, const CombatEffectBufferStats& next) {
    return current.source_key.has_value()
        && next.source_key.has_value()
        && *current.source_key == *next.source_key;
}

bool is_supported_first_battle_effect_pair(
    const CombatEffectBufferStats& current,
    const CombatEffectBufferStats& next) {
    if (current.loop_count != 16) {
        return false;
    }

    if (source_keys_match(current, next)) {
        const auto expected_second_loop_count =
            expected_first_battle_second_loop_count_for_source_key(*current.source_key);
        return expected_second_loop_count.has_value()
            && next.loop_count == *expected_second_loop_count;
    }

    if (current.source_key.has_value()) {
        const auto expected_second_loop_count =
            expected_first_battle_second_loop_count_for_source_key(*current.source_key);
        return expected_second_loop_count.has_value()
            && next.loop_count == *expected_second_loop_count;
    }

    // Preserve the original source-key-free test fixture behavior.
    return !next.source_key.has_value() && next.loop_count == 6;
}

EffectCheckpointStatus classify_status(const EffectCheckpointSummary& summary) {
    if (summary.observed_combat_effect_draws == 0) {
        return EffectCheckpointStatus::ObservedOnly;
    }

    const bool matches_binary_variant_shape =
        summary.observed_binary_position_draws > 0
        && summary.observed_four_way_position_draws == 0
        && summary.observed_axis_assignment_draws == 0
        && summary.observed_binary_position_draws == summary.observed_scale_x_draws
        && summary.observed_binary_position_draws == summary.observed_scale_y_draws
        && summary.observed_binary_position_draws == summary.observed_scale_z_draws
        && summary.observed_binary_position_draws == summary.observed_variant_index_draws;

    return matches_binary_variant_shape
        ? EffectCheckpointStatus::MatchesBinaryVariantBurstShape
        : EffectCheckpointStatus::UnbalancedCombatEffectBurst;
}

} // namespace

EffectCheckpointSummary summarize_effect_checkpoints(const std::vector<CheckpointEvent>& events) {
    EffectCheckpointSummary summary;
    std::map<std::string, CombatEffectBufferStats> combat_effect_buffers;

    for (const auto& event : events) {
        if (owner_in(event, kCombatEffectOwners)) {
            ++summary.observed_combat_effect_draws;
            if (const auto buffer = event.fields.find("r29_effect_buffer");
                buffer != event.fields.end() && !buffer->second.empty()) {
                ++summary.combat_effect_draws_with_buffer_pointer;
                auto& stats = combat_effect_buffers[buffer->second];
                stats.key = buffer->second;
                observe_combat_effect_buffer_draw(stats, event);
            }
            if (has_field(event, "effect_loop_count_0x5c")) {
                ++summary.combat_effect_draws_with_loop_count;
            }
            if (has_field(event, "effect_flags_0x38")) {
                ++summary.combat_effect_draws_with_flags;
            }
            if (has_field(event, "effect_variant_count_0x5e")) {
                ++summary.combat_effect_draws_with_variant_count;
            }
            if (has_field(event, "effect_axis_mode_0x60")) {
                ++summary.combat_effect_draws_with_axis_mode;
            }
            if (has_field(event, "effect_source_key_0x28")) {
                ++summary.combat_effect_draws_with_source_key;
            }
            if (has_field(event, "effect_source_subtype_0x2a")) {
                ++summary.combat_effect_draws_with_source_subtype;
            }
            if (has_field(event, "effect_source_secondary_0x2c")) {
                ++summary.combat_effect_draws_with_source_secondary;
            }
            if (has_field(event, "effect_source_resource_id_0x30")) {
                ++summary.combat_effect_draws_with_source_resource_id;
            }
            if (event.rng_draw_index_before.has_value()) {
                if (!summary.first_combat_effect_draw_index.has_value()) {
                    summary.first_combat_effect_draw_index = *event.rng_draw_index_before;
                }
                summary.last_combat_effect_draw_index = *event.rng_draw_index_before;
            }
        } else if (owner_in(event, kEmitterSpawnOwners)) {
            ++summary.observed_emitter_spawn_draws;
        } else if (owner_in(event, kParticleTickOwners)) {
            ++summary.observed_particle_tick_draws;
            if (has_field(event, "particle_payload_source_ptr_0x20")) {
                ++summary.particle_tick_draws_with_payload_source;
            }
            if (has_field(event, "particle_payload_lifetime_0x28")) {
                ++summary.particle_tick_draws_with_lifetime;
            }
        } else if (event.checkpoint == "effect_emitter_source_gate") {
            ++summary.observed_emitter_source_gate_events;
            if (has_field(event, "emitter_outer_count_0x142")) {
                ++summary.emitter_source_gate_events_with_outer_count;
            }
            if (has_field(event, "emitter_child_count_0x1c")) {
                ++summary.emitter_source_gate_events_with_child_count;
            }
            if (has_field(event, "emitter_variant_count_0x16")) {
                ++summary.emitter_source_gate_events_with_variant_count;
            }
            if (has_field(event, "emitter_axis_mode_0x3c")) {
                ++summary.emitter_source_gate_events_with_axis_mode;
            }
        } else if (is_effect_record_copy_checkpoint(event)) {
            EffectRecordCopyCheckpointEvent copy_event;
            copy_event.draw_index = event.rng_draw_index_before;
            copy_event.effect_buffer = parse_string_field(event, "r6_effect_buffer");
            if (!copy_event.effect_buffer.has_value()) {
                copy_event.effect_buffer = parse_string_field(event, "effect_buffer");
            }
            copy_event.source_record = parse_string_field(event, "r31_source_record");
            if (!copy_event.source_record.has_value()) {
                copy_event.source_record = parse_string_field(event, "source_record");
            }
            copy_event.parent_action_thread = parse_string_field(event, "r30_parent_action_thread");
            if (!copy_event.parent_action_thread.has_value()) {
                copy_event.parent_action_thread = parse_string_field(event, "parent_action_thread");
            }
            copy_event.copied_parent_action_thread =
                parse_string_field(event, "effect_parent_action_thread_0x04");
            copy_event.source_key = parse_int_field(event, "effect_source_key_0x28");
            copy_event.source_record_key = parse_int_field(event, "source_record_key_0x00");
            copy_event.loop_count = parse_int_field(event, "effect_loop_count_0x5c");
            copy_event.source_record_loop_count = parse_int_field(event, "source_record_loop_count_0x34");

            ++summary.observed_effect_record_copy_events;
            if (copy_event.effect_buffer.has_value()) {
                ++summary.effect_record_copy_events_with_effect_buffer;
            }
            if (copy_event.parent_action_thread.has_value()
                || copy_event.copied_parent_action_thread.has_value()) {
                ++summary.effect_record_copy_events_with_parent_action_thread;
            }
            if (copy_event.source_key.has_value()) {
                ++summary.effect_record_copy_events_with_source_key;
            }
            if (copy_event.loop_count.has_value()) {
                ++summary.effect_record_copy_events_with_loop_count;
            }
            if (copy_event.source_key.has_value()
                && copy_event.source_record_key.has_value()
                && *copy_event.source_key == *copy_event.source_record_key
                && copy_event.loop_count.has_value()
                && copy_event.source_record_loop_count.has_value()
                && *copy_event.loop_count == *copy_event.source_record_loop_count) {
                ++summary.effect_record_copy_events_matching_source_record_fields;
            }
            summary.record_copy_events.push_back(std::move(copy_event));
        }

        if (owner_is(event, kPositionBinaryOwner)) {
            ++summary.observed_binary_position_draws;
        } else if (owner_is(event, kPositionFourWayOwner)) {
            ++summary.observed_four_way_position_draws;
        } else if (owner_is(event, kScaleXOwner)) {
            ++summary.observed_scale_x_draws;
        } else if (owner_is(event, kScaleYOwner)) {
            ++summary.observed_scale_y_draws;
        } else if (owner_is(event, kScaleZOwner)) {
            ++summary.observed_scale_z_draws;
        } else if (owner_is(event, kVariantIndexOwner)) {
            ++summary.observed_variant_index_draws;
        } else if (owner_is(event, kAxisAssignmentOwner)) {
            ++summary.observed_axis_assignment_draws;
        }
    }

    summary.observed_combat_effect_buffers = static_cast<int>(combat_effect_buffers.size());

    std::vector<CombatEffectBufferStats> complete_first_battle_buffers;
    for (const auto& [_, stats] : combat_effect_buffers) {
        if (!is_complete_binary_variant_buffer(stats)) {
            continue;
        }
        ++summary.complete_binary_variant_buffers;
        if (stats.loop_count == 16) {
            ++summary.complete_binary_variant_16_loop_buffers;
            complete_first_battle_buffers.push_back(stats);
        } else if (stats.loop_count == 6) {
            ++summary.complete_binary_variant_6_loop_buffers;
            complete_first_battle_buffers.push_back(stats);
        } else if (stats.loop_count == 4) {
            ++summary.complete_binary_variant_4_loop_buffers;
            complete_first_battle_buffers.push_back(stats);
        }
    }

    std::sort(
        complete_first_battle_buffers.begin(),
        complete_first_battle_buffers.end(),
        [](const CombatEffectBufferStats& lhs, const CombatEffectBufferStats& rhs) {
            const auto lhs_draw = lhs.first_draw_index.value_or(std::numeric_limits<int>::max());
            const auto rhs_draw = rhs.first_draw_index.value_or(std::numeric_limits<int>::max());
            if (lhs_draw != rhs_draw) {
                return lhs_draw < rhs_draw;
            }
            return lhs.key < rhs.key;
        });

    std::vector<bool> paired(complete_first_battle_buffers.size(), false);
    std::map<FirstBattleEffectPairKey, int> pairs_by_source_key;
    for (std::size_t i = 0; i + 1 < complete_first_battle_buffers.size(); ++i) {
        if (paired[i]) {
            continue;
        }
        const auto& current = complete_first_battle_buffers[i];
        const auto& next = complete_first_battle_buffers[i + 1];
        if (!paired[i + 1] && is_supported_first_battle_effect_pair(current, next)) {
            paired[i] = true;
            paired[i + 1] = true;
            ++summary.complete_first_battle_landed_attack_effect_pairs;
            if (current.loop_count == 16 && next.loop_count == 6) {
                ++summary.complete_first_battle_16_6_effect_pairs;
            } else if (current.loop_count == 16 && next.loop_count == 4) {
                ++summary.complete_first_battle_16_4_effect_pairs;
            }
            const int pair_iterations = current.loop_count + next.loop_count;
            summary.complete_first_battle_landed_attack_effect_pair_iterations += pair_iterations;
            summary.complete_first_battle_landed_attack_effect_pair_draws += pair_iterations * 5;
            if (source_keys_match(current, next)) {
                ++summary.complete_first_battle_landed_attack_effect_pairs_with_matching_source_key;
                ++pairs_by_source_key[FirstBattleEffectPairKey{
                    *current.source_key,
                    current.loop_count,
                    next.loop_count,
                }];
            } else {
                ++summary.complete_first_battle_landed_attack_effect_pairs_without_matching_source_key;
            }
            ++i;
        }
    }
    for (const auto& [pair_key, pair_count] : pairs_by_source_key) {
        summary.complete_first_battle_effect_pairs_by_source_key.push_back(
            EffectSourceKeyPairCount{
                pair_key.source_key,
                pair_key.first_loop_count,
                pair_key.second_loop_count,
                pair_key.first_loop_count + pair_key.second_loop_count,
                (pair_key.first_loop_count + pair_key.second_loop_count) * 5,
                pair_count,
            });
    }
    for (std::size_t i = 0; i < complete_first_battle_buffers.size(); ++i) {
        if (!paired[i]) {
            ++summary.unpaired_first_battle_effect_buffers;
        }
    }

    for (auto& copy_event : summary.record_copy_events) {
        if (!copy_event.effect_buffer.has_value()) {
            continue;
        }
        const auto found = combat_effect_buffers.find(*copy_event.effect_buffer);
        if (found == combat_effect_buffers.end()) {
            continue;
        }
        ++summary.effect_record_copy_events_matching_combat_effect_buffer;
        copy_event.matched_combat_effect_first_draw_index = found->second.first_draw_index;
    }

    summary.complete_binary_variant_iterations = std::min({
        summary.observed_binary_position_draws,
        summary.observed_scale_x_draws,
        summary.observed_scale_y_draws,
        summary.observed_scale_z_draws,
        summary.observed_variant_index_draws,
    });
    summary.incomplete_binary_variant_iteration_remainder =
        summary.complete_binary_variant_iterations
        - summary.complete_first_battle_landed_attack_effect_pair_iterations;
    summary.status = classify_status(summary);
    return summary;
}

const char* effect_checkpoint_status_name(EffectCheckpointStatus status) {
    switch (status) {
    case EffectCheckpointStatus::ObservedOnly:
        return "ObservedOnly";
    case EffectCheckpointStatus::MatchesBinaryVariantBurstShape:
        return "MatchesBinaryVariantBurstShape";
    case EffectCheckpointStatus::UnbalancedCombatEffectBurst:
        return "UnbalancedCombatEffectBurst";
    default:
        return "Unknown";
    }
}

const char* first_battle_effect_checkpoint_rule_detail() {
    return "Live first-battle checkpoints observe supported landed basic attacks as "
           "two FUN_80042b10 effect buffers selected from copied source records: "
           "source keys 4/5 use 16 binary-selector/variant loops followed by 6 "
           "more, for 110 draws; the observed successful-crit key 8 path uses "
           "16 followed by 4, for 100 draws; "
           "FUN_8003ba08 effect-record copy checkpoints should match those later "
           "FUN_80042b10 buffers by effect-buffer pointer; workbook 22-loop groups "
           "are a flattened view without the live r29 buffer split";
}

} // namespace savor::predict
