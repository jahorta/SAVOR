#include "SqliteAnalysisDb.h"

#include "../Common/Events/OutboxEventIds.h"

#include <string>
#include <utility>

namespace savor::db::analysis {
namespace {

struct Statement {
    sqlite3_stmt* value = nullptr;
    ~Statement() { if (value) sqlite3_finalize(value); }
};

std::string Text(sqlite3_stmt* statement, int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value ? reinterpret_cast<const char*>(value) : std::string{};
}

std::string Blob(sqlite3_stmt* statement, int column) {
    const auto* value = static_cast<const char*>(
        sqlite3_column_blob(statement, column));
    const auto size = sqlite3_column_bytes(statement, column);
    return value && size > 0 ? std::string(value, value + size) : std::string{};
}

std::optional<std::int64_t> OptionalI64(sqlite3_stmt* statement, int column) {
    return sqlite3_column_type(statement, column) == SQLITE_NULL
        ? std::nullopt
        : std::optional<std::int64_t>(sqlite3_column_int64(statement, column));
}

std::optional<int> OptionalInt(sqlite3_stmt* statement, int column) {
    return sqlite3_column_type(statement, column) == SQLITE_NULL
        ? std::nullopt
        : std::optional<int>(sqlite3_column_int(statement, column));
}

std::optional<std::string> OptionalText(
    sqlite3_stmt* statement, int column) {
    return sqlite3_column_type(statement, column) == SQLITE_NULL
        ? std::nullopt
        : std::optional<std::string>(Text(statement, column));
}

std::optional<std::string> OptionalBlob(
    sqlite3_stmt* statement, int column) {
    return sqlite3_column_type(statement, column) == SQLITE_NULL
        ? std::nullopt
        : std::optional<std::string>(Blob(statement, column));
}

std::optional<types::UtcTimePoint> OptionalTime(
    sqlite3_stmt* statement, int column) {
    const auto value = OptionalI64(statement, column);
    return value ? std::optional<types::UtcTimePoint>(
        types::UtcTimePoint(types::UtcTimePoint::duration(*value)))
        : std::nullopt;
}

void BindOptionalI64(sqlite3_stmt* statement, int index,
                     const std::optional<std::int64_t>& value) {
    if (value) sqlite3_bind_int64(statement, index, *value);
    else sqlite3_bind_null(statement, index);
}

void BindOptionalInt(sqlite3_stmt* statement, int index,
                     const std::optional<int>& value) {
    if (value) sqlite3_bind_int(statement, index, *value);
    else sqlite3_bind_null(statement, index);
}

void BindOptionalText(sqlite3_stmt* statement, int index,
                      const std::optional<std::string>& value) {
    if (value) sqlite3_bind_text(
        statement, index, value->c_str(), -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(statement, index);
}

void BindOptionalBlob(sqlite3_stmt* statement, int index,
                      const std::optional<std::string>& value) {
    if (value) sqlite3_bind_blob(statement, index, value->data(),
        static_cast<int>(value->size()), SQLITE_TRANSIENT);
    else sqlite3_bind_null(statement, index);
}

bool Begin(sqlite3* db, std::string* error_out) {
    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr)
        == SQLITE_OK) return true;
    if (error_out) *error_out = sqlite3_errmsg(db);
    return false;
}

bool Commit(sqlite3* db, std::string* error_out) {
    if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK)
        return true;
    if (error_out) *error_out = sqlite3_errmsg(db);
    return false;
}

bool Rollback(sqlite3* db, std::string message, std::string* error_out) {
    (void)sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    if (error_out) *error_out = std::move(message);
    return false;
}

