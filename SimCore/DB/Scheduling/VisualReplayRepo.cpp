#include "VisualReplayRepo.h"

#include "../DBCore/DbService.h"

namespace simcore::db {

namespace {
DbResult<VisualReplayRow> impl_get(DbEnv& env, int64_t visual_replay_id)
{
    sqlite3* db = env.handle();
    sqlite3_stmt* st = nullptr;
    int rc = sqlite3_prepare_v2(db,
        "SELECT visual_replay_id, job_id, requested_at, state, worker_id, started_at, ended_at, error_text "
        "FROM visual_replay_entries WHERE visual_replay_id=?1;",
        -1, &st, nullptr);
    if (rc != SQLITE_OK) return DbResult<VisualReplayRow>::Err({ DbErrorKind::Unknown, rc, "prepare get visual replay" });
    sqlite3_bind_int64(st, 1, visual_replay_id);
    rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(st);
        return DbResult<VisualReplayRow>::Err({ DbErrorKind::NotFound, 0, "visual replay entry not found" });
    }

    VisualReplayRow row{};
    row.visual_replay_id = sqlite3_column_int64(st, 0);
    row.job_id = sqlite3_column_int64(st, 1);
    row.requested_at = sqlite3_column_int64(st, 2);
    row.state = std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 3)));
    if (sqlite3_column_type(st, 4) != SQLITE_NULL) row.worker_id = sqlite3_column_int64(st, 4);
    if (sqlite3_column_type(st, 5) != SQLITE_NULL) row.started_at = sqlite3_column_int64(st, 5);
    if (sqlite3_column_type(st, 6) != SQLITE_NULL) row.ended_at = sqlite3_column_int64(st, 6);
    if (sqlite3_column_type(st, 7) != SQLITE_NULL) row.error_text = std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 7)));
    sqlite3_finalize(st);
    return DbResult<VisualReplayRow>::Ok(row);
}
}

std::future<DbResult<int64_t>> VisualReplayRepo::EnqueueAsync(int64_t job_id, RetryPolicy rp)
{
    return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::High, rp,
        [=](DbEnv& env) -> DbResult<int64_t> {
            sqlite3* db = env.handle();
            sqlite3_stmt* st = nullptr;
            int rc = sqlite3_prepare_v2(db,
                "INSERT INTO visual_replay_entries(job_id, requested_at, state) VALUES (?1, strftime('%s','now'), 'QUEUED');",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<int64_t>::Err({ DbErrorKind::Unknown, rc, "prepare enqueue visual replay" });
            sqlite3_bind_int64(st, 1, job_id);
            rc = sqlite3_step(st);
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<int64_t>::Err({ DbErrorKind::Unknown, rc, "exec enqueue visual replay" });
            return DbResult<int64_t>::Ok(static_cast<int64_t>(sqlite3_last_insert_rowid(db)));
        });
}

std::future<DbResult<std::optional<VisualReplayRow>>> VisualReplayRepo::ClaimNextQueuedAsync(int64_t worker_id, RetryPolicy rp)
{
    return DBService::instance().submit_res<std::optional<VisualReplayRow>>(OpType::Write, Priority::High, rp,
        [=](DbEnv& env) -> DbResult<std::optional<VisualReplayRow>> {
            sqlite3* db = env.handle();
            int rc = sqlite3_exec(db, "SAVEPOINT claim_visual_replay;", nullptr, nullptr, nullptr);
            if (rc != SQLITE_OK) return DbResult<std::optional<VisualReplayRow>>::Err({ DbErrorKind::Unknown, rc, "begin claim visual replay" });

            sqlite3_stmt* st = nullptr;
            rc = sqlite3_prepare_v2(db,
                "SELECT visual_replay_id FROM visual_replay_entries WHERE state='QUEUED' ORDER BY requested_at ASC, visual_replay_id ASC LIMIT 1;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) {
                sqlite3_exec(db, "ROLLBACK TO claim_visual_replay;", nullptr, nullptr, nullptr);
                sqlite3_exec(db, "RELEASE claim_visual_replay;", nullptr, nullptr, nullptr);
                return DbResult<std::optional<VisualReplayRow>>::Err({ DbErrorKind::Unknown, rc, "prepare claim visual replay select" });
            }

            int64_t visual_replay_id = 0;
            if (sqlite3_step(st) == SQLITE_ROW) visual_replay_id = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);

            if (visual_replay_id == 0) {
                sqlite3_exec(db, "RELEASE claim_visual_replay;", nullptr, nullptr, nullptr);
                return DbResult<std::optional<VisualReplayRow>>::Ok(std::nullopt);
            }

            rc = sqlite3_prepare_v2(db,
                "UPDATE visual_replay_entries SET state='RUNNING', worker_id=?2, started_at=strftime('%s','now'), error_text=NULL WHERE visual_replay_id=?1 AND state='QUEUED';",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) {
                sqlite3_exec(db, "ROLLBACK TO claim_visual_replay;", nullptr, nullptr, nullptr);
                sqlite3_exec(db, "RELEASE claim_visual_replay;", nullptr, nullptr, nullptr);
                return DbResult<std::optional<VisualReplayRow>>::Err({ DbErrorKind::Unknown, rc, "prepare claim visual replay update" });
            }
            sqlite3_bind_int64(st, 1, visual_replay_id);
            sqlite3_bind_int64(st, 2, worker_id);
            rc = sqlite3_step(st);
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) {
                sqlite3_exec(db, "ROLLBACK TO claim_visual_replay;", nullptr, nullptr, nullptr);
                sqlite3_exec(db, "RELEASE claim_visual_replay;", nullptr, nullptr, nullptr);
                return DbResult<std::optional<VisualReplayRow>>::Err({ DbErrorKind::Unknown, rc, "exec claim visual replay update" });
            }

            auto row = impl_get(env, visual_replay_id);
            if (!row.ok) {
                sqlite3_exec(db, "ROLLBACK TO claim_visual_replay;", nullptr, nullptr, nullptr);
                sqlite3_exec(db, "RELEASE claim_visual_replay;", nullptr, nullptr, nullptr);
                return DbResult<std::optional<VisualReplayRow>>::Err(row.error);
            }

            sqlite3_exec(db, "RELEASE claim_visual_replay;", nullptr, nullptr, nullptr);
            return DbResult<std::optional<VisualReplayRow>>::Ok(std::optional<VisualReplayRow>(std::move(row.value)));
        });
}

