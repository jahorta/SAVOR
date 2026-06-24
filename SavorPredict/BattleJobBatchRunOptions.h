#pragma once

#include "BattleJobRunOptions.h"

#include "DbRootCopy.h"

#include <filesystem>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace savor::predict {

struct BattleJobBatchRunRequest {
    long long exec_job_id = 0;
    std::optional<std::uint32_t> override_start_rng_seed;
    std::optional<std::uint32_t> battle_run_ms;
};

struct BattleJobBatchRunOptions {
    std::filesystem::path db_root = "D:/SavorPredictDB";
    std::filesystem::path run_root;
    std::filesystem::path iso_path;
    std::filesystem::path dolphin_base_dir;
    std::filesystem::path worker_exe_path;
    std::filesystem::path capture_profile_path;
    savor::dbutils::SandboxMode sandbox_mode = savor::dbutils::SandboxMode::MinimalBattleSingleTurn;
    std::vector<long long> exec_job_ids;
    std::vector<BattleJobBatchRunRequest> seeded_exec_job_requests;
    int poll_ms = 100;
    std::optional<int> timeout_ms;
    int max_workers = 2;
    std::optional<std::uint32_t> battle_run_ms;
    std::optional<std::uint32_t> override_start_rng_seed;
};

struct BattleJobBatchRunParseResult {
    BattleJobBatchRunOptions options;
    bool help_requested = false;
    std::vector<std::string> errors;
};

std::filesystem::path default_battle_job_batch_run_root();
std::vector<BattleJobBatchRunRequest> resolved_battle_job_batch_requests(const BattleJobBatchRunOptions& options);
std::vector<long long> unique_battle_job_batch_source_exec_job_ids(const BattleJobBatchRunOptions& options);
int resolved_battle_job_batch_timeout_ms(const BattleJobBatchRunOptions& options);
std::vector<std::string> validate_battle_job_batch_run_options(const BattleJobBatchRunOptions& options);
BattleJobBatchRunParseResult parse_battle_job_batch_run_tokens(
    const std::vector<std::string>& args,
    const std::filesystem::path& executable_path);

} // namespace savor::predict
