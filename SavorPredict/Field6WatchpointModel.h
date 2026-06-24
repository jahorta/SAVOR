#pragma once

#include "CheckpointTrace.h"

#include <optional>
#include <string>
#include <vector>

namespace savor::predict {

struct Field6WatchpointEvent {
    std::optional<int> capture_sequence;
    std::optional<int> draw_index;
    std::string pc;
    std::string label;
    std::string address;
    std::string watch_access;
    std::string decoded_access;
    std::string mnemonic;
    std::string value;
    std::string memory_value;
    std::string source_pc;
    bool confirmed_current_instruction = false;
    bool producer_not_seen = false;
};

struct Field6WatchpointSummary {
    int observed_events = 0;
    int confirmed_events = 0;
    int unattributed_delta_events = 0;
    int confirmed_reads = 0;
    int confirmed_writes = 0;
    int producer_not_seen_reads = 0;
    std::vector<Field6WatchpointEvent> events;
};

Field6WatchpointSummary summarize_field6_watchpoints(
    const std::vector<CheckpointEvent>& events);

const char* field6_watchpoint_status(const Field6WatchpointSummary& summary);
std::string first_battle_field6_watchpoint_rule_detail();

} // namespace savor::predict
