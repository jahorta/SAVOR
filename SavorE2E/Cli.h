#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
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
    std::int64_t timeout_ms = 30000;
    std::int64_t poll_ms = 100;
    std::int64_t worker_count = 1;
    std::uint32_t durable_line_mask = kDurableLineNormalMask;
    std::filesystem::path savestate_file;
    std::filesystem::path dtm_file;
    std::filesystem::path iso_path;
    std::filesystem::path dolphin_base_dir;
    std::optional<std::filesystem::path> migration_root;
    std::optional<std::filesystem::path> workspace_root;
    std::optional<std::filesystem::path> worker_dir_root;
    bool visual_worker = false;
    std::optional<std::filesystem::path> visual_screenshot_dir;
    std::optional<int> tasmovie_headroom_x10;
    std::optional<int> tasmovie_rtc;
    std::optional<int> seedprobe_samples_per_axis;
    std::optional<int> seedprobe_combo_attempts_per_target;
    std::optional<int> battle_fake_attack_low;
    std::optional<int> battle_fake_attack_high;
};

void PrintUsage();
bool ParseArgs(int argc, char** argv, CliOptions* options_out, std::string* error_out);
std::filesystem::path ResolveWorkerExePath(const char* argv0);
std::filesystem::path ResolveMigrationRoot(const std::optional<std::filesystem::path>& explicit_root);

} // namespace savor::e2e
