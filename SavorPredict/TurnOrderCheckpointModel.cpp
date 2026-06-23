#include "TurnOrderCheckpointModel.h"

#include "FirstBattleDataModel.h"
#include "SoaQSortModel.h"

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>

namespace savor::predict {

namespace {

constexpr std::string_view kTurnOrderOwner = "turn_order_priority_jitter";

bool owner_is(const CheckpointEvent& event, std::string_view owner) {
    return event.known_rng_owner == owner;
}

std::optional<int> parse_field_int(const CheckpointEvent& event, const char* key) {
    const auto found = event.fields.find(key);
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

bool event_named(const CheckpointEvent& event, std::initializer_list<std::string_view> names) {
    for (const auto name : names) {
        if (event.function == name || event.checkpoint == name) {
            return true;
        }
    }
    return false;
}

bool is_queue_entry_checkpoint(const CheckpointEvent& event) {
    return event_named(event, {
        "turn_order_queue",
        "queued_entry",
        "queue_entry",
        "action_queue_entry",
        "qsort_input",
    });
}

bool is_qsort_output_checkpoint(const CheckpointEvent& event) {
    return event_named(event, {
        "turn_order_qsort_output",
        "qsort_output",
        "sorted_queue_entry",
    });
}

bool is_execution_order_checkpoint(const CheckpointEvent& event) {
    return event_named(event, {
        "turn_order_execution",
        "execution_order",
        "final_turn_order",
        "s8_ARRAY_803092f4",
        "s8_array",
    });
}

std::optional<int> parse_index_after_marker(std::string_view text, std::string_view marker) {
    const auto marker_pos = text.find(marker);
    if (marker_pos == std::string_view::npos) {
        return std::nullopt;
    }
    auto pos = marker_pos + marker.size();
    if (pos >= text.size() || text[pos] < '0' || text[pos] > '9') {
        return std::nullopt;
    }
    int value = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        value = value * 10 + (text[pos] - '0');
        ++pos;
    }
    return value;
}

std::optional<int> parse_index_from_identifier(
    const CheckpointEvent& event,
    std::initializer_list<std::string_view> markers) {
    for (const auto* key : {"checkpoint_id", "checkpoint_name", "name"}) {
        const auto found = event.fields.find(key);
        if (found == event.fields.end()) {
            continue;
        }
        for (const auto marker : markers) {
            if (const auto parsed = parse_index_after_marker(found->second, marker); parsed.has_value()) {
                return parsed;
            }
        }
    }
    return std::nullopt;
}

bool priority_fields_complete(const TurnOrderCheckpointDraw& draw) {
    return draw.assigned_priority.has_value()
        && draw.expected_assigned_priority.has_value();
}

bool queue_entry_complete(const TurnOrderCheckpointDraw& draw) {
    return draw.slot.has_value()
        && draw.assigned_priority.has_value();
}

bool execution_entry_complete(const TurnOrderCheckpointDraw& draw) {
    return draw.slot.has_value();
}

bool qsort_output_entry_complete(const TurnOrderCheckpointDraw& draw) {
    return draw.slot.has_value();
}

std::optional<int> expected_first_battle_quick_for_slot(int slot) {
    const auto actor = first_battle_actor_by_slot(slot);
    if (!actor.has_value()) {
        return std::nullopt;
    }
    return actor->quick;
}

std::optional<int> expected_jitter_modulus_from_metadata(const TurnOrderCheckpointDraw& draw) {
    if (!draw.sum_quick.has_value()
        || !draw.queued_count.has_value()
        || *draw.queued_count == 0) {
        return std::nullopt;
    }
    return (*draw.sum_quick / *draw.queued_count) / 2;
}

void assign_expected_priority(TurnOrderCheckpointDraw& draw) {
    if (draw.initial_priority.has_value() && *draw.initial_priority != -1) {
        draw.expected_assigned_priority = *draw.initial_priority;
    } else if (draw.fixed_priority_result.has_value()
        && *draw.fixed_priority_result != 0) {
        draw.expected_assigned_priority = draw.fixed_priority_value;
    } else if (draw.quick.has_value()
        && draw.jitter_modulus.has_value()
        && *draw.jitter_modulus == 0) {
        draw.expected_assigned_priority = *draw.quick;
    } else if (draw.quick.has_value()
        && draw.rand_value.has_value()
        && draw.jitter_modulus.has_value()
        && *draw.jitter_modulus > 0) {
        draw.expected_assigned_priority =
            *draw.quick + (*draw.rand_value % *draw.jitter_modulus);
    }

    if (draw.assigned_priority.has_value() && draw.expected_assigned_priority.has_value()) {
        draw.priority_matches = *draw.assigned_priority == *draw.expected_assigned_priority;
    }
}

std::vector<TurnOrderCheckpointDraw> sorted_execution_entries(
    const std::vector<TurnOrderCheckpointDraw>& entries) {
    auto sorted = entries;
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.execution_index.has_value() && rhs.execution_index.has_value()
            && *lhs.execution_index != *rhs.execution_index) {
            return *lhs.execution_index < *rhs.execution_index;
        }
        if (lhs.draw_index.has_value() && rhs.draw_index.has_value()
            && *lhs.draw_index != *rhs.draw_index) {
            return *lhs.draw_index < *rhs.draw_index;
        }
        return false;
    });
    return sorted;
}

