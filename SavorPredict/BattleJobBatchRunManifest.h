#pragma once

#include "BattleJobBatchRunOptions.h"
#include "BattleJobClone.h"
#include "BattleJobSandbox.h"

#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace savor::predict {

struct BattleJobBatchRunJobSummary {
    BattleJobCloneResult clone;
    std::filesystem::path expected_capture_path;
    std::filesystem::path stable_capture_path;
    std::filesystem::path trace_report_path;
    std::string terminal_state;
    bool timed_out = false;
    bool capture_found = false;
    int trace_exit_code = -1;
    std::vector<std::string> errors;
};

struct BattleJobBatchRunSummary {
    BattleJobBatchRunOptions options;
    BattleJobSandboxResult sandbox;
    BattleJobBatchCloneResult clone;
    std::filesystem::path capture_profile_path;
    int worker_count = 0;
    int timeout_ms = 0;
    bool timed_out = false;
    std::vector<BattleJobBatchRunJobSummary> jobs;
    std::vector<std::string> events;
    std::vector<std::string> errors;
};

bool write_battle_job_batch_run_manifest(
    const BattleJobBatchRunSummary& summary,
    const std::filesystem::path& manifest_path,
    std::ostream& err);

bool write_battle_job_batch_run_text_summary(
    const BattleJobBatchRunSummary& summary,
    const std::filesystem::path& summary_path,
    std::ostream& err);

bool write_battle_job_batch_run_csv_summary(
    const BattleJobBatchRunSummary& summary,
    const std::filesystem::path& csv_path,
    std::ostream& err);

} // namespace savor::predict
