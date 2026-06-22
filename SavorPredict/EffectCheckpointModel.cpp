#include "EffectCheckpointModel.h"

#include <algorithm>
#include <array>
#include <string_view>

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

    for (const auto& event : events) {
        if (owner_in(event, kCombatEffectOwners)) {
            ++summary.observed_combat_effect_draws;
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

    summary.complete_binary_variant_iterations = std::min({
        summary.observed_binary_position_draws,
        summary.observed_scale_x_draws,
        summary.observed_scale_y_draws,
        summary.observed_scale_z_draws,
        summary.observed_variant_index_draws,
    });
    summary.complete_binary_variant_22_loop_executions =
        summary.complete_binary_variant_iterations / 22;
    summary.incomplete_binary_variant_iteration_remainder =
        summary.complete_binary_variant_iterations % 22;
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
    return "Battle1_007 exact RNG Calls observe FUN_80042b10 as four 22-iteration "
           "binary-selector bursts with scale x/y/z and variant-index draws, for "
           "110 draws per execution and 440 draws total; sibling 80041e64 and "
           "800422d0 helpers remain separate live-validation buckets";
}

} // namespace savor::predict
