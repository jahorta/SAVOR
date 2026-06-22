#include "ActionViewGateCheckpointModel.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <map>
#include <utility>

namespace savor::predict {

namespace {

constexpr int kExpectedQueryArg0 = 4;
constexpr int kExpectedQueryArg1 = -1;
constexpr int kExpectedQueryArg2 = 0x2a;
constexpr int kExpectedQueryArg3 = 3;
constexpr int kExpectedSelectedRecordMode = 0;
constexpr const char* kMode0eOwner = "mode0e_action_view_camera";
constexpr const char* kMode0FallbackOwner = "mode0_action_view_camera_fallback";
constexpr const char* kAttackHitOwner = "attack_hit_dodge";

std::optional<int> parse_field_int(const CheckpointEvent& event, const char* field_name) {
    const auto found = event.fields.find(field_name);
    if (found == event.fields.end()) {
        return std::nullopt;
    }

    const auto& text = found->second;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(text.c_str(), &end, 16);
        if (end == text.c_str() || *end != '\0') {
            return std::nullopt;
        }
        if (parsed <= static_cast<unsigned long>(std::numeric_limits<int>::max())) {
            return static_cast<int>(parsed);
        }
        if (parsed <= static_cast<unsigned long>(std::numeric_limits<std::uint32_t>::max())) {
            return static_cast<int>(static_cast<std::int32_t>(parsed));
        }
        return std::nullopt;
    }

