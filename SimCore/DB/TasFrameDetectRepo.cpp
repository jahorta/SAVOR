#include "TasFrameDetectRepo.h"
#include <sqlite3.h>

namespace simcore::db {

static DbResult<int64_t> Impl_Insert(DbEnv& env, int64_t artifact_id, int64_t dtm_artifact_id)
{
    sqlite3* db = env.handle();
    sqlite3_stmt* st{};
    const char* sql =
        "INSERT INTO tas_frame_detect(artifact_id, dtm_artifact_id, created_at) "
        "VALUES(?,?,strftime('%s','now')) RETURNING id;";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
        return DbResult<int64_t>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });

    sqlite3_bind_int64(st, 1, artifact_id);
    sqlite3_bind_int64(st, 2, dtm_artifact_id);

    const int rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(st);
        return DbResult<int64_t>::Err({ map_sqlite_err(sqlite3_errcode(db)), rc, "insert" });
    }

    const int64_t id = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return DbResult<int64_t>::Ok(id);
}

std::future<DbResult<int64_t>> TasFrameDetectRepo::InsertAsync(int64_t artifact_id, int64_t dtm_artifact_id, RetryPolicy rp)
{
    return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::Normal, rp,
        [=](DbEnv& env) { return Impl_Insert(env, artifact_id, dtm_artifact_id); });
}

} // namespace simcore::db
