#include "SqliteUiReadDb.h"

#include <chrono>

namespace simcore::db {

namespace {

std::int64_t ToEpochMillis(types::UtcTimePoint value) {
    return value.time_since_epoch().count();
}

types::UtcTimePoint FromEpochMillis(std::int64_t value) {
    return types::UtcTimePoint{ std::chrono::milliseconds(value) };
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
        UiJobSetSummary row{};
        row.job_set_id = sqlite3_column_int64(st, 0);
        row.program_kind = sqlite3_column_int(st, 1);
        row.created_at_utc = sqlite3_column_int64(st, 2);
        row.total_jobs = sqlite3_column_int64(st, 3);
        row.completed_jobs = sqlite3_column_int64(st, 4);
        row.succeeded_jobs = sqlite3_column_int64(st, 5);
        row.failed_jobs = sqlite3_column_int64(st, 6);
        row.canceled_jobs = sqlite3_column_int64(st, 7);
        page.items.push_back(std::move(row));
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

} // namespace simcore::db
