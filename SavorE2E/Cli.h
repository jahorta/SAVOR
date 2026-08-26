#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
    std::int64_t poll_ms = 100;
    std::int64_t worker_count = 1;
    bool wait_for_workers_ready = false;
    std::uint32_t durable_line_mask = kDurableLineNormalMask;
    std::filesystem::path savestate_file;
    std::filesystem::path dtm_file;
    std::filesystem::path iso_path;
    std::filesystem::path dolphin_base_dir;
    std::optional<std::int64_t> source_savestate_id;
    std::optional<std::filesystem::path> workspace_root;
    std::optional<std::filesystem::path> worker_dir_root;
    std::optional<std::filesystem::path> perf_report_dir;
    bool visual_worker = false;
    bool breakpoint_diagnostics = false;
    std::optional<std::filesystem::path> visual_screenshot_dir;
    std::optional<std::string> workflow_unit;
    std::optional<std::string> source_ref_kind;
    std::optional<std::int64_t> source_ref_id;
    std::int64_t perf_snapshot_interval_ms = 1000;
    int repeat = 1;
    std::string load_level;
    std::optional<std::int64_t> tasmovie_rtc;
    std::optional<std::int64_t> tasmovie_rtc_min;
    std::optional<std::int64_t> tasmovie_rtc_max;
    std::optional<std::int64_t> tasmovie_establishment_id;
    std::optional<int> seedprobe_min_value;
    std::optional<int> seedprobe_max_value;
    std::optional<int> seedprobe_samples_per_axis;
    std::optional<int> seedprobe_combo_attempts_per_target;
    std::optional<int> seedprobe_combo_sampler_tries;
    std::optional<int> battle_fake_attack_min;
    std::optional<int> battle_fake_attack_max;
};

struct TasMovieRtcRange {
    std::int64_t low = 0;
    std::int64_t high = 0;
};

enum class E2eScenarioKind {
    SeedProbe,
    Battle,
    TasMovieEstablish,
    TasMovieValidation,
    TasMovieSeedProbe,
    TasMovieSterile,
    TasMovieInputEpochRewrite,
    WorkflowUnit,
};

enum class E2eScenarioEntrySource : std::uint32_t {
    ImportedSavestateFile = 1u << 0,
    FreshTasMovieValidation = 1u << 1,
    PreparedSterilizedCheckpoint = 1u << 2,
    TasMovieEstablishmentAttempt = 1u << 3,
    ExistingWorkspaceReference = 1u << 4,
};

struct E2eScenarioDescriptor {
    std::string_view name;
    E2eScenarioKind kind = E2eScenarioKind::SeedProbe;
    std::uint32_t supported_entry_sources = 0;
    E2eScenarioEntrySource default_entry_source =
        E2eScenarioEntrySource::ImportedSavestateFile;
    bool include_in_all = false;
    bool must_run_alone = false;
    bool requires_repeat_one = false;
    bool requires_one_worker = false;
};

constexpr std::uint32_t EntrySourceBit(E2eScenarioEntrySource source) {
    return static_cast<std::uint32_t>(source);
}

std::span<const E2eScenarioDescriptor> E2eScenarioCatalog();
const E2eScenarioDescriptor* FindE2eScenarioDescriptor(std::string_view name);
E2eScenarioEntrySource SelectE2eScenarioEntrySource(
    const E2eScenarioDescriptor& descriptor,
    const CliOptions& options);
std::string_view ToString(E2eScenarioEntrySource source);
bool EntrySourceRequiresFreshWorkspace(E2eScenarioEntrySource source);

void PrintUsage();
bool ParseArgs(int argc, char** argv, CliOptions* options_out, std::string* error_out);
std::filesystem::path ResolveWorkerExePath(const char* argv0);
TasMovieRtcRange ResolveTasMovieRtcRange(
    const CliOptions& options,
    std::int64_t default_value);

} // namespace savor::e2e
