#include "ActionSourceCheckpointModel.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <utility>

namespace savor::predict {

namespace {

constexpr std::string_view kSourceSelectionPc = "8006782C";
constexpr std::string_view kActionSourcePc = "8006721C";
constexpr std::string_view kExpectedFirstBattleHandlerPc = "800662BC";

std::string normalize_pc(std::string value) {
    if (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0) {
        value = value.substr(2);
    }
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
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

std::optional<std::string> parse_first_pc_field(
    const CheckpointEvent& event,
    std::initializer_list<const char*> field_names) {
    for (const auto* field_name : field_names) {
        const auto found = event.fields.find(field_name);
        if (found != event.fields.end() && !found->second.empty()) {
            return normalize_pc(found->second);
        }
    }
    return std::nullopt;
}

bool is_source_selection_checkpoint(const CheckpointEvent& event) {
    if (event.pc == kSourceSelectionPc) {
        return true;
    }
    return event.function == "FUN_8006782c"
        || event.function == "FUN_8006782C"
        || event.checkpoint == "source_selection"
        || event.checkpoint == "action_source_selection";
}

bool is_action_source_checkpoint(const CheckpointEvent& event) {
    if (event.pc == kActionSourcePc) {
        return true;
    }
    return event.function == "FUN_8006721c"
        || event.function == "FUN_8006721C"
        || event.checkpoint == "action_source";
}

ActionSourceCheckpointStatus classify_status(const ActionSourceCheckpointSummary& summary) {
    if (summary.observed_source_selection_events == 0
        && summary.observed_action_source_events == 0) {
        return ActionSourceCheckpointStatus::ObservedOnly;
    }
    if (summary.observed_action_source_events == 0) {
        return ActionSourceCheckpointStatus::ObservedOnly;
    }
    if (summary.observed_source_selection_events < summary.observed_action_source_events) {
        return ActionSourceCheckpointStatus::MissingSourceSelectionCheckpoint;
    }
    if (summary.source_selection_bridge_missing_by_action_sequence_id > 0) {
        return ActionSourceCheckpointStatus::MissingSourceSelectionCheckpoint;
    }
    if (summary.source_selection_events_with_source_slot != summary.observed_source_selection_events) {
        return ActionSourceCheckpointStatus::MissingLiveSourceFields;
    }
    if (summary.events_with_actor_slot != summary.observed_action_source_events
        || summary.events_with_source_slot != summary.observed_action_source_events
        || summary.events_with_source_field6 != summary.observed_action_source_events
        || summary.events_with_actor_field6 != summary.observed_action_source_events) {
        return ActionSourceCheckpointStatus::MissingLiveSourceFields;
    }
    if (summary.source_selection_bridge_mismatches > 0) {
        return ActionSourceCheckpointStatus::SourceSelectionMismatch;
    }
    if (summary.field6_mismatches > 0) {
        return ActionSourceCheckpointStatus::Field6Mismatch;
    }
    if (summary.expected_handler_pc.has_value()) {
        if (summary.events_with_handler_pc != summary.observed_action_source_events) {
            return ActionSourceCheckpointStatus::MissingLiveSourceFields;
        }
        if (summary.handler_mismatches > 0) {
            return ActionSourceCheckpointStatus::HandlerMismatch;
        }
    }
    if (summary.expected_callback_pc.has_value()) {
        if (summary.events_with_callback_pc != summary.observed_action_source_events) {
            return ActionSourceCheckpointStatus::MissingLiveSourceFields;
        }
        if (summary.callback_mismatches > 0) {
            return ActionSourceCheckpointStatus::CallbackMismatch;
        }
    }
    return ActionSourceCheckpointStatus::MatchesExpected;
}

void maybe_set_first(std::optional<int>& target, const std::optional<int>& value) {
    if (!target.has_value() && value.has_value()) {
        target = *value;
    }
}

std::optional<int> action_sequence_id_from_event(const CheckpointEvent& event) {
    return parse_first_field_int(
        event,
        {"action_sequence_id", "action_sequence", "attack_sequence", "attack_index", "action_index", "sequence_id"});
}

bool all_bridges_have_action_sequence_id(
    const std::vector<ActionSourceCheckpointEvent*>& bridges) {
    return !bridges.empty() && std::all_of(bridges.begin(), bridges.end(), [](const auto* bridge) {
        return bridge->action_sequence_id.has_value();
    });
}

bool any_selection_has_action_sequence_id(
    const std::vector<const ActionSourceCheckpointEvent*>& source_selections) {
    return std::any_of(source_selections.begin(), source_selections.end(), [](const auto* selection) {
        return selection->action_sequence_id.has_value();
    });
}

std::map<int, const ActionSourceCheckpointEvent*> source_selection_by_action_sequence_id(
    const std::vector<const ActionSourceCheckpointEvent*>& source_selections) {
    std::map<int, const ActionSourceCheckpointEvent*> result;
    for (const auto* selection : source_selections) {
        if (selection->action_sequence_id.has_value()
            && result.find(*selection->action_sequence_id) == result.end()) {
            result.emplace(*selection->action_sequence_id, selection);
        }
    }
    return result;
}

void compare_source_selection_pair(
    ActionSourceCheckpointSummary& summary,
    ActionSourceCheckpointEvent& bridge,
    const ActionSourceCheckpointEvent& selection) {
    bridge.matched_source_selection_draw_index = selection.draw_index;
    bridge.matched_source_selection_source_slot = selection.source_slot;
    if (selection.draw_index.has_value() && bridge.draw_index.has_value()) {
        bridge.source_selection_before_bridge = *selection.draw_index <= *bridge.draw_index;
        if (*bridge.source_selection_before_bridge) {
            ++summary.source_selection_bridge_order_matches;
        } else {
            ++summary.source_selection_bridge_order_mismatches;
        }
    }
    if (!selection.source_slot.has_value() || !bridge.source_slot.has_value()) {
        return;
    }
    bridge.source_slot_matches_selection = *selection.source_slot == *bridge.source_slot;
    if (*bridge.source_slot_matches_selection) {
        ++summary.source_selection_bridge_matches;
    } else {
        ++summary.source_selection_bridge_mismatches;
    }
}

} // namespace

ActionSourceCheckpointExpectation first_battle_action_source_checkpoint_expectation() {
    ActionSourceCheckpointExpectation expectation;
    expectation.expected_handler_pc = std::string(kExpectedFirstBattleHandlerPc);
    expectation.expected_callback_pc = std::string(kExpectedFirstBattleHandlerPc);
    return expectation;
}

ActionSourceCheckpointSummary summarize_action_source_checkpoints(
    const std::vector<CheckpointEvent>& events,
    std::optional<std::string> expected_handler_pc) {
    ActionSourceCheckpointSummary summary;
    if (expected_handler_pc.has_value()) {
        summary.expected_handler_pc = normalize_pc(std::move(*expected_handler_pc));
        summary.expected_callback_pc = summary.expected_handler_pc;
    }

    for (const auto& event : events) {
        if (is_source_selection_checkpoint(event)) {
            ActionSourceCheckpointEvent observed;
            observed.kind = ActionSourceCheckpointKind::SourceSelection;
            observed.draw_index = event.rng_draw_index_before;
            observed.actor_slot = event.active_slot.has_value()
                ? event.active_slot
                : parse_first_field_int(event, {"actor_slot", "active_slot"});
            observed.source_slot =
                parse_first_field_int(event, {"selected_source_slot", "source_slot", "source_actor_slot"});
            observed.target_slot = event.target_slot.has_value()
                ? event.target_slot
                : parse_first_field_int(event, {"target_slot"});
            observed.action_sequence_id = action_sequence_id_from_event(event);
            observed.action_id = parse_first_field_int(event, {"action_id", "source_action_id"});

            ++summary.observed_source_selection_events;
            maybe_set_first(summary.first_source_selection_draw_index, observed.draw_index);
            if (observed.action_sequence_id.has_value()) {
                ++summary.events_with_action_sequence_id;
                ++summary.source_selection_events_with_action_sequence_id;
            }
            if (observed.source_slot.has_value()) {
                ++summary.source_selection_events_with_source_slot;
            }
            if (observed.actor_slot.has_value()) {
                ++summary.source_selection_events_with_actor_slot;
            }
            if (observed.target_slot.has_value()) {
                ++summary.source_selection_events_with_target_slot;
            }

            summary.events.push_back(std::move(observed));
            continue;
        }

        if (!is_action_source_checkpoint(event)) {
            continue;
        }

        ActionSourceCheckpointEvent observed;
        observed.kind = ActionSourceCheckpointKind::Field6Bridge;
        observed.draw_index = event.rng_draw_index_before;
        observed.actor_slot = event.active_slot.has_value()
            ? event.active_slot
            : parse_first_field_int(event, {"actor_slot", "active_slot"});
        observed.source_slot = parse_first_field_int(event, {"source_slot", "source_actor_slot"});
        observed.target_slot = event.target_slot.has_value()
            ? event.target_slot
            : parse_first_field_int(event, {"target_slot"});
        observed.action_sequence_id = action_sequence_id_from_event(event);
        observed.action_id = parse_first_field_int(event, {"action_id", "source_action_id"});
        observed.source_field6_0x6 =
            parse_first_field_int(event, {"source_field6_0x6", "source_field6", "source_field6_6"});
        observed.actor_field6_0x6 =
            parse_first_field_int(event, {"actor_field6_0x6", "actor_field6", "actor_field6_6"});
        observed.handler_pc =
            parse_first_pc_field(event, {"handler_pc", "selected_handler_pc", "action_handler_pc"});
        observed.callback_pc = parse_first_pc_field(
            event,
            {"callback_pc", "instruction_callback_pc", "instr_callback_pc", "instruction_0xe0", "callback_0xe0"});

        ++summary.observed_action_source_events;
        maybe_set_first(summary.first_action_source_draw_index, observed.draw_index);
        if (observed.actor_slot.has_value()) {
            ++summary.events_with_actor_slot;
        }
        if (observed.source_slot.has_value()) {
            ++summary.events_with_source_slot;
        }
        if (observed.action_sequence_id.has_value()) {
            ++summary.events_with_action_sequence_id;
            ++summary.action_source_events_with_action_sequence_id;
        }
        if (observed.action_id.has_value()) {
            ++summary.events_with_action_id;
        }
        if (observed.handler_pc.has_value()) {
            ++summary.events_with_handler_pc;
            if (summary.expected_handler_pc.has_value()) {
                if (*observed.handler_pc == *summary.expected_handler_pc) {
                    ++summary.handler_matches;
                } else {
                    ++summary.handler_mismatches;
                }
            }
        }
        if (observed.callback_pc.has_value()) {
            ++summary.events_with_callback_pc;
            if (summary.expected_callback_pc.has_value()) {
                if (*observed.callback_pc == *summary.expected_callback_pc) {
                    ++summary.callback_matches;
                } else {
                    ++summary.callback_mismatches;
                }
            }
        }
        if (observed.source_field6_0x6.has_value()) {
            ++summary.events_with_source_field6;
        }
        if (observed.actor_field6_0x6.has_value()) {
            ++summary.events_with_actor_field6;
        }
        if (observed.source_field6_0x6.has_value() && observed.actor_field6_0x6.has_value()) {
            if (*observed.source_field6_0x6 == *observed.actor_field6_0x6) {
                ++summary.field6_matches;
            } else {
                ++summary.field6_mismatches;
            }
        }

        summary.events.push_back(std::move(observed));
    }

    std::vector<const ActionSourceCheckpointEvent*> source_selections;
    std::vector<ActionSourceCheckpointEvent*> bridges;
    source_selections.reserve(summary.events.size());
    bridges.reserve(summary.events.size());
    for (auto& event : summary.events) {
        if (event.kind == ActionSourceCheckpointKind::SourceSelection) {
            source_selections.push_back(&event);
        } else {
            bridges.push_back(&event);
        }
    }

    const bool use_action_sequence_id =
        all_bridges_have_action_sequence_id(bridges)
        && any_selection_has_action_sequence_id(source_selections);
    if (use_action_sequence_id) {
        summary.source_selection_bridge_pairing_strategy = "action_sequence_id";
        const auto selections_by_sequence =
            source_selection_by_action_sequence_id(source_selections);
        for (auto* bridge : bridges) {
            const auto found =
                selections_by_sequence.find(*bridge->action_sequence_id);
            if (found == selections_by_sequence.end()) {
                ++summary.source_selection_bridge_missing_by_action_sequence_id;
                continue;
            }
            ++summary.source_selection_bridge_pairs;
            ++summary.source_selection_bridge_pairs_by_action_sequence_id;
            compare_source_selection_pair(summary, *bridge, *found->second);
        }
    } else {
        const auto pair_count = std::min(source_selections.size(), bridges.size());
        summary.source_selection_bridge_pairs = static_cast<int>(pair_count);
        for (std::size_t i = 0; i < pair_count; ++i) {
            compare_source_selection_pair(summary, *bridges[i], *source_selections[i]);
        }
    }

    summary.status = classify_status(summary);
    return summary;
}

const char* action_source_checkpoint_status_name(ActionSourceCheckpointStatus status) {
    switch (status) {
    case ActionSourceCheckpointStatus::ObservedOnly: return "ObservedOnly";
    case ActionSourceCheckpointStatus::MatchesExpected: return "MatchesExpected";
    case ActionSourceCheckpointStatus::MissingSourceSelectionCheckpoint: return "MissingSourceSelectionCheckpoint";
    case ActionSourceCheckpointStatus::MissingLiveSourceFields: return "MissingLiveSourceFields";
    case ActionSourceCheckpointStatus::SourceSelectionMismatch: return "SourceSelectionMismatch";
    case ActionSourceCheckpointStatus::Field6Mismatch: return "Field6Mismatch";
    case ActionSourceCheckpointStatus::HandlerMismatch: return "HandlerMismatch";
    case ActionSourceCheckpointStatus::CallbackMismatch: return "CallbackMismatch";
    default: return "Unknown";
    }
}

const char* action_source_checkpoint_kind_name(ActionSourceCheckpointKind kind) {
    switch (kind) {
    case ActionSourceCheckpointKind::SourceSelection: return "SourceSelection";
    case ActionSourceCheckpointKind::Field6Bridge: return "Field6Bridge";
    default: return "Unknown";
    }
}

const char* first_battle_action_source_checkpoint_rule_detail() {
    return "first-battle live action-source checkpoints should connect FUN_8006782c source "
           "selection from DAT_80346bd8+0x90 to the FUN_8006721c field6 bridge, expose source "
           "field6_0x6 and actor field6_0x6, and validate both selected handler and "
           "InstructionWorksheet+0xe0 callback; current static resource extraction expects "
           "800662bc for first-battle basic action ids; when action_sequence_id is present, "
           "source-selection rows are paired to field6 bridge rows by sequence before falling "
           "back to trace order";
}

} // namespace savor::predict
