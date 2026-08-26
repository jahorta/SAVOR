#include "WorkflowExpansionService.h"

#include "WorkflowGraphLaunchService.h"
#include "WorkflowOrchestration.h"
#include "../../Analysis/IAnalysisDb.h"
#include "../../Authoring/IAuthoringDb.h"
#include "../../State/IStateDb.h"
#include "../IExecutionDb.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace savor::db::execution::workflow {
namespace {

using namespace std::chrono_literals;

bool Exec(sqlite3* db, const char* sql, std::string* error_out) {
    char* message = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    if (rc == SQLITE_OK) return true;
    if (error_out) *error_out = message ? message : sqlite3_errmsg(db);
    sqlite3_free(message);
    return false;
}

std::string Text(sqlite3_stmt* statement, int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value ? reinterpret_cast<const char*>(value) : std::string{};
}

std::string KindText(WorkflowExpansionKind kind) {
    switch (kind) {
    case WorkflowExpansionKind::TasMovieFirstBattleExploration:
        return "TAS_FIRST_BATTLE";
    case WorkflowExpansionKind::TasMovieDelayExploration:
        return "TAS_DELAY";
    default:
        return {};
    }
}

WorkflowExpansionKind ParseKind(const std::string& value) {
    if (value == "TAS_FIRST_BATTLE")
        return WorkflowExpansionKind::TasMovieFirstBattleExploration;
    if (value == "TAS_DELAY")
        return WorkflowExpansionKind::TasMovieDelayExploration;
    return WorkflowExpansionKind::None;
}

std::string StateText(WorkflowInstanceState state) {
    switch (state) {
    case WorkflowInstanceState::Pending: return "PENDING";
    case WorkflowInstanceState::Running: return "RUNNING";
    case WorkflowInstanceState::Completed: return "COMPLETED";
    case WorkflowInstanceState::Failed: return "FAILED";
    case WorkflowInstanceState::Cancelling: return "CANCELLING";
    case WorkflowInstanceState::Canceled: return "CANCELED";
    case WorkflowInstanceState::Interrupted: return "INTERRUPTED";
    default: return "UNKNOWN";
    }
}

bool IsAttention(const std::string& state) {
    return state == "FAILED" || state == "INTERRUPTED";
}

bool IsActive(const std::string& state) {
    return state == "PENDING" || state == "RUNNING" || state == "CANCELLING";
}

const WorkflowExpansionMemberSnapshot* FindMember(
    const WorkflowExpansionSnapshot& expansion, const std::string& role,
    std::int64_t delay, std::optional<std::int64_t> rtc = std::nullopt) {
    const auto found = std::find_if(expansion.members.begin(), expansion.members.end(),
        [&](const auto& member) {
            return member.role == role && member.neutral_epoch_count == delay &&
                member.rtc_value == rtc;
        });
    return found == expansion.members.end() ? nullptr : &*found;
}

std::optional<std::int64_t> Output(IExecutionDb* execution,
    std::int64_t workflow, std::string_view node, std::string_view key,
    std::string_view ref_kind) {
    auto* query = execution ? execution->WorkflowQueryService() : nullptr;
    if (!query) return std::nullopt;
    std::optional<std::int64_t> value;
    for (const auto& output : query->ListStepOutputs(workflow)) {
        if (output.graph_node_key == node && output.output_key == key &&
            output.ref_kind == ref_kind) {
            if (value && *value != output.ref_id) return std::nullopt;
            value = output.ref_id;
        }
    }
    return value;
}

std::optional<std::int64_t> GraphRevision(IAuthoringDb* authoring,
    std::string_view name) {
    if (!authoring) return std::nullopt;
    std::optional<std::int64_t> found;
    for (const auto& graph : authoring->ListWorkflowGraphs(1000, true)) {
        if (graph.name != name || graph.status != "ACTIVE") continue;
        if (!found || graph.workflow_graph_revision_id > *found)
            found = graph.workflow_graph_revision_id;
    }
    return found;
}

bool BindInt64(sqlite3_stmt* statement, int index,
    std::optional<std::int64_t> value) {
    return value ? sqlite3_bind_int64(statement, index, *value) == SQLITE_OK
                 : sqlite3_bind_null(statement, index) == SQLITE_OK;
}

} // namespace

std::vector<WorkflowExpansionTarget> NormalizeFirstBattleExpansionTargets(
    const std::vector<WorkflowExpansionTarget>& requested) {
    std::set<WorkflowExpansionTarget> normalized;
    for (const auto& target : requested) {
        if (target.rtc_value < 0 || target.neutral_epoch_count < 0) continue;
        for (std::int64_t delay = 0; delay <= target.neutral_epoch_count; ++delay)
            normalized.insert({delay, target.rtc_value});
    }
    return {normalized.begin(), normalized.end()};
}

WorkflowExpansionService::WorkflowExpansionService(
    std::filesystem::path execution_db_path,
    std::filesystem::path analysis_db_path, IAuthoringDb* authoring,
    IExecutionDb* execution, IStateDb* state, IAnalysisDb* analysis)
    : execution_db_path_(std::move(execution_db_path)),
      analysis_db_path_(std::move(analysis_db_path)), authoring_(authoring),
      execution_(execution), state_(state), analysis_(analysis) {}

WorkflowExpansionService::~WorkflowExpansionService() { Stop(); }

