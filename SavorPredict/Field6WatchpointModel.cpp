#include "Field6WatchpointModel.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <set>

namespace savor::predict {
namespace {

std::string to_lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool contains_field6_label(const CheckpointEvent& event)
{
    const auto checkpoint_id = event.fields.find("checkpoint_id");
    if (checkpoint_id != event.fields.end()
        && to_lower(checkpoint_id->second).find("field6") != std::string::npos) {
        return true;
    }
    const auto label = event.fields.find("memwatch_label");
    if (label != event.fields.end()
        && to_lower(label->second).find("field6") != std::string::npos) {
        return true;
    }
    return to_lower(event.checkpoint).find("field6") != std::string::npos;
}

std::optional<int> parse_int_field(const CheckpointEvent& event, const char* name)
{
    const auto found = event.fields.find(name);
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

std::string field_or_empty(const CheckpointEvent& event, const char* name)
{
    const auto found = event.fields.find(name);
    return found == event.fields.end() ? std::string{} : found->second;
}

bool parse_bool_field(const CheckpointEvent& event, const char* name)
{
    const auto lowered = to_lower(field_or_empty(event, name));
    return lowered == "1" || lowered == "true" || lowered == "yes";
}

} // namespace

Field6WatchpointSummary summarize_field6_watchpoints(
    const std::vector<CheckpointEvent>& events)
{
    Field6WatchpointSummary summary{};
    std::set<std::string> addresses_with_writes;

    for (const auto& event : events) {
        if (event.function != "memory_watchpoint"
            && event.function != "memory_watchpoint_delta") {
            continue;
        }
        if (!contains_field6_label(event)) {
            continue;
        }

        Field6WatchpointEvent observed{};
        observed.capture_sequence = parse_int_field(event, "capture_sequence");
        observed.draw_index = event.rng_draw_index_before;
        observed.pc = event.pc;
        observed.label = field_or_empty(event, "memwatch_label");
        if (observed.label.empty()) {
            observed.label = field_or_empty(event, "checkpoint_name");
        }
        observed.address = field_or_empty(event, "memwatch_addr");
        observed.watch_access = field_or_empty(event, "memwatch_access");
        observed.decoded_access = field_or_empty(event, "decoded_access");
        observed.mnemonic = field_or_empty(event, "decoded_mnemonic");
        observed.value = field_or_empty(event, "decoded_value");
        observed.memory_value = field_or_empty(event, "decoded_memory_value");
        observed.source_pc = field_or_empty(event, "memwatch_source_pc");
        observed.confirmed_current_instruction =
            parse_bool_field(event, "memwatch_confirmed_current_instruction")
            && event.function == "memory_watchpoint";

        ++summary.observed_events;
        if (event.function == "memory_watchpoint_delta") {
            ++summary.unattributed_delta_events;
        }
        if (observed.confirmed_current_instruction) {
            ++summary.confirmed_events;
            const auto access = to_lower(observed.decoded_access);
            if (access == "read") {
                ++summary.confirmed_reads;
                if (!addresses_with_writes.count(observed.address)) {
                    observed.producer_not_seen = true;
                    ++summary.producer_not_seen_reads;
                }
            } else if (access == "write") {
                ++summary.confirmed_writes;
                addresses_with_writes.insert(observed.address);
            }
        }
        summary.events.push_back(std::move(observed));
    }

    return summary;
}

const char* field6_watchpoint_status(const Field6WatchpointSummary& summary)
{
    if (summary.observed_events == 0) {
        return "not_observed";
    }
    if (summary.unattributed_delta_events > 0) {
        return "observed_with_unattributed_deltas";
    }
    if (summary.confirmed_events == summary.observed_events) {
        return "validated";
    }
    return "partial";
}

std::string first_battle_field6_watchpoint_rule_detail()
{
    return "Field6 watch profiles use access watchpoints and classify each stop "
           "from the decoded current instruction. The watchpoint mode is not "
           "the read/write answer; decoded_access is authoritative.";
}

} // namespace savor::predict
