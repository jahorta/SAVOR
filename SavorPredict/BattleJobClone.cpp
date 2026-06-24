#include "BattleJobClone.h"

#include "Common/Types/UtcTimestamp.h"
#include "Execution/IExecutionDb.h"
#include "Phases/Programs/BattleTurnRunner/BattleTurnRunnerPayload.h"
#include "Runner/IPC/Wire.h"
#include "Runner/Script/PhaseScriptVM.h"
#include "Utils/IniDoc.h"

#include <sqlite3.h>

#include <optional>
#include <ostream>
#include <set>
#include <sstream>
#include <string>

namespace savor::predict {
namespace {

constexpr const char* kTurnJobRefKind = "analysis_battle.turn_job";
constexpr const char* kJobSection = "BattleSingleTurn.Job";

struct Statement {
    sqlite3_stmt* st = nullptr;
    ~Statement() {
        if (st != nullptr) {
            sqlite3_finalize(st);
        }
    }
};

bool prepare(sqlite3* db, const char* sql, Statement* out, std::ostream& err) {
    if (db == nullptr) {
        err << "SQLite handle is null.\n";
        return false;
    }
    if (sqlite3_prepare_v2(db, sql, -1, &out->st, nullptr) != SQLITE_OK) {
        err << sqlite3_errmsg(db) << "\n";
        return false;
    }
    return true;
}

bool exec(sqlite3* db, const char* sql, std::ostream& err) {
    char* raw_error = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &raw_error) != SQLITE_OK) {
        err << (raw_error != nullptr ? raw_error : sqlite3_errmsg(db)) << "\n";
        sqlite3_free(raw_error);
        return false;
    }
    return true;
}

std::optional<std::int64_t> column_i64_optional(sqlite3_stmt* st, int column) {
    if (sqlite3_column_type(st, column) == SQLITE_NULL) {
        return std::nullopt;
    }
    return sqlite3_column_int64(st, column);
}

std::optional<int> column_int_optional(sqlite3_stmt* st, int column) {
    if (sqlite3_column_type(st, column) == SQLITE_NULL) {
        return std::nullopt;
    }
    return sqlite3_column_int(st, column);
}

std::optional<std::string> column_text_optional(sqlite3_stmt* st, int column) {
    if (sqlite3_column_type(st, column) == SQLITE_NULL) {
        return std::nullopt;
    }
    const auto* text = sqlite3_column_text(st, column);
    if (text == nullptr) {
        return std::string{};
    }
    return std::string(reinterpret_cast<const char*>(text));
}

struct SourceTurnJob {
    std::int64_t turn_job_id = 0;
    std::optional<std::int64_t> exec_job_id;
    std::int64_t wave_id = 0;
    std::int64_t battle_set_id = 0;
    int turn_index = 0;
    std::int64_t plan_id = 0;
    std::optional<std::int64_t> source_savestate_id;
    std::optional<std::int64_t> seed_candidate_id;
    std::optional<std::int64_t> authored_plan_id;
    std::optional<int> authored_turn_index;
    std::optional<std::string> resolved_turn_commands_blob;
    std::optional<std::string> resolved_turn_variant_key;
    int fake_attacks_this_turn = 0;
    int fake_attacks_used_before = 0;
};

