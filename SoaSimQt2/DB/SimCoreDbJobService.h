#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "SimCoreDbRuntime.h"
#include "DB/SimCoreDbServiceResult.h"
#include "Execution/IExecutionDb.h"
#include "UIRead/IUiReadDb.h"

namespace soasimqt2::db {

class SimCoreDbJobService {
public:
    static ServiceResult<std::vector<simcore::db::UiProgramKind>> ListProgramKinds() {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<std::vector<simcore::db::UiProgramKind>>("SimCoreDB UIRead is unavailable");
        }

        return ServiceResult<std::vector<simcore::db::UiProgramKind>>::Ok(db->ListProgramKinds());
    }

    static ServiceResult<simcore::db::UiReadPage<simcore::db::UiJobSummary>> FetchJobsPage(
        const simcore::db::UiReadJobListQuery& scope,
        std::optional<simcore::db::UiReadListCursor> before,
        std::optional<simcore::db::UiReadListCursor> after,
        int limit) {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<simcore::db::UiReadPage<simcore::db::UiJobSummary>>("SimCoreDB UIRead is unavailable");
        }

        simcore::db::UiReadJobListQuery query{};
        query.limit = limit;
        query.states = scope.states;
        query.program_kind = scope.program_kind;
        query.job_set_id = scope.job_set_id;
        if (after.has_value()) {
            query.after = after;
        }
        if (before.has_value()) {
            query.before = before;
        }

        return ServiceResult<simcore::db::UiReadPage<simcore::db::UiJobSummary>>::Ok(db->ListJobs(query));
    }

    static ServiceResult<std::vector<simcore::db::ExecutionJobEventRecord>> FetchJobEvents(std::int64_t job_id, int limit) {
        auto* db = ExecutionDb();
        if (db == nullptr) {
            return Unavailable<std::vector<simcore::db::ExecutionJobEventRecord>>("SimCoreDB execution DB is unavailable");
        }
        if (job_id <= 0) {
            return Invalid<std::vector<simcore::db::ExecutionJobEventRecord>>("job_id is required");
        }

        return ServiceResult<std::vector<simcore::db::ExecutionJobEventRecord>>::Ok(
            db->ListJobEvents(job_id, (std::max)(1, limit)));
    }

    static ServiceResult<std::vector<simcore::db::UiJobArtifact>> FetchJobArtifactRefs(std::int64_t job_id) {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<std::vector<simcore::db::UiJobArtifact>>("SimCoreDB UIRead is unavailable");
        }

        return ServiceResult<std::vector<simcore::db::UiJobArtifact>>::Ok(db->ListJobArtifacts(job_id));
    }

    static ServiceResult<std::string> FetchJobInputIni(std::int64_t job_id) {
        auto* db = ExecutionDb();
        if (db == nullptr) {
            return Unavailable<std::string>("SimCoreDB execution DB is unavailable");
        }

        std::string error;
        const auto input_ini = db->GetJobInputIni(job_id, &error);
        if (!input_ini.has_value()) {
            return ServiceResult<std::string>::Err({ ServiceErrorKind::NotFound, error.empty() ? "job not found" : std::move(error) });
        }
        return ServiceResult<std::string>::Ok(*input_ini);
    }

    static ServiceResult<void> RequeueJob(std::int64_t job_id) {
        auto* db = ExecutionDb();
        if (db == nullptr) {
            return UnavailableVoid("SimCoreDB execution DB is unavailable");
        }
        std::string error;
        if (!db->RequeueJob(job_id, &error)) {
            return FailedVoid(error.empty() ? "requeue failed" : std::move(error));
        }
        return ServiceResult<void>::Ok();
    }

    static ServiceResult<void> RestartFailedJob(std::int64_t job_id, std::optional<std::string> input_ini_override = std::nullopt) {
        auto* db = ExecutionDb();
        if (db == nullptr) {
            return UnavailableVoid("SimCoreDB execution DB is unavailable");
        }
        std::string error;
        if (!db->RestartFailedJob(job_id, std::move(input_ini_override), &error)) {
            return FailedVoid(error.empty() ? "restart failed" : std::move(error));
        }
        return ServiceResult<void>::Ok();
    }

    static ServiceResult<void> CancelJob(std::int64_t job_id) {
        auto* db = ExecutionDb();
        if (db == nullptr) {
            return UnavailableVoid("SimCoreDB execution DB is unavailable");
        }
        std::string error;
        if (!db->CancelQueuedOrClaimedJob(job_id, &error)) {
            return FailedVoid(error.empty() ? "cancel failed" : std::move(error));
        }
        return ServiceResult<void>::Ok();
    }

private:
    static simcore::db::IUiReadDb* UiReadDb() {
        return soasimqt2::SimCoreDbRuntime::instance().uiReadDb();
    }

    static simcore::db::IExecutionDb* ExecutionDb() {
        return soasimqt2::SimCoreDbRuntime::instance().executionDb();
    }

    template <typename T>
    static ServiceResult<T> Unavailable(std::string message) {
        return ServiceResult<T>::Err({ ServiceErrorKind::Unavailable, std::move(message) });
    }

    template <typename T>
    static ServiceResult<T> Invalid(std::string message) {
        return ServiceResult<T>::Err({ ServiceErrorKind::InvalidInput, std::move(message) });
    }

    static ServiceResult<void> UnavailableVoid(std::string message) {
        return ServiceResult<void>::Err({ ServiceErrorKind::Unavailable, std::move(message) });
    }

    static ServiceResult<void> FailedVoid(std::string message) {
        return ServiceResult<void>::Err({ ServiceErrorKind::Failed, std::move(message) });
    }
};

} // namespace soasimqt2::db

