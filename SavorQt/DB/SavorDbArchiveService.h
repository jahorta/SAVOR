#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "SavorDbRuntime.h"
#include "DB/SavorDbServiceResult.h"
#include "Archive/ArchivePackageService.h"
#include "Archive/RehydrateExecutor.h"
#include "Common/DbService.h"
#include "Execution/ArchiveWorkflowCommands.h"
#include "UIRead/IUiReadDb.h"

namespace savorqt::db {

enum class ArchiveFinalVictoryMode {
    Any,
    Present,
    Absent,
};

enum class ArchiveProblemMode {
    Any,
    HasProblems,
    NoProblems,
};

struct ArchiveWorkflowFilter {
    std::string display_state;
    std::string workflow_kind;
    ArchiveFinalVictoryMode final_victory_mode = ArchiveFinalVictoryMode::Any;
    ArchiveProblemMode problem_mode = ArchiveProblemMode::Any;
    std::optional<std::int64_t> created_from_utc;
    std::optional<std::int64_t> created_to_utc;
    std::optional<std::int64_t> completed_from_utc;
    std::optional<std::int64_t> completed_to_utc;
    std::string text_filter;
};

struct ArchiveCandidateRow {
    std::int64_t workflow_instance_id = 0;
    std::string workflow_kind;
    std::string display_state;
    std::int64_t created_at_utc = 0;
    std::optional<std::int64_t> completed_at_utc;
    std::int64_t blocked_step_count = 0;
    std::int64_t failed_step_count = 0;
    std::int64_t battle_final_victory_count = 0;
};

struct ArchiveCandidatePage {
    std::vector<ArchiveCandidateRow> rows;
};

struct ArchiveSelectionBuildRequest {
    ArchiveWorkflowFilter filter;
    bool include_filter_matches = false;
    std::vector<std::int64_t> explicit_includes;
    std::vector<std::int64_t> explicit_exclusions;
};

struct ArchiveSelectionBuildResult {
    savor::db::archive::ArchiveWorkflowSelection selection;
    int selected_count = 0;
    int excluded_count = 0;
};

struct ArchiveWorkflowExecuteRequest {
    savor::db::archive::ArchiveWorkflowSelection selection;
    bool purge_after_verify = false;
    std::string trace_id;
    std::string archive_name;
    std::optional<std::string> archive_notes;
    savor::db::archive::ArchiveProgressSink progress_sink;
};

using ArchiveWorkflowPreviewResult = savor::runner::parallel::savordb::WorkflowArchiveCommandSummary;
using ArchiveWorkflowExecuteResult = savor::runner::parallel::savordb::WorkflowArchiveCommandSummary;

class SavorDbArchiveService {
public:
    static ServiceResult<ArchiveCandidatePage> ListWorkflowCandidates(const ArchiveWorkflowFilter& filter) {
        auto* ui_read = UiReadDb();
        if (ui_read == nullptr) {
            return ServiceResult<ArchiveCandidatePage>::Err({ ServiceErrorKind::Unavailable, kSavorDbRuntimeUnavailableMessage });
        }

        ArchiveCandidatePage result{};
        std::set<std::int64_t> seen_ids;
        std::optional<savor::db::UiReadListCursor> older_than;
        do {
            savor::db::UiWorkflowInstanceListQuery query{};
            query.display_state = filter.display_state;
            query.workflow_kind = filter.workflow_kind;
            query.before = older_than;
            query.limit = kFetchBatchSize;
            query.battle_final_victory_only = filter.final_victory_mode == ArchiveFinalVictoryMode::Present;
            query.battle_final_victory_absent_only = filter.final_victory_mode == ArchiveFinalVictoryMode::Absent;

            const auto page = ui_read->ListWorkflowInstances(query);
            if (page.items.empty()) {
                break;
            }
            bool added_new_row = false;
            for (const auto& item : page.items) {
                if (!seen_ids.insert(item.workflow_instance_id).second) {
                    continue;
                }
                if (!MatchesClientFilter(item, filter)) {
                    continue;
                }
                result.rows.push_back(ToRow(item));
                added_new_row = true;
            }
            if (!page.next.has_value()) {
                break;
            }
            if (older_than.has_value()
                && older_than->primary == page.next->primary
                && older_than->secondary == page.next->secondary) {
                break;
            }
            older_than = page.next;
            if (!added_new_row && page.items.size() < static_cast<std::size_t>(kFetchBatchSize)) {
                break;
            }
        } while (older_than.has_value());
        return ServiceResult<ArchiveCandidatePage>::Ok(std::move(result));
    }

