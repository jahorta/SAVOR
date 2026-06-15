#include "SqliteUiReadDb.h"

#include <chrono>

namespace savor::db {

namespace {

std::int64_t ToEpochMillis(types::UtcTimePoint value) {
    return value.time_since_epoch().count();
}

types::UtcTimePoint FromEpochMillis(std::int64_t value) {
    return types::UtcTimePoint{ std::chrono::milliseconds(value) };
}

std::string ColumnText(sqlite3_stmt* st, int column) {
    const auto* text = sqlite3_column_text(st, column);
    return text == nullptr ? std::string{} : reinterpret_cast<const char*>(text);
}

std::optional<std::string> ColumnTextOptional(sqlite3_stmt* st, int column) {
    if (sqlite3_column_type(st, column) == SQLITE_NULL) {
        return std::nullopt;
    }
    return ColumnText(st, column);
}

std::optional<std::int64_t> ColumnInt64Optional(sqlite3_stmt* st, int column) {
    if (sqlite3_column_type(st, column) == SQLITE_NULL) {
        return std::nullopt;
    }
    return sqlite3_column_int64(st, column);
}

bool IsSubscriptionKeyValid(
    const std::string& projector_name,
    const std::string& source_context,
    const std::string& source_outbox_table) {
    return !projector_name.empty() && !source_context.empty() && !source_outbox_table.empty();
}

bool ExecSql(sqlite3* db, const char* sql) {
    char* err_msg = nullptr;
    const auto rc = sqlite3_exec(db, sql, nullptr, nullptr, &err_msg);
    sqlite3_free(err_msg);
    return rc == SQLITE_OK;
}

std::optional<UiProjectionSubscription> ReadSubscription(
    sqlite3* db,
    const std::string& projector_name,
    const std::string& source_context,
    const std::string& source_outbox_table) {
    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT projector_name, source_context, source_outbox_table, "
        "COALESCE(last_outbox_id, 0), COALESCE(last_event_id, ''), updated_at_utc, "
        "COALESCE(status, 'ACTIVE'), COALESCE(last_error, '') "
        "FROM ui_projection_subscription "
        "WHERE projector_name=?1 AND source_context=?2 AND source_outbox_table=?3;";

    if (sqlite3_prepare_v2(db, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_text(st, 1, projector_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);

    std::optional<UiProjectionSubscription> subscription;
    if (sqlite3_step(st) == SQLITE_ROW) {
        subscription = UiProjectionSubscription{};
        const auto* projector_name_text = sqlite3_column_text(st, 0);
        const auto* source_context_text = sqlite3_column_text(st, 1);
        const auto* source_outbox_table_text = sqlite3_column_text(st, 2);
        const auto* last_event_id_text = sqlite3_column_text(st, 4);
        const auto* status_text = sqlite3_column_text(st, 6);
        const auto* last_error_text = sqlite3_column_text(st, 7);

        subscription->projector_name = projector_name_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(projector_name_text);
        subscription->source_context = source_context_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(source_context_text);
        subscription->source_outbox_table = source_outbox_table_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(source_outbox_table_text);
        subscription->last_outbox_id = sqlite3_column_int64(st, 3);
        subscription->last_event_id = last_event_id_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(last_event_id_text);
        subscription->updated_at_utc = FromEpochMillis(sqlite3_column_int64(st, 5));
        subscription->status = status_text == nullptr
            ? std::string{ "ACTIVE" }
            : reinterpret_cast<const char*>(status_text);
        subscription->last_error = last_error_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(last_error_text);
    }

    sqlite3_finalize(st);
    return subscription;
}

std::optional<UiSeedProbeRunSummary> ReadSeedProbeSummaryRow(sqlite3_stmt* st) {
    UiSeedProbeRunSummary summary{};
    summary.probe_run_id = sqlite3_column_int64(st, 0);
    summary.probe_set_id = sqlite3_column_int64(st, 1);
    summary.entry_savestate_id = sqlite3_column_int64(st, 2);
    summary.seed_probe_spec_id = sqlite3_column_int64(st, 3);
    summary.codec_version = sqlite3_column_int(st, 4);
    const auto* status_text = sqlite3_column_text(st, 5);
    summary.status = status_text == nullptr ? std::string{} : reinterpret_cast<const char*>(status_text);
    if (sqlite3_column_type(st, 6) != SQLITE_NULL) {
        summary.neutral_seed_value = sqlite3_column_int64(st, 6);
    }
    summary.grid_count = sqlite3_column_int(st, 7);
    summary.unique_count = sqlite3_column_int(st, 8);
    summary.requested_at_utc = sqlite3_column_int64(st, 9);
    if (sqlite3_column_type(st, 10) != SQLITE_NULL) {
        summary.completed_at_utc = sqlite3_column_int64(st, 10);
    }
    return summary;
}

UiJobSummary ReadJobSummaryRow(sqlite3_stmt* st) {
    UiJobSummary row{};
    row.job_id = sqlite3_column_int64(st, 0);
    row.job_set_id = sqlite3_column_int64(st, 1);
    row.program_kind = sqlite3_column_int(st, 2);
    const auto* state_text = sqlite3_column_text(st, 3);
    row.state = state_text == nullptr ? std::string{} : reinterpret_cast<const char*>(state_text);
    row.priority = sqlite3_column_int(st, 4);
    row.queued_at_utc = sqlite3_column_int64(st, 5);
    if (sqlite3_column_type(st, 6) != SQLITE_NULL) {
        row.started_at_utc = sqlite3_column_int64(st, 6);
    }
    if (sqlite3_column_type(st, 7) != SQLITE_NULL) {
        row.ended_at_utc = sqlite3_column_int64(st, 7);
    }
    const auto* error_code_text = sqlite3_column_text(st, 8);
    row.error_code = error_code_text == nullptr ? std::string{} : reinterpret_cast<const char*>(error_code_text);
    row.attempts = sqlite3_column_int(st, 9);
    row.max_attempts = sqlite3_column_int(st, 10);
    const auto* error_text = sqlite3_column_text(st, 11);
    row.error_text = error_text == nullptr ? std::string{} : reinterpret_cast<const char*>(error_text);
    return row;
}

UiJobDetail ReadJobDetailRow(sqlite3_stmt* st) {
    UiJobDetail row{};
    row.summary = ReadJobSummaryRow(st);
    row.fingerprint = ColumnText(st, 12);
    row.claimed_by_token = ColumnTextOptional(st, 13);
    if (sqlite3_column_type(st, 14) != SQLITE_NULL) {
        row.lease_expires_at_utc = sqlite3_column_int64(st, 14);
    }
    return row;
}

void AddJobStateCount(UiJobStateCounts& counts, const std::string& state, std::int64_t count) {
    counts.total += count;
    if (state == "QUEUED") {
        counts.queued += count;
    } else if (state == "CLAIMED") {
        counts.claimed += count;
    } else if (state == "RUNNING") {
        counts.running += count;
    } else if (state == "FAILED") {
        counts.failed += count;
    } else if (state == "CANCELED") {
        counts.canceled += count;
    } else if (state == "SUPERSEDED") {
        counts.superseded += count;
    } else if (state == "SUCCEEDED"
        || state == "SUCCEEDED_WINNER"
        || state == "SUCCEEDED_DUPLICATE") {
        counts.succeeded += count;
    } else {
        counts.other += count;
    }
}

UiJobSetSummary ReadJobSetSummaryRow(sqlite3_stmt* st) {
    UiJobSetSummary row{};
    row.job_set_id = sqlite3_column_int64(st, 0);
    row.program_kind = sqlite3_column_int(st, 1);
    row.created_at_utc = sqlite3_column_int64(st, 2);
    row.total_jobs = sqlite3_column_int64(st, 3);
    row.completed_jobs = sqlite3_column_int64(st, 4);
    row.succeeded_jobs = sqlite3_column_int64(st, 5);
    row.failed_jobs = sqlite3_column_int64(st, 6);
    row.canceled_jobs = sqlite3_column_int64(st, 7);
    return row;
}

UiWorkflowInstanceSummary ReadWorkflowInstanceRow(sqlite3_stmt* st) {
    UiWorkflowInstanceSummary row{};
    row.workflow_instance_id = sqlite3_column_int64(st, 0);
    row.workflow_kind = ColumnText(st, 1);
    row.state = ColumnText(st, 2);
    row.root_scope_kind = ColumnText(st, 3);
    row.root_scope_id = ColumnInt64Optional(st, 4);
    row.created_by = ColumnText(st, 5);
    row.blocked_step_count = sqlite3_column_int64(st, 6);
    row.failed_step_count = sqlite3_column_int64(st, 7);
    row.created_at_utc = sqlite3_column_int64(st, 8);
    row.started_at_utc = ColumnInt64Optional(st, 9);
    row.completed_at_utc = ColumnInt64Optional(st, 10);
    row.failure_code = ColumnText(st, 11);
    row.failure_text = ColumnText(st, 12);
    return row;
}

UiWorkflowStepSummary ReadWorkflowStepRow(sqlite3_stmt* st) {
    UiWorkflowStepSummary row{};
    row.workflow_step_id = sqlite3_column_int64(st, 0);
    row.workflow_instance_id = sqlite3_column_int64(st, 1);
    row.workflow_unit_activation_id = ColumnInt64Optional(st, 2);
    row.step_key = ColumnText(st, 3);
    row.step_kind = ColumnText(st, 4);
    row.state = ColumnText(st, 5);
    row.blocked_reason = ColumnText(st, 6);
    row.job_set_id = ColumnInt64Optional(st, 7);
    row.job_count = sqlite3_column_int64(st, 8);
    row.job_completed_count = sqlite3_column_int64(st, 9);
    row.job_failed_count = sqlite3_column_int64(st, 10);
    row.priority = sqlite3_column_int(st, 11);
    row.attempts = sqlite3_column_int(st, 12);
    row.max_attempts = sqlite3_column_int(st, 13);
    row.ready_at_utc = ColumnInt64Optional(st, 14);
    row.started_at_utc = ColumnInt64Optional(st, 15);
    row.completed_at_utc = ColumnInt64Optional(st, 16);
    row.failed_at_utc = ColumnInt64Optional(st, 17);
    row.created_at_utc = sqlite3_column_int64(st, 18);
    return row;
}

UiWorkflowUnitActivationSummary ReadWorkflowUnitActivationRow(sqlite3_stmt* st) {
    UiWorkflowUnitActivationSummary row{};
    row.workflow_unit_activation_id = sqlite3_column_int64(st, 0);
    row.workflow_instance_id = sqlite3_column_int64(st, 1);
    row.parent_workflow_unit_activation_id = ColumnInt64Optional(st, 2);
    row.activation_key = ColumnText(st, 3);
    row.graph_node_key = ColumnText(st, 4);
    row.unit_kind = ColumnText(st, 5);
    row.display_name = ColumnText(st, 6);
    row.state = ColumnText(st, 7);
    row.activation_params_json = ColumnText(st, 8);
    row.authored_ref_kind = ColumnText(st, 9);
    row.authored_ref_id = ColumnInt64Optional(st, 10);
    row.failure_code = ColumnText(st, 11);
    row.failure_text = ColumnText(st, 12);
    row.created_at_utc = sqlite3_column_int64(st, 13);
    row.ready_at_utc = ColumnInt64Optional(st, 14);
    row.started_at_utc = ColumnInt64Optional(st, 15);
    row.completed_at_utc = ColumnInt64Optional(st, 16);
    row.failed_at_utc = ColumnInt64Optional(st, 17);
    return row;
}

UiWorkflowEdgeSummary ReadWorkflowEdgeRow(sqlite3_stmt* st) {
    UiWorkflowEdgeSummary row{};
    row.workflow_edge_id = sqlite3_column_int64(st, 0);
    row.workflow_instance_id = sqlite3_column_int64(st, 1);
    row.from_step_id = sqlite3_column_int64(st, 2);
    row.to_step_id = sqlite3_column_int64(st, 3);
    row.condition_kind = ColumnText(st, 4);
    row.condition_value = ColumnText(st, 5);
    row.created_at_utc = sqlite3_column_int64(st, 6);
    return row;
}

UiWorkflowUnitActivationEdgeSummary ReadWorkflowUnitActivationEdgeRow(sqlite3_stmt* st) {
    UiWorkflowUnitActivationEdgeSummary row{};
    row.workflow_unit_activation_edge_id = sqlite3_column_int64(st, 0);
    row.workflow_instance_id = sqlite3_column_int64(st, 1);
    row.from_workflow_unit_activation_id = sqlite3_column_int64(st, 2);
    row.to_workflow_unit_activation_id = sqlite3_column_int64(st, 3);
    row.output_key = ColumnText(st, 4);
    row.input_key = ColumnText(st, 5);
    row.condition_kind = ColumnText(st, 6);
    row.condition_value = ColumnText(st, 7);
    row.created_at_utc = sqlite3_column_int64(st, 8);
    return row;
}

UiWorkflowAlertSummary ReadWorkflowAlertRow(sqlite3_stmt* st) {
    UiWorkflowAlertSummary row{};
    row.workflow_alert_id = sqlite3_column_int64(st, 0);
    row.workflow_instance_id = sqlite3_column_int64(st, 1);
    row.workflow_step_id = ColumnInt64Optional(st, 2);
    row.alert_kind = ColumnText(st, 3);
    row.alert_code = ColumnText(st, 4);
    row.message = ColumnText(st, 5);
    row.is_active = sqlite3_column_int(st, 6) != 0;
    row.first_seen_at_utc = sqlite3_column_int64(st, 7);
    row.last_seen_at_utc = sqlite3_column_int64(st, 8);
    row.cleared_at_utc = ColumnInt64Optional(st, 9);
    return row;
}

} // namespace

SqliteUiReadDb::SqliteUiReadDb(sqlite3* db)
    : db_(db) {
}

std::vector<UiProgramKind> SqliteUiReadDb::ListProgramKinds() const {
    std::vector<UiProgramKind> rows;
    if (db_ == nullptr) {
        return rows;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT DISTINCT program_kind FROM ui_job_summary ORDER BY program_kind ASC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return rows;
    }

    while (sqlite3_step(st) == SQLITE_ROW) {
        UiProgramKind row{};
        row.id = sqlite3_column_int(st, 0);
        row.name = "kind " + std::to_string(row.id);
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(st);
    return rows;
}

UiReadPage<UiJobSummary> SqliteUiReadDb::ListJobs(
    const UiReadJobListQuery& query) const {
    UiReadPage<UiJobSummary> page{};
    if (db_ == nullptr || query.limit <= 0) {
        return page;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT s.job_id,s.job_set_id,s.program_kind,s.state,s.priority,s.queued_at_utc,"
        "s.started_at_utc,s.ended_at_utc,COALESCE(s.error_code,''),"
        "COALESCE(d.attempts,0),COALESCE(d.max_attempts,0),COALESCE(d.error_text,'') "
        "FROM ui_job_summary s LEFT JOIN ui_job_detail d ON d.job_id=s.job_id "
        "WHERE (?1=0 OR s.program_kind=?2) "
        "AND (?3=0 OR s.job_set_id=?4) "
        "AND (?5=1 OR s.state IN (?6,?7,?8,?9,?10)) "
        "AND (?11=0 OR s.queued_at_utc < ?12 OR (s.queued_at_utc=?12 AND s.job_id < ?13)) "
        "AND (?14=0 OR s.queued_at_utc > ?15 OR (s.queued_at_utc=?15 AND s.job_id > ?16)) "
        "ORDER BY s.queued_at_utc DESC, s.job_id DESC LIMIT ?17;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return page;
    }

    sqlite3_bind_int(st, 1, query.program_kind.has_value() ? 1 : 0);
    sqlite3_bind_int(st, 2, query.program_kind.value_or(0));
    sqlite3_bind_int(st, 3, query.job_set_id.has_value() ? 1 : 0);
    sqlite3_bind_int64(st, 4, query.job_set_id.value_or(0));
    sqlite3_bind_int(st, 5, query.states.empty() ? 1 : 0);
    for (int i = 0; i < 5; ++i) {
        const std::string value = i < static_cast<int>(query.states.size()) ? query.states[static_cast<std::size_t>(i)] : std::string{};
        sqlite3_bind_text(st, 6 + i, value.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(st, 11, query.before.has_value() ? 1 : 0);
    sqlite3_bind_int64(st, 12, query.before.value_or(UiReadListCursor{}).primary);
    sqlite3_bind_int64(st, 13, query.before.value_or(UiReadListCursor{}).secondary);
    sqlite3_bind_int(st, 14, query.after.has_value() ? 1 : 0);
    sqlite3_bind_int64(st, 15, query.after.value_or(UiReadListCursor{}).primary);
    sqlite3_bind_int64(st, 16, query.after.value_or(UiReadListCursor{}).secondary);
    sqlite3_bind_int(st, 17, query.limit);

    while (sqlite3_step(st) == SQLITE_ROW) {
        page.items.push_back(ReadJobSummaryRow(st));
    }
    sqlite3_finalize(st);

    if (!page.items.empty()) {
        const auto& first = page.items.front();
        const auto& last = page.items.back();
        page.prev = UiReadListCursor{ first.queued_at_utc, first.job_id };
        page.next = UiReadListCursor{ last.queued_at_utc, last.job_id };
    }
    return page;
}

UiJobStateCounts SqliteUiReadDb::CountJobsByState(
    const UiReadJobListQuery& query) const {
    UiJobStateCounts counts{};
    if (db_ == nullptr) {
        return counts;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT s.state, COUNT(*) "
        "FROM ui_job_summary s "
        "WHERE (?1=0 OR s.program_kind=?2) "
        "AND (?3=0 OR s.job_set_id=?4) "
        "AND (?5=1 OR s.state IN (?6,?7,?8,?9,?10)) "
        "GROUP BY s.state;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return counts;
    }

    sqlite3_bind_int(st, 1, query.program_kind.has_value() ? 1 : 0);
    sqlite3_bind_int(st, 2, query.program_kind.value_or(0));
    sqlite3_bind_int(st, 3, query.job_set_id.has_value() ? 1 : 0);
    sqlite3_bind_int64(st, 4, query.job_set_id.value_or(0));
    sqlite3_bind_int(st, 5, query.states.empty() ? 1 : 0);
    for (int i = 0; i < 5; ++i) {
        const std::string value = i < static_cast<int>(query.states.size()) ? query.states[static_cast<std::size_t>(i)] : std::string{};
        sqlite3_bind_text(st, 6 + i, value.c_str(), -1, SQLITE_TRANSIENT);
    }

    while (sqlite3_step(st) == SQLITE_ROW) {
        AddJobStateCount(counts, ColumnText(st, 0), sqlite3_column_int64(st, 1));
    }
    sqlite3_finalize(st);
    return counts;
}

std::optional<UiJobSummary> SqliteUiReadDb::GetJobSummary(std::int64_t job_id) const {
    if (db_ == nullptr || job_id <= 0) {
        return std::nullopt;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT s.job_id,s.job_set_id,s.program_kind,s.state,s.priority,s.queued_at_utc,"
        "s.started_at_utc,s.ended_at_utc,COALESCE(s.error_code,''),"
        "COALESCE(d.attempts,0),COALESCE(d.max_attempts,0),COALESCE(d.error_text,'') "
        "FROM ui_job_summary s LEFT JOIN ui_job_detail d ON d.job_id=s.job_id "
        "WHERE s.job_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st, 1, job_id);
    std::optional<UiJobSummary> row;
    if (sqlite3_step(st) == SQLITE_ROW) {
        row = ReadJobSummaryRow(st);
    }
    sqlite3_finalize(st);
    return row;
}

std::optional<UiJobDetail> SqliteUiReadDb::GetJobDetail(std::int64_t job_id) const {
    if (db_ == nullptr || job_id <= 0) {
        return std::nullopt;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT s.job_id,s.job_set_id,s.program_kind,s.state,s.priority,s.queued_at_utc,"
        "s.started_at_utc,s.ended_at_utc,COALESCE(s.error_code,''),"
        "COALESCE(d.attempts,0),COALESCE(d.max_attempts,0),COALESCE(d.error_text,''),"
        "COALESCE(d.fingerprint,''),d.claimed_by_token,d.lease_expires_at_utc "
        "FROM ui_job_summary s LEFT JOIN ui_job_detail d ON d.job_id=s.job_id "
        "WHERE s.job_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st, 1, job_id);
    std::optional<UiJobDetail> row;
    if (sqlite3_step(st) == SQLITE_ROW) {
        row = ReadJobDetailRow(st);
    }
    sqlite3_finalize(st);
    return row;
}

std::vector<UiJobArtifact> SqliteUiReadDb::ListJobArtifacts(std::int64_t job_id) const {
    std::vector<UiJobArtifact> rows;
    if (db_ == nullptr || job_id <= 0) {
        return rows;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT a.artifact_id,a.role_kind,COALESCE(b.filename,''),COALESCE(b.size_bytes,0),"
        "COALESCE(b.artifact_kind,''),COALESCE(a.created_at_utc,0) "
        "FROM ui_job_artifact a LEFT JOIN ui_artifact_browser b ON b.artifact_id=a.artifact_id "
        "WHERE a.job_id=?1 ORDER BY a.created_at_utc ASC, a.ui_job_artifact_id ASC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_int64(st, 1, job_id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        UiJobArtifact row{};
        row.artifact_id = sqlite3_column_int64(st, 0);
        const auto* role_text = sqlite3_column_text(st, 1);
        row.role_kind = role_text == nullptr ? std::string{} : reinterpret_cast<const char*>(role_text);
        const auto* filename_text = sqlite3_column_text(st, 2);
        row.filename = filename_text == nullptr ? std::string{} : reinterpret_cast<const char*>(filename_text);
        row.size_bytes = static_cast<std::uint64_t>(sqlite3_column_int64(st, 3));
        const auto* kind_text = sqlite3_column_text(st, 4);
        row.artifact_kind = kind_text == nullptr ? std::string{} : reinterpret_cast<const char*>(kind_text);
        row.created_at_utc = sqlite3_column_int64(st, 5);
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(st);
    return rows;
}

UiReadPage<UiJobSetSummary> SqliteUiReadDb::ListJobSets(
    const UiReadJobSetListQuery& query) const {
    UiReadPage<UiJobSetSummary> page{};
    if (db_ == nullptr || query.limit <= 0) {
        return page;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT job_set_id,MIN(program_kind),MIN(queued_at_utc),COUNT(*),"
        "SUM(CASE WHEN state IN ('COMPLETED','DONE','SUCCEEDED') THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN state='SUCCEEDED' THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN state='FAILED' THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN state='CANCELED' THEN 1 ELSE 0 END) "
        "FROM ui_job_summary "
        "WHERE (?1=0 OR program_kind=?2) "
        "GROUP BY job_set_id "
        "HAVING (?3=0 OR MIN(queued_at_utc) < ?4 OR (MIN(queued_at_utc)=?4 AND job_set_id < ?5)) "
        "AND (?6=0 OR MIN(queued_at_utc) > ?7 OR (MIN(queued_at_utc)=?7 AND job_set_id > ?8)) "
        "ORDER BY MIN(queued_at_utc) DESC, job_set_id DESC LIMIT ?9;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return page;
    }

    sqlite3_bind_int(st, 1, query.program_kind.has_value() ? 1 : 0);
    sqlite3_bind_int(st, 2, query.program_kind.value_or(0));
    sqlite3_bind_int(st, 3, query.before.has_value() ? 1 : 0);
    sqlite3_bind_int64(st, 4, query.before.value_or(UiReadListCursor{}).primary);
    sqlite3_bind_int64(st, 5, query.before.value_or(UiReadListCursor{}).secondary);
    sqlite3_bind_int(st, 6, query.after.has_value() ? 1 : 0);
    sqlite3_bind_int64(st, 7, query.after.value_or(UiReadListCursor{}).primary);
    sqlite3_bind_int64(st, 8, query.after.value_or(UiReadListCursor{}).secondary);
    sqlite3_bind_int(st, 9, query.limit);

    while (sqlite3_step(st) == SQLITE_ROW) {
        page.items.push_back(ReadJobSetSummaryRow(st));
    }
    sqlite3_finalize(st);

    if (!page.items.empty()) {
        const auto& first = page.items.front();
        const auto& last = page.items.back();
        page.prev = UiReadListCursor{ first.created_at_utc, first.job_set_id };
        page.next = UiReadListCursor{ last.created_at_utc, last.job_set_id };
    }
    return page;
}

std::optional<UiJobSetDetail> SqliteUiReadDb::GetJobSetDetail(
    std::int64_t job_set_id,
    int jobs_limit) const {
    if (db_ == nullptr || job_set_id <= 0) {
        return std::nullopt;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT job_set_id,MIN(program_kind),MIN(queued_at_utc),COUNT(*),"
        "SUM(CASE WHEN state IN ('COMPLETED','DONE','SUCCEEDED') THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN state='SUCCEEDED' THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN state='FAILED' THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN state='CANCELED' THEN 1 ELSE 0 END) "
        "FROM ui_job_summary WHERE job_set_id=?1 GROUP BY job_set_id;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st, 1, job_set_id);
    std::optional<UiJobSetDetail> detail;
    if (sqlite3_step(st) == SQLITE_ROW) {
        detail = UiJobSetDetail{};
        detail->summary = ReadJobSetSummaryRow(st);
        detail->hierarchy_projection_available = false;
    }
    sqlite3_finalize(st);

    if (!detail.has_value()) {
        return std::nullopt;
    }

    if (jobs_limit > 0) {
        UiReadJobListQuery query{};
        query.job_set_id = job_set_id;
        query.limit = jobs_limit;
        detail->jobs = ListJobs(query).items;
    }
    return detail;
}

UiReadPage<UiArtifactSummary> SqliteUiReadDb::ListArtifacts(
    const UiReadArtifactListQuery& query) const {
    UiReadPage<UiArtifactSummary> page{};
    if (db_ == nullptr || query.limit <= 0) {
        return page;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT artifact_id,sha256,size_bytes,artifact_kind,filename,created_at_utc "
        "FROM ui_artifact_browser "
        "WHERE (?1=1 OR filename LIKE ?2 OR sha256 LIKE ?2 OR artifact_kind LIKE ?2) "
        "AND (?3=1 OR filename LIKE ?4) "
        "AND (?5=0 OR created_at_utc < ?6 OR (created_at_utc=?6 AND artifact_id < ?7)) "
        "AND (?8=0 OR created_at_utc > ?9 OR (created_at_utc=?9 AND artifact_id > ?10)) "
        "ORDER BY created_at_utc DESC, artifact_id DESC LIMIT ?11;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return page;
    }

    const bool search_empty = query.search.empty();
    const std::string search_like = "%" + query.search + "%";
    const bool extension_empty = query.extension.empty();
    const std::string extension_like = "%" + query.extension;
    sqlite3_bind_int(st, 1, search_empty ? 1 : 0);
    sqlite3_bind_text(st, 2, search_like.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, extension_empty ? 1 : 0);
    sqlite3_bind_text(st, 4, extension_like.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 5, query.before.has_value() ? 1 : 0);
    sqlite3_bind_int64(st, 6, query.before.value_or(UiReadListCursor{}).primary);
    sqlite3_bind_int64(st, 7, query.before.value_or(UiReadListCursor{}).secondary);
    sqlite3_bind_int(st, 8, query.after.has_value() ? 1 : 0);
    sqlite3_bind_int64(st, 9, query.after.value_or(UiReadListCursor{}).primary);
    sqlite3_bind_int64(st, 10, query.after.value_or(UiReadListCursor{}).secondary);
    sqlite3_bind_int(st, 11, query.limit);

    while (sqlite3_step(st) == SQLITE_ROW) {
        UiArtifactSummary row{};
        row.artifact_id = sqlite3_column_int64(st, 0);
        const auto* sha_text = sqlite3_column_text(st, 1);
        row.sha256 = sha_text == nullptr ? std::string{} : reinterpret_cast<const char*>(sha_text);
        row.size_bytes = static_cast<std::uint64_t>(sqlite3_column_int64(st, 2));
        const auto* kind_text = sqlite3_column_text(st, 3);
        row.artifact_kind = kind_text == nullptr ? std::string{} : reinterpret_cast<const char*>(kind_text);
        const auto* filename_text = sqlite3_column_text(st, 4);
        row.filename = filename_text == nullptr ? std::string{} : reinterpret_cast<const char*>(filename_text);
        row.created_at_utc = sqlite3_column_int64(st, 5);
        page.items.push_back(std::move(row));
    }
    sqlite3_finalize(st);

    if (!page.items.empty()) {
        const auto& first = page.items.front();
        const auto& last = page.items.back();
        page.prev = UiReadListCursor{ first.created_at_utc, first.artifact_id };
        page.next = UiReadListCursor{ last.created_at_utc, last.artifact_id };
    }
    return page;
}

bool SqliteUiReadDb::UpsertArtifactSummary(
    const UiArtifactSummary& summary,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out != nullptr) *error_out = "database handle is null";
        return false;
    }
    if (summary.artifact_id <= 0 || summary.sha256.empty() || summary.filename.empty()) {
        if (error_out != nullptr) *error_out = "artifact_id, sha256, and filename are required";
        return false;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "INSERT INTO ui_artifact_browser(artifact_id,sha256,size_bytes,artifact_kind,filename,created_at_utc) "
        "VALUES(?1,?2,?3,?4,?5,?6) "
        "ON CONFLICT(artifact_id) DO UPDATE SET "
        "sha256=excluded.sha256,size_bytes=excluded.size_bytes,artifact_kind=excluded.artifact_kind,"
        "filename=excluded.filename,created_at_utc=excluded.created_at_utc;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(st, 1, summary.artifact_id);
    sqlite3_bind_text(st, 2, summary.sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, static_cast<sqlite3_int64>(summary.size_bytes));
    sqlite3_bind_text(st, 4, summary.artifact_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, summary.filename.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 6, summary.created_at_utc);

    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok && error_out != nullptr) {
        *error_out = sqlite3_errmsg(db_);
    }
    sqlite3_finalize(st);
    return ok;
}

UiReadPage<UiWorkflowInstanceSummary> SqliteUiReadDb::ListWorkflowInstances(
    const UiWorkflowInstanceListQuery& query) const {
    UiReadPage<UiWorkflowInstanceSummary> page{};
    if (db_ == nullptr || query.limit <= 0) {
        return page;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT workflow_instance_id,workflow_kind,state,root_scope_kind,root_scope_id,COALESCE(created_by,''),"
        "blocked_step_count,failed_step_count,created_at_utc,started_at_utc,completed_at_utc,"
        "COALESCE(failure_code,''),COALESCE(failure_text,'') "
        "FROM ui_workflow_instance "
        "WHERE (?1=1 OR state=?2) "
        "AND (?3=1 OR workflow_kind=?4) "
        "AND (?5=0 OR created_at_utc < ?6 OR (created_at_utc=?6 AND workflow_instance_id < ?7)) "
        "AND (?8=0 OR created_at_utc > ?9 OR (created_at_utc=?9 AND workflow_instance_id > ?10)) "
        "ORDER BY created_at_utc DESC, workflow_instance_id DESC LIMIT ?11;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return page;
    }

    sqlite3_bind_int(st, 1, query.state.empty() ? 1 : 0);
    sqlite3_bind_text(st, 2, query.state.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, query.workflow_kind.empty() ? 1 : 0);
    sqlite3_bind_text(st, 4, query.workflow_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 5, query.before.has_value() ? 1 : 0);
    sqlite3_bind_int64(st, 6, query.before.value_or(UiReadListCursor{}).primary);
    sqlite3_bind_int64(st, 7, query.before.value_or(UiReadListCursor{}).secondary);
    sqlite3_bind_int(st, 8, query.after.has_value() ? 1 : 0);
    sqlite3_bind_int64(st, 9, query.after.value_or(UiReadListCursor{}).primary);
    sqlite3_bind_int64(st, 10, query.after.value_or(UiReadListCursor{}).secondary);
    sqlite3_bind_int(st, 11, query.limit);

    while (sqlite3_step(st) == SQLITE_ROW) {
        page.items.push_back(ReadWorkflowInstanceRow(st));
    }
    sqlite3_finalize(st);

    if (!page.items.empty()) {
        const auto& first = page.items.front();
        const auto& last = page.items.back();
        page.prev = UiReadListCursor{ first.created_at_utc, first.workflow_instance_id };
        page.next = UiReadListCursor{ last.created_at_utc, last.workflow_instance_id };
    }
    return page;
}

std::optional<UiWorkflowDetail> SqliteUiReadDb::GetWorkflowDetail(
    std::int64_t workflow_instance_id) const {
    if (db_ == nullptr || workflow_instance_id <= 0) {
        return std::nullopt;
    }

    sqlite3_stmt* inst = nullptr;
    constexpr const char* kInstanceSql =
        "SELECT workflow_instance_id,workflow_kind,state,root_scope_kind,root_scope_id,COALESCE(created_by,''),"
        "blocked_step_count,failed_step_count,created_at_utc,started_at_utc,completed_at_utc,"
        "COALESCE(failure_code,''),COALESCE(failure_text,'') "
        "FROM ui_workflow_instance WHERE workflow_instance_id=?1;";
    if (sqlite3_prepare_v2(db_, kInstanceSql, -1, &inst, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(inst, 1, workflow_instance_id);
    std::optional<UiWorkflowDetail> detail;
    if (sqlite3_step(inst) == SQLITE_ROW) {
        detail = UiWorkflowDetail{};
        detail->instance = ReadWorkflowInstanceRow(inst);
    }
    sqlite3_finalize(inst);
    if (!detail.has_value()) {
        return std::nullopt;
    }

    sqlite3_stmt* activations = nullptr;
    constexpr const char* kActivationsSql =
        "SELECT workflow_unit_activation_id,workflow_instance_id,parent_workflow_unit_activation_id,activation_key,graph_node_key,"
        "unit_kind,display_name,state,COALESCE(activation_params_json,''),COALESCE(authored_ref_kind,''),authored_ref_id,"
        "COALESCE(failure_code,''),COALESCE(failure_text,''),created_at_utc,ready_at_utc,started_at_utc,completed_at_utc,failed_at_utc "
        "FROM ui_workflow_unit_activation WHERE workflow_instance_id=?1 ORDER BY created_at_utc ASC, workflow_unit_activation_id ASC;";
    if (sqlite3_prepare_v2(db_, kActivationsSql, -1, &activations, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(activations, 1, workflow_instance_id);
        while (sqlite3_step(activations) == SQLITE_ROW) {
            detail->unit_activations.push_back(ReadWorkflowUnitActivationRow(activations));
        }
    }
    sqlite3_finalize(activations);

    sqlite3_stmt* activation_edges = nullptr;
    constexpr const char* kActivationEdgesSql =
        "SELECT workflow_unit_activation_edge_id,workflow_instance_id,from_workflow_unit_activation_id,to_workflow_unit_activation_id,"
        "COALESCE(output_key,''),COALESCE(input_key,''),COALESCE(condition_kind,''),COALESCE(condition_value,''),created_at_utc "
        "FROM ui_workflow_unit_activation_edge WHERE workflow_instance_id=?1 ORDER BY created_at_utc ASC, workflow_unit_activation_edge_id ASC;";
    if (sqlite3_prepare_v2(db_, kActivationEdgesSql, -1, &activation_edges, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(activation_edges, 1, workflow_instance_id);
        while (sqlite3_step(activation_edges) == SQLITE_ROW) {
            detail->unit_activation_edges.push_back(ReadWorkflowUnitActivationEdgeRow(activation_edges));
        }
    }
    sqlite3_finalize(activation_edges);

    sqlite3_stmt* steps = nullptr;
    constexpr const char* kStepsSql =
        "SELECT workflow_step_id,workflow_instance_id,workflow_unit_activation_id,step_key,step_kind,state,COALESCE(blocked_reason,''),job_set_id,"
        "job_count,job_completed_count,job_failed_count,priority,attempts,max_attempts,"
        "ready_at_utc,started_at_utc,completed_at_utc,failed_at_utc,created_at_utc "
        "FROM ui_workflow_step WHERE workflow_instance_id=?1 ORDER BY created_at_utc ASC, workflow_step_id ASC;";
    if (sqlite3_prepare_v2(db_, kStepsSql, -1, &steps, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(steps, 1, workflow_instance_id);
        while (sqlite3_step(steps) == SQLITE_ROW) {
            detail->steps.push_back(ReadWorkflowStepRow(steps));
        }
    }
    sqlite3_finalize(steps);

    sqlite3_stmt* edges = nullptr;
    constexpr const char* kEdgesSql =
        "SELECT workflow_edge_id,workflow_instance_id,from_step_id,to_step_id,"
        "COALESCE(condition_kind,''),COALESCE(condition_value,''),created_at_utc "
        "FROM ui_workflow_edge WHERE workflow_instance_id=?1 ORDER BY created_at_utc ASC, workflow_edge_id ASC;";
    if (sqlite3_prepare_v2(db_, kEdgesSql, -1, &edges, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(edges, 1, workflow_instance_id);
        while (sqlite3_step(edges) == SQLITE_ROW) {
            detail->edges.push_back(ReadWorkflowEdgeRow(edges));
        }
    }
    sqlite3_finalize(edges);

    sqlite3_stmt* alerts = nullptr;
    constexpr const char* kAlertsSql =
        "SELECT workflow_alert_id,workflow_instance_id,workflow_step_id,alert_kind,COALESCE(alert_code,''),"
        "message,is_active,first_seen_at_utc,last_seen_at_utc,cleared_at_utc "
        "FROM ui_workflow_alert WHERE workflow_instance_id=?1 ORDER BY is_active DESC, last_seen_at_utc DESC, workflow_alert_id DESC;";
    if (sqlite3_prepare_v2(db_, kAlertsSql, -1, &alerts, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(alerts, 1, workflow_instance_id);
        while (sqlite3_step(alerts) == SQLITE_ROW) {
            detail->alerts.push_back(ReadWorkflowAlertRow(alerts));
        }
    }
    sqlite3_finalize(alerts);

    return detail;
}

UiSeedProbeRunPage SqliteUiReadDb::ListSeedProbeRuns(
    const UiReadSeedProbeRunListQuery& query) const {
    UiSeedProbeRunPage page{};
    if (db_ == nullptr || query.limit <= 0) {
        return page;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT probe_run_id,probe_set_id,entry_savestate_id,seed_probe_spec_id,codec_version,"
        "status,neutral_seed_value,grid_count,unique_count,requested_at_utc,completed_at_utc "
        "FROM ui_seed_probe_summary "
        "WHERE (?1=1 OR status LIKE ?2 OR CAST(probe_run_id AS TEXT) LIKE ?2) "
        "AND (?3=0 OR lower(status)='completed' OR lower(status)='done') "
        "AND (?4=0 OR requested_at_utc < ?5 OR (requested_at_utc=?5 AND probe_run_id < ?6)) "
        "AND (?7=0 OR requested_at_utc > ?8 OR (requested_at_utc=?8 AND probe_run_id > ?9)) "
        "ORDER BY requested_at_utc DESC, probe_run_id DESC LIMIT ?10;";

    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return page;
    }

    const bool search_empty = query.search.empty();
    const std::string search_like = "%" + query.search + "%";
    sqlite3_bind_int(st, 1, search_empty ? 1 : 0);
    sqlite3_bind_text(st, 2, search_like.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, query.only_completed ? 1 : 0);
    sqlite3_bind_int(st, 4, query.before.has_value() ? 1 : 0);
    sqlite3_bind_int64(st, 5, query.before.value_or(UiReadSeedProbeRunListCursor{}).requested_at_utc);
    sqlite3_bind_int64(st, 6, query.before.value_or(UiReadSeedProbeRunListCursor{}).probe_run_id);
    sqlite3_bind_int(st, 7, query.after.has_value() ? 1 : 0);
    sqlite3_bind_int64(st, 8, query.after.value_or(UiReadSeedProbeRunListCursor{}).requested_at_utc);
    sqlite3_bind_int64(st, 9, query.after.value_or(UiReadSeedProbeRunListCursor{}).probe_run_id);
    sqlite3_bind_int(st, 10, query.limit);

    while (sqlite3_step(st) == SQLITE_ROW) {
        if (auto row = ReadSeedProbeSummaryRow(st); row.has_value()) {
            page.items.push_back(std::move(*row));
        }
    }
    sqlite3_finalize(st);

    if (!page.items.empty()) {
        const auto& first = page.items.front();
        const auto& last = page.items.back();
        page.prev = UiReadSeedProbeRunListCursor{ first.requested_at_utc, first.probe_run_id };
        page.next = UiReadSeedProbeRunListCursor{ last.requested_at_utc, last.probe_run_id };
    }
    return page;
}

std::optional<UiSeedProbeRunSummary> SqliteUiReadDb::GetSeedProbeRunSummary(
    std::int64_t probe_run_id) const {
    if (db_ == nullptr || probe_run_id <= 0) {
        return std::nullopt;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT probe_run_id,probe_set_id,entry_savestate_id,seed_probe_spec_id,codec_version,"
        "status,neutral_seed_value,grid_count,unique_count,requested_at_utc,completed_at_utc "
        "FROM ui_seed_probe_summary WHERE probe_run_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st, 1, probe_run_id);
    std::optional<UiSeedProbeRunSummary> summary;
    if (sqlite3_step(st) == SQLITE_ROW) {
        summary = ReadSeedProbeSummaryRow(st);
    }
    sqlite3_finalize(st);
    return summary;
}

std::vector<UiSeedProbeDeltaPoint> SqliteUiReadDb::ListSeedProbeDeltaPoints(
    std::int64_t probe_run_id) const {
    std::vector<UiSeedProbeDeltaPoint> points;
    if (db_ == nullptr || probe_run_id <= 0) {
        return points;
    }
    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT delta_point_id,probe_run_id,source_family,axis_x,axis_y,seed_value,seed_delta "
        "FROM ui_seed_probe_delta_point WHERE probe_run_id=?1 "
        "ORDER BY source_family ASC, axis_x ASC, axis_y ASC, seed_value ASC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return points;
    }
    sqlite3_bind_int64(st, 1, probe_run_id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        UiSeedProbeDeltaPoint point{};
        point.delta_point_id = sqlite3_column_int64(st, 0);
        point.probe_run_id = sqlite3_column_int64(st, 1);
        const auto* family = sqlite3_column_text(st, 2);
        point.source_family = family == nullptr ? std::string{} : reinterpret_cast<const char*>(family);
        point.axis_x = sqlite3_column_int(st, 3);
        point.axis_y = sqlite3_column_int(st, 4);
        point.seed_value = sqlite3_column_int64(st, 5);
        point.seed_delta = sqlite3_column_int64(st, 6);
        points.push_back(std::move(point));
    }
    sqlite3_finalize(st);
    return points;
}

std::vector<UiSeedProbeUniqueValue> SqliteUiReadDb::ListSeedProbeUniqueValues(
    std::int64_t probe_run_id) const {
    std::vector<UiSeedProbeUniqueValue> values;
    if (db_ == nullptr || probe_run_id <= 0) {
        return values;
    }
    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT unique_value_id,probe_run_id,seed_value,seed_delta,main_x,main_y,cstick_x,cstick_y,trigger_x,trigger_y "
        "FROM ui_seed_probe_unique_value WHERE probe_run_id=?1 ORDER BY seed_value ASC, unique_value_id ASC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return values;
    }
    sqlite3_bind_int64(st, 1, probe_run_id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        UiSeedProbeUniqueValue value{};
        value.unique_value_id = sqlite3_column_int64(st, 0);
        value.probe_run_id = sqlite3_column_int64(st, 1);
        value.seed_value = sqlite3_column_int64(st, 2);
        value.seed_delta = sqlite3_column_int64(st, 3);
        value.main_x = sqlite3_column_int(st, 4);
        value.main_y = sqlite3_column_int(st, 5);
        value.cstick_x = sqlite3_column_int(st, 6);
        value.cstick_y = sqlite3_column_int(st, 7);
        value.trigger_x = sqlite3_column_int(st, 8);
        value.trigger_y = sqlite3_column_int(st, 9);
        values.push_back(std::move(value));
    }
    sqlite3_finalize(st);
    return values;
}

bool SqliteUiReadDb::UpsertSeedProbeRunSummary(
    const UiSeedProbeRunSummary& summary,
    std::string* error_out) {
    if (db_ == nullptr || summary.probe_run_id <= 0) {
        if (error_out) *error_out = "invalid seed probe summary";
        return false;
    }
    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "INSERT INTO ui_seed_probe_summary("
        "probe_run_id,probe_set_id,entry_savestate_id,seed_probe_spec_id,codec_version,status,"
        "neutral_seed_value,grid_count,unique_count,requested_at_utc,completed_at_utc) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11) "
        "ON CONFLICT(probe_run_id) DO UPDATE SET "
        "probe_set_id=excluded.probe_set_id,entry_savestate_id=excluded.entry_savestate_id,"
        "seed_probe_spec_id=excluded.seed_probe_spec_id,codec_version=excluded.codec_version,"
        "status=excluded.status,neutral_seed_value=excluded.neutral_seed_value,"
        "grid_count=excluded.grid_count,unique_count=excluded.unique_count,"
        "requested_at_utc=excluded.requested_at_utc,completed_at_utc=excluded.completed_at_utc;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st, 1, summary.probe_run_id);
    sqlite3_bind_int64(st, 2, summary.probe_set_id);
    sqlite3_bind_int64(st, 3, summary.entry_savestate_id);
    sqlite3_bind_int64(st, 4, summary.seed_probe_spec_id);
    sqlite3_bind_int(st, 5, summary.codec_version);
    sqlite3_bind_text(st, 6, summary.status.c_str(), -1, SQLITE_TRANSIENT);
    if (summary.neutral_seed_value.has_value()) sqlite3_bind_int64(st, 7, *summary.neutral_seed_value);
    else sqlite3_bind_null(st, 7);
    sqlite3_bind_int(st, 8, summary.grid_count);
    sqlite3_bind_int(st, 9, summary.unique_count);
    sqlite3_bind_int64(st, 10, summary.requested_at_utc);
    if (summary.completed_at_utc.has_value()) sqlite3_bind_int64(st, 11, *summary.completed_at_utc);
    else sqlite3_bind_null(st, 11);
    const auto rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE && error_out) *error_out = sqlite3_errmsg(db_);
    return rc == SQLITE_DONE;
}

bool SqliteUiReadDb::ReplaceSeedProbeDeltaPoints(
    std::int64_t probe_run_id,
    const std::vector<UiSeedProbeDeltaPoint>& points,
    std::string* error_out) {
    if (db_ == nullptr || probe_run_id <= 0) {
        if (error_out) *error_out = "invalid seed probe delta projection";
        return false;
    }
    if (!ExecSql(db_, "BEGIN IMMEDIATE;")) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    bool success = true;
    sqlite3_stmt* del = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM ui_seed_probe_delta_point WHERE probe_run_id=?1;", -1, &del, nullptr) != SQLITE_OK) {
        success = false;
    } else {
        sqlite3_bind_int64(del, 1, probe_run_id);
        success = sqlite3_step(del) == SQLITE_DONE;
    }
    sqlite3_finalize(del);

    sqlite3_stmt* ins = nullptr;
    constexpr const char* kInsert =
        "INSERT INTO ui_seed_probe_delta_point(delta_point_id,probe_run_id,source_family,axis_x,axis_y,seed_value,seed_delta) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7);";
    if (success && sqlite3_prepare_v2(db_, kInsert, -1, &ins, nullptr) != SQLITE_OK) {
        success = false;
    }
    for (const auto& point : points) {
        if (!success) break;
        sqlite3_reset(ins);
        sqlite3_clear_bindings(ins);
        sqlite3_bind_int64(ins, 1, point.delta_point_id);
        sqlite3_bind_int64(ins, 2, probe_run_id);
        sqlite3_bind_text(ins, 3, point.source_family.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(ins, 4, point.axis_x);
        sqlite3_bind_int(ins, 5, point.axis_y);
        sqlite3_bind_int64(ins, 6, point.seed_value);
        sqlite3_bind_int64(ins, 7, point.seed_delta);
        success = sqlite3_step(ins) == SQLITE_DONE;
    }
    sqlite3_finalize(ins);

    if (success) success = ExecSql(db_, "COMMIT;");
    else ExecSql(db_, "ROLLBACK;");
    if (!success && error_out) *error_out = sqlite3_errmsg(db_);
    return success;
}

bool SqliteUiReadDb::ReplaceSeedProbeUniqueValues(
    std::int64_t probe_run_id,
    const std::vector<UiSeedProbeUniqueValue>& values,
    std::string* error_out) {
    if (db_ == nullptr || probe_run_id <= 0) {
        if (error_out) *error_out = "invalid seed probe unique projection";
        return false;
    }
    if (!ExecSql(db_, "BEGIN IMMEDIATE;")) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    bool success = true;
    sqlite3_stmt* del = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM ui_seed_probe_unique_value WHERE probe_run_id=?1;", -1, &del, nullptr) != SQLITE_OK) {
        success = false;
    } else {
        sqlite3_bind_int64(del, 1, probe_run_id);
        success = sqlite3_step(del) == SQLITE_DONE;
    }
    sqlite3_finalize(del);

    sqlite3_stmt* ins = nullptr;
    constexpr const char* kInsert =
        "INSERT INTO ui_seed_probe_unique_value(unique_value_id,probe_run_id,seed_value,seed_delta,main_x,main_y,cstick_x,cstick_y,trigger_x,trigger_y) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10);";
    if (success && sqlite3_prepare_v2(db_, kInsert, -1, &ins, nullptr) != SQLITE_OK) {
        success = false;
    }
    for (const auto& value : values) {
        if (!success) break;
        sqlite3_reset(ins);
        sqlite3_clear_bindings(ins);
        sqlite3_bind_int64(ins, 1, value.unique_value_id);
        sqlite3_bind_int64(ins, 2, probe_run_id);
        sqlite3_bind_int64(ins, 3, value.seed_value);
        sqlite3_bind_int64(ins, 4, value.seed_delta);
        sqlite3_bind_int(ins, 5, value.main_x);
        sqlite3_bind_int(ins, 6, value.main_y);
        sqlite3_bind_int(ins, 7, value.cstick_x);
        sqlite3_bind_int(ins, 8, value.cstick_y);
        sqlite3_bind_int(ins, 9, value.trigger_x);
        sqlite3_bind_int(ins, 10, value.trigger_y);
        success = sqlite3_step(ins) == SQLITE_DONE;
    }
    sqlite3_finalize(ins);

    if (success) success = ExecSql(db_, "COMMIT;");
    else ExecSql(db_, "ROLLBACK;");
    if (!success && error_out) *error_out = sqlite3_errmsg(db_);
    return success;
}

std::optional<UiProjectionSubscription> SqliteUiReadDb::GetProjectionSubscription(
    const std::string& projector_name,
    const std::string& source_context,
    const std::string& source_outbox_table) const {
    if (!IsSubscriptionKeyValid(projector_name, source_context, source_outbox_table)) {
        return std::nullopt;
    }

    return ReadSubscription(db_, projector_name, source_context, source_outbox_table);
}

std::vector<UiProjectionSubscription> SqliteUiReadDb::ListProjectionSubscriptions(
    const std::string& source_context,
    const std::string& source_outbox_table) const {
    std::vector<UiProjectionSubscription> subscriptions;
    if (source_context.empty() || source_outbox_table.empty()) {
        return subscriptions;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT projector_name, source_context, source_outbox_table, "
        "COALESCE(last_outbox_id, 0), COALESCE(last_event_id, ''), updated_at_utc, "
        "COALESCE(status, 'ACTIVE'), COALESCE(last_error, '') "
        "FROM ui_projection_subscription "
        "WHERE source_context=?1 AND source_outbox_table=?2 "
        "ORDER BY projector_name ASC;";

    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return subscriptions;
    }
    sqlite3_bind_text(st, 1, source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(st) == SQLITE_ROW) {
        UiProjectionSubscription subscription{};
        const auto* projector_name_text = sqlite3_column_text(st, 0);
        const auto* source_context_text = sqlite3_column_text(st, 1);
        const auto* source_outbox_table_text = sqlite3_column_text(st, 2);
        const auto* last_event_id_text = sqlite3_column_text(st, 4);
        const auto* status_text = sqlite3_column_text(st, 6);
        const auto* last_error_text = sqlite3_column_text(st, 7);
        subscription.projector_name = projector_name_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(projector_name_text);
        subscription.source_context = source_context_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(source_context_text);
        subscription.source_outbox_table = source_outbox_table_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(source_outbox_table_text);
        subscription.last_outbox_id = sqlite3_column_int64(st, 3);
        subscription.last_event_id = last_event_id_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(last_event_id_text);
        subscription.updated_at_utc = FromEpochMillis(sqlite3_column_int64(st, 5));
        subscription.status = status_text == nullptr
            ? std::string{ "ACTIVE" }
            : reinterpret_cast<const char*>(status_text);
        subscription.last_error = last_error_text == nullptr
            ? std::string{}
            : reinterpret_cast<const char*>(last_error_text);
        subscriptions.push_back(std::move(subscription));
    }

    sqlite3_finalize(st);
    return subscriptions;
}

std::optional<std::int64_t> SqliteUiReadDb::ComputeSafeFloorOutboxId(
    const std::string& source_context,
    const std::string& source_outbox_table) const {
    if (source_context.empty() || source_outbox_table.empty()) {
        return std::nullopt;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT MIN(last_outbox_id) "
        "FROM ui_projection_subscription "
        "WHERE source_context=?1 AND source_outbox_table=?2 AND status='ACTIVE';";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_text(st, 1, source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<std::int64_t> safe_floor;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL) {
        safe_floor = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return safe_floor;
}

std::optional<UiProjectionSubscription> SqliteUiReadDb::GetOrCreateProjectionSubscription(
    const UiProjectionSubscription& subscription) {
    if (!IsSubscriptionKeyValid(
            subscription.projector_name,
            subscription.source_context,
            subscription.source_outbox_table)) {
        return std::nullopt;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "INSERT INTO ui_projection_subscription("
        "projector_name, source_context, source_outbox_table, last_outbox_id, last_event_id, updated_at_utc, status, last_error) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) "
        "ON CONFLICT(projector_name, source_context, source_outbox_table) DO NOTHING;";

    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_text(st, 1, subscription.projector_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, subscription.source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, subscription.source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, subscription.last_outbox_id);
    if (subscription.last_event_id.empty()) {
        sqlite3_bind_null(st, 5);
    }
    else {
        sqlite3_bind_text(st, 5, subscription.last_event_id.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int64(st, 6, ToEpochMillis(subscription.updated_at_utc));
    const auto status = subscription.status.empty() ? std::string{ "ACTIVE" } : subscription.status;
    sqlite3_bind_text(st, 7, status.c_str(), -1, SQLITE_TRANSIENT);
    if (subscription.last_error.empty()) {
        sqlite3_bind_null(st, 8);
    }
    else {
        sqlite3_bind_text(st, 8, subscription.last_error.c_str(), -1, SQLITE_TRANSIENT);
    }

    const auto rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        return std::nullopt;
    }

    return GetProjectionSubscription(
        subscription.projector_name,
        subscription.source_context,
        subscription.source_outbox_table);
}

bool SqliteUiReadDb::AdvanceProjectionSubscriptionCursor(
    const std::string& projector_name,
    const std::string& source_context,
    const std::string& source_outbox_table,
    std::int64_t last_outbox_id,
    const std::string& last_event_id,
    types::UtcTimePoint updated_at_utc,
    const std::optional<UiProjectionSubscriptionBatchAudit>& batch_audit) {
    if (!IsSubscriptionKeyValid(projector_name, source_context, source_outbox_table)) {
        return false;
    }

    if (!ExecSql(db_, "BEGIN IMMEDIATE;")) {
        return false;
    }

    bool success = true;
    sqlite3_stmt* st = nullptr;
    constexpr const char* kUpdateSql =
        "UPDATE ui_projection_subscription "
        "SET last_outbox_id=?4, last_event_id=?5, updated_at_utc=?6, status='ACTIVE', last_error=NULL "
        "WHERE projector_name=?1 AND source_context=?2 AND source_outbox_table=?3;";

    if (sqlite3_prepare_v2(db_, kUpdateSql, -1, &st, nullptr) != SQLITE_OK) {
        success = false;
    }
    else {
        sqlite3_bind_text(st, 1, projector_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, source_context.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 4, last_outbox_id);
        if (last_event_id.empty()) {
            sqlite3_bind_null(st, 5);
        }
        else {
            sqlite3_bind_text(st, 5, last_event_id.c_str(), -1, SQLITE_TRANSIENT);
        }
        sqlite3_bind_int64(st, 6, ToEpochMillis(updated_at_utc));

        if (sqlite3_step(st) != SQLITE_DONE || sqlite3_changes(db_) == 0) {
            success = false;
        }
    }
    sqlite3_finalize(st);

    if (success && batch_audit.has_value()) {
        sqlite3_stmt* audit_st = nullptr;
        constexpr const char* kAuditSql =
            "INSERT INTO ui_projection_subscription_audit("
            "projector_name, source_context, source_outbox_table, from_outbox_id, to_outbox_id, processed_count, failed_count, recorded_at_utc) "
            "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8);";

        if (sqlite3_prepare_v2(db_, kAuditSql, -1, &audit_st, nullptr) != SQLITE_OK) {
            success = false;
        }
        else {
            sqlite3_bind_text(audit_st, 1, batch_audit->projector_name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(audit_st, 2, batch_audit->source_context.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(audit_st, 3, batch_audit->source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(audit_st, 4, batch_audit->from_outbox_id);
            sqlite3_bind_int64(audit_st, 5, batch_audit->to_outbox_id);
            sqlite3_bind_int64(audit_st, 6, batch_audit->processed_count);
            sqlite3_bind_int64(audit_st, 7, batch_audit->failed_count);
            sqlite3_bind_int64(audit_st, 8, ToEpochMillis(batch_audit->recorded_at_utc));

            if (sqlite3_step(audit_st) != SQLITE_DONE) {
                success = false;
            }
        }
        sqlite3_finalize(audit_st);
    }

    if (success) {
        success = ExecSql(db_, "COMMIT;");
    }
    else {
        ExecSql(db_, "ROLLBACK;");
    }

    return success;
}

bool SqliteUiReadDb::SetProjectionSubscriptionError(
    const std::string& projector_name,
    const std::string& source_context,
    const std::string& source_outbox_table,
    const std::string& last_error,
    types::UtcTimePoint updated_at_utc) {
    if (!IsSubscriptionKeyValid(projector_name, source_context, source_outbox_table)) {
        return false;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "UPDATE ui_projection_subscription "
        "SET status='ERROR', last_error=?4, updated_at_utc=?5 "
        "WHERE projector_name=?1 AND source_context=?2 AND source_outbox_table=?3;";

    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(st, 1, projector_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, last_error.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, ToEpochMillis(updated_at_utc));

    const auto rc = sqlite3_step(st);
    const auto rows = sqlite3_changes(db_);
    sqlite3_finalize(st);

    return rc == SQLITE_DONE && rows > 0;
}

bool SqliteUiReadDb::PauseProjectionSubscription(
    const std::string& projector_name,
    const std::string& source_context,
    const std::string& source_outbox_table,
    types::UtcTimePoint updated_at_utc,
    const std::string& reason) {
    if (!IsSubscriptionKeyValid(projector_name, source_context, source_outbox_table)) {
        return false;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "UPDATE ui_projection_subscription "
        "SET status='PAUSED', last_error=?4, updated_at_utc=?5 "
        "WHERE projector_name=?1 AND source_context=?2 AND source_outbox_table=?3;";

    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(st, 1, projector_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
    if (reason.empty()) {
        sqlite3_bind_null(st, 4);
    }
    else {
        sqlite3_bind_text(st, 4, reason.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int64(st, 5, ToEpochMillis(updated_at_utc));

    const auto rc = sqlite3_step(st);
    const auto rows = sqlite3_changes(db_);
    sqlite3_finalize(st);

    return rc == SQLITE_DONE && rows > 0;
}

bool SqliteUiReadDb::ResumeProjectionSubscription(
    const std::string& projector_name,
    const std::string& source_context,
    const std::string& source_outbox_table,
    types::UtcTimePoint updated_at_utc) {
    if (!IsSubscriptionKeyValid(projector_name, source_context, source_outbox_table)) {
        return false;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "UPDATE ui_projection_subscription "
        "SET status='ACTIVE', last_error=NULL, updated_at_utc=?4 "
        "WHERE projector_name=?1 AND source_context=?2 AND source_outbox_table=?3;";

    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(st, 1, projector_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, source_context.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, source_outbox_table.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, ToEpochMillis(updated_at_utc));

    const auto rc = sqlite3_step(st);
    const auto rows = sqlite3_changes(db_);
    sqlite3_finalize(st);

    return rc == SQLITE_DONE && rows > 0;
}

} // namespace savor::db
