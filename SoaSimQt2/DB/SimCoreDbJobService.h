#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "SimCoreDbRuntime.h"
#include "DB/Querying/DataService.h"
#include "Execution/IExecutionDb.h"
#include "UIRead/IUiReadDb.h"

namespace soasimqt2::db {

class SimCoreDbJobService {
public:
    static simcore::db::DbResult<std::vector<simcore::db::ProgramKindKV>> ListProgramKinds() {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<std::vector<simcore::db::ProgramKindKV>>("legacy SimCore/DB path is temporarily unavailable in this Qt2 migration slice");
        }

        std::vector<simcore::db::ProgramKindKV> out;
        for (const auto& row : db->ListProgramKinds()) {
            out.push_back({ row.id, row.name });
        }
        return simcore::db::DbResult<std::vector<simcore::db::ProgramKindKV>>::Ok(std::move(out));
    }

    static simcore::db::DbResult<Page<JobLite>> FetchJobsPage(
        const JobsListScope& scope,
        std::optional<KeysetCursor> before,
        std::optional<KeysetCursor> after,
        int limit) {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<Page<JobLite>>("legacy SimCore/DB path is temporarily unavailable in this Qt2 migration slice");
        }

        simcore::db::UiReadJobListQuery query{};
        query.limit = limit;
        query.states = scope.states;
        query.program_kind = scope.program_kind;
        query.job_set_id = scope.job_set_id;
        if (before.has_value()) {
            query.before = { before->primary, before->secondary };
        }
        if (after.has_value()) {
            query.after = { after->primary, after->secondary };
        }

