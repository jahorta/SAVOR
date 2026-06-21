#include "ActionSourceCheckpointModel.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <utility>

namespace savor::predict {

namespace {

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

bool is_action_source_checkpoint(const CheckpointEvent& event) {
    if (event.pc == kActionSourcePc) {
        return true;
    }
    return event.function == "FUN_8006721c"
        || event.function == "FUN_8006721C"
        || event.checkpoint == "action_source";
}

ActionSourceCheckpointStatus classify_status(const ActionSourceCheckpointSummary& summary) {
    if (summary.observed_action_source_events == 0) {
        return ActionSourceCheckpointStatus::ObservedOnly;
    }
    if (summary.events_with_actor_slot != summary.observed_action_source_events
        || summary.events_with_source_slot != summary.observed_action_source_events
        || summary.events_with_source_field6 != summary.observed_action_source_events
        || summary.events_with_actor_field6 != summary.observed_action_source_events) {
        return ActionSourceCheckpointStatus::MissingLiveSourceFields;
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
    return ActionSourceCheckpointStatus::MatchesExpected;
}

} // namespace

ActionSourceCheckpointExpectation first_battle_action_source_checkpoint_expectation() {
    ActionSourceCheckpointExpectation expectation;
    expectation.expected_handler_pc = std::string(kExpectedFirstBattleHandlerPc);
    return expectation;
}

ActionSourceCheckpointSummary summarize_action_source_checkpoints(
    const std::vector<CheckpointEvent>& events,
    std::optional<std::string> expected_handler_pc) {
    ActionSourceCheckpointSummary summary;
    if (expected_handler_pc.has_value()) {
        summary.expected_handler_pc = normalize_pc(std::move(*expected_handler_pc));
    }

    for (const auto& event : events) {
        if (!is_action_source_checkpoint(event)) {
            continue;
        }

        ActionSourceCheckpointEvent observed;
        observed.draw_index = event.rng_draw_index_before;
        observed.actor_slot = event.active_slot.has_value()
            ? event.active_slot
            : parse_first_field_int(event, {"actor_slot", "active_slot"});
        observed.source_slot = parse_first_field_int(event, {"source_slot", "source_actor_slot"});
        observed.target_slot = event.target_slot.has_value()
            ? event.target_slot
            : parse_first_field_int(event, {"target_slot"});
        observed.action_id = parse_first_field_int(event, {"action_id", "source_action_id"});
        observed.source_field6_0x6 =
            parse_first_field_int(event, {"source_field6_0x6", "source_field6", "source_field6_6"});
        observed.actor_field6_0x6 =
            parse_first_field_int(event, {"actor_field6_0x6", "actor_field6", "actor_field6_6"});
        observed.handler_pc =
            parse_first_pc_field(event, {"handler_pc", "selected_handler_pc", "action_handler_pc"});

        ++summary.observed_action_source_events;
        if (!summary.first_action_source_draw_index.has_value()) {
            summary.first_action_source_draw_index = observed.draw_index;
        }
        if (observed.actor_slot.has_value()) {
            ++summary.events_with_actor_slot;
        }
        if (observed.source_slot.has_value()) {
            ++summary.events_with_source_slot;
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

    summary.status = classify_status(summary);
    return summary;
}

const char* action_source_checkpoint_status_name(ActionSourceCheckpointStatus status) {
    switch (status) {
    case ActionSourceCheckpointStatus::ObservedOnly: return "ObservedOnly";
    case ActionSourceCheckpointStatus::MatchesExpected: return "MatchesExpected";
    case ActionSourceCheckpointStatus::MissingLiveSourceFields: return "MissingLiveSourceFields";
    case ActionSourceCheckpointStatus::Field6Mismatch: return "Field6Mismatch";
    case ActionSourceCheckpointStatus::HandlerMismatch: return "HandlerMismatch";
    default: return "Unknown";
    }
}

const char* first_battle_action_source_checkpoint_rule_detail() {
    return "first-battle live action-source checkpoints at FUN_8006721c should expose source slot, "
           "source field6_0x6, actor field6_0x6, action id, and selected handler; current static "
           "resource extraction expects handler 800662bc for first-battle basic action ids";
}

} // namespace savor::predict
