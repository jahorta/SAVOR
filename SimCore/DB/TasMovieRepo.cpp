#include "TasMovieRepo.h"
#include <sqlite3.h>

namespace simcore::db {

    static std::string text_or_empty(sqlite3_stmt* st, int col) {
        if (sqlite3_column_type(st, col) == SQLITE_NULL) return {};
        const unsigned char* p = sqlite3_column_text(st, col);
        return p ? reinterpret_cast<const char*>(p) : std::string{};
    }

    static DbResult<int64_t> Impl_Enqueue(DbEnv& env, int64_t base_file_id, int64_t new_rtc, int32_t priority) {
        sqlite3* db = env.handle();
        sqlite3_stmt* st{};
        int rc = sqlite3_prepare_v2(db,
            "INSERT INTO tas_movie(base_file_id, new_rtc, status, priority) VALUES(?, ?, 'planned', ?);",
            -1, &st, nullptr);
        if (rc != SQLITE_OK) return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "prepare" });
        sqlite3_bind_int64(st, 1, base_file_id);
        sqlite3_bind_int64(st, 2, new_rtc);
        sqlite3_bind_int(st, 3, priority);
        rc = sqlite3_step(st);
        if (rc != SQLITE_DONE) { sqlite3_finalize(st); return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "insert" }); }
        int64_t id = sqlite3_last_insert_rowid(db);
        sqlite3_finalize(st);
        return DbResult<int64_t>::Ok(id);
    }

    static DbResult<int64_t> Impl_IdempotentEnqueue(DbEnv& env, int64_t base_file_id, int64_t new_rtc, int32_t priority) {
        // Strategy: try find existing planned for (base_file_id,new_rtc); else insert
        sqlite3* db = env.handle();
        sqlite3_stmt* st{};
        int rc = sqlite3_prepare_v2(db,
            "SELECT id FROM tas_movie WHERE base_file_id=? AND new_rtc=? AND status='planned' LIMIT 1;",
            -1, &st, nullptr);
        if (rc != SQLITE_OK) return DbResult<int64_t>::Err({ map_sqlite_err(rc), rc, "prepare" });
        sqlite3_bind_int64(st, 1, base_file_id);
        sqlite3_bind_int64(st, 2, new_rtc);
        rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            int64_t id = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
            return DbResult<int64_t>::Ok(id);
        }
        sqlite3_finalize(st);
        return Impl_Enqueue(env, base_file_id, new_rtc, priority);
    }

    static DbResult<void> Impl_MarkRunning(DbEnv& env, int64_t id) {
        sqlite3* db = env.handle();
        sqlite3_stmt* st{};
        int rc = sqlite3_prepare_v2(db,
            "UPDATE tas_movie SET status='running', started_at=strftime('%s','now'), attempt_count=attempt_count+1 "
            "WHERE id=? AND status='planned';",
            -1, &st, nullptr);
        if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare" });
        sqlite3_bind_int64(st, 1, id);
        rc = sqlite3_step(st); sqlite3_finalize(st);
        if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "update" });
        return DbResult<void>{ true };
    }

    static DbResult<void> Impl_AppendProgress(DbEnv& env, int64_t id, const std::string& text, int max_bytes) {
        sqlite3* db = env.handle();
        sqlite3_stmt* st{};
        int rc = sqlite3_prepare_v2(db,
            "UPDATE tas_movie SET progress_log=substr(progress_log || ?, -?), last_error='' WHERE id=?;",
            -1, &st, nullptr);
        if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare" });
        sqlite3_bind_text(st, 1, text.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, max_bytes);
        sqlite3_bind_int64(st, 3, id);
        rc = sqlite3_step(st); sqlite3_finalize(st);
        if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "update" });
        return DbResult<void>{ true };
    }

    static DbResult<void> Impl_MarkFailed(DbEnv& env, int64_t id, const std::string& err) {
        sqlite3* db = env.handle();
        sqlite3_stmt* st{};
        int rc = sqlite3_prepare_v2(db,
            "UPDATE tas_movie SET last_error=?, status='planned' WHERE id=? AND status='running';",
            -1, &st, nullptr);
        if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare" });
        sqlite3_bind_text(st, 1, err.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, id);
        rc = sqlite3_step(st); sqlite3_finalize(st);
        if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "update" });
        return DbResult<void>{ true };
    }

    static DbResult<void> Impl_MarkDone(DbEnv& env, int64_t id, int64_t savestate_id) {
        sqlite3* db = env.handle();
        sqlite3_stmt* st{};
        int rc = sqlite3_prepare_v2(db,
            "UPDATE tas_movie SET status='done', completed_at=strftime('%s','now'), output_savestate_id=? "
            "WHERE id=? AND status='running';",
            -1, &st, nullptr);
        if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "prepare" });
        sqlite3_bind_int64(st, 1, savestate_id);
        sqlite3_bind_int64(st, 2, id);
        rc = sqlite3_step(st); sqlite3_finalize(st);
        if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc), rc, "update" });
        return DbResult<void>{ true };
    }

    static TasMovieRow ReadRow(sqlite3_stmt* st) {
        TasMovieRow r{};
        // Column order:
        // 0:id, 1:base_file_id, 2:new_rtc, 3:status, 4:progress_log, 5:created_at,
        // 6:started_at, 7:completed_at, 8:attempt_count, 9:last_error, 10:priority, 11:output_savestate_id
        r.id = sqlite3_column_int64(st, 0);
        r.base_file_id = sqlite3_column_int64(st, 1);
        if (sqlite3_column_type(st, 2) != SQLITE_NULL) r.new_rtc = sqlite3_column_int64(st, 2);
        r.status = text_or_empty(st, 3);
        r.progress_log = text_or_empty(st, 3);
        if (sqlite3_column_type(st, 5) != SQLITE_NULL) r.created_at = sqlite3_column_int64(st, 5);
        if (sqlite3_column_type(st, 6) != SQLITE_NULL) r.started_at = sqlite3_column_int64(st, 6);
        if (sqlite3_column_type(st, 7) != SQLITE_NULL) r.completed_at = sqlite3_column_int64(st, 7);

        if (sqlite3_column_type(st, 8) != SQLITE_NULL) r.attempt_count = static_cast<int32_t>(sqlite3_column_int64(st, 8));
        else r.attempt_count = 0;

        r.last_error = text_or_empty(st, 3);

        if (sqlite3_column_type(st, 10) != SQLITE_NULL) r.priority = static_cast<int32_t>(sqlite3_column_int64(st, 10));
        else r.priority = 0;

        if (sqlite3_column_type(st, 11) != SQLITE_NULL) r.output_savestate_id = sqlite3_column_int64(st, 11);
        return r;
    }

    static DbResult<TasMovieRow> Impl_Get(DbEnv& env, int64_t id) {
        sqlite3* db = env.handle();
        sqlite3_stmt* st{};
        int rc = sqlite3_prepare_v2(db,
            "SELECT id, base_file_id, new_rtc, status, progress_log, created_at, started_at, "
            "completed_at, attempt_count, last_error, priority, output_savestate_id "
            "FROM tas_movie WHERE id=? LIMIT 1;",
            -1, &st, nullptr);
        if (rc != SQLITE_OK) return DbResult<TasMovieRow>::Err({ map_sqlite_err(rc), rc, "prepare" });
        sqlite3_bind_int64(st, 1, id);
        rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            TasMovieRow row = ReadRow(st);
            sqlite3_finalize(st);
            return DbResult<TasMovieRow>::Ok(row);
        }
        sqlite3_finalize(st);
        return DbResult<TasMovieRow>::Err({ DbErrorKind::NotFound, SQLITE_DONE, "not found" });
    }

    static DbResult<std::vector<TasMovieRow>> Impl_ListByStatus(DbEnv& env, const char* status) {
        sqlite3* db = env.handle();
        sqlite3_stmt* st{};
        int rc = sqlite3_prepare_v2(db,
            "SELECT id, base_file_id, new_rtc, status, progress_log, created_at, started_at, "
            "completed_at, attempt_count, last_error, priority, output_savestate_id "
            "FROM tas_movie WHERE status=? ORDER BY priority DESC, id ASC;",
            -1, &st, nullptr);
        if (rc != SQLITE_OK) return DbResult<std::vector<TasMovieRow>>::Err({ map_sqlite_err(rc), rc, "prepare" });
        sqlite3_bind_text(st, 1, status, -1, SQLITE_STATIC);
        std::vector<TasMovieRow> out;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            out.push_back(ReadRow(st));
        }
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) return DbResult<std::vector<TasMovieRow>>::Err({ map_sqlite_err(rc), rc, "select" });
        return DbResult<std::vector<TasMovieRow>>::Ok(std::move(out));
    }

    // Async wrappers via DBService

    std::future<DbResult<int64_t>> TasMovieRepo::EnqueueAsync(int64_t base_file_id, int64_t new_rtc, int32_t priority, RetryPolicy rp) {
        return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::Normal, rp,
            [=](DbEnv& e) { return Impl_Enqueue(e, base_file_id, new_rtc, priority); });
    }

    std::future<DbResult<int64_t>> TasMovieRepo::IdempotentEnqueueAsync(int64_t base_file_id, int64_t new_rtc, int32_t priority, RetryPolicy rp) {
        return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::Normal, rp,
            [=](DbEnv& e) { return Impl_IdempotentEnqueue(e, base_file_id, new_rtc, priority); });
    }

    std::future<DbResult<void>> TasMovieRepo::MarkRunningAsync(int64_t id, RetryPolicy rp) {
        return DBService::instance().submit_res<void>(OpType::Write, Priority::High, rp,
            [=](DbEnv& e) { return Impl_MarkRunning(e, id); });
    }

    std::future<DbResult<void>> TasMovieRepo::AppendProgressAsync(int64_t id, std::string text, int max_bytes, RetryPolicy rp) {
        return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
            [id, text = std::move(text), max_bytes](DbEnv& e) { return Impl_AppendProgress(e, id, text, max_bytes); });
    }

    std::future<DbResult<void>> TasMovieRepo::MarkFailedAsync(int64_t id, std::string err, RetryPolicy rp) {
        return DBService::instance().submit_res<void>(OpType::Write, Priority::Normal, rp,
            [id, err = std::move(err)](DbEnv& e) { return Impl_MarkFailed(e, id, err); });
    }

    std::future<DbResult<void>> TasMovieRepo::MarkDoneAsync(int64_t id, int64_t savestate_id, RetryPolicy rp) {
        return DBService::instance().submit_res<void>(OpType::Write, Priority::High, rp,
            [=](DbEnv& e) { return Impl_MarkDone(e, id, savestate_id); });
    }

    std::future<DbResult<TasMovieRow>> TasMovieRepo::GetAsync(int64_t id, RetryPolicy rp) {
        return DBService::instance().submit_res<TasMovieRow>(OpType::Read, Priority::Normal, rp,
            [=](DbEnv& e) { return Impl_Get(e, id); });
    }

    std::future<DbResult<std::vector<TasMovieRow>>> TasMovieRepo::ListPlannedAsync(RetryPolicy rp) {
        return DBService::instance().submit_res<std::vector<TasMovieRow>>(OpType::Read, Priority::Normal, rp,
            [](DbEnv& e) { return Impl_ListByStatus(e, "planned"); });
    }

    std::future<DbResult<std::vector<TasMovieRow>>> TasMovieRepo::ListActiveAsync(RetryPolicy rp) {
        return DBService::instance().submit_res<std::vector<TasMovieRow>>(OpType::Read, Priority::Normal, rp,
            [](DbEnv& e) { return Impl_ListByStatus(e, "running"); });
    }

    static inline DbResult<Page<TasMovieLite>> Impl_ListTasMovie(DbEnv& env, const PagedQuery<>& q, const std::string& search, bool only_done) {
        sqlite3* db = env.handle();
        std::string sql = "SELECT id,base_file_id,new_rtc,status,created_at,started_at,completed_at FROM tas_movie ";
        std::string where;
        if (only_done) { where += (where.empty() ? "WHERE " : " AND "); where += "status='done'"; }
        bool s_is_num = false; int64_t s_num = 0;
        try { if (!search.empty()) { s_num = std::stoll(search); s_is_num = true; } }
        catch (...) {}
        if (s_is_num) { where += (where.empty() ? "WHERE " : " AND "); where += "new_rtc=?"; }
        std::string keyset; KeysetCursor cur{}; bool has_cursor = false;
        if (q.before) { has_cursor = true; cur = *q.before; where += (where.empty() ? "WHERE " : " AND "); where += "(created_at < ? OR (created_at = ? AND id < ?))"; }
        if (q.after) { has_cursor = true; cur = *q.after;  where += (where.empty() ? "WHERE " : " AND "); where += "(created_at > ? OR (created_at = ? AND id > ?))"; }
        std::string order = q.after ? " ORDER BY created_at ASC, id ASC " : " ORDER BY created_at DESC, id DESC ";
        sqlite3_stmt* st{};
        std::string final = sql + where + order + " LIMIT ?;";
        if (sqlite3_prepare_v2(db, final.c_str(), -1, &st, nullptr) != SQLITE_OK)
            return DbResult<Page<TasMovieLite>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
        int b = 1;
        if (s_is_num) sqlite3_bind_int64(st, b++, s_num);
        if (q.before) { sqlite3_bind_int64(st, b++, cur.primary); sqlite3_bind_int64(st, b++, cur.primary); sqlite3_bind_int64(st, b++, cur.secondary); }
        if (q.after) { sqlite3_bind_int64(st, b++, cur.primary); sqlite3_bind_int64(st, b++, cur.primary); sqlite3_bind_int64(st, b++, cur.secondary); }
        sqlite3_bind_int(st, b++, q.limit);
        Page<TasMovieLite> page{};
        while (sqlite3_step(st) == SQLITE_ROW) {
            TasMovieLite r{};
            r.id = sqlite3_column_int64(st, 0);
            r.base_file_id = sqlite3_column_int64(st, 1);
            if (sqlite3_column_type(st, 2) != SQLITE_NULL) r.new_rtc = sqlite3_column_int64(st, 2);
            r.status = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
            if (sqlite3_column_type(st, 4) != SQLITE_NULL) r.created_at = sqlite3_column_int64(st, 4);
            if (sqlite3_column_type(st, 5) != SQLITE_NULL) r.started_at = sqlite3_column_int64(st, 5);
            if (sqlite3_column_type(st, 6) != SQLITE_NULL) r.completed_at = sqlite3_column_int64(st, 6);
            page.items.push_back(std::move(r));
        }
        sqlite3_finalize(st);
        if (q.after && !page.items.empty()) std::reverse(page.items.begin(), page.items.end());
        if (!page.items.empty()) {
            int64_t fprim = page.items.front().created_at.value_or(0);
            int64_t sprim = page.items.back().created_at.value_or(0);
            page.prev = KeysetCursor{ fprim, page.items.front().id };
            page.next = KeysetCursor{ sprim, page.items.back().id };
        }
        return DbResult<Page<TasMovieLite>>::Ok(std::move(page));
    }
    std::future<DbResult<Page<TasMovieLite>>> TasMovieRepo::ListPagedAsync(const PagedQuery<>& q, const std::string& search, bool only_done, RetryPolicy rp) {
        return DBService::instance().submit_res<Page<TasMovieLite>>(OpType::Read, Priority::Normal, rp,
            [=](DbEnv& e) { return Impl_ListTasMovie(e, q, search, only_done); });
    }

} // namespace simcore::db
