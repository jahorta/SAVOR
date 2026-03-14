// SimCore/DB/JobEventsRepo.cpp
#include "JobEventsRepo.h"
#include <sqlite3.h>
#include <sstream>

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


    static DbResult<std::vector<JobEventRow>> impl_list_set_tree(DbEnv& env, int64_t root_job_set_id, const std::string& kind) {
        auto* db = env.handle();
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db,
            "WITH RECURSIVE tree(job_set_id) AS ("
            "  SELECT ?"
            "  UNION ALL"
            "  SELECT js.job_set_id FROM job_sets js JOIN tree t ON js.parent_job_set_id = t.job_set_id"
            ")"
            "SELECT je.event_id, je.job_id, je.ts, je.event_kind, je.payload"
            " FROM job_events je"
            " JOIN jobs j ON j.job_id = je.job_id"
            " JOIN tree t ON t.job_set_id = j.job_set_id"
            " WHERE je.event_kind=?"
            " ORDER BY je.event_id", -1, &st, nullptr) != SQLITE_OK) {
            return DbResult<std::vector<JobEventRow>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
        }
        sqlite3_bind_int64(st, 1, root_job_set_id);
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

    std::future<DbResult<std::vector<JobEventRow>>> JobEventsRepo::ListByJobSetTreeAndKindAsync(int64_t root_job_set_id, std::string kind, RetryPolicy rp) {
        return DBService::instance().submit_res<std::vector<JobEventRow>>(OpType::Read, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_list_set_tree(e, root_job_set_id, kind); });
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
    
    static DbResult<Page<JobEventLite>> impl_list_events_time(
        DbEnv& env,
        const JobEventsListScope& scope,
        const std::optional<KeysetCursor>& before,
        int limit)
    {
        auto* db = env.handle();
        std::ostringstream sql;
        // Use substr(payload,1,128) to avoid pulling huge blobs
        sql << "SELECT je.event_id, je.job_id, je.ts, je.event_kind, "
            "CASE WHEN je.payload IS NULL THEN NULL ELSE substr(je.payload,1,128) END AS payload_preview "
            "FROM job_events je ";

        bool joined_jobs = false;
        bool hasWhere = false;
        auto add_and = [&](bool cond) { if (cond) { sql << (hasWhere ? " AND " : " WHERE "); hasWhere = true; } };

        if (scope.job_set_id) {
            sql << "JOIN jobs j ON j.job_id = je.job_id ";
            joined_jobs = true;
        }

        if (scope.event_kind) { add_and(true); sql << "je.event_kind = ?"; }
        if (scope.job_id) { add_and(true); sql << "je.job_id = ?"; }
        if (scope.job_set_id) { add_and(true); sql << "j.job_set_id = ?"; }
        if (scope.since_ts) { add_and(true); sql << "je.ts >= ?"; }
        if (before) {
            add_and(true);
            sql << "(je.ts < ? OR (je.ts = ? AND je.event_id < ?))";
        }

        sql << " ORDER BY je.ts DESC, je.event_id DESC LIMIT ?";

        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, sql.str().c_str(), -1, &st, nullptr) != SQLITE_OK) {
            return DbResult<Page<JobEventLite>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), sqlite3_errmsg(db) });
        }

        int bi = 1;
        if (scope.event_kind) sqlite3_bind_text(st, bi++, scope.event_kind->c_str(), -1, SQLITE_TRANSIENT);
        if (scope.job_id)     sqlite3_bind_int64(st, bi++, *scope.job_id);
        if (scope.job_set_id) sqlite3_bind_int64(st, bi++, *scope.job_set_id);
        if (scope.since_ts)   sqlite3_bind_int64(st, bi++, *scope.since_ts);
        if (before) {
            sqlite3_bind_int64(st, bi++, before->primary);   // ts
            sqlite3_bind_int64(st, bi++, before->primary);   // ts (tie)
            sqlite3_bind_int64(st, bi++, before->secondary); // event_id
        }
        sqlite3_bind_int(st, bi++, limit);

        Page<JobEventLite> page{};
        page.items.reserve(static_cast<size_t>(limit));
        while (true) {
            int rc = sqlite3_step(st);
            if (rc == SQLITE_ROW) {
                JobEventLite e{};
                e.event_id = sqlite3_column_int64(st, 0);
                e.job_id = sqlite3_column_int64(st, 1);
                e.ts = sqlite3_column_int64(st, 2);
                e.event_kind = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
                if (sqlite3_column_type(st, 4) != SQLITE_NULL)
                    e.payload_preview = std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 4)));
                page.items.push_back(std::move(e));
            }
            else if (rc == SQLITE_DONE) {
                break;
            }
            else {
                auto err = DbResult<Page<JobEventLite>>::Err({ map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), "step failed" });
                sqlite3_finalize(st);
                return err;
            }
        }

        sqlite3_finalize(st);

        if ((int)page.items.size() == limit) {
            const auto& last = page.items.back();
            page.next = KeysetCursor{ last.ts, last.event_id };
        }
        return DbResult<Page<JobEventLite>>::Ok(std::move(page));
    }

    std::future<DbResult<Page<JobEventLite>>> JobEventsRepo::ListPagedByTimeAsync(
        const JobEventsListScope& scope,
        std::optional<KeysetCursor> before,
        int limit,
        RetryPolicy rp)
    {
        if (limit <= 0) limit = 50;
        return DBService::instance().submit_res<Page<JobEventLite>>(OpType::Read, Priority::Normal, rp,
            [=](DbEnv& e) { return impl_list_events_time(e, scope, before, limit); });
    }

    static DbResult<std::vector<JobEventsRepo::JobIdPayload>>
        impl_latest_by_jobs(DbEnv& env, const std::vector<int64_t>& job_ids, const std::string& kind) {
        if (job_ids.empty()) return DbResult<std::vector<JobEventsRepo::JobIdPayload>>::Ok({});

        auto* db = env.handle();
        std::ostringstream sql;
        sql << "SELECT je.job_id, je.payload "
            "FROM job_events je "
            "JOIN ("
            "  SELECT job_id, MAX(event_id) AS max_eid "
            "  FROM job_events "
            "  WHERE event_kind=? AND job_id IN (";
        for (size_t i = 0; i < job_ids.size(); ++i) {
            if (i) sql << ',';
            sql << '?';
        }
        sql << ") GROUP BY job_id"
            ") t ON t.job_id = je.job_id AND t.max_eid = je.event_id "
            "ORDER BY je.job_id";

        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, sql.str().c_str(), -1, &st, nullptr) != SQLITE_OK) {
            int rc = sqlite3_errcode(db);
            return DbResult<std::vector<JobEventsRepo::JobIdPayload>>::Err({ map_sqlite_err(rc), rc, sqlite3_errmsg(db) });
        }

        int bind = 1;
        sqlite3_bind_text(st, bind++, kind.c_str(), -1, SQLITE_TRANSIENT);
        for (auto id : job_ids) sqlite3_bind_int64(st, bind++, id);

        std::vector<JobEventsRepo::JobIdPayload> out;
        for (;;) {
            int rc = sqlite3_step(st);
            if (rc == SQLITE_ROW) {
                JobEventsRepo::JobIdPayload p{};
                p.job_id = sqlite3_column_int64(st, 0);
                if (sqlite3_column_type(st, 1) != SQLITE_NULL)
                    p.payload = std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 1)));
                out.push_back(std::move(p));
            }
            else if (rc == SQLITE_DONE) {
                break;
            }
            else {
                int ec = sqlite3_errcode(db);
                sqlite3_finalize(st);
                return DbResult<std::vector<JobEventsRepo::JobIdPayload>>::Err({ map_sqlite_err(ec), ec, sqlite3_errmsg(db) });
            }
        }
        sqlite3_finalize(st);
        return DbResult<std::vector<JobEventsRepo::JobIdPayload>>::Ok(std::move(out));
    }

    std::future<DbResult<std::vector<JobEventsRepo::JobIdPayload>>> JobEventsRepo::GetLatestPayloadByJobsAsync(
        const std::vector<int64_t>& job_ids, std::string kind, RetryPolicy rp) {
        return DBService::instance().submit_res<std::vector<JobIdPayload>>(OpType::Read, Priority::Normal, rp,
            [ids = job_ids, kind = std::move(kind)](DbEnv& e) { return impl_latest_by_jobs(e, ids, kind); });
    }

} // namespace simcore::db
