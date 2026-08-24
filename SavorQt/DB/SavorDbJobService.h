#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "SavorDbRuntime.h"
#include "DB/SavorDbServiceResult.h"
#include "Execution/IExecutionDb.h"
#include "UIRead/IUiReadDb.h"

namespace savorqt::db {

class SavorDbJobService {
public:
    static ServiceResult<std::vector<savor::db::UiProgramKind>> ListProgramKinds() {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<std::vector<savor::db::UiProgramKind>>("SavorDb UIRead is unavailable");
        }

        return ServiceResult<std::vector<savor::db::UiProgramKind>>::Ok(db->ListProgramKinds());
    }

    static ServiceResult<savor::db::UiReadPage<savor::db::UiJobSummary>> FetchJobsPage(
        const savor::db::UiReadJobListQuery& scope,
        std::optional<savor::db::UiReadListCursor> before,
        std::optional<savor::db::UiReadListCursor> after,
        int limit) {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<savor::db::UiReadPage<savor::db::UiJobSummary>>("SavorDb UIRead is unavailable");
        }

        savor::db::UiReadJobListQuery query{};
        query.limit = limit;
        query.states = scope.states;
        query.program_kind = scope.program_kind;
        query.job_set_id = scope.job_set_id;
        query.job_id = scope.job_id;
        if (after.has_value()) {
            query.after = after;
        }
        if (before.has_value()) {
            query.before = before;
        }

        return ServiceResult<savor::db::UiReadPage<savor::db::UiJobSummary>>::Ok(db->ListJobs(query));
    }

    static ServiceResult<savor::db::UiJobStateCounts> CountJobsByState(
        const savor::db::UiReadJobListQuery& scope = {}) {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<savor::db::UiJobStateCounts>("SavorDb UIRead is unavailable");
        }

        return ServiceResult<savor::db::UiJobStateCounts>::Ok(db->CountJobsByState(scope));
    }

    static ServiceResult<std::vector<savor::db::ExecutionJobEventRecord>> FetchJobEvents(std::int64_t job_id, int limit) {
        auto* db = ExecutionDb();
        if (db == nullptr) {
            return Unavailable<std::vector<savor::db::ExecutionJobEventRecord>>("SavorDb execution DB is unavailable");
        }
        if (job_id <= 0) {
            return Invalid<std::vector<savor::db::ExecutionJobEventRecord>>("job_id is required");
        }

        return ServiceResult<std::vector<savor::db::ExecutionJobEventRecord>>::Ok(
            db->ListJobEvents(job_id, (std::max)(1, limit)));
    }

    static ServiceResult<std::vector<savor::db::UiJobArtifact>> FetchJobArtifactRefs(std::int64_t job_id) {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<std::vector<savor::db::UiJobArtifact>>("SavorDb UIRead is unavailable");
        }

        return ServiceResult<std::vector<savor::db::UiJobArtifact>>::Ok(db->ListJobArtifacts(job_id));
    }

    static ServiceResult<std::vector<savor::db::UiCanonicalJobProgress>>
    FetchJobProgress(std::int64_t job_id, int limit = 128) {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<std::vector<savor::db::UiCanonicalJobProgress>>(
                "SavorDb UIRead is unavailable");
        }
        if (job_id <= 0) {
            return Invalid<std::vector<savor::db::UiCanonicalJobProgress>>(
                "job_id is required");
        }
        return ServiceResult<std::vector<savor::db::UiCanonicalJobProgress>>::Ok(
            db->ListJobProgress(job_id, (std::max)(1, limit)));
    }

    static ServiceResult<std::string> FetchJobInputIni(std::int64_t job_id) {
        auto* db = ExecutionDb();
        if (db == nullptr) {
            return Unavailable<std::string>("SavorDb execution DB is unavailable");
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
            return UnavailableVoid("SavorDb execution DB is unavailable");
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
            return UnavailableVoid("SavorDb execution DB is unavailable");
        }
        std::string error;
        if (!db->RestartFailedJob(job_id, std::move(input_ini_override), &error)) {
            return FailedVoid(error.empty() ? "restart failed" : std::move(error));
        }
        return ServiceResult<void>::Ok();
    }

private:
    static savor::db::IUiReadDb* UiReadDb() {
        return savorqt::SavorDbRuntime::instance().uiReadDb();
    }

    static savor::db::IExecutionDb* ExecutionDb() {
        return savorqt::SavorDbRuntime::instance().executionDb();
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

} // namespace savorqt::db

