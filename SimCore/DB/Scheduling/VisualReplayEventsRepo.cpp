#include "VisualReplayEventsRepo.h"

#include "../DBCore/DbService.h"

namespace simcore::db {

std::future<DbResult<int64_t>> VisualReplayEventsRepo::AppendAsync(int64_t visual_replay_id, const std::string& event_kind, std::optional<std::string> payload, RetryPolicy rp)
{
    return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::High, rp,
        [=](DbEnv& env) -> DbResult<int64_t> {
            sqlite3* db = env.handle();
            sqlite3_stmt* st = nullptr;
            int rc = sqlite3_prepare_v2(db,
                "INSERT INTO visual_replay_events(visual_replay_id, event_kind, payload, created_at) VALUES (?1, ?2, ?3, strftime('%s','now'));",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<int64_t>::Err({ DbErrorKind::Internal, rc, "prepare append visual replay event" });
            sqlite3_bind_int64(st, 1, visual_replay_id);
            sqlite3_bind_text(st, 2, event_kind.c_str(), -1, SQLITE_TRANSIENT);
            if (payload.has_value()) sqlite3_bind_text(st, 3, payload->c_str(), -1, SQLITE_TRANSIENT);
            else sqlite3_bind_null(st, 3);
            rc = sqlite3_step(st);
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<int64_t>::Err({ DbErrorKind::Internal, rc, "exec append visual replay event" });
            return DbResult<int64_t>::Ok(static_cast<int64_t>(sqlite3_last_insert_rowid(db)));
        });
}

std::future<DbResult<std::vector<VisualReplayEventRow>>> VisualReplayEventsRepo::ListByReplayAsync(int64_t visual_replay_id, RetryPolicy rp)
{
    return DBService::instance().submit_res<std::vector<VisualReplayEventRow>>(OpType::Read, Priority::High, rp,
        [=](DbEnv& env) -> DbResult<std::vector<VisualReplayEventRow>> {
            sqlite3* db = env.handle();
            sqlite3_stmt* st = nullptr;
            int rc = sqlite3_prepare_v2(db,
                "SELECT visual_event_id, visual_replay_id, event_kind, payload, created_at "
                "FROM visual_replay_events WHERE visual_replay_id=?1 ORDER BY visual_event_id ASC;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<std::vector<VisualReplayEventRow>>::Err({ DbErrorKind::Internal, rc, "prepare list visual replay events" });
            sqlite3_bind_int64(st, 1, visual_replay_id);

            std::vector<VisualReplayEventRow> rows;
            for (;;) {
                rc = sqlite3_step(st);
                if (rc == SQLITE_DONE) break;
                if (rc != SQLITE_ROW) {
                    sqlite3_finalize(st);
                    return DbResult<std::vector<VisualReplayEventRow>>::Err({ DbErrorKind::Internal, rc, "step list visual replay events" });
                }

                VisualReplayEventRow row{};
                row.visual_event_id = sqlite3_column_int64(st, 0);
                row.visual_replay_id = sqlite3_column_int64(st, 1);
                row.event_kind = std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 2)));
                if (sqlite3_column_type(st, 3) != SQLITE_NULL) row.payload = std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 3)));
                row.created_at = sqlite3_column_int64(st, 4);
                rows.push_back(std::move(row));
            }

            sqlite3_finalize(st);
            return DbResult<std::vector<VisualReplayEventRow>>::Ok(std::move(rows));
        });
}

} // namespace simcore::db