bool vector_contains(const std::vector<int>& values, int needle) {
    return std::find(values.begin(), values.end(), needle) != values.end();
}

std::vector<int> reversed_copy(std::vector<int> values) {
    std::reverse(values.begin(), values.end());
    return values;
}

std::vector<TurnOrderCheckpointDraw> sorted_by_index_or_draw(
    std::vector<TurnOrderCheckpointDraw> entries,
    bool use_execution_index) {
    std::stable_sort(entries.begin(), entries.end(), [use_execution_index](const auto& lhs, const auto& rhs) {
        const auto lhs_index = use_execution_index ? lhs.execution_index : lhs.queue_index;
        const auto rhs_index = use_execution_index ? rhs.execution_index : rhs.queue_index;
        if (lhs_index.has_value() && rhs_index.has_value() && *lhs_index != *rhs_index) {
            return *lhs_index < *rhs_index;
        }
        if (lhs.draw_index.has_value() && rhs.draw_index.has_value()
            && *lhs.draw_index != *rhs.draw_index) {
            return *lhs.draw_index < *rhs.draw_index;
        }
        return false;
    });
    return entries;
}

void fill_queue_priority_from_draws(
    std::vector<TurnOrderCheckpointDraw>& queue_sources,
    const std::vector<TurnOrderCheckpointDraw>& priority_draws) {
    for (auto& source : queue_sources) {
        if (!source.rand_value.has_value()
            && source.queue_index.has_value()
            && *source.queue_index >= 0
            && static_cast<std::size_t>(*source.queue_index) < priority_draws.size()) {
            source.rand_value = priority_draws[static_cast<std::size_t>(*source.queue_index)].rand_value;
        }
        if (!source.jitter_modulus.has_value()
            && source.queue_index.has_value()
            && *source.queue_index >= 0
            && static_cast<std::size_t>(*source.queue_index) < priority_draws.size()) {
            source.jitter_modulus = priority_draws[static_cast<std::size_t>(*source.queue_index)].jitter_modulus;
        }
        if (!source.sum_quick.has_value()
            && source.queue_index.has_value()
            && *source.queue_index >= 0
            && static_cast<std::size_t>(*source.queue_index) < priority_draws.size()) {
            source.sum_quick = priority_draws[static_cast<std::size_t>(*source.queue_index)].sum_quick;
        }
        if (!source.queued_count.has_value()
            && source.queue_index.has_value()
            && *source.queue_index >= 0
            && static_cast<std::size_t>(*source.queue_index) < priority_draws.size()) {
            source.queued_count = priority_draws[static_cast<std::size_t>(*source.queue_index)].queued_count;
        }
        if (!source.quick.has_value() && source.expected_first_battle_quick.has_value()) {
            source.quick = source.expected_first_battle_quick;
            source.quick_matches = true;
        }
        source.expected_jitter_modulus = expected_jitter_modulus_from_metadata(source);
        if (source.expected_jitter_modulus.has_value() && source.jitter_modulus.has_value()) {
            source.queue_metadata_matches = *source.jitter_modulus == *source.expected_jitter_modulus;
        }
        assign_expected_priority(source);
    }
}