bool WorkflowExpansionService::Start(std::string* error_out) {
    if (thread_.joinable()) return true;
    if (sqlite3_open_v2(execution_db_path_.string().c_str(), &db_,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
        return false;
    if (sqlite3_open_v2(analysis_db_path_.string().c_str(), &analysis_sqlite_,
            SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(analysis_sqlite_);
        sqlite3_close(db_); db_ = nullptr;
        return false;
    }
    sqlite3_busy_timeout(db_, 5000);
    sqlite3_busy_timeout(analysis_sqlite_, 5000);
    stopping_ = false;
    thread_ = std::thread([this] { Run(); });
    return true;
}

void WorkflowExpansionService::Stop() {
    stopping_ = true;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (analysis_sqlite_) sqlite3_close(analysis_sqlite_);
    if (db_) sqlite3_close(db_);
    analysis_sqlite_ = nullptr;
    db_ = nullptr;
}

void WorkflowExpansionService::Wake() { cv_.notify_all(); }

bool WorkflowExpansionService::Create(
    const WorkflowExpansionCreateRequest& request,
    std::int64_t* expansion_id_out, std::string* error_out) {
    std::lock_guard lock(db_mutex_);
    if (!db_ || request.kind == WorkflowExpansionKind::None ||
        request.source_ref_id <= 0 || request.max_neutral_epochs < 0)
        return false;
    const bool first = request.kind ==
        WorkflowExpansionKind::TasMovieFirstBattleExploration;
    if (first && request.source_ref_kind != "state_artifact") {
        if (error_out) *error_out = "first-battle expansion requires a DTM artifact";
        return false;
    }
    if (!first && request.source_ref_kind != "tas_route_node") {
        if (error_out) *error_out = "delay expansion requires a TAS route node";
        return false;
    }

    std::optional<std::int64_t> source_dtm;
    std::optional<std::int64_t> source_attempt;
    std::optional<std::int64_t> inherited_rtc;
    if (first) {
        const auto artifact = state_->GetArtifact(request.source_ref_id);
        if (!artifact || artifact->artifact_kind != "DTM") {
            if (error_out) *error_out = "first-battle expansion source is not a DTM artifact";
            return false;
        }
        source_dtm = request.source_ref_id;
    } else {
        const auto nodes = analysis_->ListTasRouteNodes();
        const auto node = std::find_if(nodes.begin(), nodes.end(), [&](const auto& item) {
            return item.route_node_id == request.source_ref_id;
        });
        if (node == nodes.end()) {
            if (error_out) *error_out = "TAS route node was not found";
            return false;
        }
        std::optional<TasMovieRootRecord> root;
        if (node->tas_movie_tree_id) {
            const auto tree = state_->GetTasMovieTree(*node->tas_movie_tree_id);
            if (tree) root = state_->GetTasMovieRoot(tree->tas_movie_root_id);
        } else if (node->source_dtm_artifact_id) {
            root = state_->FindTasMovieRootByDtmArtifactId(*node->source_dtm_artifact_id);
        }
        if (!root || root->source_context_kind != "tmv_validation_request") {
            if (error_out) *error_out = "TAS route node has no qualified root-establishment lineage";
            return false;
        }
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(analysis_sqlite_,
            "SELECT validation_attempt_id FROM tmv_validation_attempt WHERE validation_request_id=?1 AND produced_tas_movie_root_id IS NOT NULL ORDER BY validation_attempt_id DESC LIMIT 1;",
            -1, &statement, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_int64(statement, 1, root->source_context_id);
        if (sqlite3_step(statement) == SQLITE_ROW)
            source_attempt = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        if (!source_attempt) {
            if (error_out) *error_out = "TAS route node establishment attempt is unavailable";
            return false;
        }
        source_dtm = root->dtm_artifact_id;
        inherited_rtc = root->rtc_value;
    }

    std::vector<WorkflowExpansionTarget> targets = request.targets;
    if (first && targets.empty()) {
        if (!request.rtc_min || !request.rtc_max || *request.rtc_min < 0 ||
            *request.rtc_max < *request.rtc_min) {
            if (error_out) *error_out = "first-battle expansion requires targets or an inclusive RTC range";
            return false;
        }
        for (std::int64_t rtc = *request.rtc_min;; ++rtc) {
            targets.push_back({request.max_neutral_epochs, rtc});
            if (rtc == *request.rtc_max) break;
        }
        targets = NormalizeFirstBattleExpansionTargets(targets);
    } else if (first) {
        targets = NormalizeFirstBattleExpansionTargets(targets);
    } else {
        if (!inherited_rtc) return false;
        targets.clear();
        for (std::int64_t delay = 0; delay <= request.max_neutral_epochs; ++delay)
            targets.push_back({delay, *inherited_rtc});
    }
    if (targets.empty()) {
        if (error_out) *error_out = "workflow expansion has no valid targets";
        return false;
    }
    const auto [rtc_min_it, rtc_max_it] = std::minmax_element(targets.begin(), targets.end(),
        [](const auto& lhs, const auto& rhs) { return lhs.rtc_value < rhs.rtc_value; });
    const auto max_delay_it = std::max_element(targets.begin(), targets.end(),
        [](const auto& lhs, const auto& rhs) {
            return lhs.neutral_epoch_count < rhs.neutral_epoch_count;
        });
    const std::int64_t rtc_min = rtc_min_it->rtc_value;
    const std::int64_t rtc_max = rtc_max_it->rtc_value;
    const std::int64_t max_delay = max_delay_it->neutral_epoch_count;

    if (!Exec(db_, "BEGIN IMMEDIATE;", error_out)) return false;
    const auto rollback = [&] { Exec(db_, "ROLLBACK;", nullptr); };

    sqlite3_stmt* statement = nullptr;
    constexpr const char* sql =
        "INSERT INTO exec_workflow_expansion(expansion_kind,state,source_ref_kind,source_ref_id,source_dtm_artifact_id,source_establishment_attempt_id,inherited_rtc,rtc_min,rtc_max,max_neutral_epochs,created_by,created_at_utc,updated_at_utc) VALUES(?1,'RUNNING',?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?11);";
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        rollback();
        return false;
    }
    const auto now = types::UtcNow().time_since_epoch().count();
    const auto kind = KindText(request.kind);
    sqlite3_bind_text(statement, 1, kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, request.source_ref_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 3, request.source_ref_id);
    BindInt64(statement, 4, source_dtm);
    BindInt64(statement, 5, source_attempt);
    BindInt64(statement, 6, inherited_rtc);
    sqlite3_bind_int64(statement, 7, rtc_min);
    sqlite3_bind_int64(statement, 8, rtc_max);
    sqlite3_bind_int64(statement, 9, max_delay);
    const auto created_by = request.created_by.empty() ? "SavorDb" : request.created_by;
    sqlite3_bind_text(statement, 10, created_by.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 11, now);
    const bool ok = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    if (!ok) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        rollback();
        return false;
    }
    const auto expansion_id = sqlite3_last_insert_rowid(db_);
    if (sqlite3_prepare_v2(db_,
        "INSERT INTO exec_workflow_expansion_target(workflow_expansion_id,neutral_epoch_count,rtc_value,created_at_utc) VALUES(?1,?2,?3,?4);",
        -1, &statement, nullptr) != SQLITE_OK) {
        rollback();
        return false;
    }
    for (const auto& target : targets) {
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
        sqlite3_bind_int64(statement, 1, expansion_id);
        sqlite3_bind_int64(statement, 2, target.neutral_epoch_count);
        sqlite3_bind_int64(statement, 3, target.rtc_value);
        sqlite3_bind_int64(statement, 4, now);
        if (sqlite3_step(statement) != SQLITE_DONE) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            sqlite3_finalize(statement);
            rollback();
            return false;
        }
    }
    sqlite3_finalize(statement);
    if (!Exec(db_, "COMMIT;", error_out)) {
        rollback();
        return false;
    }
    if (expansion_id_out) *expansion_id_out = expansion_id;
    Wake();
    return true;
}

std::vector<WorkflowExpansionSnapshot> WorkflowExpansionService::List(
    bool include_final) const {
    std::lock_guard lock(db_mutex_);
    std::vector<WorkflowExpansionSnapshot> rows;
    if (!db_) return rows;
    sqlite3_stmt* statement = nullptr;
    const char* sql = include_final
        ? "SELECT workflow_expansion_id,expansion_kind,state,source_ref_id,rtc_min,rtc_max,max_neutral_epochs,failure_text,source_dtm_artifact_id FROM exec_workflow_expansion ORDER BY workflow_expansion_id DESC;"
        : "SELECT workflow_expansion_id,expansion_kind,state,source_ref_id,rtc_min,rtc_max,max_neutral_epochs,failure_text,source_dtm_artifact_id FROM exec_workflow_expansion WHERE state NOT IN ('COMPLETED','CANCELED') ORDER BY workflow_expansion_id DESC;";
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK)
        return rows;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        WorkflowExpansionSnapshot row{};
        row.workflow_expansion_id = sqlite3_column_int64(statement, 0);
        row.kind = ParseKind(Text(statement, 1));
        row.state = Text(statement, 2);
        row.source_ref_id = sqlite3_column_int64(statement, 3);
        if (sqlite3_column_type(statement, 4) != SQLITE_NULL) row.rtc_min = sqlite3_column_int64(statement, 4);
        if (sqlite3_column_type(statement, 5) != SQLITE_NULL) row.rtc_max = sqlite3_column_int64(statement, 5);
        row.max_neutral_epochs = sqlite3_column_int64(statement, 6);
        if (sqlite3_column_type(statement, 7) != SQLITE_NULL) row.failure_text = Text(statement, 7);
        if (sqlite3_column_type(statement, 8) != SQLITE_NULL)
            row.source_dtm_artifact_id = sqlite3_column_int64(statement, 8);
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(statement);
    for (auto& row : rows) {
        if (sqlite3_prepare_v2(db_,
            "SELECT neutral_epoch_count,rtc_value FROM exec_workflow_expansion_target WHERE workflow_expansion_id=?1 ORDER BY rtc_value,neutral_epoch_count;",
            -1, &statement, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(statement, 1, row.workflow_expansion_id);
            while (sqlite3_step(statement) == SQLITE_ROW)
                row.targets.push_back({sqlite3_column_int64(statement, 0),
                                       sqlite3_column_int64(statement, 1)});
        }
        sqlite3_finalize(statement);
        if (sqlite3_prepare_v2(db_,
            "SELECT workflow_expansion_member_id,member_role,neutral_epoch_count,rtc_value,workflow_instance_id,state FROM exec_workflow_expansion_member WHERE workflow_expansion_id=?1 ORDER BY neutral_epoch_count,COALESCE(rtc_value,-1),workflow_expansion_member_id;",
            -1, &statement, nullptr) != SQLITE_OK) continue;
        sqlite3_bind_int64(statement, 1, row.workflow_expansion_id);
        while (sqlite3_step(statement) == SQLITE_ROW) {
            WorkflowExpansionMemberSnapshot member{};
            member.workflow_expansion_member_id = sqlite3_column_int64(statement, 0);
            member.role = Text(statement, 1);
            member.neutral_epoch_count = sqlite3_column_int64(statement, 2);
            if (sqlite3_column_type(statement, 3) != SQLITE_NULL) member.rtc_value = sqlite3_column_int64(statement, 3);
            member.workflow_instance_id = sqlite3_column_int64(statement, 4);
            member.state = Text(statement, 5);
            row.members.push_back(std::move(member));
        }
        sqlite3_finalize(statement);
    }
    return rows;
}

bool WorkflowExpansionService::ReadFirstBattleCoverage(
    const FirstBattleCoverageQuery& requested_query,
    FirstBattleCoverageSnapshot* snapshot_out,
    std::string* error_out) const {
    std::lock_guard lock(db_mutex_);
    if (!snapshot_out || !db_) return false;
    auto expansions = List(true);
    FirstBattleCoverageQuery request = requested_query;
    if (request.workflow_expansion_id) {
        const auto found = std::find_if(expansions.begin(), expansions.end(),
            [&](const auto& row) {
                return row.workflow_expansion_id == *request.workflow_expansion_id &&
                    row.kind == WorkflowExpansionKind::TasMovieFirstBattleExploration;
            });
        if (found == expansions.end() || !found->source_dtm_artifact_id) {
            if (error_out) *error_out = "first-battle expansion was not found";
            return false;
        }
        request.source_dtm_artifact_id = *found->source_dtm_artifact_id;
        request.rtc_min = found->rtc_min.value_or(0);
        request.rtc_max = found->rtc_max.value_or(request.rtc_min);
        request.max_neutral_epochs = found->max_neutral_epochs;
    }
    if (request.source_dtm_artifact_id <= 0 || request.rtc_min < 0 ||
        request.rtc_max < request.rtc_min || request.max_neutral_epochs < 0) {
        if (error_out) *error_out = "invalid first-battle coverage query";
        return false;
    }

    FirstBattleCoverageSnapshot result{};
    result.source_dtm_artifact_id = request.source_dtm_artifact_id;
    result.rtc_min = request.rtc_min;
    result.rtc_max = request.rtc_max;
    result.max_neutral_epochs = request.max_neutral_epochs;
    std::map<std::int64_t, FirstBattleDelayPreparationSnapshot> preparations;
    for (std::int64_t delay = 0; delay <= request.max_neutral_epochs; ++delay)
        preparations.emplace(delay, FirstBattleDelayPreparationSnapshot{.neutral_epoch_count=delay});
    std::map<std::pair<std::int64_t, std::int64_t>, FirstBattleCoverageCellSnapshot> cells;
    for (std::int64_t rtc = request.rtc_min;; ++rtc) {
        for (std::int64_t delay = 0; delay <= request.max_neutral_epochs; ++delay)
            cells.emplace(std::pair{rtc, delay},
                FirstBattleCoverageCellSnapshot{.rtc_value=rtc,
                                                 .neutral_epoch_count=delay});
        if (rtc == request.rtc_max) break;
    }

    const auto state_rank = [](const std::string& state) {
        if (state == "FAILED" || state == "INTERRUPTED") return 4;
        if (IsActive(state)) return 3;
        if (state == "COMPLETED") return 2;
        if (state == "CANCELED") return 1;
        return 0;
    };
    const auto diagnostic = [](const WorkflowGraphSnapshot& graph) {
        for (auto it = graph.unit_activations.rbegin(); it != graph.unit_activations.rend(); ++it)
            if (it->failure_text && !it->failure_text->empty()) return *it->failure_text;
        for (auto it = graph.steps.rbegin(); it != graph.steps.rend(); ++it)
            if (it->blocked_reason && !it->blocked_reason->empty()) return *it->blocked_reason;
        return std::string{};
    };
    const auto merge_preparation = [&](FirstBattleDelayPreparationSnapshot& target,
        const WorkflowExpansionMemberSnapshot& member,
        const WorkflowGraphSnapshot* graph) {
        const auto state = graph ? StateText(graph->instance.state) : member.state;
        target.workflow_instance_ids.push_back(member.workflow_instance_id);
        if (IsAttention(state))
            target.retryable_workflow_instance_ids.push_back(member.workflow_instance_id);
        if (state_rank(state) >= state_rank(target.state)) target.state = state;
        if (graph) {
            const auto text = diagnostic(*graph);
            if (!text.empty()) target.diagnostic = text;
        }
    };

    auto* workflow_query = execution_ ? execution_->WorkflowQueryService() : nullptr;
    if (!workflow_query) {
        if (error_out) *error_out = "workflow query service is unavailable";
        return false;
    }
    std::map<std::pair<std::int64_t, std::int64_t>, std::int64_t> latest_workflow;
    for (const auto& expansion : expansions) {
        if (expansion.kind != WorkflowExpansionKind::TasMovieFirstBattleExploration ||
            expansion.source_dtm_artifact_id != request.source_dtm_artifact_id) continue;
        const WorkflowExpansionMemberSnapshot* annotation = nullptr;
        for (const auto& member : expansion.members)
            if (member.role == "ANNOTATE") annotation = &member;
        for (const auto& member : expansion.members) {
            const auto graph = workflow_query->GetWorkflowGraph(member.workflow_instance_id);
            if (member.role == "SOURCE_ESTABLISH")
                merge_preparation(preparations[0], member, graph ? &*graph : nullptr);
            else if (member.role == "DELAY_PRODUCTION" &&
                     member.neutral_epoch_count <= request.max_neutral_epochs)
                merge_preparation(preparations[member.neutral_epoch_count], member,
                                  graph ? &*graph : nullptr);
        }
        if (annotation) {
            const auto graph = workflow_query->GetWorkflowGraph(annotation->workflow_instance_id);
            for (std::int64_t delay = 1; delay <= request.max_neutral_epochs; ++delay)
                merge_preparation(preparations[delay], *annotation, graph ? &*graph : nullptr);
        }

        for (const auto& target : expansion.targets) {
            if (target.rtc_value < request.rtc_min || target.rtc_value > request.rtc_max ||
                target.neutral_epoch_count > request.max_neutral_epochs) continue;
            auto& cell = cells[std::pair{target.rtc_value, target.neutral_epoch_count}];
            const auto member = FindMember(expansion, "RTC_BATTLE",
                target.neutral_epoch_count, target.rtc_value);
            if (!member) {
                if (IsActive(expansion.state)) {
                    cell.active = true;
                    if (cell.lifecycle == "NOT_RUN") cell.lifecycle = "WAITING";
                }
                continue;
            }
            cell.workflow_instance_ids.push_back(member->workflow_instance_id);
            const auto graph = workflow_query->GetWorkflowGraph(member->workflow_instance_id);
            const auto lifecycle = graph ? StateText(graph->instance.state) : member->state;
            if (IsActive(lifecycle)) cell.active = true;
            if (IsAttention(lifecycle)) {
                cell.retryable = true;
                cell.retryable_workflow_instance_ids.push_back(member->workflow_instance_id);
            }
            FirstBattleCoverageStage stage = FirstBattleCoverageStage::NotRun;
            std::int64_t confirmed = 0;
            bool seed_probe_completed = false;
            if (graph) {
                for (const auto& step : graph->steps) {
                    if (step.state != WorkflowStepState::Completed) continue;
                    if (step.step_kind == "tasmovie.validate_root")
                        stage = std::max(stage, FirstBattleCoverageStage::Validated);
                    else if (step.step_kind == "tasmovie.checkpoint_sterilize")
                        stage = std::max(stage, FirstBattleCoverageStage::Sterilized);
                    else if (step.step_kind == "seedprobe.confirm") {
                        stage = std::max(stage, FirstBattleCoverageStage::SeedProbed);
                        seed_probe_completed = true;
                    }
                }
                if (seed_probe_completed && analysis_) {
                    for (const auto& output : workflow_query->ListStepOutputs(member->workflow_instance_id)) {
                        if (output.output_key != "seed_probe_run") continue;
                        for (const auto& row : analysis_->ListSeedProbeResults(output.ref_id))
                            if (row.evidence_state == SeedProbeEvidenceState::Confirmed) ++confirmed;
                    }
                    if (confirmed == 0) {
                        cell.invariant_violation = true;
                        cell.retryable = true;
                        cell.diagnostic = "completed SeedProbe has no confirmed neutral seed";
                    }
                }
                if (graph->instance.state == WorkflowInstanceState::Completed &&
                    !cell.invariant_violation)
                    stage = FirstBattleCoverageStage::BattleTested;
                const auto text = diagnostic(*graph);
                if (!text.empty()) cell.diagnostic = text;
            }
            cell.stage = std::max(cell.stage, stage);
            cell.confirmed_seed_count = std::max(cell.confirmed_seed_count, confirmed);
            auto& latest = latest_workflow[std::pair{target.rtc_value, target.neutral_epoch_count}];
            if (member->workflow_instance_id >= latest) {
                latest = member->workflow_instance_id;
                cell.lifecycle = cell.invariant_violation ? "INVALID" : lifecycle;
            }
        }
    }

    for (auto& [coordinate, cell] : cells) {
        const auto& preparation = preparations[cell.neutral_epoch_count];
        if (!preparation.retryable_workflow_instance_ids.empty()) {
            cell.retryable = true;
            cell.retryable_workflow_instance_ids.insert(
                cell.retryable_workflow_instance_ids.end(),
                preparation.retryable_workflow_instance_ids.begin(),
                preparation.retryable_workflow_instance_ids.end());
            if (cell.lifecycle == "NOT_RUN" || cell.lifecycle == "WAITING")
                cell.lifecycle = preparation.state;
        } else if (IsActive(preparation.state) && cell.lifecycle == "NOT_RUN") {
            cell.active = true;
            cell.lifecycle = "WAITING";
        }
        std::sort(cell.workflow_instance_ids.begin(), cell.workflow_instance_ids.end());
        cell.workflow_instance_ids.erase(
            std::unique(cell.workflow_instance_ids.begin(), cell.workflow_instance_ids.end()),
            cell.workflow_instance_ids.end());
        std::sort(cell.retryable_workflow_instance_ids.begin(),
                  cell.retryable_workflow_instance_ids.end());
        cell.retryable_workflow_instance_ids.erase(
            std::unique(cell.retryable_workflow_instance_ids.begin(),
                        cell.retryable_workflow_instance_ids.end()),
            cell.retryable_workflow_instance_ids.end());
        result.cells.push_back(std::move(cell));
    }
    for (auto& [delay, preparation] : preparations) {
        std::sort(preparation.workflow_instance_ids.begin(), preparation.workflow_instance_ids.end());
        preparation.workflow_instance_ids.erase(
            std::unique(preparation.workflow_instance_ids.begin(),
                        preparation.workflow_instance_ids.end()),
            preparation.workflow_instance_ids.end());
        std::sort(preparation.retryable_workflow_instance_ids.begin(),
                  preparation.retryable_workflow_instance_ids.end());
        preparation.retryable_workflow_instance_ids.erase(
            std::unique(preparation.retryable_workflow_instance_ids.begin(),
                        preparation.retryable_workflow_instance_ids.end()),
            preparation.retryable_workflow_instance_ids.end());
        result.delay_preparations.push_back(std::move(preparation));
    }
    *snapshot_out = std::move(result);
    return true;
}

bool WorkflowExpansionService::LaunchMissingFirstBattleCoverage(
    const LaunchMissingFirstBattleCoverageRequest& request,
    LaunchMissingFirstBattleCoverageReceipt* receipt_out,
    std::string* error_out) {
    std::lock_guard lock(db_mutex_);
    if (!receipt_out || request.source_dtm_artifact_id <= 0) return false;
    LaunchMissingFirstBattleCoverageReceipt receipt{};
    std::set<WorkflowExpansionTarget> requested_unique;
    for (const auto& target : request.targets)
        if (target.rtc_value >= 0 && target.neutral_epoch_count >= 0)
            requested_unique.insert(target);
    receipt.requested_count = static_cast<std::int64_t>(requested_unique.size());
    const auto normalized = NormalizeFirstBattleExpansionTargets(request.targets);
    receipt.implied_count = static_cast<std::int64_t>(normalized.size()) - receipt.requested_count;
    if (normalized.empty()) {
        if (error_out) *error_out = "no valid first-battle coverage targets were selected";
        return false;
    }
    const auto [rtc_min_it, rtc_max_it] = std::minmax_element(normalized.begin(), normalized.end(),
        [](const auto& lhs, const auto& rhs) { return lhs.rtc_value < rhs.rtc_value; });
    const auto max_delay_it = std::max_element(normalized.begin(), normalized.end(),
        [](const auto& lhs, const auto& rhs) {
            return lhs.neutral_epoch_count < rhs.neutral_epoch_count;
        });
    FirstBattleCoverageSnapshot coverage{};
    if (!ReadFirstBattleCoverage({.source_dtm_artifact_id=request.source_dtm_artifact_id,
            .rtc_min=rtc_min_it->rtc_value, .rtc_max=rtc_max_it->rtc_value,
            .max_neutral_epochs=max_delay_it->neutral_epoch_count},
            &coverage, error_out)) return false;
    std::map<std::pair<std::int64_t, std::int64_t>, const FirstBattleCoverageCellSnapshot*> by_coordinate;
    for (const auto& cell : coverage.cells)
        by_coordinate[{cell.rtc_value, cell.neutral_epoch_count}] = &cell;
    std::vector<WorkflowExpansionTarget> launch;
    for (const auto& target : normalized) {
        const auto found = by_coordinate.find({target.rtc_value, target.neutral_epoch_count});
        if (found != by_coordinate.end()) {
            const auto& cell = *found->second;
            if (cell.stage == FirstBattleCoverageStage::BattleTested) {
                ++receipt.already_covered_count;
                continue;
            }
            if (cell.active) {
                ++receipt.active_count;
                continue;
            }
            if (cell.retryable) {
                ++receipt.retryable_count;
                continue;
            }
        }
        launch.push_back(target);
    }
    if (!launch.empty()) {
        WorkflowExpansionCreateRequest create{};
        create.kind = WorkflowExpansionKind::TasMovieFirstBattleExploration;
        create.source_ref_kind = "state_artifact";
        create.source_ref_id = request.source_dtm_artifact_id;
        create.rtc_min = rtc_min_it->rtc_value;
        create.rtc_max = rtc_max_it->rtc_value;
        create.max_neutral_epochs = max_delay_it->neutral_epoch_count;
        create.targets = std::move(launch);
        create.created_by = request.created_by.empty() ? "SavorQt.FirstBattleCoverage" : request.created_by;
        std::int64_t expansion_id = 0;
        if (!Create(create, &expansion_id, error_out)) return false;
        receipt.workflow_expansion_id = expansion_id;
        receipt.launched_count = static_cast<std::int64_t>(create.targets.size());
    }
    *receipt_out = std::move(receipt);
    return true;
}

void WorkflowExpansionService::Run() {
    while (!stopping_) {
        AdvanceAll();
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, 500ms, [this] { return stopping_.load(); });
    }
}

