// SimCore/DB/JobEventsRepo.cpp
#include "JobEventsRepo.h"
#include <sqlite3.h>

namespace simcore::db {

    static DbResult<int64_t> impl_append(DbEnv& env, int64_t job_id, const std::string& kind, const std::optional<std::string>& payload) {
        auto* db = env.handle();
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO job_events(job_id, ts, event_kind, payload)"
            " VALUES(?, strftime('%s','now'), ?, ?) RETURNING event_id", -1, &st, nullptr) != SQLITE_OK) {
            return DbResult<int64_t>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
        }
        sqlite3_bind_int64(st, 1, job_id);
        sqlite3_bind_text(st, 2, kind.c_str(), -1, SQLITE_TRANSIENT);
        if (payload && !payload->empty()) sqlite3_bind_text(st, 3, payload->c_str(), -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(st, 3);

        int64_t id = 0;
        if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        if (!id) return DbResult<int64_t>::Err({DbErrorKind::IO, sqlite3_errcode(db), "failed to insert job_event" });
        return DbResult<int64_t>::Ok(id);
    }

    std::future<DbResult<int64_t>> JobEventsRepo::AppendAsync(int64_t job_id, std::string kind, std::optional<std::string> payload, RetryPolicy rp) {
        return DBService::instance().submit_res<int64_t>(OpType::Write, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_append(e, job_id, kind, payload); });
    }

    static DbResult<std::optional<std::string>> impl_first(DbEnv& env, int64_t job_id, const std::string& kind) {
        auto* db = env.handle();
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db,
            "SELECT payload FROM job_events WHERE job_id=? AND event_kind=? ORDER BY event_id LIMIT 1", -1, &st, nullptr) != SQLITE_OK) {
            return DbResult<std::optional<std::string>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
        }
        sqlite3_bind_int64(st, 1, job_id);
        sqlite3_bind_text(st, 2, kind.c_str(), -1, SQLITE_TRANSIENT);
        std::optional<std::string> out{};
        if (sqlite3_step(st) == SQLITE_ROW) {
            if (sqlite3_column_type(st, 0) != SQLITE_NULL) out = std::string((const char*)sqlite3_column_text(st, 0));
        }
        sqlite3_finalize(st);
        return DbResult<std::optional<std::string>>::Ok(std::move(out));
    }

    std::future<DbResult<std::optional<std::string>>> JobEventsRepo::GetFirstPayloadAsync(int64_t job_id, std::string kind, RetryPolicy rp) {
        return DBService::instance().submit_res<std::optional<std::string>>(OpType::Read, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_first(e, job_id, kind); });
    }

    static DbResult<std::optional<std::string>> impl_latest(DbEnv& env, int64_t job_id, const std::string& kind) {
        auto* db = env.handle();
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db,
            "SELECT payload FROM job_events WHERE job_id=? AND event_kind=? ORDER BY event_id DESC LIMIT 1", -1, &st, nullptr) != SQLITE_OK) {
            return DbResult<std::optional<std::string>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
        }
        sqlite3_bind_int64(st, 1, job_id);
        sqlite3_bind_text(st, 2, kind.c_str(), -1, SQLITE_TRANSIENT);
        std::optional<std::string> out{};
        if (sqlite3_step(st) == SQLITE_ROW) {
            if (sqlite3_column_type(st, 0) != SQLITE_NULL) out = std::string((const char*)sqlite3_column_text(st, 0));
        }
        sqlite3_finalize(st);
        return DbResult<std::optional<std::string>>::Ok(std::move(out));
    }

    std::future<DbResult<std::optional<std::string>>> JobEventsRepo::GetLatestPayloadAsync(int64_t job_id, std::string kind, RetryPolicy rp) {
        return DBService::instance().submit_res<std::optional<std::string>>(OpType::Read, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_latest(e, job_id, kind); });
    }

    static DbResult<std::vector<JobEventRow>> impl_list_set(DbEnv& env, int64_t job_set_id, const std::string& kind) {
        auto* db = env.handle();
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db,
            "SELECT je.event_id, je.job_id, je.ts, je.event_kind, je.payload"
            " FROM job_events je"
            " JOIN jobs j ON j.job_id = je.job_id"
            " WHERE j.job_set_id=? AND je.event_kind=?"
            " ORDER BY je.event_id", -1, &st, nullptr) != SQLITE_OK) {
            return DbResult<std::vector<JobEventRow>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
        }
        sqlite3_bind_int64(st, 1, job_set_id);
        sqlite3_bind_text(st, 2, kind.c_str(), -1, SQLITE_TRANSIENT);
        std::vector<JobEventRow> v;
        while (sqlite3_step(st) == SQLITE_ROW) {
            JobEventRow e{};
            e.event_id = sqlite3_column_int64(st, 0);
            e.job_id = sqlite3_column_int64(st, 1);
            e.ts = sqlite3_column_int64(st, 2);
            e.event_kind = (const char*)sqlite3_column_text(st, 3);
            if (sqlite3_column_type(st, 4) != SQLITE_NULL) e.payload = std::string((const char*)sqlite3_column_text(st, 4));
            v.push_back(std::move(e));
        }
        sqlite3_finalize(st);
        return DbResult<std::vector<JobEventRow>>::Ok(std::move(v));
    }

    std::future<DbResult<std::vector<JobEventRow>>> JobEventsRepo::ListByJobSetAndKindAsync(int64_t job_set_id, std::string kind, RetryPolicy rp) {
        return DBService::instance().submit_res<std::vector<JobEventRow>>(OpType::Read, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_list_set(e, job_set_id, kind); });
    }

    static DbResult<std::vector<JobEventRow>> impl_list_job(DbEnv& env, int64_t job_id, const std::string& kind) {
        auto* db = env.handle();
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db,
            "SELECT event_id, job_id, ts, event_kind, payload"
            " FROM job_events WHERE job_id=? AND event_kind=?"
            " ORDER BY event_id", -1, &st, nullptr) != SQLITE_OK) {
            return DbResult<std::vector<JobEventRow>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
        }
        sqlite3_bind_int64(st, 1, job_id);
        sqlite3_bind_text(st, 2, kind.c_str(), -1, SQLITE_TRANSIENT);
        std::vector<JobEventRow> v;
        while (sqlite3_step(st) == SQLITE_ROW) {
            JobEventRow e{};
            e.event_id = sqlite3_column_int64(st, 0);
            e.job_id = sqlite3_column_int64(st, 1);
            e.ts = sqlite3_column_int64(st, 2);
            e.event_kind = (const char*)sqlite3_column_text(st, 3);
            if (sqlite3_column_type(st, 4) != SQLITE_NULL) e.payload = std::string((const char*)sqlite3_column_text(st, 4));
            v.push_back(std::move(e));
        }
        sqlite3_finalize(st);
        return DbResult<std::vector<JobEventRow>>::Ok(std::move(v));
    }

    std::future<DbResult<std::vector<JobEventRow>>> JobEventsRepo::ListByJobAndKindAsync(int64_t job_id, std::string kind, RetryPolicy rp) {
        return DBService::instance().submit_res<std::vector<JobEventRow>>(OpType::Read, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_list_job(e, job_id, kind); });
    }

} // namespace simcore::db
