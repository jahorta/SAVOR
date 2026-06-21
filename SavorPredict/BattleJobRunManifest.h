#pragma once

#include "BattleJobClone.h"
#include "BattleJobRunOptions.h"
#include "BattleJobSandbox.h"

#include <filesystem>
#include <iosfwd>
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
    std::filesystem::path trace_report_path;
    std::string terminal_state;
    bool timed_out = false;
    bool capture_found = false;
    int trace_exit_code = -1;
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