        const auto page = db->ListJobs(query);
        Page<JobLite> out{};
        out.items.reserve(page.items.size());
        for (const auto& row : page.items) {
            JobLite job{};
            job.job_id = row.job_id;
            job.job_set_id = row.job_set_id;
            job.program_kind = row.program_kind;
            job.state = row.state;
            job.priority = row.priority;
            job.attempts = row.attempts;
            job.queued_at = row.queued_at_utc;
            out.items.push_back(std::move(job));
        }
        if (page.next.has_value()) {
            out.next = { page.next->primary, page.next->secondary };
        }
        if (page.prev.has_value()) {
            out.prev = { page.prev->primary, page.prev->secondary };
        }
        return simcore::db::DbResult<Page<JobLite>>::Ok(std::move(out));
    }

    static simcore::db::DbResult<Page<JobEventLite>> FetchJobEventsPage(const JobEventsListScope& scope, const PagedQuery<>& query) {
        auto* db = ExecutionDb();
        if (db == nullptr) {
            return Unavailable<Page<JobEventLite>>("legacy SimCore/DB path is temporarily unavailable in this Qt2 migration slice");
        }
        if (!scope.job_id.has_value() || *scope.job_id <= 0) {
            return Invalid<Page<JobEventLite>>("job_id is required");
        }

        const int limit = (std::max)(1, query.limit);
        Page<JobEventLite> out{};
        for (const auto& row : db->ListJobEvents(*scope.job_id, limit)) {
            if (scope.event_kind.has_value() && row.event_kind != *scope.event_kind) {
                continue;
            }
            if (scope.since_ts.has_value() && row.event_ts_utc < *scope.since_ts) {
                continue;
            }
            JobEventLite event{};
            event.event_id = row.job_event_id;
            event.job_id = row.job_id;
            event.ts = row.event_ts_utc;
            event.event_kind = row.event_kind;
            if (!row.message.empty()) {
                event.payload_preview = row.message;
            }
            out.items.push_back(std::move(event));
        }
        return simcore::db::DbResult<Page<JobEventLite>>::Ok(std::move(out));
    }

    static simcore::db::DbResult<std::vector<simcore::db::JobEventsRepo::JobIdPayload>> BulkLatestProgressByJobs(const std::vector<std::int64_t>& ids) {
        std::vector<simcore::db::JobEventsRepo::JobIdPayload> out;
        out.reserve(ids.size());
        for (const auto id : ids) {
            out.push_back({ id, std::nullopt });
        }
        return simcore::db::DbResult<std::vector<simcore::db::JobEventsRepo::JobIdPayload>>::Ok(std::move(out));
    }

    static simcore::db::DbResult<std::vector<simcore::db::ArtifactRefLite>> FetchJobArtifactRefs(std::int64_t job_id) {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<std::vector<simcore::db::ArtifactRefLite>>("legacy SimCore/DB path is temporarily unavailable in this Qt2 migration slice");
        }

        std::vector<simcore::db::ArtifactRefLite> out;
        for (const auto& row : db->ListJobArtifacts(job_id)) {
            simcore::db::ArtifactRefLite item{};
            item.artifact_id = row.artifact_id;
            item.role = row.role_kind;
            item.filename = row.filename;
            item.size_bytes = row.size_bytes;
            item.created_at = row.created_at_utc;
            out.push_back(std::move(item));
        }
        return simcore::db::DbResult<std::vector<simcore::db::ArtifactRefLite>>::Ok(std::move(out));
    }

    static simcore::db::DbResult<std::string> FetchJobInputIni(std::int64_t job_id) {
        auto* db = ExecutionDb();
        if (db == nullptr) {
            return Unavailable<std::string>("legacy SimCore/DB path is temporarily unavailable in this Qt2 migration slice");
        }

        std::string error;
        const auto input_ini = db->GetJobInputIni(job_id, &error);
        if (!input_ini.has_value()) {
            return simcore::db::DbResult<std::string>::Err({ simcore::db::DbErrorKind::NotFound, 0, error.empty() ? "job not found" : std::move(error) });
        }
        return simcore::db::DbResult<std::string>::Ok(*input_ini);
    }

    static simcore::db::DbResult<void> RequeueJob(std::int64_t job_id) {
        auto* db = ExecutionDb();
        if (db == nullptr) {
            return UnavailableVoid("legacy SimCore/DB path is temporarily unavailable in this Qt2 migration slice");
        }
        std::string error;
        if (!db->RequeueJob(job_id, &error)) {
            return FailedVoid(error.empty() ? "requeue failed" : std::move(error));
        }
        return simcore::db::DbResult<void>::Ok();
    }

    static simcore::db::DbResult<void> RestartFailedJob(std::int64_t job_id, std::optional<std::string> input_ini_override = std::nullopt) {
        auto* db = ExecutionDb();
        if (db == nullptr) {
            return UnavailableVoid("legacy SimCore/DB path is temporarily unavailable in this Qt2 migration slice");
        }
        std::string error;
        if (!db->RestartFailedJob(job_id, std::move(input_ini_override), &error)) {
            return FailedVoid(error.empty() ? "restart failed" : std::move(error));
        }
        return simcore::db::DbResult<void>::Ok();
    }

    static simcore::db::DbResult<void> CancelJob(std::int64_t job_id) {
        auto* db = ExecutionDb();
        if (db == nullptr) {
            return UnavailableVoid("legacy SimCore/DB path is temporarily unavailable in this Qt2 migration slice");
        }
        std::string error;
        if (!db->CancelQueuedOrClaimedJob(job_id, &error)) {
            return FailedVoid(error.empty() ? "cancel failed" : std::move(error));
        }
        return simcore::db::DbResult<void>::Ok();
    }

private:
    static simcore::db::IUiReadDb* UiReadDb() {
        return soasimqt2::SimCoreDbRuntime::instance().uiReadDb();
    }

    static simcore::db::IExecutionDb* ExecutionDb() {
        return soasimqt2::SimCoreDbRuntime::instance().executionDb();
    }

    template <typename T>
    static simcore::db::DbResult<T> Unavailable(std::string message) {
        return simcore::db::DbResult<T>::Err({ simcore::db::DbErrorKind::Unavailable, 0, std::move(message) });
    }

    template <typename T>
    static simcore::db::DbResult<T> Invalid(std::string message) {
        return simcore::db::DbResult<T>::Err({ simcore::db::DbErrorKind::InvalidState, 0, std::move(message) });
    }

    static simcore::db::DbResult<void> UnavailableVoid(std::string message) {
        return simcore::db::DbResult<void>::Err({ simcore::db::DbErrorKind::Unavailable, 0, std::move(message) });
    }

    static simcore::db::DbResult<void> FailedVoid(std::string message) {
        return simcore::db::DbResult<void>::Err({ simcore::db::DbErrorKind::Unknown, 0, std::move(message) });
    }
};

} // namespace soasimqt2::db

