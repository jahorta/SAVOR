#include "BattleVisualRngModel.h"

#include "ActionViewCameraModel.h"
#include "EffectRngModel.h"

#include <sstream>
#include <utility>

namespace savor::predict {
namespace {

void append_step(BattleVisualRngModelResult& result, BattleVisualRngStep step) {
    result.total_draws += step.draws_consumed;
    if (step.status == BattleVisualRngStepStatus::Ambiguous) {
        result.has_ambiguous_steps = true;
    }
    if (step.status == BattleVisualRngStepStatus::Unsupported) {
        result.has_unsupported_steps = true;
    }
    result.steps.push_back(std::move(step));
}

std::string effect_detail(int source_key, const CombatEffectBurstSequenceModel& model) {
    std::ostringstream out;
    out << "source_key=" << source_key
        << "; buffers=" << model.bursts.size()
        << "; total_loops=" << model.total_loop_count
        << "; " << combat_effect_burst_rule_detail();
    return out.str();
}

} // namespace

std::optional<int> first_battle_basic_attack_effect_source_key(
    int actor_slot,
    bool attack_was_critical,
    bool counter_follow_up) {
    if (counter_follow_up) {
        return actor_slot < 4 ? 5 : 4;
    }

    if (attack_was_critical) {
        return 8;
    }

    switch (actor_slot) {
    case 0:
        return 4;
    case 1:
    case 4:
    case 5:
        return 5;
    default:
        return std::nullopt;
    }
}

BattleVisualRngModelResult model_first_battle_basic_attack_visual_rng(
    const BattleVisualRngActionInput& input) {
    BattleVisualRngModelResult result;

    if (input.profile_name != "first-battle") {
        append_step(result, {
            .label = "unsupported_visual_rng_profile",
            .status = BattleVisualRngStepStatus::Unsupported,
            .detail = "visual RNG adapter currently supports first-battle only",
        });
        return result;
    }

    if (input.include_action_view_camera) {
        append_step(result, {
            .label = "mode0_action_view_camera_rewrite_gate",
            .status = BattleVisualRngStepStatus::Exact,
            .draws_consumed = 1,
            .detail = first_battle_action_view_camera_rule_detail(),
        });
    }

    if (!input.include_effect_bursts || !input.attack_landed) {
        return result;
    }

    const auto source_key = first_battle_basic_attack_effect_source_key(
        input.actor_slot,
        input.attack_was_critical,
        input.counter_follow_up);
    if (!source_key.has_value()) {
        append_step(result, {
            .label = "ambiguous_effect_source_key",
            .status = BattleVisualRngStepStatus::Ambiguous,
            .detail = "first-battle effect source key is not known for this actor slot",
        });
        return result;
    }

    const auto burst_inputs = first_battle_effect_burst_sequence_for_source_key(*source_key);
    if (burst_inputs.empty()) {
        append_step(result, {
            .label = "unsupported_effect_source_key",
            .status = BattleVisualRngStepStatus::Unsupported,
            .effect_source_key = source_key,
            .detail = "no first-battle effect burst model is available for this source key",
        });
        return result;
    }

    const auto model = model_combat_effect_burst_sequence_draws(burst_inputs);
    append_step(result, {
        .label = "combat_effect_burst",
        .status = BattleVisualRngStepStatus::Exact,
        .draws_consumed = model.total_draws,
        .effect_source_key = source_key,
        .detail = effect_detail(*source_key, model),
    });

    return result;
}

const char* battle_visual_rng_step_status_name(BattleVisualRngStepStatus status) {
    switch (status) {
    case BattleVisualRngStepStatus::Exact: return "Exact";
    case BattleVisualRngStepStatus::Provisional: return "Provisional";
    case BattleVisualRngStepStatus::Ambiguous: return "Ambiguous";
    case BattleVisualRngStepStatus::Unsupported: return "Unsupported";
    }
    return "Unsupported";
}

const char* first_battle_basic_attack_visual_rng_rule_detail() {
    return "first-battle basic attacks model one action-view mode-0 rewrite-gate camera draw "
           "before hit/damage resolution, then landed-hit combat effect bursts using source "
           "keys 4/5 for non-critical first-battle actors and source key 8 for the observed "
           "successful-critical path; counter follow-ups use the forced-hit path and select "
           "source key 4 for enemy counters or source key 5 for PC counters";
}

} // namespace savor::predict
