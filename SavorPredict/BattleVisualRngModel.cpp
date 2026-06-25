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

std::string action_view_selector_detail(const ActionViewSelectorResult& selector) {
    std::ostringstream out;
    out << "requested_mode=" << static_cast<int>(selector.requested_mode)
        << "; dispatch_effective_mode_0x2f=" << static_cast<int>(selector.dispatch_effective_mode_0x2f)
        << "; selector_state_0x30=" << selector.selector_state_0x30
        << "; mode0e_query_reached=" << (selector.mode0e_query_reached ? 1 : 0);
    if (selector.mode0e_count.has_value()) {
        out << "; mode0e_count=" << selector.mode0e_count->count
            << "; scanned_entries=" << selector.mode0e_count->scanned_entries
            << "; reached_sentinel=" << (selector.mode0e_count->reached_sentinel ? 1 : 0);
    }
    if (selector.mode3_count.has_value()) {
        out << "; mode3_count=" << selector.mode3_count->count;
    }
    if (selector.mode5_count.has_value()) {
        out << "; mode5_count=" << selector.mode5_count->count;
    }
    if (selector.unsupported_without_aux_table) {
        out << "; missing_selected_aux_table=1";
    }
    if (!selector.branch_path.empty()) {
        out << "; branch_path=";
        for (std::size_t i = 0; i < selector.branch_path.size(); ++i) {
            if (i != 0) {
                out << ">";
            }
            out << selector.branch_path[i];
        }
    }
    out << "; " << action_view_selector_model_rule_detail();
    return out.str();
}

BattleVisualRngStep model_action_view_camera_step(const BattleVisualRngActionInput& input) {
    if (!input.action_view_selector.has_value()) {
        return {
            .label = "mode0_action_view_camera_rewrite_gate",
            .status = BattleVisualRngStepStatus::Exact,
            .draws_consumed = 1,
            .detail = first_battle_action_view_camera_rule_detail(),
        };
    }

    const auto& selector = *input.action_view_selector;
    auto detail = action_view_selector_detail(selector);
    if (selector.unsupported_without_aux_table) {
        return {
            .label = "ambiguous_action_view_camera_missing_aux_table",
            .status = BattleVisualRngStepStatus::Ambiguous,
            .draws_consumed = 1,
            .detail = detail,
        };
    }

    if (!selector.mode0e_query_reached || !selector.mode0e_count.has_value()) {
        return {
            .label = "ambiguous_action_view_camera_selector_path",
            .status = BattleVisualRngStepStatus::Ambiguous,
            .draws_consumed = 1,
            .detail = detail,
        };
    }

    if (selector.mode0e_synthetic_call_selected) {
        return {
            .label = "mode0e_action_view_camera",
            .status = BattleVisualRngStepStatus::Exact,
            .draws_consumed = 1,
            .detail = detail,
        };
    }

    return {
        .label = "mode0_action_view_camera_fallback",
        .status = BattleVisualRngStepStatus::Exact,
        .draws_consumed = 1,
        .detail = detail,
    };
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
        append_step(result, model_action_view_camera_step(input));
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
           "before hit/damage resolution, then landed-hit combat effect bursts as the action "
           "tail before the battle controller advances to the next actor; the tail uses source "
           "keys 4/5 for non-critical first-battle actors and source key 8 for the observed "
           "successful-critical path; counter follow-ups use the forced-hit path and select "
           "source key 4 for enemy counters or source key 5 for PC counters";
}

} // namespace savor::predict