    char* end = nullptr;
    const long parsed = std::strtol(text.c_str(), &end, 0);
    if (end == text.c_str() || *end != '\0') {
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

std::optional<int> action_sequence_id_from_event(const CheckpointEvent& event) {
    return parse_first_field_int(
        event,
        {"action_sequence_id", "action_sequence", "attack_sequence", "attack_index", "action_index", "sequence_id"});
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

std::optional<bool> parse_field_bool(const CheckpointEvent& event, const char* field_name) {
    const auto found = event.fields.find(field_name);
    if (found == event.fields.end()) {
        return std::nullopt;
    }

    std::string lowered = found->second;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (lowered == "1" || lowered == "true" || lowered == "yes") {
        return true;
    }
    if (lowered == "0" || lowered == "false" || lowered == "no") {
        return false;
    }
    return std::nullopt;
}

std::optional<bool> parse_first_field_bool(
    const CheckpointEvent& event,
    std::initializer_list<const char*> field_names) {
    for (const auto* field_name : field_names) {
        if (const auto value = parse_field_bool(event, field_name); value.has_value()) {
            return value;
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

bool is_dispatch_checkpoint(const CheckpointEvent& event) {
    return event.pc == "80051424"
        || event.checkpoint == "action_view_dispatch_state"
        || event.function == "UpdateActionViewRecord";
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

bool has_scheduler_chain(const ActionViewGateCheckpointEvent& event) {
    return event.action_child_thread.has_value()
        && event.child_payload.has_value()
        && event.nested_payload.has_value()
        && event.aux_list_root.has_value();
}

bool has_spicestd_payload_fields(const ActionViewDispatchCheckpointEvent& event) {
    return event.payload_primary_key.has_value()
        && event.payload_secondary_key.has_value()
        && event.payload_flags.has_value()
        && event.payload_start_frame.has_value()
        && event.payload_end_frame.has_value()
        && event.payload_hold.has_value()
        && event.payload_step.has_value()
        && event.payload_mode.has_value();
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
    if (summary.events_with_action_child_thread != summary.observed_gate_events
        || summary.events_with_child_payload != summary.observed_gate_events
        || summary.events_with_nested_payload != summary.observed_gate_events
        || summary.events_with_child_thread_state != summary.observed_gate_events
        || summary.events_with_scheduler_chain != summary.observed_gate_events
        || summary.events_with_mode0_fallback_flag != summary.observed_gate_events) {
        return ActionViewGateCheckpointStatus::MissingSchedulerFields;
    }
    if (summary.action_sequence_order_mismatches > 0) {
        return ActionViewGateCheckpointStatus::ActionViewOrderMismatch;
    }
    return ActionViewGateCheckpointStatus::MatchesExpected;
}

void record_first_draw_for_sequence(
    std::map<int, int>& draws_by_sequence,
    const CheckpointEvent& event) {
    const auto sequence_id = action_sequence_id_from_event(event);
    if (!sequence_id.has_value() || !event.rng_draw_index_before.has_value()) {
        return;
    }
    draws_by_sequence.emplace(*sequence_id, *event.rng_draw_index_before);
}

} // namespace

ActionViewGateCheckpointSummary summarize_action_view_gate_checkpoints(
    const std::vector<CheckpointEvent>& events) {
    ActionViewGateCheckpointSummary summary;
    std::map<int, int> mode0e_draw_by_sequence;
    std::map<int, int> attack_hit_draw_by_sequence;

    for (const auto& event : events) {
        if (owner_is(event, kMode0eOwner)) {
            ++summary.observed_mode0e_camera_draws;
            record_first_draw_for_sequence(mode0e_draw_by_sequence, event);
            if (!summary.first_mode0e_draw_index.has_value()) {
                summary.first_mode0e_draw_index = event.rng_draw_index_before;
            }
            continue;
        }
        if (owner_is(event, kMode0FallbackOwner)) {
            ++summary.observed_mode0_fallback_draws;
            if (!summary.first_mode0_fallback_draw_index.has_value()) {
                summary.first_mode0_fallback_draw_index = event.rng_draw_index_before;
            }
            continue;
        }
        if (owner_is(event, kAttackHitOwner)) {
            ++summary.observed_attack_hit_draws;
            record_first_draw_for_sequence(attack_hit_draw_by_sequence, event);
            if (!summary.first_attack_hit_draw_index.has_value()) {
                summary.first_attack_hit_draw_index = event.rng_draw_index_before;
            }
            continue;
        }
        if (is_dispatch_checkpoint(event)) {
            ActionViewDispatchCheckpointEvent observed;
            observed.draw_index = event.rng_draw_index_before;
            observed.payload_primary_key = parse_first_field_int(event, {"payload_primary_0x00", "payload_primary"});
            observed.payload_secondary_key =
                parse_first_field_int(event, {"payload_secondary_0x02", "payload_secondary"});
            observed.payload_flags =
                parse_first_field_string(event, {"payload_flags_0x10", "payload_flags"});
            observed.payload_start_frame =
                parse_first_field_int(event, {"payload_start_frame_0x18", "payload_start_frame"});
            observed.payload_end_frame =
                parse_first_field_int(event, {"payload_end_frame_0x1c", "payload_end_frame"});
            observed.payload_hold =
                parse_first_field_int(event, {"payload_hold_0x1e", "payload_hold"});
            observed.payload_step =
                parse_first_field_int(event, {"payload_step_0x20", "payload_step"});
            observed.payload_mode =
                parse_first_field_int(event, {"payload_mode_0x22", "payload_mode", "selected_record_mode"});
            observed.saved_mode =
                parse_first_field_int(event, {"worksheet_saved_mode_0x110", "saved_mode"});
            observed.effective_mode =
                parse_first_field_int(event, {"worksheet_effective_mode_0x112", "effective_mode"});
            observed.worksheet_turn_timer =
                parse_first_field_int(event, {"worksheet_turn_timer_0x70", "turn_timer"});
            observed.instruction_flags =
                parse_first_field_string(event, {"instruction_flags_0xf0", "instruction_flags"});
            observed.global_camera_override =
                parse_first_field_string(event, {"global_camera_override_80347394", "global_camera_override"});
            observed.global_camera_flags =
                parse_first_field_string(event, {"global_camera_flags_803472F4", "global_camera_flags"});

            ++summary.observed_dispatch_events;
            if (observed.payload_mode.has_value()) {
                ++summary.dispatch_events_with_payload_mode;
                if (*observed.payload_mode == 0) {
                    ++summary.dispatch_serialized_mode0_events;
                }
            }
            if (observed.effective_mode.has_value()) {
                ++summary.dispatch_events_with_effective_mode;
                if (*observed.effective_mode == 0) {
                    ++summary.dispatch_effective_mode0_events;
                } else if (*observed.effective_mode == 0x0e) {
                    ++summary.dispatch_effective_mode0e_events;
                }
            }
            if (observed.payload_mode.has_value() && observed.effective_mode.has_value()) {
                if (*observed.payload_mode == 0 && *observed.effective_mode == 0x0e) {
                    ++summary.dispatch_mode0_to_mode0e_rewrites;
                } else if (*observed.payload_mode == 0 && *observed.effective_mode == 0) {
                    ++summary.dispatch_mode0_stays_mode0_events;
                }
            }
            if (has_spicestd_payload_fields(observed)) {
                ++summary.dispatch_events_with_spicestd_payload_fields;
            }
            summary.dispatch_events.push_back(std::move(observed));
            continue;
        }
        if (!is_gate_checkpoint(event)) {
            continue;
        }

        ActionViewGateCheckpointEvent observed;
        observed.draw_index = event.rng_draw_index_before;
        observed.action_sequence_id = action_sequence_id_from_event(event);
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
        observed.action_child_thread = parse_first_field_string(
            event,
            {"action_child_thread", "child_thread", "action_view_child_thread", "action_view_thread", "thread"});
        observed.child_payload =
            parse_first_field_string(event, {"child_payload", "thread_payload", "payload_0x24"});
        observed.nested_payload =
            parse_first_field_string(event, {"nested_payload", "payload_nested", "payload_0x10", "nested_payload_0x10"});
        observed.child_thread_state_byte =
            parse_first_field_int(event, {"child_thread_state_byte", "thread_state_byte", "thread_0x19"});
        observed.mode0_fallback_reached =
            parse_first_field_bool(event, {"mode0_fallback_reached", "fallback_reached", "mode0_fallback"});

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
        if (observed.action_child_thread.has_value()) {
            ++summary.events_with_action_child_thread;
        }
        if (observed.child_payload.has_value()) {
            ++summary.events_with_child_payload;
        }
        if (observed.nested_payload.has_value()) {
            ++summary.events_with_nested_payload;
        }
        if (observed.child_thread_state_byte.has_value()) {
            ++summary.events_with_child_thread_state;
        }
        if (has_scheduler_chain(observed)) {
            ++summary.events_with_scheduler_chain;
        }
        if (observed.mode0_fallback_reached.has_value()) {
            ++summary.events_with_mode0_fallback_flag;
            if (*observed.mode0_fallback_reached) {
                ++summary.mode0_fallback_reached_events;
            }
        }
        summary.events.push_back(std::move(observed));
    }

    for (auto& event : summary.events) {
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
        if (event.action_sequence_id.has_value()) {
            ++summary.events_with_action_sequence_id;
            const auto mode0e_draw = mode0e_draw_by_sequence.find(*event.action_sequence_id);
            const auto attack_hit_draw = attack_hit_draw_by_sequence.find(*event.action_sequence_id);
            if (mode0e_draw != mode0e_draw_by_sequence.end()) {
                event.matched_mode0e_draw_index = mode0e_draw->second;
                event.gate_before_mode0e_draw = *event.draw_index < mode0e_draw->second;
            }
            if (attack_hit_draw != attack_hit_draw_by_sequence.end()) {
                event.matched_attack_hit_draw_index = attack_hit_draw->second;
                event.gate_before_attack_hit_draw = *event.draw_index < attack_hit_draw->second;
            }
            if (mode0e_draw != mode0e_draw_by_sequence.end()
                && attack_hit_draw != attack_hit_draw_by_sequence.end()) {
                event.mode0e_draw_before_attack_hit_draw =
                    mode0e_draw->second < attack_hit_draw->second;
            }

            if (event.gate_before_mode0e_draw.has_value()
                && event.gate_before_attack_hit_draw.has_value()
                && event.mode0e_draw_before_attack_hit_draw.has_value()) {
                ++summary.action_sequence_order_comparisons;
                if (*event.gate_before_mode0e_draw
                    && *event.gate_before_attack_hit_draw
                    && *event.mode0e_draw_before_attack_hit_draw) {
                    ++summary.action_sequence_order_matches;
                } else {
                    ++summary.action_sequence_order_mismatches;
                }
            } else {
                ++summary.action_sequence_order_missing_camera_or_hit;
            }
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

    if (summary.first_mode0e_draw_index.has_value()) {
        for (const auto& event : events) {
            if (owner_is(event, kMode0FallbackOwner)
                && event.rng_draw_index_before.has_value()
                && *event.rng_draw_index_before < *summary.first_mode0e_draw_index) {
                ++summary.mode0_fallback_draws_before_first_mode0e;
            }
        }
    }

    if (summary.first_attack_hit_draw_index.has_value()) {
        for (const auto& event : events) {
            if (owner_is(event, kMode0FallbackOwner)
                && event.rng_draw_index_before.has_value()
                && *event.rng_draw_index_before < *summary.first_attack_hit_draw_index) {
                ++summary.mode0_fallback_draws_before_first_attack_hit;
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
    case ActionViewGateCheckpointStatus::MissingSchedulerFields: return "MissingSchedulerFields";
    case ActionViewGateCheckpointStatus::QueryArgsMismatch: return "QueryArgsMismatch";
    case ActionViewGateCheckpointStatus::SelectedModeMismatch: return "SelectedModeMismatch";
    case ActionViewGateCheckpointStatus::Mode0FallbackReached: return "Mode0FallbackReached";
    case ActionViewGateCheckpointStatus::ActionViewOrderMismatch: return "ActionViewOrderMismatch";
    default: return "Unknown";
    }
}

const char* first_battle_action_view_gate_checkpoint_rule_detail() {
    return "first-battle action-view gate checkpoints should prove the aux-list root, "
           "FUN_80009030 query args (4, -1, 0x2a, 3), query result, and selected serialized "
           "mode 0 when a SpiceStd 0x0003002a record is present; dispatch checkpoints should "
           "then show whether UpdateActionViewRecord kept effective mode 0 or rewrote it to "
           "runtime mode 0xe before the shared attack hit draw";
}

} // namespace savor::predict