bool InsertBattleEvent(
    sqlite3* db,
    std::string_view event_type,
    std::string_view aggregate_kind,
    std::int64_t aggregate_id,
    std::string_view correlation_id,
    std::string_view causation_id,
    types::UtcTimePoint occurred_at,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id,
    std::string* error_out) {
    constexpr int kMaximumAttempts = 5;
    for (int attempt = 0; attempt < kMaximumAttempts; ++attempt) {
        std::string event_id;
        if (!outbox::MakeDbOwnedEventId(
                db, "AnalysisBattle", event_type, payload_ref_kind,
                payload_ref_id, &event_id, error_out))
            return false;
        Statement insert;
        constexpr auto sql =
            "INSERT INTO ab_outbox_message(event_id,event_type,event_version,"
            "context_name,aggregate_kind,aggregate_id,correlation_id,"
            "causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id) "
            "VALUES(?1,?2,1,'AnalysisBattle',?3,?4,?5,?6,?7,?8,?9);";
        if (sqlite3_prepare_v2(db, sql, -1, &insert.value, nullptr) != SQLITE_OK) {
            if (error_out) *error_out = sqlite3_errmsg(db);
            return false;
        }
        sqlite3_bind_text(insert.value, 1, event_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert.value, 2, event_type.data(),
            static_cast<int>(event_type.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(insert.value, 3, aggregate_kind.data(),
            static_cast<int>(aggregate_kind.size()), SQLITE_TRANSIENT);
        const auto aggregate = std::to_string(aggregate_id);
        sqlite3_bind_text(insert.value, 4, aggregate.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert.value, 5, correlation_id.data(),
            static_cast<int>(correlation_id.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(insert.value, 6, causation_id.data(),
            static_cast<int>(causation_id.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert.value, 7,
            occurred_at.time_since_epoch().count());
        sqlite3_bind_text(insert.value, 8, payload_ref_kind.data(),
            static_cast<int>(payload_ref_kind.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert.value, 9, payload_ref_id);
        if (sqlite3_step(insert.value) == SQLITE_DONE) return true;
        if (!outbox::IsUniqueConstraint(db)) {
            if (error_out) *error_out = sqlite3_errmsg(db);
            return false;
        }
    }
    if (error_out) *error_out = "could not allocate a Battle outbox event ID";
    return false;
}

std::optional<BattleCompletionRecord> ReadCompletion(
    sqlite3* db, std::string_view predicate, std::int64_t value) {
    const std::string sql =
        "SELECT battle_completion_id,workflow_instance_id,workflow_step_id,"
        "exec_job_id,battle_set_id,wave_id,selected_turn_job_id,"
        "selected_execution_job_id,entry_savestate_id,completion_savestate_id,"
        "manifest_version,manifest_blob,manifest_sha256,manifest_artifact_id,"
        "route_kind,transition_filename,worker_terminal_sha256,error_code,"
        "error_text,status,created_at_utc,completed_at_utc "
        "FROM ab_battle_completion WHERE " + std::string(predicate) + "=?1;";
    Statement statement;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement.value, nullptr)
        != SQLITE_OK) return std::nullopt;
    sqlite3_bind_int64(statement.value, 1, value);
    if (sqlite3_step(statement.value) != SQLITE_ROW) return std::nullopt;
    BattleCompletionRecord row{};
    row.battle_completion_id = sqlite3_column_int64(statement.value, 0);
    row.workflow_instance_id = sqlite3_column_int64(statement.value, 1);
    row.workflow_step_id = sqlite3_column_int64(statement.value, 2);
    row.exec_job_id = OptionalI64(statement.value, 3);
    row.battle_set_id = sqlite3_column_int64(statement.value, 4);
    row.wave_id = sqlite3_column_int64(statement.value, 5);
    row.selected_turn_job_id = sqlite3_column_int64(statement.value, 6);
    row.selected_execution_job_id = sqlite3_column_int64(statement.value, 7);
    row.entry_savestate_id = sqlite3_column_int64(statement.value, 8);
    row.completion_savestate_id = OptionalI64(statement.value, 9);
    row.manifest_version = OptionalInt(statement.value, 10);
    row.manifest_blob = OptionalBlob(statement.value, 11);
    row.manifest_sha256 = OptionalText(statement.value, 12);
    row.manifest_artifact_id = OptionalI64(statement.value, 13);
    row.route_kind = OptionalText(statement.value, 14);
    row.transition_filename = OptionalText(statement.value, 15);
    row.worker_terminal_sha256 = OptionalText(statement.value, 16);
    row.error_code = OptionalText(statement.value, 17);
    row.error_text = OptionalText(statement.value, 18);
    row.status = Text(statement.value, 19);
    row.created_at_utc = types::UtcTimePoint(
        types::UtcTimePoint::duration(sqlite3_column_int64(statement.value, 20)));
    row.completed_at_utc = OptionalTime(statement.value, 21);
    return row;
}

std::optional<BattleRecordingRecord> ReadRecording(
    sqlite3* db, std::string_view predicate, std::int64_t value) {
    const std::string sql =
        "SELECT battle_recording_id,battle_completion_id,workflow_instance_id,"
        "workflow_step_id,exec_job_id,source_savestate_id,source_dtm_artifact_id,"
        "source_itinerary_artifact_id,source_binding_version,source_binding_blob,"
        "source_binding_sha256,replay_plan_version,replay_plan_blob,"
        "replay_plan_sha256,outcome,recorded_dtm_artifact_id,"
        "recorded_itinerary_artifact_id,paired_checkpoint_savestate_id,"
        "timing_anchor_version,timing_anchor_blob,tas_movie_tree_id,"
        "validation_request_id,sterilization_request_id,worker_terminal_sha256,"
        "error_code,error_text,status,created_at_utc,completed_at_utc "
        "FROM ab_battle_recording WHERE " + std::string(predicate) + "=?1;";
    Statement statement;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement.value, nullptr)
        != SQLITE_OK) return std::nullopt;
    sqlite3_bind_int64(statement.value, 1, value);
    if (sqlite3_step(statement.value) != SQLITE_ROW) return std::nullopt;
    BattleRecordingRecord row{};
    row.battle_recording_id = sqlite3_column_int64(statement.value, 0);
    row.battle_completion_id = sqlite3_column_int64(statement.value, 1);
    row.workflow_instance_id = sqlite3_column_int64(statement.value, 2);
    row.workflow_step_id = sqlite3_column_int64(statement.value, 3);
    row.exec_job_id = OptionalI64(statement.value, 4);
    row.source_savestate_id = sqlite3_column_int64(statement.value, 5);
    row.source_dtm_artifact_id = sqlite3_column_int64(statement.value, 6);
    row.source_itinerary_artifact_id = sqlite3_column_int64(statement.value, 7);
    row.source_binding_version = sqlite3_column_int(statement.value, 8);
    row.source_binding_blob = Blob(statement.value, 9);
    row.source_binding_sha256 = Text(statement.value, 10);
    row.replay_plan_version = sqlite3_column_int(statement.value, 11);
    row.replay_plan_blob = Blob(statement.value, 12);
    row.replay_plan_sha256 = Text(statement.value, 13);
    row.outcome = OptionalText(statement.value, 14);
    row.recorded_dtm_artifact_id = OptionalI64(statement.value, 15);
    row.recorded_itinerary_artifact_id = OptionalI64(statement.value, 16);
    row.paired_checkpoint_savestate_id = OptionalI64(statement.value, 17);
    row.timing_anchor_version = OptionalInt(statement.value, 18);
    row.timing_anchor_blob = OptionalBlob(statement.value, 19);
    row.tas_movie_tree_id = OptionalI64(statement.value, 20);
    row.validation_request_id = OptionalI64(statement.value, 21);
    row.sterilization_request_id = OptionalI64(statement.value, 22);
    row.worker_terminal_sha256 = OptionalText(statement.value, 23);
    row.error_code = OptionalText(statement.value, 24);
    row.error_text = OptionalText(statement.value, 25);
    row.status = Text(statement.value, 26);
    row.created_at_utc = types::UtcTimePoint(
        types::UtcTimePoint::duration(sqlite3_column_int64(statement.value, 27)));
    row.completed_at_utc = OptionalTime(statement.value, 28);
    return row;
}

std::optional<BattleReplayRecord> ReadReplay(
    sqlite3* db, std::string_view predicate, std::int64_t value) {
    const std::string sql =
        "SELECT battle_replay_id,battle_completion_id,workflow_instance_id,"
        "workflow_step_id,exec_job_id,source_savestate_id,source_dtm_artifact_id,"
        "source_itinerary_artifact_id,source_binding_version,source_binding_blob,"
        "source_binding_sha256,replay_plan_version,replay_plan_blob,"
        "replay_plan_sha256,outcome,mismatch_turn,expected_rng,observed_rng,"
        "observed_completion_blob,observed_completion_sha256,"
        "observed_transition_blob,observed_transition_sha256,"
        "worker_terminal_sha256,error_code,error_text,status,created_at_utc,"
        "completed_at_utc FROM ab_battle_replay WHERE " +
        std::string(predicate) + "=?1;";
    Statement statement;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement.value, nullptr)
        != SQLITE_OK) return std::nullopt;
    sqlite3_bind_int64(statement.value, 1, value);
    if (sqlite3_step(statement.value) != SQLITE_ROW) return std::nullopt;
    BattleReplayRecord row{};
    row.battle_replay_id = sqlite3_column_int64(statement.value, 0);
    row.battle_completion_id = sqlite3_column_int64(statement.value, 1);
    row.workflow_instance_id = sqlite3_column_int64(statement.value, 2);
    row.workflow_step_id = sqlite3_column_int64(statement.value, 3);
    row.exec_job_id = OptionalI64(statement.value, 4);
    row.source_savestate_id = sqlite3_column_int64(statement.value, 5);
    row.source_dtm_artifact_id = OptionalI64(statement.value, 6);
    row.source_itinerary_artifact_id = OptionalI64(statement.value, 7);
    row.source_binding_version = sqlite3_column_int(statement.value, 8);
    row.source_binding_blob = Blob(statement.value, 9);
    row.source_binding_sha256 = Text(statement.value, 10);
    row.replay_plan_version = sqlite3_column_int(statement.value, 11);
    row.replay_plan_blob = Blob(statement.value, 12);
    row.replay_plan_sha256 = Text(statement.value, 13);
    row.outcome = OptionalText(statement.value, 14);
    row.mismatch_turn = static_cast<std::uint32_t>(
        sqlite3_column_int64(statement.value, 15));
    row.expected_rng = static_cast<std::uint32_t>(
        sqlite3_column_int64(statement.value, 16));
    row.observed_rng = static_cast<std::uint32_t>(
        sqlite3_column_int64(statement.value, 17));
    row.observed_completion_blob = OptionalBlob(statement.value, 18);
    row.observed_completion_sha256 = OptionalText(statement.value, 19);
    row.observed_transition_blob = OptionalBlob(statement.value, 20);
    row.observed_transition_sha256 = OptionalText(statement.value, 21);
    row.worker_terminal_sha256 = OptionalText(statement.value, 22);
    row.error_code = OptionalText(statement.value, 23);
    row.error_text = OptionalText(statement.value, 24);
    row.status = Text(statement.value, 25);
    row.created_at_utc = types::UtcTimePoint(
        types::UtcTimePoint::duration(sqlite3_column_int64(statement.value, 26)));
    row.completed_at_utc = OptionalTime(statement.value, 27);
    return row;
}

} // namespace