void summarize_priority_tie_groups(
    TurnOrderCheckpointSummary& summary,
    const std::vector<TurnOrderCheckpointDraw>& priority_sources,
    const std::vector<TurnOrderCheckpointDraw>& sorted_execution) {
    std::map<int, std::vector<const TurnOrderCheckpointDraw*>> sources_by_priority;
    for (const auto& source : priority_sources) {
        if (source.assigned_priority.has_value() && source.slot.has_value()) {
            sources_by_priority[*source.assigned_priority].push_back(&source);
        }
    }

    for (auto& [assigned_priority, sources] : sources_by_priority) {
        if (sources.size() < 2) {
            continue;
        }

        std::stable_sort(
            sources.begin(),
            sources.end(),
            [](const auto* lhs, const auto* rhs) {
                if (lhs->queue_index.has_value() && rhs->queue_index.has_value()
                    && *lhs->queue_index != *rhs->queue_index) {
                    return *lhs->queue_index < *rhs->queue_index;
                }
                return false;
            });

        TurnOrderTieGroup group;
        group.assigned_priority = assigned_priority;
        for (const auto* source : sources) {
            if (source->queue_index.has_value()) {
                group.queue_indices.push_back(*source->queue_index);
            }
            group.slots_by_queue_order.push_back(*source->slot);
        }

        for (const auto& entry : sorted_execution) {
            if (entry.slot.has_value()
                && vector_contains(group.slots_by_queue_order, *entry.slot)) {
                group.observed_execution_slots.push_back(*entry.slot);
            }
        }

        ++summary.priority_tie_groups;
        summary.priority_tied_entries += static_cast<int>(sources.size());
        if (group.observed_execution_slots.size() == group.slots_by_queue_order.size()) {
            group.observed_order_compared = true;
            ++summary.tie_groups_with_observed_execution_order;
            group.observed_order_matches_queue_ascending =
                group.observed_execution_slots == group.slots_by_queue_order;
            group.observed_order_matches_queue_descending =
                group.observed_execution_slots == reversed_copy(group.slots_by_queue_order);
            if (group.observed_order_matches_queue_ascending) {
                ++summary.tie_groups_matching_queue_ascending;
            }
            if (group.observed_order_matches_queue_descending) {
                ++summary.tie_groups_matching_queue_descending;
            }
        }
        summary.tie_groups.push_back(std::move(group));
    }

    if (summary.priority_tie_groups > 0) {
        summary.priority_ties_observed = true;
    }
}

TurnOrderCheckpointStatus classify_status(const TurnOrderCheckpointSummary& summary) {
    if (summary.expected_priority_jitter_draws.has_value()
        && summary.observed_priority_jitter_draws < *summary.expected_priority_jitter_draws) {
        return TurnOrderCheckpointStatus::MissingPriorityJitterDraws;
    }
    if (summary.expected_priority_jitter_draws.has_value()
        && summary.observed_priority_jitter_draws > *summary.expected_priority_jitter_draws) {
        return TurnOrderCheckpointStatus::ExtraPriorityJitterDraws;
    }
    if (summary.quick_mismatches > 0) {
        return TurnOrderCheckpointStatus::QuickMismatch;
    }
    if (summary.queue_metadata_mismatches > 0) {
        return TurnOrderCheckpointStatus::QueueMetadataMismatch;
    }
    if (summary.priority_sources_missing_fixed_priority_result > 0) {
        return TurnOrderCheckpointStatus::MissingFixedPriorityResults;
    }
    if (summary.priority_mismatches > 0) {
        return TurnOrderCheckpointStatus::PriorityMismatch;
    }
    if (summary.qsort_output_mismatches > 0) {
        return TurnOrderCheckpointStatus::QSortOutputMismatch;
    }
    if (summary.execution_order_mismatches > 0) {
        return TurnOrderCheckpointStatus::ExecutionOrderMismatch;
    }
    if (summary.incomplete_queue_entries > 0
        || summary.incomplete_qsort_output_entries > 0
        || summary.incomplete_execution_order_entries > 0) {
        return TurnOrderCheckpointStatus::MissingLivePriorityFields;
    }
    if (!summary.expected_priority_jitter_draws.has_value()
        && summary.priority_draws_with_expected_priority == 0
        && !summary.execution_order_compared) {
        return TurnOrderCheckpointStatus::ObservedOnly;
    }
    return TurnOrderCheckpointStatus::MatchesExpected;
}

} // namespace