std::optional<SourceTurnJob> read_source_turn_job(
    sqlite3* analysis_db,
    const BattleJobRunOptions& options,
    std::ostream& err) {
    const bool by_turn = options.turn_job_id.has_value();
    constexpr const char* kSqlByTurn = R"SQL(
        SELECT
            j.turn_job_id,j.exec_job_id,j.wave_id,w.battle_set_id,w.turn_index,
            j.plan_id,j.source_savestate_id,j.seed_candidate_id,j.authored_plan_id,j.authored_turn_index,
            j.resolved_turn_commands_blob,j.resolved_turn_variant_key,
            j.fake_attacks_this_turn,j.fake_attacks_used_before
        FROM ab_turn_job j
        JOIN ab_turn_wave w ON w.wave_id=j.wave_id
        WHERE j.turn_job_id=?1
        LIMIT 1;
    )SQL";
    constexpr const char* kSqlByExec = R"SQL(
        SELECT
            j.turn_job_id,j.exec_job_id,j.wave_id,w.battle_set_id,w.turn_index,
            j.plan_id,j.source_savestate_id,j.seed_candidate_id,j.authored_plan_id,j.authored_turn_index,
            j.resolved_turn_commands_blob,j.resolved_turn_variant_key,
            j.fake_attacks_this_turn,j.fake_attacks_used_before
        FROM ab_turn_job j
        JOIN ab_turn_wave w ON w.wave_id=j.wave_id
        WHERE j.exec_job_id=?1
        LIMIT 1;
    )SQL";

    Statement st;
    if (!prepare(analysis_db, by_turn ? kSqlByTurn : kSqlByExec, &st, err)) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, by_turn ? *options.turn_job_id : *options.exec_job_id);
    const auto rc = sqlite3_step(st.st);
    if (rc == SQLITE_DONE) {
        err << "No matching battle turn job found in sandbox analysis DB.\n";
        return std::nullopt;
    }
    if (rc != SQLITE_ROW) {
        err << "Failed reading battle turn job: " << sqlite3_errmsg(analysis_db) << "\n";
        return std::nullopt;
    }

    SourceTurnJob row{};
    row.turn_job_id = sqlite3_column_int64(st.st, 0);
    row.exec_job_id = column_i64_optional(st.st, 1);
    row.wave_id = sqlite3_column_int64(st.st, 2);
    row.battle_set_id = sqlite3_column_int64(st.st, 3);
    row.turn_index = sqlite3_column_int(st.st, 4);
    row.plan_id = sqlite3_column_int64(st.st, 5);
    row.source_savestate_id = column_i64_optional(st.st, 6);
    row.seed_candidate_id = column_i64_optional(st.st, 7);
    row.authored_plan_id = column_i64_optional(st.st, 8);
    row.authored_turn_index = column_int_optional(st.st, 9);
    row.resolved_turn_commands_blob = column_text_optional(st.st, 10);
    row.resolved_turn_variant_key = column_text_optional(st.st, 11);
    row.fake_attacks_this_turn = sqlite3_column_int(st.st, 12);
    row.fake_attacks_used_before = sqlite3_column_int(st.st, 13);
    return row;
}

std::optional<int> read_exec_program_kind(sqlite3* execution_db, std::int64_t exec_job_id, std::ostream& err) {
    Statement st;
    if (!prepare(execution_db, "SELECT program_kind FROM exec_job WHERE job_id=?1;", &st, err)) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, exec_job_id);
    const auto rc = sqlite3_step(st.st);
    if (rc == SQLITE_DONE) {
        err << "Execution job not found: " << exec_job_id << "\n";
        return std::nullopt;
    }
    if (rc != SQLITE_ROW) {
        err << "Failed reading execution job: " << sqlite3_errmsg(execution_db) << "\n";
        return std::nullopt;
    }
    return sqlite3_column_int(st.st, 0);
}

int quarantine_ready_jobs(sqlite3* execution_db, std::int64_t keep_job_id, std::ostream& err) {
    Statement st;
    constexpr const char* kSql =
        "UPDATE exec_job "
        "SET state='SUPERSEDED', claimed_by_token=NULL, lease_expires_at_utc=NULL, "
        "    error_text='SavorPredict sandbox quarantine before live capture run' "
        "WHERE job_id<>?1 AND state IN ('QUEUED','PENDING_MATERIALIZATION','CLAIMED','RUNNING');";
    if (!prepare(execution_db, kSql, &st, err)) {
        return -1;
    }
    sqlite3_bind_int64(st.st, 1, keep_job_id);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        err << "Failed quarantining runnable sandbox jobs: " << sqlite3_errmsg(execution_db) << "\n";
        return -1;
    }
    return sqlite3_changes(execution_db);
}

int quarantine_ready_jobs_except(sqlite3* execution_db, const std::set<std::int64_t>& keep_job_ids, std::ostream& err) {
    if (keep_job_ids.empty()) {
        err << "Internal error: batch quarantine keep set is empty.\n";
        return -1;
    }
    std::ostringstream keep_list;
    bool first = true;
    for (const auto job_id : keep_job_ids) {
        if (!first) {
            keep_list << ",";
        }
        first = false;
        keep_list << job_id;
    }
    const auto sql =
        "UPDATE exec_job "
        "SET state='SUPERSEDED', claimed_by_token=NULL, lease_expires_at_utc=NULL, "
        "    error_text='SavorPredict sandbox quarantine before batch live capture run' "
        "WHERE job_id NOT IN (" + keep_list.str() + ") "
        "AND state IN ('QUEUED','PENDING_MATERIALIZATION','CLAIMED','RUNNING');";
    if (!exec(execution_db, sql.c_str(), err)) {
        err << "Failed quarantining runnable sandbox jobs for batch.\n";
        return -1;
    }
    return sqlite3_changes(execution_db);
}

} // namespace

std::string patch_battle_single_turn_capture_profile(
    const std::string& input_ini,
    const std::filesystem::path& capture_profile_path,
    std::optional<std::uint32_t> override_start_rng_seed,
    std::optional<std::uint32_t> battle_run_ms) {
    auto ini = IniDoc::parse(input_ini);
    ini.set(kJobSection, "capture_profile_path", capture_profile_path.string());
    if (battle_run_ms.has_value()) {
        ini.set(kJobSection, "run_ms_override", std::to_string(*battle_run_ms));
    }
    if (override_start_rng_seed.has_value()) {
        ini.set(kJobSection, "override_start_rng_seed", std::to_string(*override_start_rng_seed));
    }
    return ini.to_string_sorted();
}

