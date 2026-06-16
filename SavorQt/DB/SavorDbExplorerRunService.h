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
    std::vector<savor::db::UiJobSetSummary> groups;
    std::optional<ExplorerRunCursor> next;
    std::optional<ExplorerRunCursor> prev;
};

struct ExplorerRunJobDetail {
    savor::db::UiJobDetail job;
    std::vector<savor::db::UiJobArtifact> artifacts;
    std::vector<savor::db::ExecutionJobEventRecord> events;
};

using BattleRunCursor = ExplorerRunCursor;

struct BattleRunGroupQuery {
    std::optional<BattleRunCursor> before;
    std::optional<BattleRunCursor> after;
    int limit = 50;
    bool child_victory_only = false;
};

struct BattleRunGroupPage {
    std::vector<savor::db::UiBattleGroupSummary> groups;
    std::optional<BattleRunCursor> next;
    std::optional<BattleRunCursor> prev;
};

struct BattleRunJobDetail {
    savor::db::UiBattleTurnJobDetail battle;
    std::optional<savor::db::UiJobDetail> job;
    std::vector<savor::db::ExecutionJobEventRecord> events;
};

class SavorDbExplorerRunService {
public:
    static constexpr int kBattleSingleTurnProgramKind = 5;

    static ServiceResult<ExplorerRunGroupPage> ListGroups(const ExplorerRunGroupQuery& request) {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<ExplorerRunGroupPage>("SavorDb UIRead is unavailable");
        }

        savor::db::UiReadJobSetListQuery query{};
        query.program_kind = kBattleSingleTurnProgramKind;
        query.limit = (std::max)(1, request.limit);
        if (request.before.has_value()) {
            query.before = savor::db::UiReadListCursor{ request.before->primary, request.before->secondary };
        }
        if (request.after.has_value()) {
            query.after = savor::db::UiReadListCursor{ request.after->primary, request.after->secondary };
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

    static ServiceResult<savor::db::UiJobSetDetail> GetGroupDetail(std::int64_t job_set_id, int jobs_limit) {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<savor::db::UiJobSetDetail>("SavorDb UIRead is unavailable");
        }
        const auto detail = db->GetJobSetDetail(job_set_id, (std::max)(1, jobs_limit));
        if (!detail.has_value()) {
            return NotFound<savor::db::UiJobSetDetail>("explorer run job set not found");
        }
        return ServiceResult<savor::db::UiJobSetDetail>::Ok(*detail);
    }

    static ServiceResult<ExplorerRunJobDetail> GetJobDetail(std::int64_t job_id) {
        auto* ui_read = UiReadDb();
        if (ui_read == nullptr) {
            return Unavailable<ExplorerRunJobDetail>("SavorDb UIRead is unavailable");
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

    static ServiceResult<BattleRunGroupPage> ListBattleGroups(const BattleRunGroupQuery& request) {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<BattleRunGroupPage>("SavorDb UIRead is unavailable");
        }

        savor::db::UiBattleGroupListQuery query{};
        query.limit = (std::max)(1, request.limit);
        query.child_victory_only = request.child_victory_only;
        if (request.before.has_value()) {
            query.before = savor::db::UiReadListCursor{ request.before->primary, request.before->secondary };
        }
        if (request.after.has_value()) {
            query.after = savor::db::UiReadListCursor{ request.after->primary, request.after->secondary };
        }

        const auto page = db->ListBattleGroups(query);
        BattleRunGroupPage out{};
        out.groups = page.items;
        if (page.next.has_value()) {
            out.next = BattleRunCursor{ page.next->primary, page.next->secondary };
        }
        if (page.prev.has_value()) {
            out.prev = BattleRunCursor{ page.prev->primary, page.prev->secondary };
        }
        return ServiceResult<BattleRunGroupPage>::Ok(std::move(out));
    }

    static ServiceResult<std::vector<savor::db::UiBattleWaveSummary>> ListBattleWaves(
        std::int64_t battle_set_id) {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<std::vector<savor::db::UiBattleWaveSummary>>("SavorDb UIRead is unavailable");
        }
        return ServiceResult<std::vector<savor::db::UiBattleWaveSummary>>::Ok(db->ListBattleWaves(battle_set_id));
    }

    static ServiceResult<std::vector<savor::db::UiBattleTurnJobSummary>> ListBattleTurnJobsForWaves(
        const std::vector<std::int64_t>& wave_ids) {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<std::vector<savor::db::UiBattleTurnJobSummary>>("SavorDb UIRead is unavailable");
        }
        return ServiceResult<std::vector<savor::db::UiBattleTurnJobSummary>>::Ok(
            db->ListBattleTurnJobsForWaves(wave_ids));
    }

    static ServiceResult<BattleRunJobDetail> GetBattleTurnJobDetail(std::int64_t turn_job_id) {
        auto* ui_read = UiReadDb();
        if (ui_read == nullptr) {
            return Unavailable<BattleRunJobDetail>("SavorDb UIRead is unavailable");
        }

        const auto battle = ui_read->GetBattleTurnJobDetail(turn_job_id);
        if (!battle.has_value()) {
            return NotFound<BattleRunJobDetail>("battle job not found");
        }

        BattleRunJobDetail out{};
        out.battle = *battle;
        const std::int64_t exec_job_id = battle->summary.exec_job_id.value_or(0);
        if (exec_job_id > 0) {
            out.job = ui_read->GetJobDetail(exec_job_id);
        }
        if (auto* execution = ExecutionDb(); execution != nullptr) {
            if (exec_job_id > 0) {
                out.events = execution->ListJobEvents(exec_job_id, 128);
            }
        }
        return ServiceResult<BattleRunJobDetail>::Ok(std::move(out));
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
    static ServiceResult<T> NotFound(std::string message) {
        return ServiceResult<T>::Err({ ServiceErrorKind::NotFound, std::move(message) });
    }
};

} // namespace savorqt::db