TurnOrderCheckpointExpectation turn_order_checkpoint_expectation(const TurnOrderSimulation& turn_order) {
    TurnOrderCheckpointExpectation expectation;
    expectation.expected_priority_jitter_draws = turn_order.draws_consumed;
    expectation.expected_queued_entries = turn_order.queued_count;
    expectation.expected_jitter_modulus = turn_order.jitter_modulus;
    return expectation;
}

TurnOrderCheckpointSummary summarize_turn_order_checkpoints(
    const std::vector<CheckpointEvent>& events,
    std::optional<int> expected_priority_jitter_draws) {
    TurnOrderCheckpointSummary summary;
    summary.expected_priority_jitter_draws = expected_priority_jitter_draws;
    std::vector<TurnOrderCheckpointDraw> queue_priority_sources;
    std::vector<TurnOrderCheckpointDraw> draw_priority_sources;
    std::vector<TurnOrderCheckpointDraw> qsort_output_entries;
    std::vector<TurnOrderCheckpointDraw> execution_entries;

    for (const auto& event : events) {
        const bool priority_draw = owner_is(event, kTurnOrderOwner);
        const bool queue_entry = !priority_draw && is_queue_entry_checkpoint(event);
        const bool qsort_output_entry = !priority_draw && is_qsort_output_checkpoint(event);
        const bool execution_entry = is_execution_order_checkpoint(event);
        if (!priority_draw && !queue_entry && !qsort_output_entry && !execution_entry) {
            continue;
        }

        TurnOrderCheckpointDraw draw;
        draw.kind = priority_draw
            ? TurnOrderCheckpointKind::PriorityJitterDraw
            : (execution_entry
                ? TurnOrderCheckpointKind::ExecutionOrderEntry
                : (qsort_output_entry ? TurnOrderCheckpointKind::QSortOutputEntry : TurnOrderCheckpointKind::QueueEntry));
        draw.draw_index = event.rng_draw_index_before;
        draw.slot = event.active_slot.has_value() ? event.active_slot : parse_first_field_int(event, {"slot", "actor_slot"});
        draw.quick = parse_field_int(event, "quick");
        draw.queued_instruction = parse_first_field_int(event, {"queued_instruction", "instruction"});
        draw.target_slot = event.target_slot.has_value()
            ? event.target_slot
            : parse_first_field_int(event, {"target_slot", "target"});
        draw.initial_priority = parse_field_int(event, "initial_priority");
        draw.fixed_priority_result = parse_field_int(event, "fixed_priority_result");
        if (!draw.fixed_priority_result.has_value() && (queue_entry || qsort_output_entry)) {
            draw.fixed_priority_result = 0;
        }
        draw.fixed_priority_value = parse_field_int(event, "fixed_priority_value");
        draw.jitter_modulus = parse_field_int(event, "jitter_modulus");
        draw.sum_quick = parse_field_int(event, "sum_quick");
        draw.queued_count = parse_first_field_int(event, {"queued_count", "num_actions"});
        draw.queue_index = parse_field_int(event, "queue_index");
        if (!draw.queue_index.has_value()) {
            draw.queue_index = parse_index_from_identifier(event, {"queue_entry_", "qsort_input_entry_"});
        }
        draw.execution_index = parse_field_int(event, "execution_index");
        if (!draw.execution_index.has_value()) {
            draw.execution_index = parse_index_from_identifier(event, {"execution_order_entry_"});
        }
        if (qsort_output_entry && !draw.queue_index.has_value()) {
            draw.queue_index = parse_index_from_identifier(event, {"qsort_output_entry_"});
        }
        draw.assigned_priority = parse_field_int(event, "assigned_priority");
        draw.rand_value = parse_first_field_int(event, {"priority_rand", "rand_value"});
        if (priority_draw && !draw.rand_value.has_value() && event.rng_seed_before.has_value()) {
            draw.rand_value = draw_rand15(*event.rng_seed_before).value;
        }

        if (draw.slot.has_value()) {
            draw.expected_first_battle_quick = expected_first_battle_quick_for_slot(*draw.slot);
        }
        if (draw.expected_first_battle_quick.has_value() && draw.quick.has_value()) {
            draw.quick_matches = *draw.quick == *draw.expected_first_battle_quick;
        }
        draw.expected_jitter_modulus = expected_jitter_modulus_from_metadata(draw);
        if (draw.expected_jitter_modulus.has_value() && draw.jitter_modulus.has_value()) {
            draw.queue_metadata_matches = *draw.jitter_modulus == *draw.expected_jitter_modulus;
        }

        assign_expected_priority(draw);
        if (priority_fields_complete(draw)) {
            ++summary.priority_draws_with_expected_priority;
            if (draw.priority_matches) {
                ++summary.priority_matches;
            } else {
                ++summary.priority_mismatches;
            }
        }

        if (priority_draw) {
            ++summary.observed_priority_jitter_draws;
        } else if (queue_entry) {
            ++summary.observed_queue_entries;
        } else if (qsort_output_entry) {
            ++summary.observed_qsort_output_entries;
        } else if (execution_entry) {
            ++summary.observed_execution_order_entries;
        }

        if (priority_draw && draw.draw_index.has_value()) {
            if (!summary.first_priority_jitter_draw_index.has_value()) {
                summary.first_priority_jitter_draw_index = *draw.draw_index;
            }
            summary.last_priority_jitter_draw_index = *draw.draw_index;
        }
        if (draw.slot.has_value()) {
            ++summary.draws_with_slot;
        }
        if (draw.quick.has_value()) {
            ++summary.draws_with_quick;
        }
        if (draw.assigned_priority.has_value()) {
            ++summary.draws_with_assigned_priority;
        }
        if (draw.rand_value.has_value()) {
            ++summary.draws_with_rand_value;
        }
        if (draw.expected_first_battle_quick.has_value()) {
            ++summary.events_with_expected_first_battle_quick;
            if (draw.quick.has_value()) {
                if (draw.quick_matches) {
                    ++summary.quick_matches;
                } else {
                    ++summary.quick_mismatches;
                }
            }
        }
        if (draw.fixed_priority_result.has_value()) {
            ++summary.events_with_fixed_priority_result;
            if (*draw.fixed_priority_result == 0) {
                ++summary.fixed_priority_zero_results;
            } else {
                ++summary.fixed_priority_nonzero_results;
            }
        }
        if (draw.expected_jitter_modulus.has_value() && draw.jitter_modulus.has_value()) {
            ++summary.events_with_queue_metadata;
            if (draw.queue_metadata_matches) {
                ++summary.queue_metadata_matches;
            } else {
                ++summary.queue_metadata_mismatches;
            }
        }
        if (queue_entry) {
            if (draw.slot.has_value()) {
                ++summary.queue_entries_with_slot;
            }
            if (draw.quick.has_value()) {
                ++summary.queue_entries_with_quick;
            }
            if (draw.assigned_priority.has_value()) {
                ++summary.queue_entries_with_assigned_priority;
            }
            if (!queue_entry_complete(draw)) {
                ++summary.incomplete_queue_entries;
            } else {
                queue_priority_sources.push_back(draw);
            }
        } else if (qsort_output_entry) {
            if (!qsort_output_entry_complete(draw)) {
                ++summary.incomplete_qsort_output_entries;
            } else {
                qsort_output_entries.push_back(draw);
            }
        } else if (priority_draw && draw.slot.has_value() && draw.assigned_priority.has_value()) {
            draw_priority_sources.push_back(draw);
        } else if (execution_entry) {
            if (!execution_entry_complete(draw)) {
                ++summary.incomplete_execution_order_entries;
            } else {
                execution_entries.push_back(draw);
            }
        }
        summary.draws.push_back(std::move(draw));
    }

    auto sorted_priority_draws = sorted_by_index_or_draw(draw_priority_sources, false);
    auto sorted_queue_sources = sorted_by_index_or_draw(queue_priority_sources, false);
    fill_queue_priority_from_draws(sorted_queue_sources, sorted_priority_draws);

    const auto& priority_sources =
        !sorted_queue_sources.empty() ? sorted_queue_sources : draw_priority_sources;
    for (const auto& source : priority_sources) {
        if (source.fixed_priority_result.has_value()) {
            ++summary.priority_sources_with_fixed_priority_result;
        } else {
            ++summary.priority_sources_missing_fixed_priority_result;
        }
    }
    if (!priority_sources.empty()) {
        const auto sorted_qsort_output = sorted_by_index_or_draw(qsort_output_entries, false);
        const auto sorted_execution = sorted_execution_entries(execution_entries);
        summarize_priority_tie_groups(summary, priority_sources, sorted_execution);

        std::vector<int> priority_keys;
        priority_keys.reserve(priority_sources.size());
        for (const auto& source : priority_sources) {
            priority_keys.push_back(*source.assigned_priority);
        }
        const auto sorted_indices = soa_qsort_indices_by_key_ascending(priority_keys);

        for (const auto index : sorted_indices) {
            summary.expected_qsort_slots.push_back(*priority_sources[index].slot);
        }
        for (const auto& entry : sorted_qsort_output) {
            summary.observed_qsort_slots.push_back(*entry.slot);
        }
        if (!sorted_qsort_output.empty()) {
            summary.qsort_output_compared = true;
            if (summary.expected_qsort_slots.size() == summary.observed_qsort_slots.size()) {
                for (std::size_t i = 0; i < summary.expected_qsort_slots.size(); ++i) {
                    if (summary.expected_qsort_slots[i] == summary.observed_qsort_slots[i]) {
                        ++summary.qsort_output_matches;
                    } else {
                        ++summary.qsort_output_mismatches;
                    }
                }
            } else {
                ++summary.qsort_output_mismatches;
            }
            summary.qsort_output_exact = summary.qsort_output_mismatches == 0;
        }

        for (auto it = sorted_indices.rbegin(); it != sorted_indices.rend(); ++it) {
            summary.expected_execution_slots.push_back(*priority_sources[*it].slot);
        }

        for (const auto& entry : sorted_execution) {
            summary.observed_execution_slots.push_back(*entry.slot);
        }

        if (!sorted_execution.empty() && summary.expected_execution_slots.size() == summary.observed_execution_slots.size()) {
            summary.execution_order_compared = true;
            for (std::size_t i = 0; i < summary.expected_execution_slots.size(); ++i) {
                if (summary.expected_execution_slots[i] == summary.observed_execution_slots[i]) {
                    ++summary.execution_order_matches;
                } else {
                    ++summary.execution_order_mismatches;
                }
            }
        } else if (!sorted_execution.empty()) {
            summary.execution_order_compared = true;
            ++summary.execution_order_mismatches;
        }
    }

    summary.status = classify_status(summary);
    return summary;
}

