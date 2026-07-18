#pragma once

#include "ActionViewStdJsonCache.h"
#include "BattleJobClone.h"
#include "BattleJobRunOptions.h"
#include "BattleJobSandbox.h"
#include "CaptureArtifact.h"

#include <filesystem>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace savor::predict {

struct BattleJobRunSummary {
    BattleJobRunOptions options;
    BattleJobSandboxResult sandbox;
    BattleJobCloneResult clone;
    std::filesystem::path capture_profile_path;
    std::filesystem::path expected_capture_path;
    std::filesystem::path stable_capture_path;
    std::filesystem::path capture_export_path;
    std::filesystem::path trace_report_path;
    ActionViewStdJsonCacheResolution std_json_cache;
    std::string terminal_state;
    bool timed_out = false;
    bool capture_found = false;
    PreparedCaptureArtifact capture_artifact;
    int trace_exit_code = -1;
    std::optional<std::uint32_t> captured_original_seed;
    std::optional<std::uint32_t> captured_override_seed;
    std::optional<std::uint32_t> captured_applied_seed;
    std::optional<bool> captured_seed_readback_matches;
    std::vector<std::string> events;
    std::vector<std::string> errors;
};

bool write_battle_job_run_manifest(
    const BattleJobRunSummary& summary,
    const std::filesystem::path& manifest_path,
    std::ostream& err);

bool write_battle_job_run_text_summary(
    const BattleJobRunSummary& summary,
    const std::filesystem::path& summary_path,
    std::ostream& err);

} // namespace savor::predict
