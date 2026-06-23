#pragma once

#include "BattleJobRunOptions.h"

#include "Common/DbService.h"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace savor::predict {

struct BattleJobCloneResult {
    std::int64_t original_turn_job_id = 0;
    std::int64_t original_exec_job_id = 0;
    std::int64_t cloned_turn_job_id = 0;
    std::int64_t cloned_exec_job_id = 0;
    std::int64_t original_job_set_id = 0;
    std::int64_t wave_id = 0;
    std::int64_t battle_set_id = 0;
    int turn_index = 0;
    int fake_attacks_this_turn = 0;
    int quarantined_ready_jobs = 0;
    std::string patched_input_ini;
};

struct BattleJobBatchCloneResult {
    std::vector<BattleJobCloneResult> clones;
    int quarantined_ready_jobs = 0;
};

std::string patch_battle_single_turn_capture_profile(
    const std::string& input_ini,
    const std::filesystem::path& capture_profile_path);

bool clone_battle_job_for_capture(
    savor::db::core::DBService& db_service,
    const BattleJobRunOptions& options,
    const std::filesystem::path& capture_profile_path,
    BattleJobCloneResult* result_out,
    std::ostream& err);

bool clone_battle_jobs_for_capture(
    savor::db::core::DBService& db_service,
    const std::vector<long long>& source_exec_job_ids,
    const std::filesystem::path& capture_profile_path,
    BattleJobBatchCloneResult* result_out,
    std::ostream& err);

} // namespace savor::predict
