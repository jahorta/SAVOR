#pragma once

#include "ActionViewGateCheckpointModel.h"
#include "CheckpointTrace.h"

#include <optional>
#include <string>
#include <vector>

namespace savor::predict {

enum class ActionViewResourceCheckpointStatus {
    NotObserved,
    ObservedOnly,
    MatchesSelectorRoots,
    MissingSelectorRootLinks,
    ChainMismatch,
};

struct ActionViewStd0CacheProducer {
    std::optional<int> capture_sequence;
    std::optional<std::string> loaded_file_ptr;
    std::optional<std::string> materialized_table_ptr;
    std::optional<std::string> filename_key;
};

struct ActionViewStd0CacheHit {
    std::optional<int> capture_sequence;
    std::optional<std::string> expected_key;
    std::optional<int> matched_slot;
    std::optional<std::string> root_field_ptr;
    std::optional<std::string> cached_table_ptr;
};

struct ActionViewResourceRootLink {
    std::optional<int> capture_sequence;
    std::optional<int> active_slot;
    std::optional<int> target_slot;
    std::optional<std::string> aux_list_root;
    std::optional<std::string> chain_payload_0x24;
    std::optional<std::string> chain_loaded_resource_0x10;
    std::optional<std::string> chain_aux_root_0x30;
    std::optional<bool> chain_aux_root_matches_query;
    std::optional<std::string> cache_expected_key;
    std::optional<int> cache_slot;
    std::optional<std::string> cache_root_field_ptr;
    std::optional<bool> cache_root_field_matches_loaded_resource_plus_0x30;
    std::optional<std::string> producer_loaded_file_ptr;
    std::optional<std::string> matched_resource_stem;
    std::optional<std::string> matched_std0_filename;
    bool linked_to_cache_hit = false;
    bool linked_to_cache_producer = false;
};

struct ActionViewResourceCheckpointSummary {
    int observed_cache_producer_materialize_events = 0;
    int observed_cache_producer_table_store_events = 0;
    int observed_cache_producer_key_store_events = 0;
    int complete_cache_producers = 0;
    int observed_cache_lookup_events = 0;
    int observed_cache_table_read_events = 0;
    int observed_cache_result_store_events = 0;
    int complete_cache_hits = 0;
    int selector_aux_roots = 0;
    int selector_aux_roots_linked_to_cache_hits = 0;
    int selector_aux_roots_linked_to_cache_producers = 0;
    int selector_aux_roots_with_chain_samples = 0;
    int selector_aux_roots_with_matching_chain = 0;
    int selector_aux_roots_with_mismatching_chain = 0;
    int selector_aux_roots_with_loaded_resource_root_field_match = 0;
    int selector_aux_roots_with_loaded_resource_root_field_mismatch = 0;
    std::vector<ActionViewStd0CacheProducer> producers;
    std::vector<ActionViewStd0CacheHit> cache_hits;
    std::vector<ActionViewResourceRootLink> selector_root_links;
    ActionViewResourceCheckpointStatus status = ActionViewResourceCheckpointStatus::NotObserved;
};

ActionViewResourceCheckpointSummary summarize_action_view_resource_checkpoints(
    const std::vector<CheckpointEvent>& events);
ActionViewResourceCheckpointSummary summarize_action_view_resource_checkpoints(
    const std::vector<CheckpointEvent>& events,
    const ActionViewGateCheckpointSummary& gate_summary);
const char* action_view_resource_checkpoint_status_name(
    ActionViewResourceCheckpointStatus status);
const char* first_battle_action_view_resource_checkpoint_rule_detail();

} // namespace savor::predict
