#include "EffectRngModel.h"

#include <algorithm>

namespace savor::predict {

CombatEffectBurstModel model_combat_effect_burst_draws(const CombatEffectBurstInput& input) {
    CombatEffectBurstModel model;
    model.loop_count = std::max(0, input.loop_count);
    model.position_selector_draws = model.loop_count;
    model.scale_draws = model.loop_count * 3;
    model.variant_index_draws = input.variant_index_draw ? model.loop_count : 0;
    model.axis_assignment_draws = input.axis_assignment_draw ? model.loop_count : 0;
    model.total_draws =
        model.position_selector_draws
        + model.scale_draws
        + model.variant_index_draws
        + model.axis_assignment_draws;
    model.draws_per_iteration = model.loop_count == 0 ? 0 : model.total_draws / model.loop_count;
    return model;
}

CombatEffectBurstSequenceModel model_combat_effect_burst_sequence_draws(
    const std::vector<CombatEffectBurstInput>& inputs) {
    CombatEffectBurstSequenceModel sequence;
    sequence.bursts.reserve(inputs.size());
    for (const auto& input : inputs) {
        auto burst = model_combat_effect_burst_draws(input);
        sequence.total_loop_count += burst.loop_count;
        sequence.total_draws += burst.total_draws;
        sequence.bursts.push_back(burst);
    }
    return sequence;
}

EffectEmitterSpawnModel model_effect_emitter_spawn_draws(const EffectEmitterSpawnInput& input) {
    EffectEmitterSpawnModel model;
    model.outer_count = std::max(0, input.outer_count);
    model.base_random_float_draws = model.outer_count * 5;
    model.spawn_variant_draws = input.spawn_variant_draw ? model.outer_count : 0;
    model.axis_variant_draws = input.axis_variant_draw ? model.outer_count : 0;
    model.total_draws =
        model.base_random_float_draws
        + model.spawn_variant_draws
        + model.axis_variant_draws;
    model.draws_per_outer = model.outer_count == 0 ? 0 : model.total_draws / model.outer_count;
    model.child_tasks = model.outer_count * std::max(0, input.child_count_per_outer);
    return model;
}

EffectParticleTickModel model_effect_particle_tick_draws(const EffectParticleTickInput& input) {
    EffectParticleTickModel model;
    model.tick_count = std::max(0, input.tick_count);
    model.total_draws = input.active_update_tick ? model.tick_count * model.draws_per_active_tick : 0;
    return model;
}

std::vector<CombatEffectBurstInput> first_battle_landed_basic_attack_effect_burst_sequence() {
    return first_battle_effect_burst_sequence_for_source_key(4);
}

std::optional<CombatEffectBurstInput>
first_battle_effect_burst_for_source_key_occurrence(
    int source_key,
    std::size_t occurrence) {
    if (occurrence > 1U) {
        return std::nullopt;
    }

    int second_loop_count = 0;
    if (source_key == 4 || source_key == 5) {
        second_loop_count = 6;
    } else if (source_key == 8) {
        second_loop_count = 4;
    } else {
        return std::nullopt;
    }

    return CombatEffectBurstInput{
        .loop_count = occurrence == 0U ? 16 : second_loop_count,
        .position_selector = CombatEffectPositionSelector::Binary,
        .variant_index_draw = true,
        .axis_assignment_draw = false,
    };
}

std::vector<CombatEffectBurstInput> first_battle_effect_burst_sequence_for_source_key(int source_key) {
    std::vector<CombatEffectBurstInput> sequence;
    sequence.reserve(2);
    for (std::size_t occurrence = 0; occurrence < 2U; ++occurrence) {
        const auto burst =
            first_battle_effect_burst_for_source_key_occurrence(source_key, occurrence);
        if (!burst.has_value()) {
            return {};
        }
        sequence.push_back(*burst);
    }
    return sequence;
}

const char* combat_effect_burst_rule_detail() {
    return "FUN_80042b10 spends one mutually exclusive position-selector draw, "
           "three scale draws, an optional variant-index draw, and an optional "
           "axis-assignment draw per loop iteration; live first-battle landed basic "
           "attacks use copied source-record loop counts: keys 4/5 are two "
           "selected binary/variant buffers, 16 loops then 6 loops, while the "
           "observed successful-crit key 8 path is 16 loops then 4 loops; neither "
           "uses an axis draw";
}

const char* effect_emitter_spawn_rule_detail() {
    return "FUN_80041e64 spends five base random-float draws per outer spawn, "
           "plus optional spawn-variant and axis-variant draws; child task draws "
           "belong to FUN_800422d0 and should be modeled separately";
}

const char* effect_particle_tick_rule_detail() {
    return "FUN_800422d0 spends four draws per active update tick while its "
           "state/lifetime gate is active; exact first-battle tick counts still "
           "need live validation";
}

} // namespace savor::predict
