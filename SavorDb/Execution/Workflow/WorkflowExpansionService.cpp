#include "WorkflowExpansionService.h"

#include "WorkflowGraphLaunchService.h"
#include "WorkflowOrchestration.h"
#include "../ProgramDB/TasMovieValidation/TasMovieInputEpochProgram.h"
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
    const auto graph = authoring->GetWorkflowGraphByName(std::string(name));
    return graph
        ? std::optional<std::int64_t>{graph->workflow_graph_revision_id}
        : std::nullopt;
}

bool BindInt64(sqlite3_stmt* statement, int index,
    std::optional<std::int64_t> value) {
    return value ? sqlite3_bind_int64(statement, index, *value) == SQLITE_OK
                 : sqlite3_bind_null(statement, index) == SQLITE_OK;
}

std::optional<std::int64_t> WorkflowGraphRevision(sqlite3* db,
    const std::int64_t workflow_instance_id) {
    sqlite3_stmt* statement = nullptr;
    if (!db || workflow_instance_id <= 0 || sqlite3_prepare_v2(db,
            "SELECT workflow_graph_revision_id FROM exec_workflow_instance WHERE workflow_instance_id=?1;",
            -1, &statement, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_int64(statement, 1, workflow_instance_id);
    std::optional<std::int64_t> value;
    if (sqlite3_step(statement) == SQLITE_ROW
        && sqlite3_column_type(statement, 0) != SQLITE_NULL)
        value = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    return value;
}

std::optional<std::string> WorkflowState(sqlite3* db,
    const std::int64_t workflow_instance_id) {
    sqlite3_stmt* statement = nullptr;
    if (!db || workflow_instance_id <= 0 || sqlite3_prepare_v2(db,
            "SELECT state FROM exec_workflow_instance WHERE workflow_instance_id=?1;",
            -1, &statement, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_int64(statement, 1, workflow_instance_id);
    std::optional<std::string> value;
    if (sqlite3_step(statement) == SQLITE_ROW)
        value = Text(statement, 0);
    sqlite3_finalize(statement);
    return value;
}

std::vector<std::int64_t> MatchingRewriteRequestIds(sqlite3* analysis_db,
    const programdb::tasmovieinputepoch::TasMovieDelayPreparationIdentity& identity) {
    std::vector<std::int64_t> ids;
    sqlite3_stmt* statement = nullptr;
    constexpr const char* sql =
        "SELECT rewrite_request_id FROM tmv_input_epoch_rewrite_request "
        "WHERE source_dtm_sha256=?1 AND schedule_sha256=?2 AND insert_before_epoch=?3 "
        "AND neutral_epoch_count=?4 AND placement_profile=?5 AND full_phase_program_kind=?6 "
        "AND full_phase_program_version=?7 AND full_phase_canonical_id=?8 "
        "AND full_phase_contract_revision=?9 AND full_phase_sha256=?10 "
        "AND module_canonical_id=?11 AND module_revision=?12 AND module_sha256=?13 "
        "ORDER BY rewrite_request_id;";
    if (!analysis_db || sqlite3_prepare_v2(analysis_db, sql, -1, &statement,
            nullptr) != SQLITE_OK) return ids;
    sqlite3_bind_text(statement, 1, identity.source_dtm_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, identity.source_schedule_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 3, static_cast<std::int64_t>(identity.insert_before_epoch));
    sqlite3_bind_int64(statement, 4, static_cast<std::int64_t>(identity.neutral_epoch_count));
    sqlite3_bind_text(statement, 5, identity.placement_profile.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 6, identity.full_phase_program_kind);
    sqlite3_bind_int64(statement, 7, identity.full_phase_program_version);
    sqlite3_bind_text(statement, 8, identity.full_phase_canonical_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 9, identity.full_phase_contract_revision);
    sqlite3_bind_text(statement, 10, identity.full_phase_sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 11, identity.module_canonical_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 12, identity.module_revision);
    sqlite3_bind_text(statement, 13, identity.module_sha256.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(statement) == SQLITE_ROW)
        ids.push_back(sqlite3_column_int64(statement, 0));
    sqlite3_finalize(statement);
    return ids;
}

std::optional<std::int64_t> SuccessfulRewriteAttemptId(sqlite3* analysis_db,
    const std::int64_t request_id) {
    sqlite3_stmt* statement = nullptr;
    if (!analysis_db || sqlite3_prepare_v2(analysis_db,
            "SELECT rewrite_attempt_id FROM tmv_input_epoch_rewrite_attempt WHERE rewrite_request_id=?1 AND succeeded=1 ORDER BY rewrite_attempt_id DESC LIMIT 1;",
            -1, &statement, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_int64(statement, 1, request_id);
    std::optional<std::int64_t> value;
    if (sqlite3_step(statement) == SQLITE_ROW)
        value = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    return value;
}

} // namespace

std::vector<WorkflowExpansionTarget> NormalizeExactFirstBattleExpansionTargets(
    const std::vector<WorkflowExpansionTarget>& requested) {
    std::set<WorkflowExpansionTarget> normalized;
    for (const auto& target : requested) {
        if (target.rtc_value < 0 || target.neutral_epoch_count < 0) continue;
        normalized.insert(target);
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
        request.max_neutral_epochs < 0)
        return false;
    const bool first = request.kind ==
        WorkflowExpansionKind::TasMovieFirstBattleExploration;
    std::set<std::pair<std::string, std::string>> argument_keys;
    for (const auto& argument : request.arguments) {
        const bool integer_value = argument.value_type == "integer"
            || argument.value_type == "boolean";
        const bool text_value = argument.value_type == "text"
            || argument.value_type == "json"
            || argument.value_type == "choice";
        if (argument.member_role != "RTC_BATTLE" || argument.argument_key.empty()
            || (!integer_value && !text_value)
            || (integer_value && (!argument.integer_value || argument.text_value))
            || (text_value && (argument.integer_value || !argument.text_value))
            || !argument_keys.emplace(argument.member_role, argument.argument_key).second) {
            if (error_out) *error_out = "workflow expansion contains an invalid or duplicate child argument";
            return false;
        }
    }
    if (first && (request.source_ref_kind != "tmv_input_epoch_annotation_attempt"
        || request.source_ref_id <= 0)) {
        if (error_out) *error_out = "first-battle expansion requires one annotation authority";
        return false;
    }
    if (!first && request.source_ref_kind != "tas_route_node") {
        if (error_out) *error_out = "delay expansion requires a TAS route node";
        return false;
    }

    std::optional<std::int64_t> source_dtm;
    std::optional<std::int64_t> source_annotation;
    std::optional<std::int64_t> source_attempt;
    std::optional<std::int64_t> inherited_rtc;
    if (first) {
        const auto annotation = analysis_->GetTasMovieInputEpochAnnotationAttempt(
            request.source_ref_id);
        const auto root = annotation && annotation->root_establishment_attempt_id
            ? analysis_->GetTasMovieRootEstablishmentAttempt(
                *annotation->root_establishment_attempt_id)
            : std::nullopt;
        if (!annotation || !annotation->succeeded || !annotation->schedule_artifact_id
            || !root || annotation->source_dtm_artifact_id != root->source_dtm_artifact_id
            || annotation->source_dtm_sha256 != root->source_dtm_sha256) {
            if (error_out) *error_out = "prepared TAS root authorities are missing, unsuccessful, or refer to different DTMs";
            return false;
        }
        const auto artifact = state_->GetArtifact(root->source_dtm_artifact_id);
        if (!artifact || artifact->artifact_kind != "DTM"
            || artifact->sha256 != root->source_dtm_sha256) {
            if (error_out) *error_out = "prepared TAS root DTM artifact is unavailable or drifted";
            return false;
        }
        source_dtm = root->source_dtm_artifact_id;
        source_annotation = annotation->annotation_attempt_id;
        source_attempt = root->root_establishment_attempt_id;
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
        std::optional<std::int64_t> validation_attempt;
        if (sqlite3_step(statement) == SQLITE_ROW)
            validation_attempt = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        if (validation_attempt && sqlite3_prepare_v2(analysis_sqlite_,
            "SELECT root_establishment_attempt_id FROM tmv_root_establishment_attempt WHERE validation_attempt_id=?1 ORDER BY root_establishment_attempt_id DESC LIMIT 1;",
            -1, &statement, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(statement, 1, *validation_attempt);
            if (sqlite3_step(statement) == SQLITE_ROW)
                source_attempt = sqlite3_column_int64(statement, 0);
            sqlite3_finalize(statement);
        }
        if (!source_attempt) {
            if (error_out) *error_out = "TAS route node establishment attempt is unavailable";
            return false;
        }
        if (sqlite3_prepare_v2(analysis_sqlite_,
            "SELECT annotation_attempt_id FROM tmv_input_epoch_annotation_attempt WHERE root_establishment_attempt_id=?1 AND succeeded=1 AND schedule_artifact_id IS NOT NULL ORDER BY annotation_attempt_id DESC LIMIT 1;",
            -1, &statement, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(statement, 1, *source_attempt);
            if (sqlite3_step(statement) == SQLITE_ROW)
                source_annotation = sqlite3_column_int64(statement, 0);
            sqlite3_finalize(statement);
        }
        if (!source_annotation) {
            if (error_out) *error_out = "TAS route node annotation authority is unavailable";
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
            for (std::int64_t delay = 0; delay <= request.max_neutral_epochs; ++delay)
                targets.push_back({delay, rtc});
            if (rtc == *request.rtc_max) break;
        }
        targets = NormalizeExactFirstBattleExpansionTargets(targets);
    } else if (first) {
        targets = NormalizeExactFirstBattleExpansionTargets(targets);
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
        "INSERT INTO exec_workflow_expansion(expansion_kind,state,source_ref_kind,source_ref_id,inherited_rtc,rtc_min,rtc_max,max_neutral_epochs,created_by,created_at_utc,updated_at_utc) VALUES(?1,'RUNNING',?2,?3,?4,?5,?6,?7,?8,?9,?9);";
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        rollback();
        return false;
    }
    const auto now = types::UtcNow().time_since_epoch().count();
    const auto kind = KindText(request.kind);
    sqlite3_bind_text(statement, 1, kind.c_str(), -1, SQLITE_TRANSIENT);
    static constexpr auto kAnnotationAuthorityKind =
        "tmv_input_epoch_annotation_attempt";
    sqlite3_bind_text(statement, 2, kAnnotationAuthorityKind, -1, SQLITE_STATIC);
    sqlite3_bind_int64(statement, 3, *source_annotation);
    BindInt64(statement, 4, inherited_rtc);
    sqlite3_bind_int64(statement, 5, rtc_min);
    sqlite3_bind_int64(statement, 6, rtc_max);
    sqlite3_bind_int64(statement, 7, max_delay);
    const auto created_by = request.created_by.empty() ? "SavorDb" : request.created_by;
    sqlite3_bind_text(statement, 8, created_by.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 9, now);
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
    if (!request.arguments.empty()) {
        constexpr const char* argument_sql =
            "INSERT INTO exec_workflow_expansion_argument(workflow_expansion_id,member_role,argument_key,value_type,integer_value,text_value,source_kind,created_at_utc) VALUES(?1,?2,?3,?4,?5,?6,?7,?8);";
        if (sqlite3_prepare_v2(db_, argument_sql, -1, &statement, nullptr) != SQLITE_OK) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            rollback();
            return false;
        }
        for (const auto& argument : request.arguments) {
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
            sqlite3_bind_int64(statement, 1, expansion_id);
            sqlite3_bind_text(statement, 2, argument.member_role.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 3, argument.argument_key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(statement, 4, argument.value_type.c_str(), -1, SQLITE_TRANSIENT);
            BindInt64(statement, 5, argument.integer_value);
            if (argument.text_value) sqlite3_bind_text(statement, 6, argument.text_value->c_str(), -1, SQLITE_TRANSIENT);
            else sqlite3_bind_null(statement, 6);
            const auto source_kind = argument.source_kind.empty() ? "expansion" : argument.source_kind;
            sqlite3_bind_text(statement, 7, source_kind.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(statement, 8, now);
            if (sqlite3_step(statement) != SQLITE_DONE) {
                if (error_out) *error_out = sqlite3_errmsg(db_);
                sqlite3_finalize(statement);
                rollback();
                return false;
            }
        }
        sqlite3_finalize(statement);
    }
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
        row.source_annotation_attempt_id = row.source_ref_id;
        const auto annotation = analysis_
            ? analysis_->GetTasMovieInputEpochAnnotationAttempt(row.source_ref_id)
            : std::nullopt;
        if (annotation) {
            row.source_dtm_artifact_id = annotation->source_dtm_artifact_id;
            row.source_root_establishment_attempt_id =
                annotation->root_establishment_attempt_id;
        }
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
            "SELECT member_role,argument_key,value_type,integer_value,text_value,source_kind FROM exec_workflow_expansion_argument WHERE workflow_expansion_id=?1 ORDER BY member_role,argument_key;",
            -1, &statement, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(statement, 1, row.workflow_expansion_id);
            while (sqlite3_step(statement) == SQLITE_ROW) {
                WorkflowExpansionArgumentValue argument{};
                argument.member_role = Text(statement, 0);
                argument.argument_key = Text(statement, 1);
                argument.value_type = Text(statement, 2);
                if (sqlite3_column_type(statement, 3) != SQLITE_NULL)
                    argument.integer_value = sqlite3_column_int64(statement, 3);
                if (sqlite3_column_type(statement, 4) != SQLITE_NULL)
                    argument.text_value = Text(statement, 4);
                argument.source_kind = Text(statement, 5);
                row.arguments.push_back(std::move(argument));
            }
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

std::vector<PreparedTasRootSourceSnapshot>
WorkflowExpansionService::ListPreparedTasRootSources(const int limit) const {
    std::lock_guard lock(db_mutex_);
    std::vector<PreparedTasRootSourceSnapshot> rows;
    if (!analysis_ || !analysis_sqlite_ || limit <= 0) return rows;
    const auto route_nodes = analysis_->ListTasRouteNodes();
    for (const auto& root : analysis_->ListTasMovieRootEstablishmentAttempts(limit)) {
        if (root.producer != TasMovieRootEstablishmentProducer::Establish
            || root.parent_root_establishment_attempt_id) continue;
        sqlite3_stmt* statement = nullptr;
        constexpr const char* sql =
            "SELECT annotation_attempt_id FROM tmv_input_epoch_annotation_attempt "
            "WHERE succeeded=1 AND schedule_artifact_id IS NOT NULL "
            "AND root_establishment_attempt_id=?1 "
            "ORDER BY annotation_attempt_id DESC LIMIT 1;";
        if (sqlite3_prepare_v2(analysis_sqlite_, sql, -1, &statement, nullptr)
            != SQLITE_OK) continue;
        sqlite3_bind_int64(statement, 1, root.root_establishment_attempt_id);
        if (sqlite3_step(statement) == SQLITE_ROW) {
            const auto annotation_id = sqlite3_column_int64(statement, 0);
            const auto route = std::find_if(route_nodes.begin(), route_nodes.end(),
                [&](const auto& node) {
                    return node.node_kind == TasRouteNodeKind::Checkpoint
                        && node.root_establishment_attempt_id
                            == root.root_establishment_attempt_id;
                });
            const std::string prefix = route == route_nodes.end()
                ? "Established TAS" : route->label;
            rows.push_back({
                .annotation_attempt_id = annotation_id,
                .root_establishment_attempt_id = root.root_establishment_attempt_id,
                .source_dtm_artifact_id = root.source_dtm_artifact_id,
                .source_dtm_sha256 = root.source_dtm_sha256,
                .display_name = prefix + " | DTM #" + std::to_string(root.source_dtm_artifact_id)
                    + " | annotation #" + std::to_string(annotation_id)
                    + " | root #" + std::to_string(root.root_establishment_attempt_id),
            });
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
    if (!snapshot_out || !db_ || !analysis_) return false;
    auto expansions = List(true);
    FirstBattleCoverageQuery request = requested_query;
    const auto canonical_root = [&](std::int64_t root_id,
        std::string* diagnostic) -> std::optional<TasMovieRootEstablishmentAttemptRecord> {
        std::set<std::int64_t> seen;
        for (;;) {
            if (root_id <= 0 || !seen.insert(root_id).second) {
                if (diagnostic) *diagnostic = "root-establishment lineage is missing or cyclic";
                return std::nullopt;
            }
            const auto root = analysis_->GetTasMovieRootEstablishmentAttempt(root_id);
            if (!root) {
                if (diagnostic) *diagnostic = "root-establishment lineage is incomplete";
                return std::nullopt;
            }
            if (!root->parent_root_establishment_attempt_id) {
                if (root->producer != TasMovieRootEstablishmentProducer::Establish) {
                    if (diagnostic) *diagnostic = "root lineage does not terminate at ESTABLISH";
                    return std::nullopt;
                }
                return root;
            }
            if (root->producer != TasMovieRootEstablishmentProducer::Revise) {
                if (diagnostic) *diagnostic = "non-REVISE root has a parent authority";
                return std::nullopt;
            }
            root_id = *root->parent_root_establishment_attempt_id;
        }
    };
    if (request.workflow_expansion_id) {
        const auto found = std::find_if(expansions.begin(), expansions.end(),
            [&](const auto& row) {
                return row.workflow_expansion_id == *request.workflow_expansion_id &&
                    row.kind == WorkflowExpansionKind::TasMovieFirstBattleExploration;
            });
        if (found == expansions.end() || !found->source_annotation_attempt_id) {
            if (error_out) *error_out = "first-battle expansion was not found";
            return false;
        }
        const auto annotation = analysis_->GetTasMovieInputEpochAnnotationAttempt(
            *found->source_annotation_attempt_id);
        const auto root = annotation && annotation->root_establishment_attempt_id
            ? canonical_root(*annotation->root_establishment_attempt_id, error_out)
            : std::nullopt;
        if (!root) return false;
        request.source_root_establishment_attempt_id =
            root->root_establishment_attempt_id;
    }
    const auto root = canonical_root(
        request.source_root_establishment_attempt_id, error_out);
    if (!root || root->root_establishment_attempt_id
            != request.source_root_establishment_attempt_id) {
        if (error_out) *error_out = "invalid first-battle coverage query";
        return false;
    }
    sqlite3_stmt* latest_statement = nullptr;
    constexpr auto latest_sql =
        "SELECT annotation_attempt_id FROM tmv_input_epoch_annotation_attempt "
        "WHERE succeeded=1 AND schedule_artifact_id IS NOT NULL "
        "AND root_establishment_attempt_id=?1 ORDER BY annotation_attempt_id DESC LIMIT 1;";
    if (!analysis_sqlite_ || sqlite3_prepare_v2(analysis_sqlite_, latest_sql, -1,
            &latest_statement, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = "latest root annotation could not be queried";
        return false;
    }
    sqlite3_bind_int64(latest_statement, 1, root->root_establishment_attempt_id);
    const auto latest_annotation_id = sqlite3_step(latest_statement) == SQLITE_ROW
        ? std::optional<std::int64_t>(sqlite3_column_int64(latest_statement, 0))
        : std::nullopt;
    sqlite3_finalize(latest_statement);
    if (!latest_annotation_id) {
        if (error_out) *error_out = "established TAS route has no successful annotation";
        return false;
    }

    FirstBattleCoverageSnapshot result{};
    result.source_dtm_artifact_id = root->source_dtm_artifact_id;
    result.source_annotation_attempt_id = *latest_annotation_id;
    result.source_root_establishment_attempt_id =
        root->root_establishment_attempt_id;
    std::vector<const WorkflowExpansionSnapshot*> matching_expansions;
    std::set<std::int64_t> rtc_values;
    std::set<std::int64_t> delay_values;
    for (const auto& expansion : expansions) {
        if (expansion.kind != WorkflowExpansionKind::TasMovieFirstBattleExploration
            || !expansion.source_annotation_attempt_id) continue;
        const auto annotation = analysis_->GetTasMovieInputEpochAnnotationAttempt(
            *expansion.source_annotation_attempt_id);
        const auto expansion_root = annotation && annotation->root_establishment_attempt_id
            ? canonical_root(*annotation->root_establishment_attempt_id, nullptr)
            : std::nullopt;
        if (!expansion_root || expansion_root->root_establishment_attempt_id
                != root->root_establishment_attempt_id) continue;
        matching_expansions.push_back(&expansion);
        for (const auto& target : expansion.targets) {
            rtc_values.insert(target.rtc_value);
            delay_values.insert(target.neutral_epoch_count);
        }
    }
    result.rtc_values.assign(rtc_values.begin(), rtc_values.end());
    result.neutral_epoch_counts.assign(delay_values.begin(), delay_values.end());
    std::map<std::int64_t, FirstBattleDelayPreparationSnapshot> preparations;
    for (const auto delay : result.neutral_epoch_counts)
        preparations.emplace(delay, FirstBattleDelayPreparationSnapshot{.neutral_epoch_count=delay});
    std::map<std::pair<std::int64_t, std::int64_t>, FirstBattleCoverageCellSnapshot> cells;
    for (const auto rtc : result.rtc_values) {
        for (const auto delay : result.neutral_epoch_counts)
            cells.emplace(std::pair{rtc, delay},
                FirstBattleCoverageCellSnapshot{.rtc_value=rtc,
                                                 .neutral_epoch_count=delay});
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
    for (const auto* expansion_ptr : matching_expansions) {
        const auto& expansion = *expansion_ptr;
        for (const auto& member : expansion.members) {
            const auto graph = workflow_query->GetWorkflowGraph(member.workflow_instance_id);
            if (member.role == "DELAY_PRODUCTION"
                && preparations.contains(member.neutral_epoch_count))
                merge_preparation(preparations[member.neutral_epoch_count], member,
                                  graph ? &*graph : nullptr);
        }

        for (const auto& target : expansion.targets) {
            auto& cell = cells[std::pair{target.rtc_value, target.neutral_epoch_count}];
            cell.requested = true;
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
        if (cell.requested && !preparation.retryable_workflow_instance_ids.empty()) {
            cell.retryable = true;
            cell.retryable_workflow_instance_ids.insert(
                cell.retryable_workflow_instance_ids.end(),
                preparation.retryable_workflow_instance_ids.begin(),
                preparation.retryable_workflow_instance_ids.end());
            if (cell.lifecycle == "NOT_RUN" || cell.lifecycle == "WAITING")
                cell.lifecycle = preparation.state;
        } else if (cell.requested && IsActive(preparation.state) && cell.lifecycle == "NOT_RUN") {
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
    if (!receipt_out || request.source_root_establishment_attempt_id <= 0) {
        if (error_out) *error_out = "established TAS route authority is required";
        return false;
    }
    LaunchMissingFirstBattleCoverageReceipt receipt{};
    std::set<WorkflowExpansionTarget> requested_unique;
    for (const auto& target : request.targets)
        if (target.rtc_value >= 0 && target.neutral_epoch_count >= 0)
            requested_unique.insert(target);
    receipt.requested_count = static_cast<std::int64_t>(requested_unique.size());
    const auto normalized = NormalizeExactFirstBattleExpansionTargets(request.targets);
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
    if (!ReadFirstBattleCoverage({
            .source_root_establishment_attempt_id=
                request.source_root_establishment_attempt_id},
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
        create.source_ref_kind = "tmv_input_epoch_annotation_attempt";
        create.source_ref_id = *coverage.source_annotation_attempt_id;
        create.rtc_min = rtc_min_it->rtc_value;
        create.rtc_max = rtc_max_it->rtc_value;
        create.max_neutral_epochs = max_delay_it->neutral_epoch_count;
        create.targets = std::move(launch);
        create.arguments = {
            {"RTC_BATTLE","samples_per_axis","integer",5,std::nullopt,"coverage_default"},
            {"RTC_BATTLE","fake_attack_min","integer",0,std::nullopt,"coverage_default"},
            {"RTC_BATTLE","fake_attack_max","integer",0,std::nullopt,"coverage_default"},
            {"RTC_BATTLE","continuation_mode","choice",std::nullopt,
             std::string("automatic_best_per_ending_rng"),"coverage_default"},
            {"RTC_BATTLE","continue_automatic_exploration_after_victory","boolean",0,
             std::nullopt,"coverage_default"},
        };
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

    const auto attach = [&](const std::string& role, const std::int64_t delay,
        const std::optional<std::int64_t> rtc,
        const std::int64_t workflow) -> bool {
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

    const auto launch = [&](std::string_view graph_name, std::string role,
        std::int64_t delay, std::optional<std::int64_t> rtc,
        std::vector<WorkflowLaunchInputValue> inputs,
        std::vector<WorkflowLaunchArgumentValue> arguments,
        std::optional<std::string> semantic_launch_key = std::nullopt) -> bool {
        if (FindMember(refreshed, role, delay, rtc)) return true;
        const auto revision = GraphRevision(authoring_, graph_name);
        if (!revision) {
            if (error_out) *error_out = "required expansion workflow graph is unavailable: " + std::string(graph_name);
            return false;
        }
        const auto graph = authoring_->GetWorkflowGraphRevision(*revision);
        if (!graph) {
            if (error_out) *error_out = "required expansion workflow graph revision is unavailable: " + std::string(graph_name);
            return false;
        }
        for (const auto& expansion_argument : refreshed.arguments) {
            if (expansion_argument.member_role != role) continue;
            const WorkflowGraphNodeSnapshot* matched_node = nullptr;
            const WorkflowGraphNodeArgumentSnapshot* matched_argument = nullptr;
            for (const auto& node : graph->nodes) {
                for (const auto& candidate : node.arguments) {
                    if (candidate.argument_key != expansion_argument.argument_key) continue;
                    if (matched_argument != nullptr) {
                        if (error_out) *error_out = "expansion child argument is ambiguous: "
                            + std::string(graph_name) + "." + expansion_argument.argument_key;
                        return false;
                    }
                    matched_node = &node;
                    matched_argument = &candidate;
                }
            }
            if (matched_node == nullptr || matched_argument == nullptr) {
                if (error_out) *error_out = "expansion child argument is unavailable: "
                    + std::string(graph_name) + "." + expansion_argument.argument_key;
                return false;
            }
            if (matched_argument->value_type != expansion_argument.value_type) {
                if (error_out) *error_out = "expansion child argument type does not match: "
                    + std::string(graph_name) + "." + expansion_argument.argument_key;
                return false;
            }
            arguments.push_back({
                matched_node->node_key,
                expansion_argument.argument_key,
                expansion_argument.value_type,
                expansion_argument.integer_value,
                expansion_argument.text_value,
                "expansion",
            });
        }
        const auto launch_key = semantic_launch_key.value_or("workflow-expansion:" +
            std::to_string(refreshed.workflow_expansion_id) + ":" + role + ":" +
            std::to_string(delay) + ":" + (rtc ? std::to_string(*rtc) : "none"));
        std::int64_t workflow = 0;
        if (!WorkflowGraphLaunchService(authoring_, execution_).Start(
            {.workflow_graph_revision_id=*revision, .created_by="WorkflowExpansionService",
             .launch_key=launch_key, .input_bindings=std::move(inputs),
             .arguments=std::move(arguments)}, &workflow, error_out)) return false;
        return attach(role, delay, rtc, workflow);
    };

    std::optional<std::int64_t> source_dtm;
    std::optional<std::int64_t> source_annotation;
    std::optional<std::int64_t> source_attempt;
    std::optional<std::int64_t> inherited_rtc;
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db_,
        "SELECT source_ref_id,inherited_rtc FROM exec_workflow_expansion WHERE workflow_expansion_id=?1;",
        -1, &statement, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(statement, 1, refreshed.workflow_expansion_id);
    if (sqlite3_step(statement) == SQLITE_ROW) {
        source_annotation=sqlite3_column_int64(statement,0);
        if (sqlite3_column_type(statement,1)!=SQLITE_NULL) inherited_rtc=sqlite3_column_int64(statement,1);
    }
    sqlite3_finalize(statement);
    const auto annotation = source_annotation
        ? analysis_->GetTasMovieInputEpochAnnotationAttempt(*source_annotation)
        : std::nullopt;
    const auto root = annotation && annotation->root_establishment_attempt_id
        ? analysis_->GetTasMovieRootEstablishmentAttempt(
            *annotation->root_establishment_attempt_id)
        : std::nullopt;
    if (!annotation || !root || !annotation->succeeded
        || annotation->source_dtm_artifact_id != root->source_dtm_artifact_id
        || annotation->source_dtm_sha256 != root->source_dtm_sha256) return false;
    source_dtm = annotation->source_dtm_artifact_id;
    source_attempt = root->root_establishment_attempt_id;

    const auto establishment_rtc = refreshed.kind == WorkflowExpansionKind::TasMovieDelayExploration
        ? inherited_rtc : refreshed.rtc_min;
    if (!establishment_rtc) return false;
    for (const auto& target : refreshed.targets) {
        if (target.neutral_epoch_count != 0) continue;
        const auto rtc = target.rtc_value;
        if (!launch("First Battle Exploration: RTC Branch", "RTC_BATTLE", 0, rtc,
            {{"tas_movie_validate_root_1","root_establishment",
              "analysis.tas_movie_root_establishment_attempt_id","tmv_root_establishment_attempt",*source_attempt,"expansion"}},
            {{"tas_movie_validate_root_1","rtc","integer",rtc,std::nullopt,"expansion"}})) return false;
    }

    const bool has_positive_delay = std::any_of(refreshed.targets.begin(), refreshed.targets.end(),
        [](const auto& target) { return target.neutral_epoch_count > 0; });
    if (has_positive_delay) {
    if (!source_annotation) return true;

    std::set<std::int64_t> delays;
    for (const auto& target : refreshed.targets)
        if (target.neutral_epoch_count > 0) delays.insert(target.neutral_epoch_count);
    for (const auto delay : delays) {
        const auto revise_revision = GraphRevision(authoring_,
            "TAS Movie Expansion: Revise");
        if (!revise_revision) {
            if (error_out) *error_out =
                "required expansion workflow graph is unavailable: TAS Movie Expansion: Revise";
            return false;
        }
        programdb::tasmovieinputepoch::TasMovieDelayPlacementResolution placement;
        if (!programdb::tasmovieinputepoch::ResolveTasMovieDelayPlacement(
                state_, *annotation, std::nullopt,
                std::string_view("first_battle.final_dialog"), &placement,
                error_out)) return false;
        const auto identity =
            programdb::tasmovieinputepoch::BuildTasMovieDelayPreparationIdentity(
                *annotation, *root, placement,
                static_cast<std::uint64_t>(delay), *revise_revision);
        const auto preparation_hash =
            programdb::tasmovieinputepoch::HashTasMovieDelayPreparationIdentity(
                identity);
        const auto production = FindMember(refreshed, "DELAY_PRODUCTION", delay);
        if (!production) {
            std::optional<std::int64_t> reusable_workflow;
            std::optional<std::int64_t> active_workflow;
            std::optional<std::int64_t> attention_workflow;
            for (const auto request_id : MatchingRewriteRequestIds(
                    analysis_sqlite_, identity)) {
                const auto request =
                    analysis_->GetTasMovieInputEpochRewriteRequest(request_id);
                const auto request_root = request
                    ? analysis_->GetTasMovieRootEstablishmentAttempt(
                        request->root_establishment_attempt_id) : std::nullopt;
                if (!request || !request_root
                    || !programdb::tasmovieinputepoch::TasMovieRewriteRequestMatchesPreparation(
                        *request, *request_root, identity)) continue;
                const auto graph_revision = WorkflowGraphRevision(
                    db_, request->workflow_instance_id);
                if (!graph_revision || *graph_revision != *revise_revision) continue;
                const auto attempt_id = SuccessfulRewriteAttemptId(
                    analysis_sqlite_, request_id);
                if (attempt_id) {
                    const auto attempt =
                        analysis_->GetTasMovieInputEpochRewriteAttempt(*attempt_id);
                    if (!attempt
                        || !programdb::tasmovieinputepoch::ValidateReusableTasMovieDelayPreparation(
                            state_, analysis_, *request, *attempt, identity,
                            error_out)) return false;
                    reusable_workflow = request->workflow_instance_id;
                    break;
                }
                const auto workflow_state = WorkflowState(
                    db_, request->workflow_instance_id);
                if (workflow_state && IsActive(*workflow_state)
                    && !active_workflow) {
                    active_workflow = request->workflow_instance_id;
                } else if (workflow_state && IsAttention(*workflow_state)
                    && !attention_workflow) {
                    attention_workflow = request->workflow_instance_id;
                } else if (workflow_state && *workflow_state == "COMPLETED") {
                    if (error_out) *error_out =
                        "matching TAS delay producer completed without a successful durable rewrite attempt";
                    return false;
                }
            }
            if (!reusable_workflow)
                reusable_workflow = active_workflow
                    ? active_workflow : attention_workflow;
            if (reusable_workflow) {
                if (!attach("DELAY_PRODUCTION", delay, std::nullopt,
                        *reusable_workflow)) return false;
                continue;
            }
            if (!launch("TAS Movie Expansion: Revise", "DELAY_PRODUCTION",
                delay, std::nullopt,
                {{"tas_movie_revise_1","annotation_attempt",
                  "analysis.tas_movie_input_epoch_annotation_attempt_id",
                  "tmv_input_epoch_annotation_attempt",*source_annotation,"expansion"},
                },
                {{"tas_movie_revise_1","neutral_epoch_count","integer",delay,std::nullopt,"expansion"},
                 {"tas_movie_revise_1","placement_profile","choice",std::nullopt,
                  std::string("first_battle.final_dialog"),"expansion"}},
                "tas-delay-preparation:" + preparation_hash)) return false;
            continue;
        }
        if (production->state != "COMPLETED") continue;
        std::optional<TasMovieInputEpochRewriteRequestRecord> producer_request;
        for (const auto request_id : MatchingRewriteRequestIds(
                analysis_sqlite_, identity)) {
            const auto candidate =
                analysis_->GetTasMovieInputEpochRewriteRequest(request_id);
            if (candidate
                && candidate->workflow_instance_id == production->workflow_instance_id
                && WorkflowGraphRevision(db_, candidate->workflow_instance_id)
                    == std::optional<std::int64_t>(*revise_revision)) {
                producer_request = candidate;
                break;
            }
        }
        const auto producer_attempt_id = producer_request
            ? SuccessfulRewriteAttemptId(analysis_sqlite_,
                producer_request->rewrite_request_id) : std::nullopt;
        const auto producer_attempt = producer_attempt_id
            ? analysis_->GetTasMovieInputEpochRewriteAttempt(*producer_attempt_id)
            : std::nullopt;
        if (!producer_request || !producer_attempt
            || !programdb::tasmovieinputepoch::ValidateReusableTasMovieDelayPreparation(
                state_, analysis_, *producer_request, *producer_attempt,
                identity, error_out)) return false;
        const auto revised_annotation = Output(execution_, production->workflow_instance_id,
            "tas_movie_revise_1", "annotation_attempt",
            "tmv_input_epoch_annotation_attempt");
        const auto child_annotation = revised_annotation
            ? analysis_->GetTasMovieInputEpochAnnotationAttempt(*revised_annotation)
            : std::nullopt;
        if (!child_annotation || !child_annotation->root_establishment_attempt_id
            || producer_attempt->produced_annotation_attempt_id != revised_annotation
            || producer_attempt->produced_root_establishment_attempt_id
                != child_annotation->root_establishment_attempt_id) {
            if (error_out) *error_out =
                "shared TAS delay producer output does not match its verified authority set";
            return false;
        }
        for (const auto& target : refreshed.targets) {
            if (target.neutral_epoch_count != delay) continue;
            const auto rtc = target.rtc_value;
            if (!launch("First Battle Exploration: RTC Branch", "RTC_BATTLE", delay, rtc,
                {{"tas_movie_validate_root_1","root_establishment",
                  "analysis.tas_movie_root_establishment_attempt_id","tmv_root_establishment_attempt",*child_annotation->root_establishment_attempt_id,"expansion"}},
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