const char* turn_order_checkpoint_status_name(TurnOrderCheckpointStatus status) {
    switch (status) {
    case TurnOrderCheckpointStatus::ObservedOnly:
        return "ObservedOnly";
    case TurnOrderCheckpointStatus::MatchesExpected:
        return "MatchesExpected";
    case TurnOrderCheckpointStatus::MissingPriorityJitterDraws:
        return "MissingPriorityJitterDraws";
    case TurnOrderCheckpointStatus::ExtraPriorityJitterDraws:
        return "ExtraPriorityJitterDraws";
    case TurnOrderCheckpointStatus::MissingLivePriorityFields:
        return "MissingLivePriorityFields";
    case TurnOrderCheckpointStatus::MissingFixedPriorityResults:
        return "MissingFixedPriorityResults";
    case TurnOrderCheckpointStatus::QuickMismatch:
        return "QuickMismatch";
    case TurnOrderCheckpointStatus::QueueMetadataMismatch:
        return "QueueMetadataMismatch";
    case TurnOrderCheckpointStatus::PriorityMismatch:
        return "PriorityMismatch";
    case TurnOrderCheckpointStatus::QSortOutputMismatch:
        return "QSortOutputMismatch";
    case TurnOrderCheckpointStatus::ExecutionOrderMismatch:
        return "ExecutionOrderMismatch";
    default:
        return "Unknown";
    }
}

const char* turn_order_checkpoint_kind_name(TurnOrderCheckpointKind kind) {
    switch (kind) {
    case TurnOrderCheckpointKind::PriorityJitterDraw:
        return "PriorityJitterDraw";
    case TurnOrderCheckpointKind::QueueEntry:
        return "QueueEntry";
    case TurnOrderCheckpointKind::QSortOutputEntry:
        return "QSortOutputEntry";
    case TurnOrderCheckpointKind::ExecutionOrderEntry:
        return "ExecutionOrderEntry";
    default:
        return "Unknown";
    }
}

const char* turn_order_checkpoint_rule_detail() {
    return "first-battle turn order should prove queued quick values, FUN_8006ee54 fixed-priority result, queue metadata-derived jitter_modulus, one 800711f8 priority-jitter draw per randomized queued action, assigned priority, and final reversed execution order";
}

} // namespace savor::predict
