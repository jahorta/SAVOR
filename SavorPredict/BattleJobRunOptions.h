#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace savor::predict {

struct BattleJobRunOptions {
    std::filesystem::path db_root = "D:/SavorPredictDB";
    std::filesystem::path run_root;
    std::filesystem::path iso_path;
    std::filesystem::path dolphin_base_dir;
    std::filesystem::path worker_exe_path;
    std::filesystem::path capture_profile_path;
    std::optional<long long> turn_job_id;
    std::optional<long long> exec_job_id;
    int poll_ms = 100;
    int timeout_ms = 180000;
};

struct BattleJobRunParseResult {
    BattleJobRunOptions options;
    bool help_requested = false;
    std::vector<std::string> errors;
};

std::filesystem::path default_battle_job_run_root();
std::filesystem::path default_battle_job_worker_exe(const std::filesystem::path& executable_path);
bool is_mutable_debug_db_root(const std::filesystem::path& path);
std::vector<std::string> validate_battle_job_run_options(const BattleJobRunOptions& options);
BattleJobRunParseResult parse_battle_job_run_tokens(
    const std::vector<std::string>& args,
    const std::filesystem::path& executable_path);

} // namespace savor::predict