bool clone_battle_job_for_capture_internal(
    savor::db::core::DBService& db_service,
    const BattleJobRunOptions& options,
    const std::filesystem::path& capture_profile_path,
    bool quarantine_after_clone,
    BattleJobCloneResult* result_out,
    std::ostream& err) {
    if (result_out == nullptr) {
        err << "Internal error: clone result output is null.\n";
        return false;
    }
    auto* analysis_db = db_service.AnalysisDb();
    auto* execution_db = db_service.ExecutionDb();
    auto* raw_analysis = db_service.RawAnalysisSqlite();
    auto* raw_execution = db_service.RawExecutionSqlite();
    if (analysis_db == nullptr || execution_db == nullptr || raw_analysis == nullptr || raw_execution == nullptr) {
        err << "DBService is not fully started.\n";
        return false;
    }

    const auto source = read_source_turn_job(raw_analysis, options, err);
    if (!source.has_value()) {
        return false;
    }
    if (source->turn_index != 1) {
        err << "run-battle-job v1 only supports first-turn battle jobs; source turn_index="
            << source->turn_index << ".\n";
        return false;
    }
    if (!source->exec_job_id.has_value() || *source->exec_job_id <= 0) {
        err << "Source battle turn job has no execution job id.\n";
        return false;
    }

    const auto exec_program_kind = read_exec_program_kind(raw_execution, *source->exec_job_id, err);
    if (!exec_program_kind.has_value()) {
        return false;
    }
    if (*exec_program_kind != static_cast<int>(savor::PK_BattleSingleTurnRunner)) {
        err << "Source execution job is not a BattleSingleTurnRunner job; program_kind="
            << *exec_program_kind << ".\n";
        return false;
    }

    const auto original_exec = execution_db->GetJob(*source->exec_job_id);
    if (!original_exec.has_value()) {
        err << "Source execution job not found through execution DB API: " << *source->exec_job_id << "\n";
        return false;
    }
    if (original_exec->program_ref_kind != kTurnJobRefKind || original_exec->program_ref_id != source->turn_job_id) {
        err << "Source execution job does not point at the selected battle turn job.\n";
        return false;
    }

    const auto now = savor::db::types::UtcNow();
    std::string error;
    std::int64_t cloned_turn_job_id = 0;
    if (!analysis_db->RecordBattleTurnJob(
            {
                .wave_id = source->wave_id,
                .plan_id = source->plan_id,
                .source_savestate_id = source->source_savestate_id,
                .seed_candidate_id = source->seed_candidate_id,
                .authored_plan_id = source->authored_plan_id,
                .authored_turn_index = source->authored_turn_index,
                .resolved_turn_commands_blob = source->resolved_turn_commands_blob,
                .resolved_turn_variant_key = source->resolved_turn_variant_key,
                .fake_attacks_this_turn = source->fake_attacks_this_turn,
                .fake_attacks_used_before = source->fake_attacks_used_before,
                .job_state = savor::db::BattleTurnJobState::Queued,
                .started_at_utc = now,
                .recorded_at_utc = now,
                .correlation_id = "savorpredict-run-battle-job",
                .causation_id = "turn-job-" + std::to_string(source->turn_job_id),
            },
            &cloned_turn_job_id,
            &error)
        || cloned_turn_job_id <= 0) {
        err << "Failed cloning analysis battle turn job: " << error << "\n";
        return false;
    }

    const auto patched_input = patch_battle_single_turn_capture_profile(
        original_exec->input_ini,
        capture_profile_path,
        options.override_start_rng_seed,
        options.battle_run_ms);
    std::ostringstream fingerprint;
    fingerprint << original_exec->fingerprint << ":savorpredict-capture:" << source->exec_job_id.value()
                << ":" << cloned_turn_job_id;

    std::int64_t cloned_exec_job_id = 0;
    if (!execution_db->EnqueueJob(
            {
                .job_set_id = original_exec->job_set_id,
                .parent_job_id = original_exec->job_id,
                .program_kind = static_cast<std::int32_t>(savor::PK_BattleSingleTurnRunner),
                .program_version = phase::battle::turnrunner::PayloadVersion,
                .program_ref_kind = kTurnJobRefKind,
                .program_ref_id = cloned_turn_job_id,
                .savestate_id = original_exec->savestate_id,
                .fingerprint = fingerprint.str(),
                .priority = original_exec->priority + 100000,
                .max_attempts = 1,
                .input_ini = patched_input,
                .pending_until_workflow_materialized = false,
            },
            &cloned_exec_job_id,
            &error)
        || cloned_exec_job_id <= 0) {
        err << "Failed enqueueing cloned execution job: " << error << "\n";
        return false;
    }

    if (!analysis_db->SetBattleTurnJobExecJobId(cloned_turn_job_id, cloned_exec_job_id, &error)) {
        err << "Failed linking cloned turn job to execution job: " << error << "\n";
        return false;
    }

    int quarantined = 0;
    if (quarantine_after_clone) {
        quarantined = quarantine_ready_jobs(raw_execution, cloned_exec_job_id, err);
        if (quarantined < 0) {
            return false;
        }
    }

    result_out->original_turn_job_id = source->turn_job_id;
    result_out->original_exec_job_id = *source->exec_job_id;
    result_out->cloned_turn_job_id = cloned_turn_job_id;
    result_out->cloned_exec_job_id = cloned_exec_job_id;
    result_out->original_job_set_id = original_exec->job_set_id;
    result_out->wave_id = source->wave_id;
    result_out->battle_set_id = source->battle_set_id;
    result_out->turn_index = source->turn_index;
    result_out->fake_attacks_this_turn = source->fake_attacks_this_turn;
    result_out->quarantined_ready_jobs = quarantined;
    result_out->battle_run_ms = options.battle_run_ms;
    result_out->override_start_rng_seed = options.override_start_rng_seed;
    result_out->patched_input_ini = patched_input;
    return true;
}

