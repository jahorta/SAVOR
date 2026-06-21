#include "ActionViewGateCheckpointModel.h"

#include <cstdlib>
#include <initializer_list>
#include <utility>

namespace savor::predict {

namespace {

constexpr int kExpectedQueryArg0 = 4;
constexpr int kExpectedQueryArg1 = -1;
constexpr int kExpectedQueryArg2 = 0x2a;
constexpr int kExpectedQueryArg3 = 3;
constexpr int kExpectedSelectedRecordMode = 0x0e;
constexpr const char* kMode0eOwner = "mode0e_action_view_camera";
constexpr const char* kAttackHitOwner = "attack_hit_dodge";

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

std::optional<std::string> parse_first_field_string(
    const CheckpointEvent& event,
    std::initializer_list<const char*> field_names) {
    for (const auto* field_name : field_names) {
        const auto found = event.fields.find(field_name);
        if (found != event.fields.end() && !found->second.empty()) {
            return found->second;
        }
    }
    return std::nullopt;
}

bool owner_is(const CheckpointEvent& event, const char* owner) {
    return event.known_rng_owner == owner;
}

bool is_gate_checkpoint(const CheckpointEvent& event) {
    return event.pc == "80012F58"
        || event.function == "FUN_80012f58"
        || event.function == "FUN_80012F58"
        || event.function == "FUN_80009030"
        || event.checkpoint == "action_view_gate"
        || event.checkpoint == "action_view_query";
}

bool has_query_args(const ActionViewGateCheckpointEvent& event) {
    return event.query_arg0.has_value()
        && event.query_arg1.has_value()
        && event.query_arg2.has_value()
        && event.query_arg3.has_value();
}

bool query_args_match(const ActionViewGateCheckpointEvent& event) {
    return has_query_args(event)
        && *event.query_arg0 == kExpectedQueryArg0
        && *event.query_arg1 == kExpectedQueryArg1
        && *event.query_arg2 == kExpectedQueryArg2
        && *event.query_arg3 == kExpectedQueryArg3;
}

ActionViewGateCheckpointStatus classify_status(const ActionViewGateCheckpointSummary& summary) {
    if (summary.observed_gate_events == 0) {
        return ActionViewGateCheckpointStatus::ObservedOnly;
    }
    if (summary.events_with_aux_list_root != summary.observed_gate_events
        || summary.events_with_query_args != summary.observed_gate_events
        || summary.events_with_query_result != summary.observed_gate_events
        || summary.events_with_selected_record_mode != summary.observed_gate_events) {
        return ActionViewGateCheckpointStatus::MissingLiveGateFields;
    }
    if (summary.query_args_mismatch > 0) {
        return ActionViewGateCheckpointStatus::QueryArgsMismatch;
    }
    if (summary.selected_mode_mismatches > 0) {
        return ActionViewGateCheckpointStatus::SelectedModeMismatch;
    }
    return ActionViewGateCheckpointStatus::MatchesExpected;
}

} // namespace

ActionViewGateCheckpointSummary summarize_action_view_gate_checkpoints(
    const std::vector<CheckpointEvent>& events) {
    ActionViewGateCheckpointSummary summary;

    for (const auto& event : events) {
        if (owner_is(event, kMode0eOwner)) {
            ++summary.observed_mode0e_camera_draws;
            if (!summary.first_mode0e_draw_index.has_value()) {
                summary.first_mode0e_draw_index = event.rng_draw_index_before;
            }
            continue;
        }
        if (owner_is(event, kAttackHitOwner)) {
            ++summary.observed_attack_hit_draws;
            if (!summary.first_attack_hit_draw_index.has_value()) {
                summary.first_attack_hit_draw_index = event.rng_draw_index_before;
            }
            continue;
        }
        if (!is_gate_checkpoint(event)) {
            continue;
        }

        ActionViewGateCheckpointEvent observed;
        observed.draw_index = event.rng_draw_index_before;
        observed.active_slot = event.active_slot.has_value()
            ? event.active_slot
            : parse_first_field_int(event, {"active_slot", "actor_slot"});
        observed.source_slot = parse_first_field_int(event, {"source_slot", "source_actor_slot"});
        observed.target_slot = event.target_slot.has_value()
            ? event.target_slot
            : parse_first_field_int(event, {"target_slot"});
        observed.source_field6_0x6 =
            parse_first_field_int(event, {"source_field6_0x6", "source_field6", "source_field6_6"});
        observed.actor_field6_0x6 =
            parse_first_field_int(event, {"actor_field6_0x6", "actor_field6", "actor_field6_6"});
        observed.aux_list_root =
            parse_first_field_string(event, {"aux_list_root", "aux_root", "list_root"});
        observed.query_arg0 = parse_first_field_int(event, {"query_arg0", "query_a"});
        observed.query_arg1 = parse_first_field_int(event, {"query_arg1", "query_b"});
        observed.query_arg2 = parse_first_field_int(event, {"query_arg2", "query_c"});
        observed.query_arg3 = parse_first_field_int(event, {"query_arg3", "query_d"});
        observed.query_result =
            parse_first_field_string(event, {"query_result", "aux_result", "selected_record_ptr"});
        observed.selected_record_mode =
            parse_first_field_int(event, {"selected_record_mode", "record_mode", "selected_mode"});

        ++summary.observed_gate_events;
        if (!summary.first_gate_draw_index.has_value()) {
            summary.first_gate_draw_index = observed.draw_index;
        }
        if (observed.aux_list_root.has_value()) {
            ++summary.events_with_aux_list_root;
        }
        if (has_query_args(observed)) {
            ++summary.events_with_query_args;
            if (query_args_match(observed)) {
                ++summary.query_args_match;
            } else {
                ++summary.query_args_mismatch;
            }
        }
        if (observed.query_result.has_value()) {
            ++summary.events_with_query_result;
        }
        if (observed.selected_record_mode.has_value()) {
            ++summary.events_with_selected_record_mode;
            if (*observed.selected_record_mode == kExpectedSelectedRecordMode) {
                ++summary.selected_mode_matches;
            } else {
                ++summary.selected_mode_mismatches;
            }
        }
        summary.events.push_back(std::move(observed));
    }

    for (const auto& event : summary.events) {
        if (!event.draw_index.has_value()) {
            continue;
        }
        if (summary.first_mode0e_draw_index.has_value()
            && *event.draw_index < *summary.first_mode0e_draw_index) {
            ++summary.gate_events_before_first_mode0e;
        }
        if (summary.first_attack_hit_draw_index.has_value()
            && *event.draw_index < *summary.first_attack_hit_draw_index) {
            ++summary.gate_events_before_first_attack_hit;
        }
    }

    if (summary.first_attack_hit_draw_index.has_value()) {
        for (const auto& event : events) {
            if (owner_is(event, kMode0eOwner)
                && event.rng_draw_index_before.has_value()
                && *event.rng_draw_index_before < *summary.first_attack_hit_draw_index) {
                ++summary.mode0e_draws_before_first_attack_hit;
            }
        }
    }

    summary.status = classify_status(summary);
    return summary;
}

const char* action_view_gate_checkpoint_status_name(ActionViewGateCheckpointStatus status) {
    switch (status) {
    case ActionViewGateCheckpointStatus::ObservedOnly: return "ObservedOnly";
    case ActionViewGateCheckpointStatus::MatchesExpected: return "MatchesExpected";
    case ActionViewGateCheckpointStatus::MissingLiveGateFields: return "MissingLiveGateFields";
    case ActionViewGateCheckpointStatus::QueryArgsMismatch: return "QueryArgsMismatch";
    case ActionViewGateCheckpointStatus::SelectedModeMismatch: return "SelectedModeMismatch";
    default: return "Unknown";
    }
}

const char* first_battle_action_view_gate_checkpoint_rule_detail() {
    return "first-battle action-view gate checkpoints should prove the aux-list root, "
           "FUN_80009030 query args (4, -1, 0x2a, 3), query result, and selected mode 0xe "
           "before the mode-0xe camera draw and shared attack hit draw";
}

} // namespace savor::predict