    static ServiceResult<ArchiveSelectionBuildResult> BuildSelection(const ArchiveSelectionBuildRequest& request) {
        std::set<std::int64_t> selected;
        std::set<std::int64_t> excluded;
        for (const auto id : request.explicit_exclusions) {
            if (id > 0) excluded.insert(id);
        }
        for (const auto id : request.explicit_includes) {
            if (id > 0 && excluded.find(id) == excluded.end()) selected.insert(id);
        }

        if (request.include_filter_matches) {
            auto filter = request.filter;
            auto page = ListWorkflowCandidates(filter);
            if (!page.ok) {
                return ServiceResult<ArchiveSelectionBuildResult>::Err(page.error);
            }
            for (const auto& row : page.value.rows) {
                if (excluded.find(row.workflow_instance_id) == excluded.end()) {
                    selected.insert(row.workflow_instance_id);
                }
            }
        }

        ArchiveSelectionBuildResult result{};
        result.selection.workflow_instance_ids.assign(selected.begin(), selected.end());
        result.selection.explicit_exclusions.assign(excluded.begin(), excluded.end());
        result.selection.created_by_filter_snapshot = FilterSnapshot(request.filter, request.include_filter_matches);
        result.selected_count = static_cast<int>(result.selection.workflow_instance_ids.size());
        result.excluded_count = static_cast<int>(excluded.size());
        return ServiceResult<ArchiveSelectionBuildResult>::Ok(std::move(result));
    }

    static std::string BuildFallbackArchiveName(
        const ArchiveWorkflowFilter& filter,
        int selected_count,
        const std::string& local_timestamp) {
        if (filter.workflow_kind == "BATTLE_RUN"
            && filter.display_state == "COMPLETED"
            && filter.final_victory_mode == ArchiveFinalVictoryMode::Absent) {
            return "Completed battle workflows without final victory - " + local_timestamp;
        }

        if (!filter.display_state.empty() || !filter.workflow_kind.empty()) {
            std::ostringstream out;
            if (!filter.display_state.empty()) {
                out << filter.display_state << ' ';
            }
            if (!filter.workflow_kind.empty()) {
                out << filter.workflow_kind << ' ';
            }
            out << "workflows - " << selected_count << " selected - " << local_timestamp;
            return out.str();
        }

        return "Workflow archive - " + std::to_string(selected_count) + " workflows - " + local_timestamp;
    }

    static ServiceResult<ArchiveWorkflowPreviewResult> PreviewWorkflowArchive(
        const savor::db::archive::ArchiveWorkflowSelection& selection) {
        auto context = BuildContext();
        if (!context.ok) {
            return ServiceResult<ArchiveWorkflowPreviewResult>::Err(context.error);
        }
        auto* service = context.value.service;
        savor::db::archive::SqliteArchivePackageService package_service(
            service->RawExecutionSqlite(),
            service->ExecutionDb(),
            service->UiReadDb(),
            service->ArchiveDb(),
            context.value.paths,
            service->RawStateSqlite(),
            service->RawAnalysisSqlite(),
            service->RawUiReadSqlite());
        savor::db::archive::SqliteRehydrateExecutor rehydrate_executor(
            service->RawExecutionSqlite(),
            service->RawArchiveSqlite(),
            service->ArchiveDb(),
            context.value.paths.archive_store_root,
            service->RawStateSqlite(),
            service->RawAnalysisSqlite());
        savor::runner::parallel::savordb::ArchiveWorkflowCommands commands(
            service->RawArchiveSqlite(),
            service->ArchiveDb(),
            &package_service,
            &rehydrate_executor);
        savor::runner::parallel::savordb::WorkflowArchiveCommandRequest request{};
        request.selection = selection;
        request.now_utc = savor::db::types::UtcNow();
        const auto summary = commands.WorkflowArchivePreview(request);
        if (!summary.success) {
            return ServiceResult<ArchiveWorkflowPreviewResult>::Err({ ServiceErrorKind::Failed, JoinErrors(summary) });
        }
        return ServiceResult<ArchiveWorkflowPreviewResult>::Ok(summary);
    }

