#include "BattleProjector.h"

#include <vector>

#include "../../Common/Events/OutboxRelay.h"
#include "../SqliteUiReadDb.h"

namespace simcore::db::uiread::projectors {

namespace {

bool RunRelay(
    sqlite3* db,
    const std::string& checkpoint_name,
    std::int64_t checkpoint,
    const std::string& outbox_table,
    const std::string& context_name,
    const std::string& aggregate_kind,
    const std::string& payload_ref_kind,
    const std::vector<simcore::db::events::OutboxRelayDispatchBinding>& bindings,
    int max_batch_size,
    int max_attempts,
    std::string* error_out) {
    events::OutboxRelay relay({
        .db = db,
        .outbox_table = outbox_table,
        .context_name = context_name,
        .aggregate_kind = aggregate_kind,
        .payload_ref_kind = payload_ref_kind,
        .max_attempts = max_attempts,
    });

    events::OutboxRelayResult relay_result{};
    if (!relay.RelayBatch(checkpoint, max_batch_size, bindings, &relay_result, error_out)) {
        return false;
    }

    if (relay_result.last_scanned_outbox_id > checkpoint) {
        simcore::db::SqliteUiReadDb ui_read_db(db);
        if (!ui_read_db.UpsertProjectionCheckpoint({
            checkpoint_name,
            std::string{},
            relay_result.last_scanned_outbox_id,
            simcore::db::types::UtcNow(),
        })) {
            if (error_out) *error_out = sqlite3_errmsg(db);
            return false;
        }
    }

    return true;
}

} // namespace

BattleProjector::BattleProjector(sqlite3* db)
    : db_(db) {
}

std::int64_t BattleProjector::GetCheckpoint(const std::string& projector_name, std::string* error_out) const {
    if (projector_name.empty()) {
        if (error_out) *error_out = "projector_name is required";
        return 0;
    }

    simcore::db::SqliteUiReadDb ui_read_db(db_);
    const auto checkpoint = ui_read_db.GetProjectionCheckpoint(projector_name);
    return checkpoint.has_value() ? checkpoint->last_outbox_id : 0;
}

bool BattleProjector::ProjectAll(std::string* error_out) {
    char* err = nullptr;
    constexpr const char* kSql =
        "BEGIN IMMEDIATE;"
        "INSERT INTO ui_battle_group(battle_set_id,name,status,created_at_utc,completed_at_utc) "
        "SELECT battle_set_id,name,status,created_at_utc,completed_at_utc FROM ab_battle_set "
        "ON CONFLICT(battle_set_id) DO UPDATE SET "
        "name=excluded.name,status=excluded.status,created_at_utc=excluded.created_at_utc,completed_at_utc=excluded.completed_at_utc;"
        "INSERT INTO ui_battle_wave(wave_id,battle_set_id,parent_wave_id,turn_index,status,created_at_utc,completed_at_utc) "
        "SELECT wave_id,battle_set_id,parent_wave_id,turn_index,status,created_at_utc,completed_at_utc FROM ab_turn_wave "
        "ON CONFLICT(wave_id) DO UPDATE SET "
        "battle_set_id=excluded.battle_set_id,parent_wave_id=excluded.parent_wave_id,turn_index=excluded.turn_index,"
        "status=excluded.status,created_at_utc=excluded.created_at_utc,completed_at_utc=excluded.completed_at_utc;"
        "INSERT INTO ui_battle_turn_job(turn_job_id,wave_id,job_state,fake_attacks_this_turn,fake_attacks_used_before,rng_seed,delta_vi,pred_passed,pred_total,battle_outcome,started_at_utc,ended_at_utc) "
        "SELECT turn_job_id,wave_id,job_state,fake_attacks_this_turn,fake_attacks_used_before,rng_seed,delta_vi,pred_passed,pred_total,battle_outcome,started_at_utc,ended_at_utc "
        "FROM ab_turn_job "
        "ON CONFLICT(turn_job_id) DO UPDATE SET "
        "wave_id=excluded.wave_id,job_state=excluded.job_state,fake_attacks_this_turn=excluded.fake_attacks_this_turn,"
        "fake_attacks_used_before=excluded.fake_attacks_used_before,rng_seed=excluded.rng_seed,delta_vi=excluded.delta_vi,"
        "pred_passed=excluded.pred_passed,pred_total=excluded.pred_total,battle_outcome=excluded.battle_outcome,"
        "started_at_utc=excluded.started_at_utc,ended_at_utc=excluded.ended_at_utc;"
        "INSERT INTO ui_battle_followup(turn_job_id,is_victory,manual_followup_status,recorded_dtm_artifact_id,note,updated_at_utc) "
        "SELECT turn_job_id,is_victory,manual_followup_status,recorded_dtm_artifact_id,note,updated_at_utc FROM ab_terminal_followup "
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
    if (projector_name.empty()) {
        if (error_out) *error_out = "projector_name is required";
        return false;
    }
    if (max_batch_size <= 0) {
        if (error_out) *error_out = "max_batch_size must be > 0";
        return false;
    }
    if (max_attempts <= 0) {
        if (error_out) *error_out = "max_attempts must be > 0";
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

    const auto battle_checkpoint_name = projector_name + ".analysis_battle";
    if (!RunRelay(
        db_,
        battle_checkpoint_name,
        GetCheckpoint(battle_checkpoint_name, error_out),
        "ab_outbox_message",
        "AnalysisBattle",
        "battle_set",
        "",
        battle_bindings,
        max_batch_size,
        max_attempts,
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

    const auto execution_checkpoint_name = projector_name + ".execution_rollup";
    return RunRelay(
        db_,
        execution_checkpoint_name,
        GetCheckpoint(execution_checkpoint_name, error_out),
        "exec_outbox_message",
        "Execution",
        "",
        "",
        execution_bindings,
        max_batch_size,
        max_attempts,
        error_out);
}

} // namespace simcore::db::uiread::projectors
