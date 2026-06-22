#include "SstActionCommandCheckpointModel.h"

#include <cstdlib>
#include <initializer_list>
#include <utility>

namespace savor::predict {
namespace {

constexpr std::string_view kCase2StoreCompletePc = "8000C4C8";
constexpr std::string_view kCase8StoreCompletePc = "8000C6E8";

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

bool is_case2_store_checkpoint(const CheckpointEvent& event) {
    if (event.pc == kCase2StoreCompletePc) {
        return true;
    }
    return event.checkpoint == "sst_action_field6_case2_store_complete"
        || event.checkpoint == "sst_command_case2_field6_store_complete";
}

bool is_case8_store_checkpoint(const CheckpointEvent& event) {
    if (event.pc == kCase8StoreCompletePc) {
        return true;
    }
    return event.checkpoint == "sst_action_field6_case8_store_complete"
        || event.checkpoint == "sst_command_case8_field6_store_complete";
}

std::optional<int> action_sequence_id_from_event(const CheckpointEvent& event) {
    return parse_first_field_int(
        event,
        {"action_sequence_id", "action_sequence", "attack_sequence", "attack_index", "action_index", "sequence_id"});
}

void maybe_set_first(std::optional<int>& target, const std::optional<int>& value) {
    if (!target.has_value() && value.has_value()) {
        target = *value;
    }
}

SstActionCommandCheckpointStatus classify_status(
    const SstActionCommandCheckpointSummary& summary) {
    if (summary.observed_store_events == 0) {
        return SstActionCommandCheckpointStatus::ObservedOnly;
    }
    if (summary.events_with_source_field6 != summary.observed_store_events
        || summary.events_with_destination_field6 != summary.observed_store_events) {
        return SstActionCommandCheckpointStatus::MissingLiveFields;
    }
    if (summary.source_destination_mismatches > 0) {
        return SstActionCommandCheckpointStatus::Field6Mismatch;
    }
    return SstActionCommandCheckpointStatus::MatchesExpected;
}

} // namespace

SstActionCommandCheckpointSummary summarize_sst_action_command_checkpoints(
    const std::vector<CheckpointEvent>& events) {
    SstActionCommandCheckpointSummary summary;

    for (const auto& event : events) {
        const bool case2 = is_case2_store_checkpoint(event);
        const bool case8 = is_case8_store_checkpoint(event);
        if (!case2 && !case8) {
            continue;
        }

        SstActionCommandCheckpointEvent observed;
        observed.kind = case2
            ? SstActionCommandCheckpointKind::Case2Field6Store
            : SstActionCommandCheckpointKind::Case8Field6Store;
        observed.draw_index = event.rng_draw_index_before;
        observed.action_sequence_id = action_sequence_id_from_event(event);
        observed.source_field6 = case2
            ? parse_first_field_int(event, {"source_field6_0x06", "source_field6", "sst_source_field6"})
            : parse_first_field_int(event, {"source_field6_0x0a", "source_field6", "sst_source_field6"});
        observed.destination_field6 = parse_first_field_int(
            event,
            {"dest_field6_after_0x06", "destination_field6_0x06", "dest_field6", "sst_destination_field6"});
        observed.written_field6_register = parse_first_field_int(
            event,
            {"r0_written_field6", "written_field6_register", "written_field6"});

        ++summary.observed_store_events;
        if (case2) {
            ++summary.observed_case2_store_events;
        } else {
            ++summary.observed_case8_store_events;
        }
        maybe_set_first(summary.first_store_draw_index, observed.draw_index);
        if (observed.action_sequence_id.has_value()) {
            ++summary.events_with_action_sequence_id;
        }
        if (observed.source_field6.has_value()) {
            ++summary.events_with_source_field6;
            if (*observed.source_field6 == 8) {
                ++summary.key8_store_events;
                maybe_set_first(summary.first_key8_store_draw_index, observed.draw_index);
            }
        }
        if (observed.destination_field6.has_value()) {
            ++summary.events_with_destination_field6;
        }
        if (observed.written_field6_register.has_value()) {
            ++summary.events_with_written_field6_register;
        }
        if (observed.source_field6.has_value() && observed.destination_field6.has_value()) {
            observed.source_matches_destination =
                *observed.source_field6 == *observed.destination_field6;
            if (*observed.source_matches_destination) {
                ++summary.source_destination_matches;
            } else {
                ++summary.source_destination_mismatches;
            }
        }

        summary.events.push_back(std::move(observed));
    }

    summary.status = classify_status(summary);
    return summary;
}

const char* sst_action_command_checkpoint_status_name(SstActionCommandCheckpointStatus status) {
    switch (status) {
    case SstActionCommandCheckpointStatus::ObservedOnly: return "ObservedOnly";
    case SstActionCommandCheckpointStatus::MatchesExpected: return "MatchesExpected";
    case SstActionCommandCheckpointStatus::MissingLiveFields: return "MissingLiveFields";
    case SstActionCommandCheckpointStatus::Field6Mismatch: return "Field6Mismatch";
    default: return "Unknown";
    }
}

const char* sst_action_command_checkpoint_kind_name(SstActionCommandCheckpointKind kind) {
    switch (kind) {
    case SstActionCommandCheckpointKind::Case2Field6Store: return "Case2Field6Store";
    case SstActionCommandCheckpointKind::Case8Field6Store: return "Case8Field6Store";
    default: return "Unknown";
    }
}

const char* first_battle_sst_action_command_checkpoint_rule_detail() {
    return "SST::Command::Dispatch_8000c19c case-2 and case-8 field6 stores are "
           "the current static candidate for data-driven action/source key writes; "
           "store-complete checkpoints at 8000c4c8 and 8000c6e8 should show the "
           "serialized source field copied into the destination worksheet field6, "
           "with key 8 rows preceding successful-crit action-source and effect-copy rows";
}

} // namespace savor::predict
