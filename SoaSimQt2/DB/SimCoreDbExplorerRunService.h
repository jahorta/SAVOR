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

struct ExplorerRunCursor {
    std::int64_t primary = 0;
    std::int64_t secondary = 0;
};

struct ExplorerRunGroupQuery {
    std::optional<ExplorerRunCursor> before;
    std::optional<ExplorerRunCursor> after;
    int limit = 50;
};

struct ExplorerRunGroupPage {
    std::vector<simcore::db::UiJobSetSummary> groups;
    std::optional<ExplorerRunCursor> next;
    std::optional<ExplorerRunCursor> prev;
};

struct ExplorerRunJobDetail {
    simcore::db::UiJobDetail job;
    std::vector<simcore::db::UiJobArtifact> artifacts;
    std::vector<simcore::db::ExecutionJobEventRecord> events;
};

class SimCoreDbExplorerRunService {
public:
    static constexpr int kBattleSingleTurnProgramKind = 5;

    static ServiceResult<ExplorerRunGroupPage> ListGroups(const ExplorerRunGroupQuery& request) {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<ExplorerRunGroupPage>("SimCoreDB UIRead is unavailable");
        }

        simcore::db::UiReadJobSetListQuery query{};
        query.program_kind = kBattleSingleTurnProgramKind;
        query.limit = (std::max)(1, request.limit);
        if (request.before.has_value()) {
            query.before = simcore::db::UiReadListCursor{ request.before->primary, request.before->secondary };
        }
        if (request.after.has_value()) {
            query.after = simcore::db::UiReadListCursor{ request.after->primary, request.after->secondary };
        }

        const auto page = db->ListJobSets(query);
        ExplorerRunGroupPage out{};
        out.groups = page.items;
        if (page.next.has_value()) {
            out.next = ExplorerRunCursor{ page.next->primary, page.next->secondary };
        }
        if (page.prev.has_value()) {
            out.prev = ExplorerRunCursor{ page.prev->primary, page.prev->secondary };
        }
        return ServiceResult<ExplorerRunGroupPage>::Ok(std::move(out));
    }

    static ServiceResult<simcore::db::UiJobSetDetail> GetGroupDetail(std::int64_t job_set_id, int jobs_limit) {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<simcore::db::UiJobSetDetail>("SimCoreDB UIRead is unavailable");
        }
        const auto detail = db->GetJobSetDetail(job_set_id, (std::max)(1, jobs_limit));
        if (!detail.has_value()) {
            return NotFound<simcore::db::UiJobSetDetail>("explorer run job set not found");
        }
        return ServiceResult<simcore::db::UiJobSetDetail>::Ok(*detail);
    }

    static ServiceResult<ExplorerRunJobDetail> GetJobDetail(std::int64_t job_id) {
        auto* ui_read = UiReadDb();
        if (ui_read == nullptr) {
            return Unavailable<ExplorerRunJobDetail>("SimCoreDB UIRead is unavailable");
        }

        const auto job = ui_read->GetJobDetail(job_id);
        if (!job.has_value()) {
            return NotFound<ExplorerRunJobDetail>("job not found");
        }

        ExplorerRunJobDetail out{};
        out.job = *job;
        out.artifacts = ui_read->ListJobArtifacts(job_id);

        if (auto* execution = ExecutionDb(); execution != nullptr) {
            out.events = execution->ListJobEvents(job_id, 128);
        }

        return ServiceResult<ExplorerRunJobDetail>::Ok(std::move(out));
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
    static ServiceResult<T> NotFound(std::string message) {
        return ServiceResult<T>::Err({ ServiceErrorKind::NotFound, std::move(message) });
    }
};

} // namespace soasimqt2::db