    static ServiceResult<ArchiveWorkflowExecuteResult> ExecuteWorkflowArchive(const ArchiveWorkflowExecuteRequest& request) {
        auto context = BuildContext();
        if (!context.ok) {
            return ServiceResult<ArchiveWorkflowExecuteResult>::Err(context.error);
        }
        auto* service = context.value.service;
        savor::db::archive::SqliteArchivePackageService package_service(
            service->RawExecutionSqlite(),
            service->ExecutionDb(),
            service->UiReadDb(),
            service->ArchiveDb(),
            context.value.paths,
            service->RawStateSqlite(),
            service->RawAnalysisSqlite(),
            service->RawUiReadSqlite());
        savor::db::archive::SqliteRehydrateExecutor rehydrate_executor(
            service->RawExecutionSqlite(),
            service->RawArchiveSqlite(),
            service->ArchiveDb(),
            context.value.paths.archive_store_root,
            service->RawStateSqlite(),
            service->RawAnalysisSqlite());
        savor::runner::parallel::savordb::ArchiveWorkflowCommands commands(
            service->RawArchiveSqlite(),
            service->ArchiveDb(),
            &package_service,
            &rehydrate_executor);
        savor::runner::parallel::savordb::WorkflowArchiveCommandRequest command_request{};
        command_request.selection = request.selection;
        command_request.now_utc = savor::db::types::UtcNow();
        command_request.purge_after_verify = request.purge_after_verify;
        command_request.trace_id = request.trace_id;
        command_request.archive_name = request.archive_name;
        command_request.archive_notes = request.archive_notes;
        command_request.progress_sink = request.progress_sink;
        const auto summary = commands.WorkflowArchiveExecute(command_request);
        if (!summary.success) {
            return ServiceResult<ArchiveWorkflowExecuteResult>::Err({ ServiceErrorKind::Failed, JoinErrors(summary) });
        }
        return ServiceResult<ArchiveWorkflowExecuteResult>::Ok(summary);
    }

private:
    static constexpr int kFetchBatchSize = 250;

    struct RuntimeArchiveContext {
        savor::db::core::DBService* service = nullptr;
        savor::db::DbConfigPaths paths{};
    };

    static savor::db::DbConfigPaths BuildPaths(const std::filesystem::path& root) {
        return savor::db::DbConfigPaths{
            .execution_db_path = root / "execution.db",
            .state_db_path = root / "state.db",
            .analysis_db_path = root / "analysis.db",
            .authoring_db_path = root / "authoring.db",
            .ui_read_db_path = root / "ui_read.db",
            .archive_db_path = root / "archive.db",
            .object_store_root = root / "object_store",
            .archive_store_root = root / "archive_store",
        };
    }

