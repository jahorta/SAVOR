#include "BattleVisualRngModel.h"

#include "ActionViewCameraModel.h"
#include "EffectRngModel.h"

#include <iomanip>
#include <sstream>
#include <utility>

namespace savor::predict {
namespace {

constexpr int kMode0eRecordMode = 0x0e;
constexpr std::uint32_t kMode0eCameraRngPc = 0x80052bf0U;
constexpr std::uint32_t kMode0FallbackRngPc = 0x800513d4U;
constexpr std::uint32_t kMode0eDispatchPc = 0x800514c4U;

void append_step(BattleVisualRngModelResult& result, BattleVisualRngStep step) {
    result.total_draws += step.draws_consumed;
    if (step.status == BattleVisualRngStepStatus::MissingInput) {
        result.has_missing_input_steps = true;
    }
    if (step.status == BattleVisualRngStepStatus::Ambiguous) {
        result.has_ambiguous_steps = true;
    }
    if (step.status == BattleVisualRngStepStatus::Unsupported) {
        result.has_unsupported_steps = true;
    }
    result.steps.push_back(std::move(step));
}

std::string effect_detail(
    int source_key,
    const CombatEffectBurstSequenceModel& model,
    std::optional<int> instruction_mode_0x6,
    bool attack_was_critical,
    bool counter_follow_up) {
    std::ostringstream out;
    out << "source_key=" << source_key
        << "; buffers=" << model.bursts.size()
        << "; total_loops=" << model.total_loop_count;
    if (counter_follow_up) {
        out << "; source_key_selection=counter_follow_up_forced_hit";
    } else if (instruction_mode_0x6.has_value()) {
        out << "; instruction_mode_0x6=" << *instruction_mode_0x6
            << "; source_key_selection=instruction_mode";
    } else if (attack_was_critical) {
        out << "; source_key_selection=legacy_critical_fallback";
    } else {
        out << "; source_key_selection=legacy_actor_slot_fallback";
    }
    out << "; " << combat_effect_burst_rule_detail();
    return out.str();
}

std::string hex_u32(std::uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::nouppercase << value;
    return out.str();
}

std::string action_view_selector_detail(const ActionViewSelectorResult& selector) {
    std::ostringstream out;
    out << "requested_mode=" << static_cast<int>(selector.requested_mode)
        << "; dispatch_effective_mode_0x2f=" << static_cast<int>(selector.dispatch_effective_mode_0x2f)
        << "; selector_state_0x30=" << selector.selector_state_0x30
        << "; mode0e_query_reached=" << (selector.mode0e_query_reached ? 1 : 0);
    if (selector.spawned_action_view_record_mode_if_known.has_value()) {
        out << "; spawned_action_view_record_mode="
            << *selector.spawned_action_view_record_mode_if_known;
    }
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

void append_dispatch_evidence_detail(
    std::string& detail,
    const ActionViewDispatchEvidence& evidence) {
    std::ostringstream out;
    out << "; action_view_dispatch_evidence=1";
    if (evidence.record_mode_0x22.has_value()) {
        out << "; live_record_mode_0x22=" << *evidence.record_mode_0x22;
    }
    if (evidence.effective_mode_0x112.has_value()) {
        out << "; live_effective_mode_0x112=" << *evidence.effective_mode_0x112;
    }
    if (evidence.saved_mode_0x110.has_value()) {
        out << "; live_saved_mode_0x110=" << *evidence.saved_mode_0x110;
    }
    if (evidence.dispatch_pc.has_value()) {
        out << "; live_dispatch_pc=" << hex_u32(*evidence.dispatch_pc);
    }
    if (evidence.rng_pc.has_value()) {
        out << "; live_rng_pc=" << hex_u32(*evidence.rng_pc);
    }
    if (!evidence.source_tag.empty()) {
        out << "; live_source=" << evidence.source_tag;
    }
    detail += out.str();
}

BattleVisualRngStep mode0e_camera_step(std::string detail) {
    return {
        .label = "mode0e_action_view_camera",
        .status = BattleVisualRngStepStatus::Provisional,
        .draws_consumed = 1,
        .detail = std::move(detail),
    };
}

BattleVisualRngStep mode0_fallback_camera_step(std::string detail) {
    return {
        .label = "mode0_action_view_camera_fallback",
        .status = BattleVisualRngStepStatus::Provisional,
        .draws_consumed = 1,
        .detail = std::move(detail),
    };
}

BattleVisualRngStep known_no_camera_step(std::string detail) {
    return {
        .label = "action_view_record_mode_no_camera_rng",
        .status = BattleVisualRngStepStatus::Provisional,
        .detail = std::move(detail),
    };
}

BattleVisualRngStep ambiguous_dispatch_evidence_step(std::string detail) {
    return {
        .label = "ambiguous_action_view_camera_dispatch_evidence",
        .status = BattleVisualRngStepStatus::Ambiguous,
        .detail = std::move(detail),
    };
}

std::optional<BattleVisualRngStep> model_action_view_camera_from_dispatch_evidence(
    const ActionViewDispatchEvidence& evidence,
    std::string detail) {
    append_dispatch_evidence_detail(detail, evidence);

    const bool rng_pc_is_mode0e =
        evidence.rng_pc.has_value() && *evidence.rng_pc == kMode0eCameraRngPc;
    const bool rng_pc_is_mode0_fallback =
        evidence.rng_pc.has_value() && *evidence.rng_pc == kMode0FallbackRngPc;
    const bool dispatch_pc_is_mode0e =
        evidence.dispatch_pc.has_value() && *evidence.dispatch_pc == kMode0eDispatchPc;
    const bool record_mode_is_mode0e =
        evidence.record_mode_0x22.has_value() && *evidence.record_mode_0x22 == kMode0eRecordMode;
    const bool record_mode_conflicts_with_mode0e_pc =
        evidence.record_mode_0x22.has_value()
        && *evidence.record_mode_0x22 != kMode0eRecordMode
        && (rng_pc_is_mode0e || dispatch_pc_is_mode0e);
    const bool record_mode_conflicts_with_fallback_pc =
        record_mode_is_mode0e && rng_pc_is_mode0_fallback;

    if (record_mode_conflicts_with_mode0e_pc || record_mode_conflicts_with_fallback_pc) {
        return ambiguous_dispatch_evidence_step(std::move(detail));
    }
    if (rng_pc_is_mode0e || dispatch_pc_is_mode0e || record_mode_is_mode0e) {
        return mode0e_camera_step(std::move(detail));
    }
    if (rng_pc_is_mode0_fallback) {
        return mode0_fallback_camera_step(std::move(detail));
    }
    if (evidence.record_mode_0x22.has_value()) {
        return known_no_camera_step(std::move(detail));
    }

    return std::nullopt;
}

BattleVisualRngStep model_action_view_camera_step(const BattleVisualRngActionInput& input) {
    auto detail = input.action_view_selector.has_value()
        ? action_view_selector_detail(*input.action_view_selector)
        : std::string(first_battle_action_view_camera_rule_detail());
    if (input.action_view_dispatch_evidence.has_value()) {
        const auto live_step = model_action_view_camera_from_dispatch_evidence(
            *input.action_view_dispatch_evidence,
            detail);
        if (live_step.has_value()) {
            return *live_step;
        }
    }

    if (!input.action_view_selector.has_value()) {
        return {
            .label = "mode0_action_view_camera_rewrite_gate",
            .status = BattleVisualRngStepStatus::Exact,
            .draws_consumed = 1,
            .detail = std::move(detail),
        };
    }

    const auto& selector = *input.action_view_selector;
    if (selector.unsupported_without_aux_table) {
        return {
            .label = "missing_input_action_view_camera_aux_table",
            .status = BattleVisualRngStepStatus::MissingInput,
            .detail = detail,
        };
    }

    if (!selector.mode0e_query_reached) {
        if (selector.spawned_action_view_record_mode_if_known.has_value()) {
            if (*selector.spawned_action_view_record_mode_if_known == kMode0eRecordMode) {
                return mode0e_camera_step(std::move(detail));
            }
            return known_no_camera_step(std::move(detail));
        }
        return known_no_camera_step(std::move(detail));
    }
    if (!selector.mode0e_count.has_value()) {
        return {
            .label = "missing_input_action_view_camera_mode0e_count",
            .status = BattleVisualRngStepStatus::MissingInput,
            .detail = detail,
        };
    }

    if (selector.mode0e_synthetic_call_selected) {
        if (selector.spawned_action_view_record_mode_if_known.has_value()
            && *selector.spawned_action_view_record_mode_if_known != kMode0eRecordMode) {
            return known_no_camera_step(std::move(detail));
        }
        return mode0e_camera_step(std::move(detail));
    }

    return mode0_fallback_camera_step(std::move(detail));
}

} // namespace

std::optional<int> first_battle_basic_attack_effect_source_key(
    int actor_slot,
    bool attack_was_critical,
    bool counter_follow_up,
    std::optional<int> instruction_mode_0x6) {
    if (counter_follow_up) {
        return actor_slot < 4 ? 5 : 4;
    }

    if (instruction_mode_0x6.has_value()) {
        switch (*instruction_mode_0x6) {
        case 4:
            return 4;
        case 5:
            return 5;
        case 8:
            return 8;
        default:
            return std::nullopt;
        }
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

    if (!is_first_battle_soldiers_profile_name(input.profile_name)) {
        append_step(result, {
            .label = "unsupported_visual_rng_profile",
            .status = BattleVisualRngStepStatus::Unsupported,
            .detail = "visual RNG adapter currently supports first-battle only",
        });
        return result;
    }

    if (input.include_action_view_camera) {
        append_step(result, model_action_view_camera_step(input));
        if (result.has_missing_input_steps) {
            return result;
        }
    }

    if (!input.include_effect_bursts || !input.attack_landed) {
        return result;
    }

    const auto source_key = first_battle_basic_attack_effect_source_key(
        input.actor_slot,
        input.attack_was_critical,
        input.counter_follow_up,
        input.instruction_mode_0x6);
    if (!source_key.has_value()) {
        append_step(result, {
            .label = "unsupported_effect_source_actor_slot",
            .status = BattleVisualRngStepStatus::Unsupported,
            .detail = "first-battle effect source key supports basic attacks from slots 0, 1, 4, and 5 only",
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
    const bool legacy_critical_fallback =
        input.attack_was_critical
        && !input.counter_follow_up
        && !input.instruction_mode_0x6.has_value();
    append_step(result, {
        .label = "combat_effect_burst",
        .status = legacy_critical_fallback
            ? BattleVisualRngStepStatus::Provisional
            : BattleVisualRngStepStatus::Exact,
        .draws_consumed = model.total_draws,
        .effect_source_key = source_key,
        .detail = effect_detail(
            *source_key,
            model,
            input.instruction_mode_0x6,
            input.attack_was_critical,
            input.counter_follow_up),
    });

    return result;
}

const char* battle_visual_rng_step_status_name(BattleVisualRngStepStatus status) {
    switch (status) {
    case BattleVisualRngStepStatus::Exact: return "Exact";
    case BattleVisualRngStepStatus::Provisional: return "Provisional";
    case BattleVisualRngStepStatus::MissingInput: return "MissingInput";
    case BattleVisualRngStepStatus::Ambiguous: return "Ambiguous";
    case BattleVisualRngStepStatus::Unsupported: return "Unsupported";
    }
    return "Unsupported";
}

const char* first_battle_basic_attack_visual_rng_rule_detail() {
    return "first-battle basic attacks model one action-view mode-0 rewrite-gate camera draw "
           "before hit/damage resolution, then landed-hit combat effect bursts as the action "
           "tail before the battle controller advances to the next actor; the tail uses source "
           "keys 4/5/8; modeled InstructionWorksheet+0x6 mode is preferred over raw critical "
           "state, with the critical-to-key8 shortcut retained only as a visible fallback; "
           "counter follow-ups use the forced-hit path and select "
           "source key 4 for enemy counters or source key 5 for PC counters";
}

} // namespace savor::predict