void WorkflowExpansionService::AdvanceAll() {
    for (const auto& expansion : List(false)) {
        std::string error;
        if (Advance(expansion, &error)) continue;
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db_,
            "UPDATE exec_workflow_expansion SET state='ATTENTION',failure_text=?2,updated_at_utc=?3 WHERE workflow_expansion_id=?1;",
            -1, &statement, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(statement, 1, expansion.workflow_expansion_id);
            sqlite3_bind_text(statement, 2, error.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(statement, 3, types::UtcNow().time_since_epoch().count());
            sqlite3_step(statement);
        }
        sqlite3_finalize(statement);
    }
}

bool WorkflowExpansionService::Advance(
    const WorkflowExpansionSnapshot& expansion, std::string* error_out) {
    std::lock_guard lock(db_mutex_);
    auto* query = execution_ ? execution_->WorkflowQueryService() : nullptr;
    if (!query) return false;
    for (const auto& member : expansion.members) {
        const auto graph = query->GetWorkflowGraph(member.workflow_instance_id);
        if (!graph) continue;
        const auto state = StateText(graph->instance.state);
        if (state == member.state) continue;
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db_,
            "UPDATE exec_workflow_expansion_member SET state=?2,updated_at_utc=?3 WHERE workflow_expansion_member_id=?1;",
            -1, &statement, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(statement, 1, member.workflow_expansion_member_id);
            sqlite3_bind_text(statement, 2, state.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(statement, 3, types::UtcNow().time_since_epoch().count());
            sqlite3_step(statement);
        }
        sqlite3_finalize(statement);
    }
    const auto refreshed = [&]() -> WorkflowExpansionSnapshot {
        for (auto row : List(true))
            if (row.workflow_expansion_id == expansion.workflow_expansion_id) return row;
        return {};
    }();

    const auto launch = [&](std::string_view graph_name, std::string role,
        std::int64_t delay, std::optional<std::int64_t> rtc,
        std::vector<WorkflowLaunchInputValue> inputs,
        std::vector<WorkflowLaunchArgumentValue> arguments) -> bool {
        if (FindMember(refreshed, role, delay, rtc)) return true;
        const auto revision = GraphRevision(authoring_, graph_name);
        if (!revision) {
            if (error_out) *error_out = "required expansion workflow graph is unavailable: " + std::string(graph_name);
            return false;
        }
        const auto launch_key = "workflow-expansion:" +
            std::to_string(refreshed.workflow_expansion_id) + ":" + role + ":" +
            std::to_string(delay) + ":" + (rtc ? std::to_string(*rtc) : "none");
        std::int64_t workflow = 0;
        if (!WorkflowGraphLaunchService(authoring_, execution_).Start(
            {.workflow_graph_revision_id=*revision, .created_by="WorkflowExpansionService",
             .launch_key=launch_key, .input_bindings=std::move(inputs),
             .arguments=std::move(arguments)}, &workflow, error_out)) return false;
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db_,
            "INSERT OR IGNORE INTO exec_workflow_expansion_member(workflow_expansion_id,member_role,neutral_epoch_count,rtc_value,workflow_instance_id,state,created_at_utc,updated_at_utc) VALUES(?1,?2,?3,?4,?5,'PENDING',?6,?6);",
            -1, &statement, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_int64(statement, 1, refreshed.workflow_expansion_id);
        sqlite3_bind_text(statement, 2, role.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 3, delay);
        BindInt64(statement, 4, rtc);
        sqlite3_bind_int64(statement, 5, workflow);
        sqlite3_bind_int64(statement, 6, types::UtcNow().time_since_epoch().count());
        const bool inserted = sqlite3_step(statement) == SQLITE_DONE;
        sqlite3_finalize(statement);
        return inserted;
    };

    std::optional<std::int64_t> source_dtm;
    std::optional<std::int64_t> source_attempt;
    std::optional<std::int64_t> inherited_rtc;
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db_,
        "SELECT source_dtm_artifact_id,source_establishment_attempt_id,inherited_rtc,rtc_min,rtc_max,max_neutral_epochs FROM exec_workflow_expansion WHERE workflow_expansion_id=?1;",
        -1, &statement, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(statement, 1, refreshed.workflow_expansion_id);
    if (sqlite3_step(statement) == SQLITE_ROW) {
        if (sqlite3_column_type(statement,0)!=SQLITE_NULL) source_dtm=sqlite3_column_int64(statement,0);
        if (sqlite3_column_type(statement,1)!=SQLITE_NULL) source_attempt=sqlite3_column_int64(statement,1);
        if (sqlite3_column_type(statement,2)!=SQLITE_NULL) inherited_rtc=sqlite3_column_int64(statement,2);
    }
    sqlite3_finalize(statement);
    if (!source_dtm) return false;

    if (refreshed.kind == WorkflowExpansionKind::TasMovieFirstBattleExploration && !source_attempt) {
        const auto source = FindMember(refreshed, "SOURCE_ESTABLISH", 0);
        if (!source) {
            return launch("Standalone TAS Movie: Establish Root Cursor", "SOURCE_ESTABLISH", 0,
                std::nullopt,
                {{"tas_movie_establish_root_cursor_standalone","root_dtm",
                  "state_artifact.dtm_artifact_id","state_artifact",*source_dtm,"expansion"}},
                {{"tas_movie_establish_root_cursor_standalone","rtc","integer",
                  refreshed.rtc_min,std::nullopt,"expansion"}});
        }
        if (source->state == "COMPLETED")
            source_attempt = Output(execution_, source->workflow_instance_id,
                "tas_movie_establish_root_cursor_standalone",
                "established_root_cursor_attempt", "tmv_validation_attempt");
        if (!source_attempt) return true;
        if (sqlite3_prepare_v2(db_,
            "UPDATE exec_workflow_expansion SET source_establishment_attempt_id=?2,updated_at_utc=?3 WHERE workflow_expansion_id=?1;",
            -1, &statement, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(statement,1,refreshed.workflow_expansion_id);
            sqlite3_bind_int64(statement,2,*source_attempt);
            sqlite3_bind_int64(statement,3,types::UtcNow().time_since_epoch().count());
            sqlite3_step(statement);
        }
        sqlite3_finalize(statement);
    }
    if (!source_attempt) return true;

    const auto establishment_rtc = refreshed.kind == WorkflowExpansionKind::TasMovieDelayExploration
        ? inherited_rtc : refreshed.rtc_min;
    if (!establishment_rtc) return false;
    for (const auto& target : refreshed.targets) {
        if (target.neutral_epoch_count != 0) continue;
        const auto rtc = target.rtc_value;
        if (!launch("1st battle RTC", "RTC_BATTLE", 0, rtc,
            {{"tas_movie_validate_root_1","root_establishment",
              "analysis.tas_movie_validation_attempt_id","tmv_validation_attempt",*source_attempt,"expansion"}},
            {{"tas_movie_validate_root_1","rtc","integer",rtc,std::nullopt,"expansion"}})) return false;
    }

    const bool has_positive_delay = std::any_of(refreshed.targets.begin(), refreshed.targets.end(),
        [](const auto& target) { return target.neutral_epoch_count > 0; });
    if (has_positive_delay) {
    const auto annotation = FindMember(refreshed, "ANNOTATE", 0);
    if (!annotation) {
        return launch("TAS Movie Expansion: Annotate", "ANNOTATE", 0,
            std::nullopt,
            {{"tas_movie_annotate_expansion","root_dtm",
              "state_artifact.dtm_artifact_id","state_artifact",*source_dtm,"expansion"}}, {});
    }
    if (annotation->state != "COMPLETED") return true;
    const auto annotation_attempt = Output(execution_, annotation->workflow_instance_id,
        "tas_movie_annotate_expansion", "annotation_attempt",
        "tmv_input_epoch_annotation_attempt");
    if (!annotation_attempt) return true;

    std::set<std::int64_t> delays;
    for (const auto& target : refreshed.targets)
        if (target.neutral_epoch_count > 0) delays.insert(target.neutral_epoch_count);
    for (const auto delay : delays) {
        const auto production = FindMember(refreshed, "DELAY_PRODUCTION", delay);
        if (!production) {
            if (!launch("TAS Movie Expansion: Revise and Establish", "DELAY_PRODUCTION",
                delay, std::nullopt,
                {{"tas_movie_revise_1","annotation_attempt",
                  "analysis.tas_movie_input_epoch_annotation_attempt_id",
                  "tmv_input_epoch_annotation_attempt",*annotation_attempt,"expansion"}},
                {{"tas_movie_revise_1","neutral_epoch_count","integer",delay,std::nullopt,"expansion"},
                 {"tas_movie_revise_1","placement_profile","choice",std::nullopt,
                  std::string("first_battle.final_dialog"),"expansion"},
                 {"tas_movie_establish_root_cursor_2","rtc","integer",*establishment_rtc,std::nullopt,"expansion"}})) return false;
            continue;
        }
        if (production->state != "COMPLETED") continue;
        const auto revised_attempt = Output(execution_, production->workflow_instance_id,
            "tas_movie_establish_root_cursor_2", "established_root_cursor_attempt",
            "tmv_validation_attempt");
        if (!revised_attempt) continue;
        for (const auto& target : refreshed.targets) {
            if (target.neutral_epoch_count != delay) continue;
            const auto rtc = target.rtc_value;
            if (!launch("1st battle RTC", "RTC_BATTLE", delay, rtc,
                {{"tas_movie_validate_root_1","root_establishment",
                  "analysis.tas_movie_validation_attempt_id","tmv_validation_attempt",*revised_attempt,"expansion"}},
                {{"tas_movie_validate_root_1","rtc","integer",rtc,std::nullopt,"expansion"}})) return false;
        }
    }
    }

    const auto final = [&] {
        auto latest = refreshed;
        for (auto row : List(true)) if (row.workflow_expansion_id == refreshed.workflow_expansion_id) latest = std::move(row);
        const std::int64_t expected = static_cast<std::int64_t>(latest.targets.size());
        std::int64_t completed = 0;
        bool attention = false;
        for (const auto& member : latest.members) {
            if (member.role == "RTC_BATTLE" && member.state == "COMPLETED") ++completed;
            if (IsAttention(member.state)) attention = true;
        }
        return std::pair{completed == expected ? std::string("COMPLETED")
                          : attention ? std::string("ATTENTION") : std::string("RUNNING"),
                         completed};
    }();
    if (sqlite3_prepare_v2(db_,
        "UPDATE exec_workflow_expansion SET state=?2,failure_text=NULL,updated_at_utc=?3 WHERE workflow_expansion_id=?1;",
        -1, &statement, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(statement,1,refreshed.workflow_expansion_id);
        sqlite3_bind_text(statement,2,final.first.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement,3,types::UtcNow().time_since_epoch().count());
        sqlite3_step(statement);
    }
    sqlite3_finalize(statement);
    return true;
}

} // namespace savor::db::execution::workflow