    static ServiceResult<RuntimeArchiveContext> BuildContext() {
        auto& runtime = savorqt::SavorDbRuntime::instance();
        auto* service = runtime.service();
        if (service == nullptr || !runtime.isRunning()) {
            return ServiceResult<RuntimeArchiveContext>::Err({ ServiceErrorKind::Unavailable, kSavorDbRuntimeUnavailableMessage });
        }
        if (service->RawExecutionSqlite() == nullptr
            || service->RawStateSqlite() == nullptr
            || service->RawAnalysisSqlite() == nullptr
            || service->RawUiReadSqlite() == nullptr
            || service->RawArchiveSqlite() == nullptr
            || service->ArchiveDb() == nullptr) {
            return ServiceResult<RuntimeArchiveContext>::Err({ ServiceErrorKind::Unavailable, "archive database handles are unavailable" });
        }

        RuntimeArchiveContext context{};
        context.service = service;
        context.paths = BuildPaths(runtime.root());
        return ServiceResult<RuntimeArchiveContext>::Ok(context);
    }

    static savor::db::IUiReadDb* UiReadDb() {
        return savorqt::SavorDbRuntime::instance().uiReadDb();
    }

    static bool MatchesClientFilter(const savor::db::UiWorkflowInstanceSummary& item, const ArchiveWorkflowFilter& filter) {
        if (filter.problem_mode == ArchiveProblemMode::HasProblems
            && item.blocked_step_count == 0
            && item.failed_step_count == 0) {
            return false;
        }
        if (filter.problem_mode == ArchiveProblemMode::NoProblems
            && (item.blocked_step_count > 0 || item.failed_step_count > 0)) {
            return false;
        }
        if (filter.created_from_utc.has_value() && item.created_at_utc < *filter.created_from_utc) return false;
        if (filter.created_to_utc.has_value() && item.created_at_utc > *filter.created_to_utc) return false;
        if (filter.completed_from_utc.has_value() && (!item.completed_at_utc.has_value() || *item.completed_at_utc < *filter.completed_from_utc)) return false;
        if (filter.completed_to_utc.has_value() && (!item.completed_at_utc.has_value() || *item.completed_at_utc > *filter.completed_to_utc)) return false;
        if (!filter.text_filter.empty()) {
            const auto id_text = std::to_string(item.workflow_instance_id);
            if (id_text.find(filter.text_filter) == std::string::npos
                && item.workflow_kind.find(filter.text_filter) == std::string::npos
                && item.display_state.find(filter.text_filter) == std::string::npos) {
                return false;
            }
        }
        return true;
    }

    static ArchiveCandidateRow ToRow(const savor::db::UiWorkflowInstanceSummary& item) {
        ArchiveCandidateRow row{};
        row.workflow_instance_id = item.workflow_instance_id;
        row.workflow_kind = item.workflow_kind;
        row.display_state = item.display_state.empty() ? item.state : item.display_state;
        row.created_at_utc = item.created_at_utc;
        row.completed_at_utc = item.completed_at_utc;
        row.blocked_step_count = item.blocked_step_count;
        row.failed_step_count = item.failed_step_count;
        row.battle_final_victory_count = item.battle_final_victory_count;
        return row;
    }

    static std::string FilterSnapshot(const ArchiveWorkflowFilter& filter, bool include_filter_matches) {
        return "{display_state:" + filter.display_state
            + ",workflow_kind:" + filter.workflow_kind
            + ",final_victory:" + std::to_string(static_cast<int>(filter.final_victory_mode))
            + ",problem_mode:" + std::to_string(static_cast<int>(filter.problem_mode))
            + ",include_filter_matches:" + (include_filter_matches ? "true" : "false")
            + "}";
    }

    static std::string JoinErrors(const ArchiveWorkflowPreviewResult& summary) {
        std::string joined;
        const auto append = [&](const std::string& value) {
            if (value.empty()) return;
            if (!joined.empty()) joined += " | ";
            joined += value;
        };
        for (const auto& error : summary.errors) append(error);
        for (const auto& blocker : summary.blocking_reasons) append(blocker);
        if (summary.preview.error.has_value()) append(*summary.preview.error);
        if (summary.purge.error.has_value()) append(*summary.purge.error);
        for (const auto& blocker : summary.purge.blockers) append(blocker);
        return joined.empty() ? "archive operation failed" : joined;
    }
};

} // namespace savorqt::db