bool SqliteAnalysisDb::CreateBattleCompletion(
    const CreateBattleCompletionCommand& command,
    std::int64_t* id_out, std::string* error_out) {
    if (id_out) *id_out = 0;
    if (!db_ || command.workflow_instance_id <= 0 ||
        command.workflow_step_id <= 0 || command.battle_set_id <= 0 ||
        command.wave_id <= 0 || command.selected_turn_job_id <= 0 ||
        command.selected_execution_job_id <= 0 ||
        command.entry_savestate_id <= 0 || command.status != "QUEUED" ||
        (command.exec_job_id && *command.exec_job_id <= 0)) {
        if (error_out) *error_out = "invalid battle completion create command";
        return false;
    }
    if (!Begin(db_, error_out)) return false;
    const auto existing = ReadCompletion(
        db_, "workflow_step_id", command.workflow_step_id);
    if (existing) {
        const bool exact = existing->workflow_instance_id == command.workflow_instance_id
            && existing->battle_set_id == command.battle_set_id
            && existing->wave_id == command.wave_id
            && existing->selected_turn_job_id == command.selected_turn_job_id
            && existing->selected_execution_job_id == command.selected_execution_job_id
            && existing->entry_savestate_id == command.entry_savestate_id
            && (!command.exec_job_id || existing->exec_job_id == command.exec_job_id);
        if (!exact) return Rollback(db_,
            "battle completion workflow step has different immutable lineage",
            error_out);
        if (!Commit(db_, error_out)) return false;
        if (id_out) *id_out = existing->battle_completion_id;
        return true;
    }
    Statement insert;
    constexpr auto sql =
        "INSERT INTO ab_battle_completion(workflow_instance_id,workflow_step_id,"
        "exec_job_id,battle_set_id,wave_id,selected_turn_job_id,"
        "selected_execution_job_id,entry_savestate_id,status,created_at_utc) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,'QUEUED',?9);";
    if (sqlite3_prepare_v2(db_, sql, -1, &insert.value, nullptr) != SQLITE_OK)
        return Rollback(db_, sqlite3_errmsg(db_), error_out);
    sqlite3_bind_int64(insert.value, 1, command.workflow_instance_id);
    sqlite3_bind_int64(insert.value, 2, command.workflow_step_id);
    BindOptionalI64(insert.value, 3, command.exec_job_id);
    sqlite3_bind_int64(insert.value, 4, command.battle_set_id);
    sqlite3_bind_int64(insert.value, 5, command.wave_id);
    sqlite3_bind_int64(insert.value, 6, command.selected_turn_job_id);
    sqlite3_bind_int64(insert.value, 7, command.selected_execution_job_id);
    sqlite3_bind_int64(insert.value, 8, command.entry_savestate_id);
    sqlite3_bind_int64(insert.value, 9,
        command.created_at_utc.time_since_epoch().count());
    if (sqlite3_step(insert.value) != SQLITE_DONE)
        return Rollback(db_, sqlite3_errmsg(db_), error_out);
    const auto id = sqlite3_last_insert_rowid(db_);
    if (!InsertBattleEvent(db_,
            "AnalysisBattle.BattleCompletionCreated.v1",
            "battle_completion", id, command.correlation_id,
            command.causation_id, command.created_at_utc,
            "battle_completion", id, error_out))
        return Rollback(db_, error_out ? *error_out :
            "Battle Completion created event failed", error_out);
    if (!Commit(db_, error_out)) return false;
    if (id_out) *id_out = id;
    return true;
}

bool SqliteAnalysisDb::BindBattleCompletionExecutionJob(
    const BindBattleCompletionExecutionJobCommand& command,
    std::string* error_out) {
    if (!db_ || command.battle_completion_id <= 0 || command.exec_job_id <= 0 ||
        command.workflow_instance_id <= 0 || command.workflow_step_id <= 0) {
        if (error_out) *error_out = "invalid battle completion job binding";
        return false;
    }
    Statement update;
    constexpr auto sql =
        "UPDATE ab_battle_completion SET exec_job_id=?2 WHERE "
        "battle_completion_id=?1 AND workflow_instance_id=?3 AND "
        "workflow_step_id=?4 AND status='QUEUED' AND "
        "(exec_job_id IS NULL OR exec_job_id=?2);";
    if (sqlite3_prepare_v2(db_, sql, -1, &update.value, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_); return false;
    }
    sqlite3_bind_int64(update.value, 1, command.battle_completion_id);
    sqlite3_bind_int64(update.value, 2, command.exec_job_id);
    sqlite3_bind_int64(update.value, 3, command.workflow_instance_id);
    sqlite3_bind_int64(update.value, 4, command.workflow_step_id);
    if (sqlite3_step(update.value) != SQLITE_DONE || sqlite3_changes(db_) != 1) {
        if (error_out) *error_out = "battle completion job binding lost its queued precondition";
        return false;
    }
    return true;
}

