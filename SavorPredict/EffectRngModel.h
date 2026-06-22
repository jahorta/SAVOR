#pragma once

#include <vector>

namespace savor::predict {

enum class CombatEffectPositionSelector {
    Binary,
    FourWay,
};

struct CombatEffectBurstInput {
    int loop_count = 0;
    CombatEffectPositionSelector position_selector = CombatEffectPositionSelector::Binary;
    bool variant_index_draw = false;
    bool axis_assignment_draw = false;
};

struct CombatEffectBurstModel {
    int loop_count = 0;
    int draws_per_iteration = 0;
    int position_selector_draws = 0;
    int scale_draws = 0;
    int variant_index_draws = 0;
    int axis_assignment_draws = 0;
    int total_draws = 0;
};

struct CombatEffectBurstSequenceModel {
    std::vector<CombatEffectBurstModel> bursts;
    int total_loop_count = 0;
    int total_draws = 0;
};

struct EffectEmitterSpawnInput {
    int outer_count = 0;
    bool spawn_variant_draw = false;
    bool axis_variant_draw = false;
    int child_count_per_outer = 0;
};

struct EffectEmitterSpawnModel {
    int outer_count = 0;
    int draws_per_outer = 0;
    int base_random_float_draws = 0;
    int spawn_variant_draws = 0;
    int axis_variant_draws = 0;
    int total_draws = 0;
    int child_tasks = 0;
};

struct EffectParticleTickInput {
    int tick_count = 0;
    bool active_update_tick = true;
};

struct EffectParticleTickModel {
    int tick_count = 0;
    int draws_per_active_tick = 4;
    int total_draws = 0;
};

CombatEffectBurstModel model_combat_effect_burst_draws(const CombatEffectBurstInput& input);
CombatEffectBurstSequenceModel model_combat_effect_burst_sequence_draws(
    const std::vector<CombatEffectBurstInput>& inputs);
EffectEmitterSpawnModel model_effect_emitter_spawn_draws(const EffectEmitterSpawnInput& input);
EffectParticleTickModel model_effect_particle_tick_draws(const EffectParticleTickInput& input);

std::vector<CombatEffectBurstInput> first_battle_landed_basic_attack_effect_burst_sequence();
const char* combat_effect_burst_rule_detail();
const char* effect_emitter_spawn_rule_detail();
const char* effect_particle_tick_rule_detail();

} // namespace savor::predict
