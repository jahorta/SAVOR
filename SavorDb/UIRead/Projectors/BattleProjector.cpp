#include "BattleProjector.h"

#include <vector>

#include "ProjectorContract.h"

namespace savor::db::uiread::projectors {

BattleProjector::BattleProjector(sqlite3* db)
    : db_(db) {
}

bool BattleProjector::ProjectAll(std::string* error_out) {
    char* err = nullptr;
    constexpr const char* kSql =
        "BEGIN IMMEDIATE;"
        "INSERT INTO ui_battle_group(battle_set_id,name,status,created_at_utc,completed_at_utc) "
        "SELECT battle_set_id,name,status,created_at_utc,completed_at_utc FROM ab_battle_set "
        "WHERE 1 "
        "ON CONFLICT(battle_set_id) DO UPDATE SET "
        "name=excluded.name,status=excluded.status,created_at_utc=excluded.created_at_utc,completed_at_utc=excluded.completed_at_utc;"
        "INSERT INTO ui_battle_wave(wave_id,battle_set_id,parent_wave_id,turn_index,status,created_at_utc,completed_at_utc) "
        "SELECT wave_id,battle_set_id,parent_wave_id,turn_index,status,created_at_utc,completed_at_utc FROM ab_turn_wave "
        "WHERE 1 "
        "ON CONFLICT(wave_id) DO UPDATE SET "
        "battle_set_id=excluded.battle_set_id,parent_wave_id=excluded.parent_wave_id,turn_index=excluded.turn_index,"
        "status=excluded.status,created_at_utc=excluded.created_at_utc,completed_at_utc=excluded.completed_at_utc;"
        "INSERT INTO ui_battle_turn_job(turn_job_id,wave_id,job_state,fake_attacks_this_turn,fake_attacks_used_before,rng_seed,delta_vi,pred_passed,pred_total,battle_outcome,started_at_utc,ended_at_utc) "
        "SELECT turn_job_id,wave_id,job_state,fake_attacks_this_turn,fake_attacks_used_before,rng_seed,delta_vi,pred_passed,pred_total,battle_outcome,started_at_utc,ended_at_utc "
        "FROM ab_turn_job "
        "WHERE 1 "
        "ON CONFLICT(turn_job_id) DO UPDATE SET "
        "wave_id=excluded.wave_id,job_state=excluded.job_state,fake_attacks_this_turn=excluded.fake_attacks_this_turn,"
        "fake_attacks_used_before=excluded.fake_attacks_used_before,rng_seed=excluded.rng_seed,delta_vi=excluded.delta_vi,"
        "pred_passed=excluded.pred_passed,pred_total=excluded.pred_total,battle_outcome=excluded.battle_outcome,"
        "started_at_utc=excluded.started_at_utc,ended_at_utc=excluded.ended_at_utc;"
        "INSERT INTO ui_battle_followup(turn_job_id,is_victory,manual_followup_status,recorded_dtm_artifact_id,note,updated_at_utc) "
        "SELECT turn_job_id,is_victory,manual_followup_status,recorded_dtm_artifact_id,note,updated_at_utc FROM ab_terminal_followup "
        "WHERE 1 "
        "ON CONFLICT(turn_job_id) DO UPDATE SET "
        "is_victory=excluded.is_victory,manual_followup_status=excluded.manual_followup_status,"
        "recorded_dtm_artifact_id=excluded.recorded_dtm_artifact_id,note=excluded.note,updated_at_utc=excluded.updated_at_utc;"
        "COMMIT;";
    if (sqlite3_exec(db_, kSql, nullptr, nullptr, &err) != SQLITE_OK) {
        if (error_out) *error_out = err ? err : sqlite3_errmsg(db_);
        sqlite3_free(err);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    return true;
}

bool BattleProjector::ProjectFromOutbox(const std::string& projector_name, int max_batch_size, std::string* error_out, int max_attempts) {
    if (!ValidateProjectorContractInputs(projector_name, max_batch_size, max_attempts, error_out)) {
        return false;
    }

    const auto project_all = [this](const events::EventEnvelope&, std::string* handler_error) {
        return ProjectAll(handler_error);
    };

    const std::vector<events::OutboxRelayDispatchBinding> battle_bindings{
        { { "AnalysisBattle.BattleSetCreated.v1", 1 }, project_all },
        { { "AnalysisBattle.SeedCandidateAdded.v1", 1 }, project_all },
        { { "AnalysisBattle.TurnWaveCreated.v1", 1 }, project_all },
        { { "AnalysisBattle.TurnJobRecorded.v1", 1 }, project_all },
        { { "AnalysisBattle.SelectionPoolCreated.v1", 1 }, project_all },
        { { "AnalysisBattle.SelectionDecisionRecorded.v1", 1 }, project_all },
        { { "AnalysisBattle.TerminalFollowupUpdated.v1", 1 }, project_all },
    };

    if (!RunProjectorRelay(
        db_,
        projector_name,
        "AnalysisBattle",
        "ab_outbox_message",
        {
            .db = db_,
            .outbox_table = "ab_outbox_message",
            .context_name = "AnalysisBattle",
            .aggregate_kind = "battle_set",
            .payload_ref_kind = "",
            .max_attempts = max_attempts,
        },
        battle_bindings,
        max_batch_size,
        error_out)) {
        return false;
    }

    const std::vector<events::OutboxRelayDispatchBinding> execution_bindings{
        { { "Execution.JobClaimed.v1", 1 }, project_all },
        { { "Execution.JobLeaseRenewed.v1", 1 }, project_all },
        { { "Execution.JobProgressed.v1", 1 }, project_all },
        { { "Execution.JobCompleted.v1", 1 }, project_all },
        { { "Execution.JobEventArchived.v1", 1 }, project_all },
        { { "Execution.JobRestored.v1", 1 }, project_all },
    };

    return RunProjectorRelay(
        db_,
        projector_name,
        "Execution",
        "exec_outbox_message",
        {
            .db = db_,
            .outbox_table = "exec_outbox_message",
            .context_name = "Execution",
            .aggregate_kind = "",
            .payload_ref_kind = "",
            .max_attempts = max_attempts,
        },
        execution_bindings,
        max_batch_size,
        error_out);
}

} // namespace savor::db::uiread::projectors