bool SqliteAnalysisDb::CompleteBattleCompletion(
    const CompleteBattleCompletionCommand& command, std::string* error_out) {
    if (!db_ || command.battle_completion_id <= 0 ||
        command.completion_savestate_id <= 0 || command.manifest_version <= 0 ||
        command.manifest_blob.empty() || command.manifest_sha256.size() != 64 ||
        command.manifest_artifact_id <= 0 || command.route_kind.empty() ||
        command.transition_filename.empty() ||
        command.worker_terminal_sha256.size() != 64 ||
        command.status != "COMPLETED") {
        if (error_out) *error_out = "invalid battle completion result command";
        return false;
    }
    if (!Begin(db_, error_out)) return false;
    const auto current = ReadCompletion(db_, "battle_completion_id",
                                        command.battle_completion_id);
    if (!current) return Rollback(db_, "battle completion row not found", error_out);
    if (current->status == "COMPLETED") {
        const bool exact = current->completion_savestate_id == command.completion_savestate_id
            && current->manifest_version == command.manifest_version
            && current->manifest_blob == command.manifest_blob
            && current->manifest_sha256 == command.manifest_sha256
            && current->manifest_artifact_id == command.manifest_artifact_id
            && current->route_kind == command.route_kind
            && current->transition_filename == command.transition_filename
            && current->worker_terminal_sha256 == command.worker_terminal_sha256;
        if (!exact) return Rollback(db_,
            "battle completion already has a different durable result", error_out);
        return Commit(db_, error_out);
    }
    if (current->status != "QUEUED" || !current->exec_job_id)
        return Rollback(db_, "battle completion is not bound and queued", error_out);
    Statement update;
    constexpr auto sql =
        "UPDATE ab_battle_completion SET completion_savestate_id=?2,"
        "manifest_version=?3,manifest_blob=?4,manifest_sha256=?5,"
        "manifest_artifact_id=?6,route_kind=?7,transition_filename=?8,"
        "worker_terminal_sha256=?9,status='COMPLETED',completed_at_utc=?10 "
        "WHERE battle_completion_id=?1 AND status='QUEUED';";
    if (sqlite3_prepare_v2(db_, sql, -1, &update.value, nullptr) != SQLITE_OK)
        return Rollback(db_, sqlite3_errmsg(db_), error_out);
    sqlite3_bind_int64(update.value, 1, command.battle_completion_id);
    sqlite3_bind_int64(update.value, 2, command.completion_savestate_id);
    sqlite3_bind_int(update.value, 3, command.manifest_version);
    sqlite3_bind_blob(update.value, 4, command.manifest_blob.data(),
        static_cast<int>(command.manifest_blob.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(update.value, 5, command.manifest_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(update.value, 6, command.manifest_artifact_id);
    sqlite3_bind_text(update.value, 7, command.route_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(update.value, 8, command.transition_filename.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(update.value, 9, command.worker_terminal_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(update.value, 10,
        command.completed_at_utc.time_since_epoch().count());
    if (sqlite3_step(update.value) != SQLITE_DONE || sqlite3_changes(db_) != 1)
        return Rollback(db_, "battle completion terminal transition failed", error_out);
    if (!InsertBattleEvent(db_,
            "AnalysisBattle.BattleCompletionCompleted.v1",
            "battle_completion", command.battle_completion_id,
            command.correlation_id, command.causation_id,
            command.completed_at_utc, "battle_completion",
            command.battle_completion_id, error_out))
        return Rollback(db_, error_out ? *error_out :
            "Battle Completion completed event failed", error_out);
    return Commit(db_, error_out);
}

bool SqliteAnalysisDb::FailBattleCompletion(
    const FailBattleCompletionCommand& command, std::string* error_out) {
    if (!db_ || command.battle_completion_id <= 0 || command.error_code.empty()) {
        if (error_out) *error_out = "invalid battle completion failure command";
        return false;
    }
    if (!Begin(db_, error_out)) return false;
    const auto current = ReadCompletion(
        db_, "battle_completion_id", command.battle_completion_id);
    if (!current)
        return Rollback(db_, "battle completion row not found", error_out);
    if (current->status == "FAILED") {
        const bool exact = current->error_code == command.error_code &&
            current->error_text == command.error_text &&
            current->worker_terminal_sha256 == command.worker_terminal_sha256;
        if (!exact) return Rollback(db_,
            "battle completion already has a different failure", error_out);
        return Commit(db_, error_out);
    }
    if (current->status != "QUEUED")
        return Rollback(db_,
            "battle completion is not queued for failure", error_out);
    Statement update;
    constexpr auto sql =
        "UPDATE ab_battle_completion SET error_code=?2,error_text=?3,"
        "worker_terminal_sha256=?4,status='FAILED',completed_at_utc=?5 "
        "WHERE battle_completion_id=?1 AND (status='QUEUED' OR "
        "(status='FAILED' AND error_code=?2));";
    if (sqlite3_prepare_v2(db_, sql, -1, &update.value, nullptr) != SQLITE_OK) {
        return Rollback(db_, sqlite3_errmsg(db_), error_out);
    }
    sqlite3_bind_int64(update.value, 1, command.battle_completion_id);
    sqlite3_bind_text(update.value, 2, command.error_code.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(update.value, 3, command.error_text.c_str(), -1, SQLITE_TRANSIENT);
    BindOptionalText(update.value, 4, command.worker_terminal_sha256);
    sqlite3_bind_int64(update.value, 5,
        command.completed_at_utc.time_since_epoch().count());
    if (sqlite3_step(update.value) != SQLITE_DONE || sqlite3_changes(db_) != 1) {
        return Rollback(db_,
            "battle completion failure transition failed", error_out);
    }
    if (!InsertBattleEvent(db_,
            "AnalysisBattle.BattleCompletionFailed.v1",
            "battle_completion", command.battle_completion_id,
            command.correlation_id, command.causation_id,
            command.completed_at_utc, "battle_completion",
            command.battle_completion_id, error_out))
        return Rollback(db_, error_out ? *error_out :
            "Battle Completion failed event failed", error_out);
    return Commit(db_, error_out);
}

std::optional<BattleCompletionRecord> SqliteAnalysisDb::GetBattleCompletion(
    std::int64_t id) const {
    return db_ && id > 0 ? ReadCompletion(db_, "battle_completion_id", id)
                         : std::nullopt;
}

std::optional<BattleCompletionRecord>
SqliteAnalysisDb::GetBattleCompletionForExecJob(std::int64_t id) const {
    return db_ && id > 0 ? ReadCompletion(db_, "exec_job_id", id)
                         : std::nullopt;
}

bool SqliteAnalysisDb::CreateBattleRecording(
    const CreateBattleRecordingCommand& command,
    std::int64_t* id_out, std::string* error_out) {
    if (id_out) *id_out = 0;
    if (!db_ || command.battle_completion_id <= 0 ||
        command.workflow_instance_id <= 0 || command.workflow_step_id <= 0 ||
        command.source_savestate_id <= 0 || command.source_dtm_artifact_id <= 0 ||
        command.source_itinerary_artifact_id <= 0 ||
        command.source_binding_version <= 0 ||
        command.source_binding_blob.empty() ||
        command.source_binding_sha256.size() != 64 ||
        command.replay_plan_version <= 0 || command.replay_plan_blob.empty() ||
        command.replay_plan_sha256.size() != 64 || command.status != "QUEUED") {
        if (error_out) *error_out = "invalid battle recording create command";
        return false;
    }
    if (!Begin(db_, error_out)) return false;
    const auto existing = ReadRecording(db_, "workflow_step_id",
                                        command.workflow_step_id);
    if (existing) {
        const bool exact = existing->battle_completion_id == command.battle_completion_id
            && existing->workflow_instance_id == command.workflow_instance_id
            && existing->source_savestate_id == command.source_savestate_id
            && existing->source_dtm_artifact_id == command.source_dtm_artifact_id
            && existing->source_itinerary_artifact_id == command.source_itinerary_artifact_id
            && existing->source_binding_version == command.source_binding_version
            && existing->source_binding_blob == command.source_binding_blob
            && existing->source_binding_sha256 == command.source_binding_sha256
            && existing->replay_plan_version == command.replay_plan_version
            && existing->replay_plan_blob == command.replay_plan_blob
            && existing->replay_plan_sha256 == command.replay_plan_sha256;
        if (!exact) return Rollback(db_,
            "battle recording workflow step has different immutable plan", error_out);
        if (!Commit(db_, error_out)) return false;
        if (id_out) *id_out = existing->battle_recording_id;
        return true;
    }
    Statement insert;
    constexpr auto sql =
        "INSERT INTO ab_battle_recording(battle_completion_id,"
        "workflow_instance_id,workflow_step_id,exec_job_id,source_savestate_id,"
        "source_dtm_artifact_id,source_itinerary_artifact_id,source_binding_version,"
        "source_binding_blob,source_binding_sha256,replay_plan_version,"
        "replay_plan_blob,replay_plan_sha256,status,created_at_utc) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,'QUEUED',?14);";
    if (sqlite3_prepare_v2(db_, sql, -1, &insert.value, nullptr) != SQLITE_OK)
        return Rollback(db_, sqlite3_errmsg(db_), error_out);
    sqlite3_bind_int64(insert.value, 1, command.battle_completion_id);
    sqlite3_bind_int64(insert.value, 2, command.workflow_instance_id);
    sqlite3_bind_int64(insert.value, 3, command.workflow_step_id);
    BindOptionalI64(insert.value, 4, command.exec_job_id);
    sqlite3_bind_int64(insert.value, 5, command.source_savestate_id);
    sqlite3_bind_int64(insert.value, 6, command.source_dtm_artifact_id);
    sqlite3_bind_int64(insert.value, 7, command.source_itinerary_artifact_id);
    sqlite3_bind_int(insert.value, 8, command.source_binding_version);
    sqlite3_bind_blob(insert.value, 9, command.source_binding_blob.data(),
        static_cast<int>(command.source_binding_blob.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(insert.value, 10,
        command.source_binding_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(insert.value, 11, command.replay_plan_version);
    sqlite3_bind_blob(insert.value, 12, command.replay_plan_blob.data(),
        static_cast<int>(command.replay_plan_blob.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(insert.value, 13, command.replay_plan_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert.value, 14,
        command.created_at_utc.time_since_epoch().count());
    if (sqlite3_step(insert.value) != SQLITE_DONE)
        return Rollback(db_, sqlite3_errmsg(db_), error_out);
    const auto id = sqlite3_last_insert_rowid(db_);
    if (!InsertBattleEvent(db_,
            "AnalysisBattle.BattleRecordingCreated.v1",
            "battle_recording", id, command.correlation_id,
            command.causation_id, command.created_at_utc,
            "battle_recording", id, error_out))
        return Rollback(db_, error_out ? *error_out :
            "Battle Recording created event failed", error_out);
    if (!Commit(db_, error_out)) return false;
    if (id_out) *id_out = id;
    return true;
}

bool SqliteAnalysisDb::BindBattleRecordingExecutionJob(
    const BindBattleRecordingExecutionJobCommand& command,
    std::string* error_out) {
    if (!db_ || command.battle_recording_id <= 0 || command.exec_job_id <= 0 ||
        command.workflow_instance_id <= 0 || command.workflow_step_id <= 0) {
        if (error_out) *error_out = "invalid battle recording job binding";
        return false;
    }
    Statement update;
    constexpr auto sql =
        "UPDATE ab_battle_recording SET exec_job_id=?2 WHERE "
        "battle_recording_id=?1 AND workflow_instance_id=?3 AND "
        "workflow_step_id=?4 AND status='QUEUED' AND "
        "(exec_job_id IS NULL OR exec_job_id=?2);";
    if (sqlite3_prepare_v2(db_, sql, -1, &update.value, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_); return false;
    }
    sqlite3_bind_int64(update.value, 1, command.battle_recording_id);
    sqlite3_bind_int64(update.value, 2, command.exec_job_id);
    sqlite3_bind_int64(update.value, 3, command.workflow_instance_id);
    sqlite3_bind_int64(update.value, 4, command.workflow_step_id);
    if (sqlite3_step(update.value) != SQLITE_DONE || sqlite3_changes(db_) != 1) {
        if (error_out) *error_out = "battle recording job binding lost its queued precondition";
        return false;
    }
    return true;
}

bool SqliteAnalysisDb::BindBattleRecordingValidation(
    const BindBattleRecordingValidationCommand& command,
    std::string* error_out) {
    if (!db_ || command.battle_recording_id <= 0 ||
        command.tas_movie_tree_id <= 0 || command.validation_request_id <= 0) {
        if (error_out) *error_out = "invalid Battle Recording validation binding";
        return false;
    }
    Statement update;
    constexpr auto sql =
        "UPDATE ab_battle_recording SET validation_request_id=?3 WHERE "
        "battle_recording_id=?1 AND tas_movie_tree_id=?2 AND "
        "status='COMPLETED' AND outcome='RECORDED' AND "
        "(validation_request_id IS NULL OR validation_request_id=?3);";
    if (sqlite3_prepare_v2(db_, sql, -1, &update.value, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(update.value, 1, command.battle_recording_id);
    sqlite3_bind_int64(update.value, 2, command.tas_movie_tree_id);
    sqlite3_bind_int64(update.value, 3, command.validation_request_id);
    if (sqlite3_step(update.value) != SQLITE_DONE || sqlite3_changes(db_) != 1) {
        if (error_out) *error_out =
            "Battle Recording validation binding lost its exact tree precondition";
        return false;
    }
    return true;
}

bool SqliteAnalysisDb::BindBattleRecordingSterilization(
    const BindBattleRecordingSterilizationCommand& command,
    std::string* error_out) {
    if (!db_ || command.battle_recording_id <= 0 ||
        command.tas_movie_tree_id <= 0 || command.sterilization_request_id <= 0) {
        if (error_out) *error_out = "invalid Battle Recording sterilization binding";
        return false;
    }
    Statement update;
    constexpr auto sql =
        "UPDATE ab_battle_recording SET sterilization_request_id=?3 WHERE "
        "battle_recording_id=?1 AND tas_movie_tree_id=?2 AND "
        "validation_request_id IS NOT NULL AND status='COMPLETED' AND "
        "outcome='RECORDED' AND (sterilization_request_id IS NULL OR "
        "sterilization_request_id=?3);";
    if (sqlite3_prepare_v2(db_, sql, -1, &update.value, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(update.value, 1, command.battle_recording_id);
    sqlite3_bind_int64(update.value, 2, command.tas_movie_tree_id);
    sqlite3_bind_int64(update.value, 3, command.sterilization_request_id);
    if (sqlite3_step(update.value) != SQLITE_DONE || sqlite3_changes(db_) != 1) {
        if (error_out) *error_out =
            "Battle Recording sterilization binding lost its validated-tree precondition";
        return false;
    }
    return true;
}

bool SqliteAnalysisDb::CompleteBattleRecording(
    const CompleteBattleRecordingCommand& command, std::string* error_out) {
    const bool recorded = command.outcome == "RECORDED";
    const bool mismatch = command.outcome == "REPLAY_MISMATCH";
    if (!db_ || command.battle_recording_id <= 0 || (!recorded && !mismatch) ||
        command.worker_terminal_sha256.size() != 64 ||
        command.status != (recorded ? "COMPLETED" : "REPLAY_MISMATCH") ||
        (recorded && (!command.recorded_dtm_artifact_id ||
            !command.recorded_itinerary_artifact_id ||
            !command.paired_checkpoint_savestate_id ||
            !command.timing_anchor_version || !command.timing_anchor_blob ||
            !command.tas_movie_tree_id)) ||
        (mismatch && (command.recorded_dtm_artifact_id ||
            command.recorded_itinerary_artifact_id ||
            command.paired_checkpoint_savestate_id || command.timing_anchor_blob ||
            command.tas_movie_tree_id))) {
        if (error_out) *error_out = "invalid battle recording result command";
        return false;
    }
    if (!Begin(db_, error_out)) return false;
    const auto current = ReadRecording(db_, "battle_recording_id",
                                       command.battle_recording_id);
    if (!current) return Rollback(db_, "battle recording row not found", error_out);
    if (current->status != "QUEUED") {
        const bool exact = current->status == command.status
            && current->outcome == command.outcome
            && current->recorded_dtm_artifact_id == command.recorded_dtm_artifact_id
            && current->recorded_itinerary_artifact_id == command.recorded_itinerary_artifact_id
            && current->paired_checkpoint_savestate_id == command.paired_checkpoint_savestate_id
            && current->timing_anchor_version == command.timing_anchor_version
            && current->timing_anchor_blob == command.timing_anchor_blob
            && current->tas_movie_tree_id == command.tas_movie_tree_id
            && current->worker_terminal_sha256 == command.worker_terminal_sha256;
        if (!exact) return Rollback(db_,
            "battle recording already has a different durable result", error_out);
        return Commit(db_, error_out);
    }
    if (!current->exec_job_id)
        return Rollback(db_, "battle recording is not bound to a job", error_out);
    Statement update;
    constexpr auto sql =
        "UPDATE ab_battle_recording SET outcome=?2,recorded_dtm_artifact_id=?3,"
        "recorded_itinerary_artifact_id=?4,paired_checkpoint_savestate_id=?5,"
        "timing_anchor_version=?6,timing_anchor_blob=?7,tas_movie_tree_id=?8,"
        "validation_request_id=?9,sterilization_request_id=?10,"
        "worker_terminal_sha256=?11,status=?12,completed_at_utc=?13 "
        "WHERE battle_recording_id=?1 AND status='QUEUED';";
    if (sqlite3_prepare_v2(db_, sql, -1, &update.value, nullptr) != SQLITE_OK)
        return Rollback(db_, sqlite3_errmsg(db_), error_out);
    sqlite3_bind_int64(update.value, 1, command.battle_recording_id);
    sqlite3_bind_text(update.value, 2, command.outcome.c_str(), -1, SQLITE_TRANSIENT);
    BindOptionalI64(update.value, 3, command.recorded_dtm_artifact_id);
    BindOptionalI64(update.value, 4, command.recorded_itinerary_artifact_id);
    BindOptionalI64(update.value, 5, command.paired_checkpoint_savestate_id);
    BindOptionalInt(update.value, 6, command.timing_anchor_version);
    BindOptionalBlob(update.value, 7, command.timing_anchor_blob);
    BindOptionalI64(update.value, 8, command.tas_movie_tree_id);
    BindOptionalI64(update.value, 9, command.validation_request_id);
    BindOptionalI64(update.value, 10, command.sterilization_request_id);
    sqlite3_bind_text(update.value, 11,
        command.worker_terminal_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(update.value, 12, command.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(update.value, 13,
        command.completed_at_utc.time_since_epoch().count());
    if (sqlite3_step(update.value) != SQLITE_DONE || sqlite3_changes(db_) != 1)
        return Rollback(db_, "battle recording terminal transition failed", error_out);
    if (!InsertBattleEvent(db_,
            "AnalysisBattle.BattleRecordingCompleted.v1",
            "battle_recording", command.battle_recording_id,
            command.correlation_id, command.causation_id,
            command.completed_at_utc, "battle_recording",
            command.battle_recording_id, error_out))
        return Rollback(db_, error_out ? *error_out :
            "Battle Recording completed event failed", error_out);
    return Commit(db_, error_out);
}

bool SqliteAnalysisDb::FailBattleRecording(
    const FailBattleRecordingCommand& command, std::string* error_out) {
    if (!db_ || command.battle_recording_id <= 0 || command.error_code.empty()) {
        if (error_out) *error_out = "invalid battle recording failure command";
        return false;
    }
    if (!Begin(db_, error_out)) return false;
    const auto current = ReadRecording(
        db_, "battle_recording_id", command.battle_recording_id);
    if (!current)
        return Rollback(db_, "battle recording row not found", error_out);
    if (current->status == "FAILED") {
        const bool exact = current->error_code == command.error_code &&
            current->error_text == command.error_text &&
            current->worker_terminal_sha256 == command.worker_terminal_sha256;
        if (!exact) return Rollback(db_,
            "battle recording already has a different failure", error_out);
        return Commit(db_, error_out);
    }
    if (current->status != "QUEUED")
        return Rollback(db_,
            "battle recording is not queued for failure", error_out);
    Statement update;
    constexpr auto sql =
        "UPDATE ab_battle_recording SET error_code=?2,error_text=?3,"
        "worker_terminal_sha256=?4,status='FAILED',completed_at_utc=?5 "
        "WHERE battle_recording_id=?1 AND (status='QUEUED' OR "
        "(status='FAILED' AND error_code=?2));";
    if (sqlite3_prepare_v2(db_, sql, -1, &update.value, nullptr) != SQLITE_OK) {
        return Rollback(db_, sqlite3_errmsg(db_), error_out);
    }
    sqlite3_bind_int64(update.value, 1, command.battle_recording_id);
    sqlite3_bind_text(update.value, 2, command.error_code.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(update.value, 3, command.error_text.c_str(), -1, SQLITE_TRANSIENT);
    BindOptionalText(update.value, 4, command.worker_terminal_sha256);
    sqlite3_bind_int64(update.value, 5,
        command.completed_at_utc.time_since_epoch().count());
    if (sqlite3_step(update.value) != SQLITE_DONE || sqlite3_changes(db_) != 1) {
        return Rollback(db_,
            "battle recording failure transition failed", error_out);
    }
    if (!InsertBattleEvent(db_,
            "AnalysisBattle.BattleRecordingFailed.v1",
            "battle_recording", command.battle_recording_id,
            command.correlation_id, command.causation_id,
            command.completed_at_utc, "battle_recording",
            command.battle_recording_id, error_out))
        return Rollback(db_, error_out ? *error_out :
            "Battle Recording failed event failed", error_out);
    return Commit(db_, error_out);
}

std::optional<BattleRecordingRecord> SqliteAnalysisDb::GetBattleRecording(
    std::int64_t id) const {
    return db_ && id > 0 ? ReadRecording(db_, "battle_recording_id", id)
                         : std::nullopt;
}

std::optional<BattleRecordingRecord>
SqliteAnalysisDb::GetBattleRecordingForExecJob(std::int64_t id) const {
    return db_ && id > 0 ? ReadRecording(db_, "exec_job_id", id)
                         : std::nullopt;
}

std::optional<BattleRecordingRecord>
SqliteAnalysisDb::GetBattleRecordingForTasMovieTree(std::int64_t id) const {
    return db_ && id > 0 ? ReadRecording(db_, "tas_movie_tree_id", id)
                         : std::nullopt;
}

std::optional<BattleRecordingRecord>
SqliteAnalysisDb::GetBattleRecordingForPairedCheckpoint(std::int64_t id) const {
    return db_ && id > 0
        ? ReadRecording(db_, "paired_checkpoint_savestate_id", id)
        : std::nullopt;
}

bool SqliteAnalysisDb::CreateBattleReplay(
    const CreateBattleReplayCommand& command,
    std::int64_t* id_out, std::string* error_out) {
    if (id_out) *id_out = 0;
    if (!db_ || command.battle_completion_id <= 0 ||
        command.workflow_instance_id <= 0 || command.workflow_step_id <= 0 ||
        command.source_savestate_id <= 0 ||
        (command.source_dtm_artifact_id && *command.source_dtm_artifact_id <= 0) ||
        (command.source_itinerary_artifact_id &&
            *command.source_itinerary_artifact_id <= 0) ||
        command.source_binding_version <= 0 ||
        command.source_binding_blob.empty() ||
        command.source_binding_sha256.size() != 64 ||
        command.replay_plan_version <= 0 || command.replay_plan_blob.empty() ||
        command.replay_plan_sha256.size() != 64 || command.status != "QUEUED") {
        if (error_out) *error_out = "invalid battle replay create command";
        return false;
    }
    if (!Begin(db_, error_out)) return false;
    const auto existing = ReadReplay(db_, "workflow_step_id",
                                     command.workflow_step_id);
    if (existing) {
        const bool exact =
            existing->battle_completion_id == command.battle_completion_id &&
            existing->workflow_instance_id == command.workflow_instance_id &&
            existing->source_savestate_id == command.source_savestate_id &&
            existing->source_dtm_artifact_id == command.source_dtm_artifact_id &&
            existing->source_itinerary_artifact_id ==
                command.source_itinerary_artifact_id &&
            existing->source_binding_version == command.source_binding_version &&
            existing->source_binding_blob == command.source_binding_blob &&
            existing->source_binding_sha256 == command.source_binding_sha256 &&
            existing->replay_plan_version == command.replay_plan_version &&
            existing->replay_plan_blob == command.replay_plan_blob &&
            existing->replay_plan_sha256 == command.replay_plan_sha256;
        if (!exact) return Rollback(db_,
            "battle replay workflow step has different immutable plan", error_out);
        if (!Commit(db_, error_out)) return false;
        if (id_out) *id_out = existing->battle_replay_id;
        return true;
    }
    Statement insert;
    constexpr auto sql =
        "INSERT INTO ab_battle_replay(battle_completion_id,workflow_instance_id,"
        "workflow_step_id,exec_job_id,source_savestate_id,source_dtm_artifact_id,"
        "source_itinerary_artifact_id,source_binding_version,source_binding_blob,"
        "source_binding_sha256,replay_plan_version,replay_plan_blob,"
        "replay_plan_sha256,status,created_at_utc) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,'QUEUED',?14);";
    if (sqlite3_prepare_v2(db_, sql, -1, &insert.value, nullptr) != SQLITE_OK)
        return Rollback(db_, sqlite3_errmsg(db_), error_out);
    sqlite3_bind_int64(insert.value, 1, command.battle_completion_id);
    sqlite3_bind_int64(insert.value, 2, command.workflow_instance_id);
    sqlite3_bind_int64(insert.value, 3, command.workflow_step_id);
    BindOptionalI64(insert.value, 4, command.exec_job_id);
    sqlite3_bind_int64(insert.value, 5, command.source_savestate_id);
    BindOptionalI64(insert.value, 6, command.source_dtm_artifact_id);
    BindOptionalI64(insert.value, 7, command.source_itinerary_artifact_id);
    sqlite3_bind_int(insert.value, 8, command.source_binding_version);
    sqlite3_bind_blob(insert.value, 9, command.source_binding_blob.data(),
        static_cast<int>(command.source_binding_blob.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(insert.value, 10,
        command.source_binding_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(insert.value, 11, command.replay_plan_version);
    sqlite3_bind_blob(insert.value, 12, command.replay_plan_blob.data(),
        static_cast<int>(command.replay_plan_blob.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(insert.value, 13,
        command.replay_plan_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert.value, 14,
        command.created_at_utc.time_since_epoch().count());
    if (sqlite3_step(insert.value) != SQLITE_DONE)
        return Rollback(db_, sqlite3_errmsg(db_), error_out);
    const auto id = sqlite3_last_insert_rowid(db_);
    if (!InsertBattleEvent(db_, "AnalysisBattle.BattleReplayCreated.v1",
            "battle_replay", id, command.correlation_id, command.causation_id,
            command.created_at_utc, "battle_replay", id, error_out))
        return Rollback(db_, error_out ? *error_out :
            "Battle Replay created event failed", error_out);
    if (!Commit(db_, error_out)) return false;
    if (id_out) *id_out = id;
    return true;
}

bool SqliteAnalysisDb::BindBattleReplayExecutionJob(
    const BindBattleReplayExecutionJobCommand& command,
    std::string* error_out) {
    if (!db_ || command.battle_replay_id <= 0 || command.exec_job_id <= 0 ||
        command.workflow_instance_id <= 0 || command.workflow_step_id <= 0) {
        if (error_out) *error_out = "invalid battle replay job binding";
        return false;
    }
    Statement update;
    constexpr auto sql =
        "UPDATE ab_battle_replay SET exec_job_id=?2 WHERE battle_replay_id=?1 "
        "AND workflow_instance_id=?3 AND workflow_step_id=?4 AND "
        "status='QUEUED' AND (exec_job_id IS NULL OR exec_job_id=?2);";
    if (sqlite3_prepare_v2(db_, sql, -1, &update.value, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(update.value, 1, command.battle_replay_id);
    sqlite3_bind_int64(update.value, 2, command.exec_job_id);
    sqlite3_bind_int64(update.value, 3, command.workflow_instance_id);
    sqlite3_bind_int64(update.value, 4, command.workflow_step_id);
    if (sqlite3_step(update.value) != SQLITE_DONE || sqlite3_changes(db_) != 1) {
        if (error_out) *error_out =
            "battle replay job binding lost its queued precondition";
        return false;
    }
    return true;
}

bool SqliteAnalysisDb::CompleteBattleReplay(
    const CompleteBattleReplayCommand& command, std::string* error_out) {
    const bool matched = command.outcome == "MATCHED";
    const bool mismatch = command.outcome == "REPLAY_MISMATCH";
    const bool evidence_pair =
        command.observed_completion_blob.has_value() ==
            command.observed_completion_sha256.has_value() &&
        command.observed_transition_blob.has_value() ==
            command.observed_transition_sha256.has_value() &&
        command.observed_completion_blob.has_value() ==
            command.observed_transition_blob.has_value();
    if (!db_ || command.battle_replay_id <= 0 || (!matched && !mismatch) ||
        command.worker_terminal_sha256.size() != 64 || !evidence_pair ||
        command.status != (matched ? "MATCHED" : "REPLAY_MISMATCH") ||
        (matched && (command.mismatch_turn != 0 || command.expected_rng != 0 ||
            command.observed_rng != 0 || !command.observed_completion_blob)) ||
        (mismatch && command.mismatch_turn == 0) ||
        (command.observed_completion_sha256 &&
            command.observed_completion_sha256->size() != 64) ||
        (command.observed_transition_sha256 &&
            command.observed_transition_sha256->size() != 64)) {
        if (error_out) *error_out = "invalid battle replay result command";
        return false;
    }
    if (!Begin(db_, error_out)) return false;
    const auto current = ReadReplay(db_, "battle_replay_id",
                                    command.battle_replay_id);
    if (!current) return Rollback(db_, "battle replay row not found", error_out);
    if (current->status != "QUEUED") {
        const bool exact = current->status == command.status &&
            current->outcome == command.outcome &&
            current->mismatch_turn == command.mismatch_turn &&
            current->expected_rng == command.expected_rng &&
            current->observed_rng == command.observed_rng &&
            current->observed_completion_blob == command.observed_completion_blob &&
            current->observed_completion_sha256 == command.observed_completion_sha256 &&
            current->observed_transition_blob == command.observed_transition_blob &&
            current->observed_transition_sha256 == command.observed_transition_sha256 &&
            current->worker_terminal_sha256 == command.worker_terminal_sha256;
        if (!exact) return Rollback(db_,
            "battle replay already has a different durable result", error_out);
        return Commit(db_, error_out);
    }
    if (!current->exec_job_id)
        return Rollback(db_, "battle replay is not bound to a job", error_out);
    Statement update;
    constexpr auto sql =
        "UPDATE ab_battle_replay SET outcome=?2,mismatch_turn=?3,"
        "expected_rng=?4,observed_rng=?5,observed_completion_blob=?6,"
        "observed_completion_sha256=?7,observed_transition_blob=?8,"
        "observed_transition_sha256=?9,worker_terminal_sha256=?10,status=?11,"
        "completed_at_utc=?12 WHERE battle_replay_id=?1 AND status='QUEUED';";
    if (sqlite3_prepare_v2(db_, sql, -1, &update.value, nullptr) != SQLITE_OK)
        return Rollback(db_, sqlite3_errmsg(db_), error_out);
    sqlite3_bind_int64(update.value, 1, command.battle_replay_id);
    sqlite3_bind_text(update.value, 2, command.outcome.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(update.value, 3, command.mismatch_turn);
    sqlite3_bind_int64(update.value, 4, command.expected_rng);
    sqlite3_bind_int64(update.value, 5, command.observed_rng);
    BindOptionalBlob(update.value, 6, command.observed_completion_blob);
    BindOptionalText(update.value, 7, command.observed_completion_sha256);
    BindOptionalBlob(update.value, 8, command.observed_transition_blob);
    BindOptionalText(update.value, 9, command.observed_transition_sha256);
    sqlite3_bind_text(update.value, 10,
        command.worker_terminal_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(update.value, 11, command.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(update.value, 12,
        command.completed_at_utc.time_since_epoch().count());
    if (sqlite3_step(update.value) != SQLITE_DONE || sqlite3_changes(db_) != 1)
        return Rollback(db_, "battle replay terminal transition failed", error_out);
    if (!InsertBattleEvent(db_, "AnalysisBattle.BattleReplayCompleted.v1",
            "battle_replay", command.battle_replay_id, command.correlation_id,
            command.causation_id, command.completed_at_utc, "battle_replay",
            command.battle_replay_id, error_out))
        return Rollback(db_, error_out ? *error_out :
            "Battle Replay completed event failed", error_out);
    return Commit(db_, error_out);
}

bool SqliteAnalysisDb::FailBattleReplay(
    const FailBattleReplayCommand& command, std::string* error_out) {
    if (!db_ || command.battle_replay_id <= 0 || command.error_code.empty()) {
        if (error_out) *error_out = "invalid battle replay failure command";
        return false;
    }
    if (!Begin(db_, error_out)) return false;
    const auto current = ReadReplay(db_, "battle_replay_id",
                                    command.battle_replay_id);
    if (!current) return Rollback(db_, "battle replay row not found", error_out);
    if (current->status == "FAILED") {
        const bool exact = current->error_code == command.error_code &&
            current->error_text == command.error_text &&
            current->worker_terminal_sha256 == command.worker_terminal_sha256;
        if (!exact) return Rollback(db_,
            "battle replay already has a different failure", error_out);
        return Commit(db_, error_out);
    }
    if (current->status != "QUEUED")
        return Rollback(db_, "battle replay is not queued for failure", error_out);
    Statement update;
    constexpr auto sql =
        "UPDATE ab_battle_replay SET error_code=?2,error_text=?3,"
        "worker_terminal_sha256=?4,status='FAILED',completed_at_utc=?5 "
        "WHERE battle_replay_id=?1 AND status='QUEUED';";
    if (sqlite3_prepare_v2(db_, sql, -1, &update.value, nullptr) != SQLITE_OK)
        return Rollback(db_, sqlite3_errmsg(db_), error_out);
    sqlite3_bind_int64(update.value, 1, command.battle_replay_id);
    sqlite3_bind_text(update.value, 2, command.error_code.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(update.value, 3, command.error_text.c_str(), -1, SQLITE_TRANSIENT);
    BindOptionalText(update.value, 4, command.worker_terminal_sha256);
    sqlite3_bind_int64(update.value, 5,
        command.completed_at_utc.time_since_epoch().count());
    if (sqlite3_step(update.value) != SQLITE_DONE || sqlite3_changes(db_) != 1)
        return Rollback(db_, "battle replay failure transition failed", error_out);
    if (!InsertBattleEvent(db_, "AnalysisBattle.BattleReplayFailed.v1",
            "battle_replay", command.battle_replay_id, command.correlation_id,
            command.causation_id, command.completed_at_utc, "battle_replay",
            command.battle_replay_id, error_out))
        return Rollback(db_, error_out ? *error_out :
            "Battle Replay failed event failed", error_out);
    return Commit(db_, error_out);
}

std::optional<BattleReplayRecord> SqliteAnalysisDb::GetBattleReplay(
    std::int64_t id) const {
    return db_ && id > 0 ? ReadReplay(db_, "battle_replay_id", id)
                         : std::nullopt;
}

std::optional<BattleReplayRecord> SqliteAnalysisDb::GetBattleReplayForExecJob(
    std::int64_t id) const {
    return db_ && id > 0 ? ReadReplay(db_, "exec_job_id", id)
                         : std::nullopt;
}

} // namespace savor::db::analysis
