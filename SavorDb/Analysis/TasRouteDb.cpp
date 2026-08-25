#include "SqliteAnalysisDb.h"

#include <sqlite3.h>

#include <optional>
#include <string>

namespace savor::db::analysis {
namespace {

struct Statement {
    sqlite3_stmt* value = nullptr;
    ~Statement() { sqlite3_finalize(value); }
};

std::string Text(sqlite3_stmt* st, int column) {
    const auto* value = sqlite3_column_text(st, column);
    return value ? reinterpret_cast<const char*>(value) : std::string{};
}

std::optional<std::int64_t> OptionalI64(sqlite3_stmt* st, int column) {
    if (sqlite3_column_type(st, column) == SQLITE_NULL) return std::nullopt;
    return sqlite3_column_int64(st, column);
}

types::UtcTimePoint Time(sqlite3_stmt* st, int column) {
    return types::UtcTimePoint(types::UtcTimePoint::duration(
        sqlite3_column_int64(st, column)));
}

bool Begin(sqlite3* db, std::string* error_out) {
    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK)
        return true;
    if (error_out) *error_out = sqlite3_errmsg(db);
    return false;
}

bool Rollback(sqlite3* db, std::string message, std::string* error_out) {
    (void)sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    if (error_out) *error_out = std::move(message);
    return false;
}

bool Commit(sqlite3* db, std::string* error_out) {
    if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK) {
        if (error_out) error_out->clear();
        return true;
    }
    return Rollback(db, sqlite3_errmsg(db), error_out);
}

std::optional<std::int64_t> ScalarI64(
    sqlite3* db, const char* sql, std::int64_t value) {
    Statement st;
    if (sqlite3_prepare_v2(db, sql, -1, &st.value, nullptr) != SQLITE_OK)
        return std::nullopt;
    sqlite3_bind_int64(st.value, 1, value);
    return sqlite3_step(st.value) == SQLITE_ROW
        ? std::optional<std::int64_t>(sqlite3_column_int64(st.value, 0))
        : std::nullopt;
}

} // namespace