bool clone_battle_job_for_capture(
    savor::db::core::DBService& db_service,
    const BattleJobRunOptions& options,
    const std::filesystem::path& capture_profile_path,
    BattleJobCloneResult* result_out,
    std::ostream& err) {
    return clone_battle_job_for_capture_internal(
        db_service,
        options,
        capture_profile_path,
        true,
        result_out,
        err);
}

bool clone_battle_jobs_for_capture(
    savor::db::core::DBService& db_service,
    const std::vector<BattleJobCloneRequest>& requests,
    const std::filesystem::path& capture_profile_path,
    BattleJobBatchCloneResult* result_out,
    std::ostream& err) {
    if (result_out == nullptr) {
        err << "Internal error: batch clone result output is null.\n";
        return false;
    }
    auto* raw_execution = db_service.RawExecutionSqlite();
    if (raw_execution == nullptr) {
        err << "DBService is missing raw execution sqlite handle.\n";
        return false;
    }
    result_out->clones.clear();
    result_out->quarantined_ready_jobs = 0;

    std::set<std::int64_t> keep_job_ids;
    for (const auto& request : requests) {
        BattleJobRunOptions single_options;
        single_options.exec_job_id = request.source_exec_job_id;
        single_options.override_start_rng_seed = request.override_start_rng_seed;
        single_options.battle_run_ms = request.battle_run_ms;
        BattleJobCloneResult clone;
        if (!clone_battle_job_for_capture_internal(
                db_service,
                single_options,
                capture_profile_path,
                false,
                &clone,
                err)) {
            return false;
        }
        keep_job_ids.insert(clone.cloned_exec_job_id);
        result_out->clones.push_back(std::move(clone));
    }

    const int quarantined = quarantine_ready_jobs_except(raw_execution, keep_job_ids, err);
    if (quarantined < 0) {
        return false;
    }
    result_out->quarantined_ready_jobs = quarantined;
    for (auto& clone : result_out->clones) {
        clone.quarantined_ready_jobs = quarantined;
    }
    return true;
}

bool clone_battle_jobs_for_capture(
    savor::db::core::DBService& db_service,
    const std::vector<long long>& source_exec_job_ids,
    const std::filesystem::path& capture_profile_path,
    std::optional<std::uint32_t> battle_run_ms,
    std::optional<std::uint32_t> override_start_rng_seed,
    BattleJobBatchCloneResult* result_out,
    std::ostream& err) {
    std::vector<BattleJobCloneRequest> requests;
    requests.reserve(source_exec_job_ids.size());
    for (const auto source_exec_job_id : source_exec_job_ids) {
        requests.push_back({
            .source_exec_job_id = source_exec_job_id,
            .override_start_rng_seed = override_start_rng_seed,
            .battle_run_ms = battle_run_ms,
        });
    }
    return clone_battle_jobs_for_capture(
        db_service,
        requests,
        capture_profile_path,
        result_out,
        err);
}

bool clone_battle_jobs_for_capture(
    savor::db::core::DBService& db_service,
    const std::vector<long long>& source_exec_job_ids,
    const std::filesystem::path& capture_profile_path,
    BattleJobBatchCloneResult* result_out,
    std::ostream& err) {
    return clone_battle_jobs_for_capture(
        db_service,
        source_exec_job_ids,
        capture_profile_path,
        std::nullopt,
        std::nullopt,
        result_out,
        err);
}

} // namespace savor::predict
