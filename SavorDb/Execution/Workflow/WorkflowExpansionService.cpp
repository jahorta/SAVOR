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
    if (!db_ || request.kind == WorkflowExpansionKind::None ||
        request.source_ref_id <= 0 || request.max_neutral_epochs < 0)
        return false;
    const bool first = request.kind ==
        WorkflowExpansionKind::TasMovieFirstBattleExploration;
    if (first && (request.source_ref_kind != "state_artifact" ||
        !request.rtc_min || !request.rtc_max || *request.rtc_min < 0 ||
        *request.rtc_max < *request.rtc_min)) {
        if (error_out) *error_out = "first-battle expansion requires a DTM artifact and an inclusive RTC range";
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

    sqlite3_stmt* statement = nullptr;
    constexpr const char* sql =
        "INSERT INTO exec_workflow_expansion(expansion_kind,state,source_ref_kind,source_ref_id,source_dtm_artifact_id,source_establishment_attempt_id,inherited_rtc,rtc_min,rtc_max,max_neutral_epochs,created_by,created_at_utc,updated_at_utc) VALUES(?1,'RUNNING',?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?11);";
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK)
        return false;
    const auto now = types::UtcNow().time_since_epoch().count();
    const auto kind = KindText(request.kind);
    sqlite3_bind_text(statement, 1, kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, request.source_ref_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 3, request.source_ref_id);
    BindInt64(statement, 4, source_dtm);
    BindInt64(statement, 5, source_attempt);
    BindInt64(statement, 6, inherited_rtc);
    BindInt64(statement, 7, request.rtc_min);
    BindInt64(statement, 8, request.rtc_max);
    sqlite3_bind_int64(statement, 9, request.max_neutral_epochs);
    const auto created_by = request.created_by.empty() ? "SavorDb" : request.created_by;
    sqlite3_bind_text(statement, 10, created_by.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 11, now);
    const bool ok = sqlite3_step(statement) == SQLITE_DONE;
    sqlite3_finalize(statement);
    if (!ok) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (expansion_id_out) *expansion_id_out = sqlite3_last_insert_rowid(db_);
    Wake();
    return true;
}

std::vector<WorkflowExpansionSnapshot> WorkflowExpansionService::List(
    bool include_final) const {
    std::vector<WorkflowExpansionSnapshot> rows;
    if (!db_) return rows;
    sqlite3_stmt* statement = nullptr;
    const char* sql = include_final
        ? "SELECT workflow_expansion_id,expansion_kind,state,source_ref_id,rtc_min,rtc_max,max_neutral_epochs,failure_text FROM exec_workflow_expansion ORDER BY workflow_expansion_id DESC;"
        : "SELECT workflow_expansion_id,expansion_kind,state,source_ref_id,rtc_min,rtc_max,max_neutral_epochs,failure_text FROM exec_workflow_expansion WHERE state NOT IN ('COMPLETED','CANCELED') ORDER BY workflow_expansion_id DESC;";
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
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(statement);
    for (auto& row : rows) {
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

    const auto rtc_low = refreshed.kind == WorkflowExpansionKind::TasMovieDelayExploration
        ? inherited_rtc : refreshed.rtc_min;
    const auto rtc_high = refreshed.kind == WorkflowExpansionKind::TasMovieDelayExploration
        ? inherited_rtc : refreshed.rtc_max;
    if (!rtc_low || !rtc_high) return false;
    for (std::int64_t rtc = *rtc_low; rtc <= *rtc_high; ++rtc) {
        if (!launch("1st battle RTC", "RTC_BATTLE", 0, rtc,
            {{"tas_movie_validate_root_1","root_establishment",
              "analysis.tas_movie_validation_attempt_id","tmv_validation_attempt",*source_attempt,"expansion"}},
            {{"tas_movie_validate_root_1","rtc","integer",rtc,std::nullopt,"expansion"}})) return false;
    }

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

    for (std::int64_t delay = 1; delay <= refreshed.max_neutral_epochs; ++delay) {
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
                 {"tas_movie_establish_root_cursor_2","rtc","integer",*rtc_low,std::nullopt,"expansion"}})) return false;
            continue;
        }
        if (production->state != "COMPLETED") continue;
        const auto revised_attempt = Output(execution_, production->workflow_instance_id,
            "tas_movie_establish_root_cursor_2", "established_root_cursor_attempt",
            "tmv_validation_attempt");
        if (!revised_attempt) continue;
        for (std::int64_t rtc = *rtc_low; rtc <= *rtc_high; ++rtc) {
            if (!launch("1st battle RTC", "RTC_BATTLE", delay, rtc,
                {{"tas_movie_validate_root_1","root_establishment",
                  "analysis.tas_movie_validation_attempt_id","tmv_validation_attempt",*revised_attempt,"expansion"}},
                {{"tas_movie_validate_root_1","rtc","integer",rtc,std::nullopt,"expansion"}})) return false;
        }
    }

    const auto final = [&] {
        auto latest = refreshed;
        for (auto row : List(true)) if (row.workflow_expansion_id == refreshed.workflow_expansion_id) latest = std::move(row);
        const std::int64_t rtc_count = *rtc_high - *rtc_low + 1;
        const std::int64_t expected = (latest.max_neutral_epochs + 1) * rtc_count;
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