bool SqliteAnalysisDb::EnsureBattleRouteActivity(
    const EnsureBattleRouteActivityCommand& command,
    EnsureBattleRouteActivityReceipt* receipt_out,
    std::string* error_out) {
    if (!db_ || command.battle_set_id <= 0 || command.entry_savestate_id <= 0 ||
        command.battle_plan_id <= 0 || command.activity_key.empty() ||
        command.default_label.empty()) {
        if (error_out) *error_out = "battle route activity is incomplete";
        return false;
    }
    EnsureBattleRouteActivityReceipt receipt{};
    if (!Begin(db_, error_out)) return false;

    if (const auto existing = ScalarI64(db_,
            "SELECT route_node_id FROM atr_battle_set_route WHERE battle_set_id=?1;",
            command.battle_set_id)) {
        receipt.activity_route_node_id = *existing;
        receipt.root_route_node_id = ScalarI64(db_,
            "WITH RECURSIVE lineage(id,parent_id) AS ("
            "SELECT route_node_id,parent_route_node_id FROM atr_route_node WHERE route_node_id=?1 "
            "UNION ALL SELECT n.route_node_id,n.parent_route_node_id FROM atr_route_node n "
            "JOIN lineage l ON l.parent_id=n.route_node_id) "
            "SELECT id FROM lineage WHERE parent_id IS NULL LIMIT 1;", *existing).value_or(0);
        if (!Commit(db_, error_out)) return false;
        if (receipt_out) *receipt_out = receipt;
        return true;
    }

    std::optional<std::int64_t> source_dtm;
    {
        Statement st;
        constexpr auto sql =
            "SELECT r.source_dtm_artifact_id FROM tmv_checkpoint_sterilization_request r "
            "LEFT JOIN tmv_checkpoint_sterilization_attempt a ON a.sterilization_request_id=r.sterilization_request_id "
            "WHERE r.reused_savestate_id=?1 OR a.produced_savestate_id=?1 "
            "ORDER BY r.sterilization_request_id DESC LIMIT 1;";
        if (sqlite3_prepare_v2(db_, sql, -1, &st.value, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(st.value, 1, command.entry_savestate_id);
            if (sqlite3_step(st.value) == SQLITE_ROW)
                source_dtm = sqlite3_column_int64(st.value, 0);
        }
    }

    std::optional<std::int64_t> parent_checkpoint;
    {
        Statement st;
        constexpr auto sql =
            "SELECT checkpoint_route_node_id FROM atr_victory_branch "
            "WHERE checkpoint_savestate_id=?1 "
            "UNION ALL "
            "SELECT b.checkpoint_route_node_id FROM atr_victory_branch b "
            "JOIN tmv_checkpoint_sterilization_request r ON r.source_savestate_id=b.checkpoint_savestate_id "
            "LEFT JOIN tmv_checkpoint_sterilization_attempt a ON a.sterilization_request_id=r.sterilization_request_id "
            "WHERE r.reused_savestate_id=?1 OR a.produced_savestate_id=?1 LIMIT 1;";
        if (sqlite3_prepare_v2(db_, sql, -1, &st.value, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(st.value, 1, command.entry_savestate_id);
            if (sqlite3_step(st.value) == SQLITE_ROW)
                parent_checkpoint = sqlite3_column_int64(st.value, 0);
        }
    }

    if (parent_checkpoint) {
        receipt.root_route_node_id = *parent_checkpoint;
    } else {
        Statement root;
        constexpr auto sql =
            "INSERT INTO atr_route_node(parent_route_node_id,node_kind,activity_kind,activity_key,label,description,"
            "source_dtm_artifact_id,source_savestate_id,battle_plan_id,tas_movie_tree_id,status,created_at_utc,updated_at_utc) "
            "VALUES(NULL,'CHECKPOINT','','',?1,'Source TAS movie',?2,?3,NULL,NULL,'READY',?4,?4) "
            "ON CONFLICT DO NOTHING;";
        if (sqlite3_prepare_v2(db_, sql, -1, &root.value, nullptr) != SQLITE_OK)
            return Rollback(db_, sqlite3_errmsg(db_), error_out);
        const std::string label = source_dtm
            ? "TAS source " + std::to_string(*source_dtm)
            : "Checkpoint " + std::to_string(command.entry_savestate_id);
        sqlite3_bind_text(root.value, 1, label.c_str(), -1, SQLITE_TRANSIENT);
        if (source_dtm) sqlite3_bind_int64(root.value, 2, *source_dtm);
        else sqlite3_bind_null(root.value, 2);
        sqlite3_bind_int64(root.value, 3, command.entry_savestate_id);
        sqlite3_bind_int64(root.value, 4, command.created_at_utc.time_since_epoch().count());
        if (sqlite3_step(root.value) != SQLITE_DONE)
            return Rollback(db_, sqlite3_errmsg(db_), error_out);
        Statement find;
        const char* find_sql = source_dtm
            ? "SELECT route_node_id FROM atr_route_node WHERE parent_route_node_id IS NULL AND source_dtm_artifact_id=?1 LIMIT 1;"
            : "SELECT route_node_id FROM atr_route_node WHERE parent_route_node_id IS NULL AND source_dtm_artifact_id IS NULL AND source_savestate_id=?1 LIMIT 1;";
        if (sqlite3_prepare_v2(db_, find_sql, -1, &find.value, nullptr) != SQLITE_OK)
            return Rollback(db_, sqlite3_errmsg(db_), error_out);
        sqlite3_bind_int64(find.value, 1, source_dtm.value_or(command.entry_savestate_id));
        if (sqlite3_step(find.value) != SQLITE_ROW)
            return Rollback(db_, "TAS route root was not persisted", error_out);
        receipt.root_route_node_id = sqlite3_column_int64(find.value, 0);
        parent_checkpoint = receipt.root_route_node_id;
    }

    Statement activity;
    constexpr auto activity_sql =
        "INSERT INTO atr_route_node(parent_route_node_id,node_kind,activity_kind,activity_key,label,description,"
        "source_dtm_artifact_id,source_savestate_id,battle_plan_id,tas_movie_tree_id,status,created_at_utc,updated_at_utc) "
        "VALUES(?1,'ACTIVITY','battle',?2,?3,?4,NULL,?5,?6,NULL,'ACTIVE',?7,?7) "
        "ON CONFLICT(parent_route_node_id,activity_kind,activity_key) DO NOTHING;";
    if (sqlite3_prepare_v2(db_, activity_sql, -1, &activity.value, nullptr) != SQLITE_OK)
        return Rollback(db_, sqlite3_errmsg(db_), error_out);
    sqlite3_bind_int64(activity.value, 1, *parent_checkpoint);
    sqlite3_bind_text(activity.value, 2, command.activity_key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(activity.value, 3, command.default_label.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(activity.value, 4, command.default_description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(activity.value, 5, command.entry_savestate_id);
    sqlite3_bind_int64(activity.value, 6, command.battle_plan_id);
    sqlite3_bind_int64(activity.value, 7, command.created_at_utc.time_since_epoch().count());
    if (sqlite3_step(activity.value) != SQLITE_DONE)
        return Rollback(db_, sqlite3_errmsg(db_), error_out);

    Statement find_activity;
    if (sqlite3_prepare_v2(db_,
            "SELECT route_node_id FROM atr_route_node WHERE parent_route_node_id=?1 AND activity_kind='battle' AND activity_key=?2;",
            -1, &find_activity.value, nullptr) != SQLITE_OK)
        return Rollback(db_, sqlite3_errmsg(db_), error_out);
    sqlite3_bind_int64(find_activity.value, 1, *parent_checkpoint);
    sqlite3_bind_text(find_activity.value, 2, command.activity_key.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(find_activity.value) != SQLITE_ROW)
        return Rollback(db_, "battle route activity was not persisted", error_out);
    receipt.activity_route_node_id = sqlite3_column_int64(find_activity.value, 0);

    Statement bind;
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO atr_battle_set_route(battle_set_id,route_node_id) VALUES(?1,?2);",
            -1, &bind.value, nullptr) != SQLITE_OK)
        return Rollback(db_, sqlite3_errmsg(db_), error_out);
    sqlite3_bind_int64(bind.value, 1, command.battle_set_id);
    sqlite3_bind_int64(bind.value, 2, receipt.activity_route_node_id);
    if (sqlite3_step(bind.value) != SQLITE_DONE)
        return Rollback(db_, sqlite3_errmsg(db_), error_out);

    if (!Commit(db_, error_out)) return false;
    if (receipt_out) *receipt_out = receipt;
    return true;
}

bool SqliteAnalysisDb::EnsurePendingVictoryRouteBranch(
    const EnsurePendingVictoryRouteBranchCommand& command,
    EnsurePendingVictoryRouteBranchReceipt* receipt_out,
    std::string* error_out) {
    if (!db_ || command.battle_set_id <= 0 || command.selected_turn_job_id <= 0) {
        if (error_out) *error_out = "pending Victory route branch is incomplete";
        return false;
    }
    EnsurePendingVictoryRouteBranchReceipt receipt{};
    if (!Begin(db_, error_out)) return false;
    Statement source;
    if (sqlite3_prepare_v2(db_,
            "SELECT route_node_id FROM atr_battle_set_route WHERE battle_set_id=?1;",
            -1, &source.value, nullptr) != SQLITE_OK)
        return Rollback(db_, sqlite3_errmsg(db_), error_out);
    sqlite3_bind_int64(source.value, 1, command.battle_set_id);
    if (sqlite3_step(source.value) != SQLITE_ROW)
        return Rollback(db_, "BattleSet has no semantic TAS route activity", error_out);
    receipt.source_route_node_id = sqlite3_column_int64(source.value, 0);

    Statement existing;
    if (sqlite3_prepare_v2(db_,
            "SELECT victory_branch_id,source_route_node_id,checkpoint_route_node_id FROM atr_victory_branch WHERE selected_turn_job_id=?1;",
            -1, &existing.value, nullptr) != SQLITE_OK)
        return Rollback(db_, sqlite3_errmsg(db_), error_out);
    sqlite3_bind_int64(existing.value, 1, command.selected_turn_job_id);
    if (sqlite3_step(existing.value) == SQLITE_ROW) {
        receipt.victory_branch_id = sqlite3_column_int64(existing.value, 0);
        if (sqlite3_column_int64(existing.value, 1) != receipt.source_route_node_id)
            return Rollback(db_, "Victory route branch changed source activity", error_out);
        receipt.checkpoint_route_node_id = sqlite3_column_int64(existing.value, 2);
        if (!Commit(db_, error_out)) return false;
        if (receipt_out) *receipt_out = receipt;
        return true;
    }

    Statement checkpoint;
    constexpr auto checkpoint_sql =
        "INSERT INTO atr_route_node(parent_route_node_id,node_kind,activity_kind,activity_key,label,description,"
        "source_dtm_artifact_id,source_savestate_id,battle_plan_id,tas_movie_tree_id,status,created_at_utc,updated_at_utc) "
        "VALUES(?1,'CHECKPOINT','','',?2,'Recorded Battle Victory checkpoint',NULL,NULL,NULL,NULL,'PENDING',?3,?3);";
    if (sqlite3_prepare_v2(db_, checkpoint_sql, -1, &checkpoint.value, nullptr) != SQLITE_OK)
        return Rollback(db_, sqlite3_errmsg(db_), error_out);
    const std::string label = command.default_label.empty()
        ? "Victory " + std::to_string(command.selected_turn_job_id)
        : command.default_label;
    sqlite3_bind_int64(checkpoint.value, 1, receipt.source_route_node_id);
    sqlite3_bind_text(checkpoint.value, 2, label.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(checkpoint.value, 3, command.created_at_utc.time_since_epoch().count());
    if (sqlite3_step(checkpoint.value) != SQLITE_DONE)
        return Rollback(db_, sqlite3_errmsg(db_), error_out);
    receipt.checkpoint_route_node_id = sqlite3_last_insert_rowid(db_);

    Statement branch;
    constexpr auto branch_sql =
        "INSERT INTO atr_victory_branch(source_route_node_id,checkpoint_route_node_id,selected_turn_job_id,status,created_at_utc,updated_at_utc) "
        "VALUES(?1,?2,?3,'PENDING',?4,?4);";
    if (sqlite3_prepare_v2(db_, branch_sql, -1, &branch.value, nullptr) != SQLITE_OK)
        return Rollback(db_, sqlite3_errmsg(db_), error_out);
    sqlite3_bind_int64(branch.value, 1, receipt.source_route_node_id);
    sqlite3_bind_int64(branch.value, 2, receipt.checkpoint_route_node_id);
    sqlite3_bind_int64(branch.value, 3, command.selected_turn_job_id);
    sqlite3_bind_int64(branch.value, 4, command.created_at_utc.time_since_epoch().count());
    if (sqlite3_step(branch.value) != SQLITE_DONE)
        return Rollback(db_, sqlite3_errmsg(db_), error_out);
    receipt.victory_branch_id = sqlite3_last_insert_rowid(db_);
    if (!Commit(db_, error_out)) return false;
    if (receipt_out) *receipt_out = receipt;
    return true;
}

bool SqliteAnalysisDb::BindVictoryRouteBranchWorkflow(
    const BindVictoryRouteBranchWorkflowCommand& command,
    std::string* error_out) {
    if (!db_ || command.selected_turn_job_id <= 0 ||
        command.workflow_instance_id <= 0 || command.status.empty()) {
        if (error_out) *error_out = "Victory route workflow binding is incomplete";
        return false;
    }
    Statement st;
    constexpr auto sql =
        "UPDATE atr_victory_branch SET workflow_instance_id=?2,status=?3,updated_at_utc=?4 "
        "WHERE selected_turn_job_id=?1 AND status NOT IN ('READY','CANCELED');";
    if (sqlite3_prepare_v2(db_, sql, -1, &st.value, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_); return false;
    }
    sqlite3_bind_int64(st.value, 1, command.selected_turn_job_id);
    sqlite3_bind_int64(st.value, 2, command.workflow_instance_id);
    sqlite3_bind_text(st.value, 3, command.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.value, 4, command.updated_at_utc.time_since_epoch().count());
    if (sqlite3_step(st.value) != SQLITE_DONE || sqlite3_changes(db_) != 1) {
        if (error_out) *error_out = "Victory route workflow binding lost its pending branch";
        return false;
    }
    return true;
}

bool SqliteAnalysisDb::RenameTasRouteNode(
    std::int64_t route_node_id, std::string_view label,
    types::UtcTimePoint updated_at_utc, std::string* error_out) {
    if (!db_ || route_node_id <= 0 || label.empty()) {
        if (error_out) *error_out = "route node label cannot be empty";
        return false;
    }
    Statement st;
    if (sqlite3_prepare_v2(db_,
            "UPDATE atr_route_node SET label=?2,updated_at_utc=?3 WHERE route_node_id=?1;",
            -1, &st.value, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_); return false;
    }
    sqlite3_bind_int64(st.value, 1, route_node_id);
    sqlite3_bind_text(st.value, 2, label.data(), static_cast<int>(label.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.value, 3, updated_at_utc.time_since_epoch().count());
    if (sqlite3_step(st.value) != SQLITE_DONE || sqlite3_changes(db_) != 1) {
        if (error_out) *error_out = "route node was not found";
        return false;
    }
    return true;
}

std::vector<TasRouteNodeSnapshot> SqliteAnalysisDb::ListTasRouteNodes() const {
    std::vector<TasRouteNodeSnapshot> rows;
    if (!db_) return rows;
    Statement st;
    constexpr auto sql =
        "SELECT route_node_id,parent_route_node_id,node_kind,activity_kind,activity_key,label,description,"
        "source_dtm_artifact_id,source_savestate_id,battle_plan_id,tas_movie_tree_id,status,created_at_utc,updated_at_utc "
        "FROM atr_route_node ORDER BY route_node_id;";
    if (sqlite3_prepare_v2(db_, sql, -1, &st.value, nullptr) != SQLITE_OK) return rows;
    while (sqlite3_step(st.value) == SQLITE_ROW) {
        TasRouteNodeSnapshot row{};
        row.route_node_id = sqlite3_column_int64(st.value, 0);
        row.parent_route_node_id = OptionalI64(st.value, 1);
        row.node_kind = Text(st.value, 2) == "ACTIVITY"
            ? TasRouteNodeKind::Activity : TasRouteNodeKind::Checkpoint;
        row.activity_kind = Text(st.value, 3);
        row.activity_key = Text(st.value, 4);
        row.label = Text(st.value, 5);
        row.description = Text(st.value, 6);
        row.source_dtm_artifact_id = OptionalI64(st.value, 7);
        row.source_savestate_id = OptionalI64(st.value, 8);
        row.battle_plan_id = OptionalI64(st.value, 9);
        row.tas_movie_tree_id = OptionalI64(st.value, 10);
        row.status = Text(st.value, 11);
        row.created_at_utc = Time(st.value, 12);
        row.updated_at_utc = Time(st.value, 13);
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<VictoryRouteBranchSnapshot> SqliteAnalysisDb::ListVictoryRouteBranches() const {
    std::vector<VictoryRouteBranchSnapshot> rows;
    if (!db_) return rows;
    Statement st;
    constexpr auto sql =
        "SELECT victory_branch_id,source_route_node_id,checkpoint_route_node_id,selected_turn_job_id,"
        "workflow_instance_id,battle_recording_id,tas_movie_tree_id,checkpoint_savestate_id,validation_request_id,"
        "status,created_at_utc,updated_at_utc FROM atr_victory_branch ORDER BY victory_branch_id;";
    if (sqlite3_prepare_v2(db_, sql, -1, &st.value, nullptr) != SQLITE_OK) return rows;
    while (sqlite3_step(st.value) == SQLITE_ROW) {
        VictoryRouteBranchSnapshot row{};
        row.victory_branch_id = sqlite3_column_int64(st.value, 0);
        row.source_route_node_id = sqlite3_column_int64(st.value, 1);
        row.checkpoint_route_node_id = sqlite3_column_int64(st.value, 2);
        row.selected_turn_job_id = sqlite3_column_int64(st.value, 3);
        row.workflow_instance_id = OptionalI64(st.value, 4);
        row.battle_recording_id = OptionalI64(st.value, 5);
        row.tas_movie_tree_id = OptionalI64(st.value, 6);
        row.checkpoint_savestate_id = OptionalI64(st.value, 7);
        row.validation_request_id = OptionalI64(st.value, 8);
        row.status = Text(st.value, 9);
        row.created_at_utc = Time(st.value, 10);
        row.updated_at_utc = Time(st.value, 11);
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<std::int64_t> SqliteAnalysisDb::ListBattleSetIdsForRouteNode(
    std::int64_t route_node_id) const {
    std::vector<std::int64_t> rows;
    if (!db_ || route_node_id <= 0) return rows;
    Statement st;
    if (sqlite3_prepare_v2(db_,
            "SELECT battle_set_id FROM atr_battle_set_route WHERE route_node_id=?1 ORDER BY battle_set_id;",
            -1, &st.value, nullptr) != SQLITE_OK) return rows;
    sqlite3_bind_int64(st.value, 1, route_node_id);
    while (sqlite3_step(st.value) == SQLITE_ROW)
        rows.push_back(sqlite3_column_int64(st.value, 0));
    return rows;
}

} // namespace savor::db::analysis
