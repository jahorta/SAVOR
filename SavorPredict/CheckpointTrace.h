#pragma once

#include <filesystem>
#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace savor::predict {

struct TraceCheckpointsOptions {
    std::filesystem::path db_root = "D:/SavorPredictDB";
    std::filesystem::path checkpoint_file;
    std::optional<long long> turn_job_id;
    std::optional<long long> exec_job_id;
    std::optional<int> expected_mode0e_camera_draws;
    std::optional<int> expected_turn_order_draws;
    std::optional<int> expected_attack_events;
    std::optional<int> expected_crit_draws;
    std::optional<int> expected_counter_roll_ceiling;
    std::optional<int> expected_drop_rolls;
    bool json = false;
};

struct CheckpointEvent {
    int line_number = 0;
    std::map<std::string, std::string> fields;
    std::string pc;
    std::string function;
    std::string checkpoint;
    std::optional<int> rng_draw_index_before;
    std::optional<unsigned int> rng_seed_before;
    std::optional<unsigned int> rng_seed_after;
    std::optional<int> active_slot;
    std::optional<int> target_slot;
    bool owns_rng_draw = false;
    std::string known_rng_owner;
};

struct CheckpointParseResult {
    std::vector<CheckpointEvent> events;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

CheckpointParseResult parse_checkpoint_stream(std::istream& in);
const std::map<std::string, std::string>& known_rng_callsite_owners();
int run_trace_checkpoints(const TraceCheckpointsOptions& options, std::ostream& out, std::ostream& err);

} // namespace savor::predict