std::future<DbResult<VisualReplayRow>> VisualReplayRepo::GetAsync(int64_t visual_replay_id, RetryPolicy rp)
{
    return DBService::instance().submit_res<VisualReplayRow>(OpType::Read, Priority::High, rp,
        [=](DbEnv& env) { return impl_get(env, visual_replay_id); });
}

std::future<DbResult<void>> VisualReplayRepo::MarkRunningAsync(int64_t visual_replay_id, int64_t worker_id, RetryPolicy rp)
{
    return DBService::instance().submit_res<void>(OpType::Write, Priority::High, rp,
        [=](DbEnv& env) -> DbResult<void> {
            sqlite3* db = env.handle();
            sqlite3_stmt* st = nullptr;
            int rc = sqlite3_prepare_v2(db,
                "UPDATE visual_replay_entries SET state='RUNNING', worker_id=?2, started_at=strftime('%s','now'), error_text=NULL WHERE visual_replay_id=?1;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<void>::Err({ DbErrorKind::Unknown, rc, "prepare mark running visual replay" });
            sqlite3_bind_int64(st, 1, visual_replay_id);
            sqlite3_bind_int64(st, 2, worker_id);
            rc = sqlite3_step(st);
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<void>::Err({ DbErrorKind::Unknown, rc, "exec mark running visual replay" });
            return DbResult<void>::Ok();
        });
}

std::future<DbResult<void>> VisualReplayRepo::MarkSucceededAsync(int64_t visual_replay_id, RetryPolicy rp)
{
    return DBService::instance().submit_res<void>(OpType::Write, Priority::High, rp,
        [=](DbEnv& env) -> DbResult<void> {
            sqlite3* db = env.handle();
            sqlite3_stmt* st = nullptr;
            int rc = sqlite3_prepare_v2(db,
                "UPDATE visual_replay_entries SET state='SUCCEEDED', ended_at=strftime('%s','now') WHERE visual_replay_id=?1;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<void>::Err({ DbErrorKind::Unknown, rc, "prepare mark succeeded visual replay" });
            sqlite3_bind_int64(st, 1, visual_replay_id);
            rc = sqlite3_step(st);
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<void>::Err({ DbErrorKind::Unknown, rc, "exec mark succeeded visual replay" });
            return DbResult<void>::Ok();
        });
}

std::future<DbResult<void>> VisualReplayRepo::MarkFailedAsync(int64_t visual_replay_id, const std::string& error_text, RetryPolicy rp)
{
    return DBService::instance().submit_res<void>(OpType::Write, Priority::High, rp,
        [=](DbEnv& env) -> DbResult<void> {
            sqlite3* db = env.handle();
            sqlite3_stmt* st = nullptr;
            int rc = sqlite3_prepare_v2(db,
                "UPDATE visual_replay_entries SET state='FAILED', ended_at=strftime('%s','now'), error_text=?2 WHERE visual_replay_id=?1;",
                -1, &st, nullptr);
            if (rc != SQLITE_OK) return DbResult<void>::Err({ DbErrorKind::Unknown, rc, "prepare mark failed visual replay" });
            sqlite3_bind_int64(st, 1, visual_replay_id);
            sqlite3_bind_text(st, 2, error_text.c_str(), -1, SQLITE_TRANSIENT);
            rc = sqlite3_step(st);
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return DbResult<void>::Err({ DbErrorKind::Unknown, rc, "exec mark failed visual replay" });
            return DbResult<void>::Ok();
        });
}

} // namespace simcore::db
