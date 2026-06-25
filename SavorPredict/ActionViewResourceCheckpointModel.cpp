#include "ActionViewResourceCheckpointModel.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace savor::predict {

namespace {

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

std::optional<std::uint32_t> parse_u32(const std::string& text) {
    char* end = nullptr;
    const int base = (text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0) ? 16 : 10;
    const unsigned long parsed = std::strtoul(text.c_str(), &end, base);
    if (end == text.c_str() || *end != '\0') {
        return std::nullopt;
    }
    if (parsed > static_cast<unsigned long>(std::numeric_limits<std::uint32_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(parsed);
}

bool same_pointer(
    const std::optional<std::string>& lhs,
    const std::optional<std::string>& rhs) {
    if (!lhs.has_value() || !rhs.has_value()) {
        return false;
    }
    const auto lhs_value = parse_u32(*lhs);
    const auto rhs_value = parse_u32(*rhs);
    return lhs_value.has_value()
        && rhs_value.has_value()
        && *lhs_value == *rhs_value;
}

std::optional<bool> loaded_resource_root_field_matches(
    const std::optional<std::string>& loaded_resource,
    const std::optional<std::string>& root_field_ptr) {
    if (!loaded_resource.has_value() || !root_field_ptr.has_value()) {
        return std::nullopt;
    }
    const auto loaded = parse_u32(*loaded_resource);
    const auto root_field = parse_u32(*root_field_ptr);
    if (!loaded.has_value() || !root_field.has_value()) {
        return std::nullopt;
    }
    return (*loaded + 0x30u) == *root_field;
}

bool is_query_call_event(const CheckpointEvent& event) {
    return event.checkpoint == "action_view_query"
        || event.checkpoint == "action_view_gate";
}

ActionViewResourceRootLink make_link_from_gate_event(
    const ActionViewGateCheckpointEvent& event) {
    ActionViewResourceRootLink link;
    link.capture_sequence = std::nullopt;
    link.active_slot = event.active_slot;
    link.target_slot = event.target_slot;
    link.aux_list_root = event.aux_list_root;
    link.matched_resource_stem = event.matched_resource_stem;
    link.matched_std0_filename = event.matched_std0_filename;
    return link;
}

ActionViewResourceRootLink make_link_from_checkpoint_event(const CheckpointEvent& event) {
    ActionViewResourceRootLink link;
    link.capture_sequence = parse_first_field_int(event, {"capture_sequence"});
    link.active_slot = event.active_slot;
    link.target_slot = event.target_slot;
    link.aux_list_root = parse_first_field_string(event, {"aux_list_root", "aux_root", "list_root"});
    link.chain_payload_0x24 = parse_first_field_string(
        event,
        {"action_view_chain_payload_0x24", "combatant_movement_worksheet_0x24"});
    link.chain_loaded_resource_0x10 = parse_first_field_string(
        event,
        {"action_view_chain_loaded_resource_0x10", "action_view_chain_resource_0x10"});
    link.chain_aux_root_0x30 = parse_first_field_string(
        event,
        {"action_view_chain_aux_root_0x30", "action_view_chain_std0_root_0x30"});
    if (link.chain_aux_root_0x30.has_value() && link.aux_list_root.has_value()) {
        link.chain_aux_root_matches_query = same_pointer(
            link.chain_aux_root_0x30,
            link.aux_list_root);
    }
    return link;
}

void attach_cache_hit_and_producer(
    ActionViewResourceRootLink& link,
    const std::vector<ActionViewStd0CacheHit>& cache_hits,
    const std::vector<ActionViewStd0CacheProducer>& producers) {
    auto cache_hit = std::find_if(
        cache_hits.begin(),
        cache_hits.end(),
        [&](const ActionViewStd0CacheHit& candidate) {
            return same_pointer(link.aux_list_root, candidate.cached_table_ptr);
        });
    if (cache_hit == cache_hits.end()) {
        return;
    }

    link.linked_to_cache_hit = true;
    link.cache_expected_key = cache_hit->expected_key;
    link.cache_slot = cache_hit->matched_slot;
    link.cache_root_field_ptr = cache_hit->root_field_ptr;
    link.cache_root_field_matches_loaded_resource_plus_0x30 =
        loaded_resource_root_field_matches(
            link.chain_loaded_resource_0x10,
            link.cache_root_field_ptr);

    auto producer = std::find_if(
        producers.begin(),
        producers.end(),
        [&](const ActionViewStd0CacheProducer& candidate) {
            return same_pointer(link.aux_list_root, candidate.materialized_table_ptr);
        });
    if (producer != producers.end()) {
        link.linked_to_cache_producer = true;
        link.producer_loaded_file_ptr = producer->loaded_file_ptr;
    }
}

std::vector<ActionViewResourceRootLink> build_query_links_from_events(
    const std::vector<CheckpointEvent>& events) {
    std::vector<ActionViewResourceRootLink> links;
    for (const auto& event : events) {
        if (!is_query_call_event(event)) {
            continue;
        }
        auto link = make_link_from_checkpoint_event(event);
        if (link.aux_list_root.has_value()) {
            links.push_back(std::move(link));
        }
    }
    return links;
}

std::vector<ActionViewResourceRootLink> build_query_links_from_gate_summary(
    const ActionViewGateCheckpointSummary& gate_summary,
    const std::vector<CheckpointEvent>& events) {
    std::vector<ActionViewResourceRootLink> links;
    for (const auto& gate_event : gate_summary.events) {
        if (!gate_event.query_call_event || !gate_event.aux_list_root.has_value()) {
            continue;
        }
        links.push_back(make_link_from_gate_event(gate_event));
    }

    const auto raw_links = build_query_links_from_events(events);
    for (auto& link : links) {
        auto raw = std::find_if(
            raw_links.begin(),
            raw_links.end(),
            [&](const ActionViewResourceRootLink& candidate) {
                if (!same_pointer(link.aux_list_root, candidate.aux_list_root)) {
                    return false;
                }
                if (link.active_slot.has_value()
                    && candidate.active_slot.has_value()
                    && *link.active_slot != *candidate.active_slot) {
                    return false;
                }
                return true;
            });
        if (raw == raw_links.end()) {
            continue;
        }
        link.capture_sequence = raw->capture_sequence;
        link.chain_payload_0x24 = raw->chain_payload_0x24;
        link.chain_loaded_resource_0x10 = raw->chain_loaded_resource_0x10;
        link.chain_aux_root_0x30 = raw->chain_aux_root_0x30;
        link.chain_aux_root_matches_query = raw->chain_aux_root_matches_query;
    }
    return links;
}

void finalize_summary(ActionViewResourceCheckpointSummary& summary) {
    summary.complete_cache_producers = static_cast<int>(std::count_if(
        summary.producers.begin(),
        summary.producers.end(),
        [](const ActionViewStd0CacheProducer& producer) {
            return producer.loaded_file_ptr.has_value()
                && producer.materialized_table_ptr.has_value()
                && producer.filename_key.has_value();
        }));
    summary.complete_cache_hits = static_cast<int>(std::count_if(
        summary.cache_hits.begin(),
        summary.cache_hits.end(),
        [](const ActionViewStd0CacheHit& hit) {
            return hit.expected_key.has_value()
                && hit.matched_slot.has_value()
                && hit.root_field_ptr.has_value()
                && hit.cached_table_ptr.has_value();
        }));

    summary.selector_aux_roots = static_cast<int>(summary.selector_root_links.size());
    for (const auto& link : summary.selector_root_links) {
        if (link.linked_to_cache_hit) {
            ++summary.selector_aux_roots_linked_to_cache_hits;
        }
        if (link.linked_to_cache_producer) {
            ++summary.selector_aux_roots_linked_to_cache_producers;
        }
        if (link.chain_payload_0x24.has_value()
            || link.chain_loaded_resource_0x10.has_value()
            || link.chain_aux_root_0x30.has_value()) {
            ++summary.selector_aux_roots_with_chain_samples;
        }
        if (link.chain_aux_root_matches_query.has_value()) {
            if (*link.chain_aux_root_matches_query) {
                ++summary.selector_aux_roots_with_matching_chain;
            } else {
                ++summary.selector_aux_roots_with_mismatching_chain;
            }
        }
        if (link.cache_root_field_matches_loaded_resource_plus_0x30.has_value()) {
            if (*link.cache_root_field_matches_loaded_resource_plus_0x30) {
                ++summary.selector_aux_roots_with_loaded_resource_root_field_match;
            } else {
                ++summary.selector_aux_roots_with_loaded_resource_root_field_mismatch;
            }
        }
    }

    if (summary.observed_cache_producer_materialize_events == 0
        && summary.observed_cache_lookup_events == 0
        && summary.selector_aux_roots == 0) {
        summary.status = ActionViewResourceCheckpointStatus::NotObserved;
        return;
    }

    if (summary.selector_aux_roots_with_mismatching_chain > 0
        || summary.selector_aux_roots_with_loaded_resource_root_field_mismatch > 0) {
        summary.status = ActionViewResourceCheckpointStatus::ChainMismatch;
        return;
    }

    if (summary.selector_aux_roots > 0) {
        if (summary.selector_aux_roots_linked_to_cache_hits == summary.selector_aux_roots) {
            summary.status = ActionViewResourceCheckpointStatus::MatchesSelectorRoots;
        } else {
            summary.status = ActionViewResourceCheckpointStatus::MissingSelectorRootLinks;
        }
        return;
    }

    summary.status = ActionViewResourceCheckpointStatus::ObservedOnly;
}

ActionViewResourceCheckpointSummary summarize_impl(
    const std::vector<CheckpointEvent>& events,
    std::vector<ActionViewResourceRootLink> links) {
    ActionViewResourceCheckpointSummary summary;

    std::optional<ActionViewStd0CacheProducer> pending_producer;
    std::optional<ActionViewStd0CacheHit> pending_hit;
    for (const auto& event : events) {
        if (event.checkpoint == "std0_cache_producer_materialize") {
            ++summary.observed_cache_producer_materialize_events;
            if (pending_producer.has_value()) {
                summary.producers.push_back(*pending_producer);
            }
            pending_producer = ActionViewStd0CacheProducer{};
            pending_producer->capture_sequence =
                parse_first_field_int(event, {"capture_sequence"});
            pending_producer->loaded_file_ptr =
                parse_first_field_string(event, {"loaded_file_ptr_arg"});
        } else if (event.checkpoint == "std0_cache_producer_table_store") {
            ++summary.observed_cache_producer_table_store_events;
            if (!pending_producer.has_value()) {
                pending_producer = ActionViewStd0CacheProducer{};
            }
            pending_producer->materialized_table_ptr =
                parse_first_field_string(event, {"materialized_table_ptr"});
        } else if (event.checkpoint == "std0_cache_producer_key_store") {
            ++summary.observed_cache_producer_key_store_events;
            if (!pending_producer.has_value()) {
                pending_producer = ActionViewStd0CacheProducer{};
            }
            pending_producer->filename_key =
                parse_first_field_string(event, {"filename_key"});
            summary.producers.push_back(*pending_producer);
            pending_producer.reset();
        } else if (event.checkpoint == "std0_cache_lookup") {
            ++summary.observed_cache_lookup_events;
            pending_hit = ActionViewStd0CacheHit{};
            pending_hit->capture_sequence =
                parse_first_field_int(event, {"capture_sequence"});
            pending_hit->expected_key =
                parse_first_field_string(event, {"cache_key_expected"});
            pending_hit->matched_slot =
                parse_first_field_int(event, {"cache_slot_index"});
            pending_hit->root_field_ptr =
                parse_first_field_string(event, {"root_field_ptr"});
        } else if (event.checkpoint == "std0_cache_table_read") {
            ++summary.observed_cache_table_read_events;
            if (!pending_hit.has_value()) {
                pending_hit = ActionViewStd0CacheHit{};
            }
            pending_hit->matched_slot =
                parse_first_field_int(event, {"cache_slot_index"});
            if (!pending_hit->root_field_ptr.has_value()) {
                pending_hit->root_field_ptr =
                    parse_first_field_string(event, {"root_field_ptr"});
            }
        } else if (event.checkpoint == "std0_cache_result_store") {
            ++summary.observed_cache_result_store_events;
            if (!pending_hit.has_value()) {
                pending_hit = ActionViewStd0CacheHit{};
            }
            if (!pending_hit->root_field_ptr.has_value()) {
                pending_hit->root_field_ptr =
                    parse_first_field_string(event, {"root_field_ptr"});
            }
            pending_hit->cached_table_ptr =
                parse_first_field_string(event, {"cached_table_ptr"});
            summary.cache_hits.push_back(*pending_hit);
            pending_hit.reset();
        }
    }
    if (pending_producer.has_value()) {
        summary.producers.push_back(*pending_producer);
    }
    if (pending_hit.has_value()) {
        summary.cache_hits.push_back(*pending_hit);
    }

    summary.selector_root_links = std::move(links);
    for (auto& link : summary.selector_root_links) {
        attach_cache_hit_and_producer(link, summary.cache_hits, summary.producers);
    }
    finalize_summary(summary);
    return summary;
}

} // namespace

ActionViewResourceCheckpointSummary summarize_action_view_resource_checkpoints(
    const std::vector<CheckpointEvent>& events) {
    return summarize_impl(events, build_query_links_from_events(events));
}

ActionViewResourceCheckpointSummary summarize_action_view_resource_checkpoints(
    const std::vector<CheckpointEvent>& events,
    const ActionViewGateCheckpointSummary& gate_summary) {
    return summarize_impl(events, build_query_links_from_gate_summary(gate_summary, events));
}

const char* action_view_resource_checkpoint_status_name(
    ActionViewResourceCheckpointStatus status) {
    switch (status) {
    case ActionViewResourceCheckpointStatus::NotObserved:
        return "NotObserved";
    case ActionViewResourceCheckpointStatus::ObservedOnly:
        return "ObservedOnly";
    case ActionViewResourceCheckpointStatus::MatchesSelectorRoots:
        return "MatchesSelectorRoots";
    case ActionViewResourceCheckpointStatus::MissingSelectorRootLinks:
        return "MissingSelectorRootLinks";
    case ActionViewResourceCheckpointStatus::ChainMismatch:
        return "ChainMismatch";
    }
    return "Unknown";
}

const char* first_battle_action_view_resource_checkpoint_rule_detail() {
    return "FUN_80012f58 loads the aux root through active combatant "
           "r31+0x24 -> +0x10 -> +0x30; STD::LoadStd0EntryTable_80035d4c "
           "writes that selected loaded-resource +0x30 root from either the "
           "global STD0 cache, transient handoff, or fresh resource lookup; "
           "cache table pointers live at 0x8030A214+slot*4 and keys at "
           "0x8030A244+slot*4.";
}

} // namespace savor::predict
