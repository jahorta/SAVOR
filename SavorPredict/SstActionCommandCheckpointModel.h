#pragma once

#include "CheckpointTrace.h"

#include <optional>
#include <string_view>
#include <vector>

namespace savor::predict {

enum class SstActionCommandCheckpointStatus {
    ObservedOnly,
    MatchesExpected,
    MissingLiveFields,
    Field6Mismatch,
};

enum class SstActionCommandCheckpointKind {
    Case2Field6Store,
    Case8Field6Store,
};

struct SstActionCommandCheckpointEvent {
    SstActionCommandCheckpointKind kind = SstActionCommandCheckpointKind::Case2Field6Store;
    std::optional<int> draw_index;
    std::optional<int> source_field6;
    std::optional<int> destination_field6;
    std::optional<int> written_field6_register;
    std::optional<bool> source_matches_destination;
    std::optional<int> action_sequence_id;
};

struct SstActionCommandCheckpointSummary {
    int observed_store_events = 0;
    int observed_case2_store_events = 0;
    int observed_case8_store_events = 0;
    int events_with_source_field6 = 0;
    int events_with_destination_field6 = 0;
    int events_with_written_field6_register = 0;
    int events_with_action_sequence_id = 0;
    int source_destination_matches = 0;
    int source_destination_mismatches = 0;
    int key8_store_events = 0;
    std::optional<int> first_store_draw_index;
    std::optional<int> first_key8_store_draw_index;
    std::vector<SstActionCommandCheckpointEvent> events;
    SstActionCommandCheckpointStatus status = SstActionCommandCheckpointStatus::ObservedOnly;
};

SstActionCommandCheckpointSummary summarize_sst_action_command_checkpoints(
    const std::vector<CheckpointEvent>& events);
const char* sst_action_command_checkpoint_status_name(SstActionCommandCheckpointStatus status);
const char* sst_action_command_checkpoint_kind_name(SstActionCommandCheckpointKind kind);
const char* first_battle_sst_action_command_checkpoint_rule_detail();

} // namespace savor::predict
