#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace savor::e2e {
class WorkerCoordinatorPerfAccumulator;
}

namespace savor::e2e {

enum class DurableLineCategory : std::uint32_t {
    Result = 1u << 0,
    Failure = 1u << 1,
    Warning = 1u << 2,
    Workflow = 1u << 3,
    Materialization = 1u << 4,
    Claim = 1u << 5,
    Dispatch = 1u << 6,
    Supersede = 1u << 7,
    Worker = 1u << 8,
    Adapter = 1u << 9,
    Db = 1u << 10,
    Debug = 1u << 11,
};

constexpr std::uint32_t DurableLineBit(DurableLineCategory category) {
    return static_cast<std::uint32_t>(category);
}

constexpr std::uint32_t kDurableLineQuietMask =
    DurableLineBit(DurableLineCategory::Result);
constexpr std::uint32_t kDurableLineNormalMask =
    DurableLineBit(DurableLineCategory::Result)
    | DurableLineBit(DurableLineCategory::Workflow)
    | DurableLineBit(DurableLineCategory::Materialization)
    | DurableLineBit(DurableLineCategory::Supersede);
constexpr std::uint32_t kDurableLineVerboseMask =
    kDurableLineNormalMask
    | DurableLineBit(DurableLineCategory::Claim)
    | DurableLineBit(DurableLineCategory::Worker)
    | DurableLineBit(DurableLineCategory::Adapter)
    | DurableLineBit(DurableLineCategory::Db);
constexpr std::uint32_t kDurableLineAllMask =
    DurableLineBit(DurableLineCategory::Result)
    | DurableLineBit(DurableLineCategory::Failure)
    | DurableLineBit(DurableLineCategory::Warning)
    | DurableLineBit(DurableLineCategory::Workflow)
    | DurableLineBit(DurableLineCategory::Materialization)
    | DurableLineBit(DurableLineCategory::Claim)
    | DurableLineBit(DurableLineCategory::Dispatch)
    | DurableLineBit(DurableLineCategory::Supersede)
    | DurableLineBit(DurableLineCategory::Worker)
    | DurableLineBit(DurableLineCategory::Adapter)
    | DurableLineBit(DurableLineCategory::Db)
    | DurableLineBit(DurableLineCategory::Debug);

struct CliOptions {
    std::vector<std::string> scenarios = {"seedprobe"};
    // The currently running scenario (set in main for each requested scenario) so
    // existing scenario implementations can report durable logs under scenario names.
    std::string scenario = "seedprobe";
    std::int64_t timeout_ms = 30000;
    std::int64_t poll_ms = 100;
    std::int64_t worker_count = 1;
    std::uint32_t durable_line_mask = kDurableLineNormalMask;
    std::filesystem::path savestate_file;
    std::filesystem::path dtm_file;
    std::filesystem::path iso_path;
    std::filesystem::path dolphin_base_dir;
    std::optional<std::int64_t> source_savestate_id;
    std::string battle_end_seed_selector = "neutral";
    std::optional<std::int64_t> battle_end_seed_value;
    std::optional<std::filesystem::path> migration_root;
    std::optional<std::filesystem::path> workspace_root;
    std::optional<std::filesystem::path> worker_dir_root;
    std::optional<std::filesystem::path> perf_report_dir;
    bool visual_worker = false;
    std::optional<std::filesystem::path> visual_screenshot_dir;
    std::int64_t perf_snapshot_interval_ms = 1000;
    int repeat = 1;
    std::string load_level;
    std::optional<int> tasmovie_rtc;
    std::optional<int> tasmovie_rtc_min;
    std::optional<int> tasmovie_rtc_max;
    std::optional<int> seedprobe_samples_per_axis;
    std::optional<int> seedprobe_combo_attempts_per_target;
    std::optional<int> battle_fake_attack_low;
    std::optional<int> battle_fake_attack_high;
    std::string battle_macro_mode = "attack";
    std::optional<int> battle_macro_target_slot;
    bool battle_macro_args_supplied = false;
    std::optional<std::string> battle_macro_plan_spec;
    std::optional<int> battle_macro_fake_attacks;
    bool battle_fake_attack_sweep = false;
    int battle_fake_sweep_trials = 10;
    int battle_fake_sweep_min_target_neutral = 0;
    int battle_fake_sweep_max_target_neutral = 10;
    int battle_fake_sweep_min_input_neutral = 0;
    int battle_fake_sweep_max_input_neutral = 20;
    std::optional<std::filesystem::path> battle_fake_sweep_output;
    bool battle_macro_debug = false;
    WorkerCoordinatorPerfAccumulator* worker_coordinator_perf = nullptr;
};

struct TasMovieRtcRange {
    int low = 0;
    int high = 0;
};

void PrintUsage();
bool ParseArgs(int argc, char** argv, CliOptions* options_out, std::string* error_out);
std::filesystem::path ResolveWorkerExePath(const char* argv0);
std::filesystem::path ResolveMigrationRoot(const std::optional<std::filesystem::path>& explicit_root);
TasMovieRtcRange ResolveTasMovieRtcRange(const CliOptions& options, int default_value);

} // namespace savor::e2e
