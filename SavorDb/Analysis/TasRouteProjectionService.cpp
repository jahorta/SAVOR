#include "TasRouteProjectionService.h"

#include "IAnalysisDb.h"

#include <sqlite3.h>

#include <algorithm>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

namespace savor::db::analysis {
namespace {

struct Connection {
    sqlite3* db = nullptr;
    ~Connection() { if (db != nullptr) sqlite3_close(db); }
};

struct Statement {
    sqlite3_stmt* value = nullptr;
    ~Statement() { sqlite3_finalize(value); }
};

struct PendingBattleRoute {
    std::int64_t battle_set_id = 0;
    std::int64_t entry_savestate_id = 0;
    std::int64_t battle_plan_id = 0;
    std::int64_t workflow_instance_id = 0;
    std::string battle_plan_fingerprint;
};

bool OpenReadOnly(
    const std::filesystem::path& path,
    Connection* connection,
    std::string* error_out) {
    if (connection == nullptr || path.empty()) {
        if (error_out) *error_out = "TAS route projection path is missing";
        return false;
    }
    const auto rc = sqlite3_open_v2(
        path.string().c_str(), &connection->db,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc == SQLITE_OK) return true;
    if (error_out) {
        *error_out = connection->db != nullptr
            ? sqlite3_errmsg(connection->db)
            : "sqlite3_open_v2 failed";
    }
    return false;
}

std::optional<std::int64_t> Scalar(
    sqlite3* db,
    const char* sql,
    std::int64_t value) {
    Statement statement;
    if (sqlite3_prepare_v2(db, sql, -1, &statement.value, nullptr) != SQLITE_OK)
        return std::nullopt;
    sqlite3_bind_int64(statement.value, 1, value);
    if (sqlite3_step(statement.value) != SQLITE_ROW ||
        sqlite3_column_type(statement.value, 0) == SQLITE_NULL)
        return std::nullopt;
    return sqlite3_column_int64(statement.value, 0);
}

std::optional<std::int64_t> ResolveRootAuthority(
    sqlite3* execution,
    sqlite3* analysis,
    const PendingBattleRoute& battle) {
    if (const auto recorded = Scalar(
            analysis,
            "SELECT n.root_establishment_attempt_id "
            "FROM atr_victory_branch b JOIN atr_route_node n "
            "ON n.route_node_id=b.checkpoint_route_node_id "
            "WHERE b.checkpoint_savestate_id=?1 "
            "AND n.root_establishment_attempt_id IS NOT NULL LIMIT 1;",
            battle.entry_savestate_id)) {
        return recorded;
    }

    std::optional<std::int64_t> annotation_attempt;
    {
        Statement statement;
        constexpr auto sql =
            "SELECT e.source_ref_id FROM exec_workflow_expansion_member m "
            "JOIN exec_workflow_expansion e "
            "ON e.workflow_expansion_id=m.workflow_expansion_id "
            "WHERE m.workflow_instance_id=?1 "
            "AND e.source_ref_kind='tmv_input_epoch_annotation_attempt' LIMIT 1;";
        if (sqlite3_prepare_v2(execution, sql, -1, &statement.value, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(statement.value, 1, battle.workflow_instance_id);
            if (sqlite3_step(statement.value) == SQLITE_ROW)
                annotation_attempt = sqlite3_column_int64(statement.value, 0);
        }
    }
    if (!annotation_attempt) {
        Statement statement;
        constexpr auto sql =
            "SELECT ref_id FROM exec_workflow_instance_input_binding "
            "WHERE workflow_instance_id=?1 "
            "AND ref_kind='tmv_input_epoch_annotation_attempt' "
            "ORDER BY workflow_instance_input_binding_id LIMIT 1;";
        if (sqlite3_prepare_v2(execution, sql, -1, &statement.value, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(statement.value, 1, battle.workflow_instance_id);
            if (sqlite3_step(statement.value) == SQLITE_ROW)
                annotation_attempt = sqlite3_column_int64(statement.value, 0);
        }
    }
    if (!annotation_attempt) return std::nullopt;
    return Scalar(
        analysis,
        "SELECT root_establishment_attempt_id "
        "FROM tmv_input_epoch_annotation_attempt "
        "WHERE annotation_attempt_id=?1 AND succeeded=1;",
        *annotation_attempt);
}

} // namespace

TasRouteProjectionService::TasRouteProjectionService(
    TasRouteProjectionConfig config,
    savor::db::IAnalysisDb* analysis_db)
    : config_(std::move(config)), analysis_db_(analysis_db) {}

TasRouteProjectionService::~TasRouteProjectionService() { Stop(); }

bool TasRouteProjectionService::Start(std::string* error_out) {
    if (running_.exchange(true)) return true;
    if (analysis_db_ == nullptr || config_.execution_db_path.empty() ||
        config_.analysis_db_path.empty()) {
        running_.store(false);
        if (error_out) *error_out = "TAS route projection dependencies are incomplete";
        return false;
    }
    stop_.store(false);
    thread_ = std::thread([this] { Run(); });
    return true;
}

void TasRouteProjectionService::Stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    running_.store(false);
}

void TasRouteProjectionService::Wake() {
    // The bounded poll is deliberately the only wake mechanism. Projection
    // latency cannot become execution coupling.
}

bool TasRouteProjectionService::RunOnce(std::string* error_out) {
    Connection execution;
    Connection analysis;
    if (!OpenReadOnly(config_.execution_db_path, &execution, error_out) ||
        !OpenReadOnly(config_.analysis_db_path, &analysis, error_out))
        return false;

    Statement pending;
    constexpr auto pending_sql =
        "SELECT b.battle_set_id,b.entry_savestate_id,b.battle_plan_id,"
        "s.workflow_instance_id,b.battle_plan_fingerprint "
        "FROM ab_battle_set b JOIN ab_battle_start s "
        "ON s.battle_set_id=b.battle_set_id "
        "LEFT JOIN atr_battle_set_route r ON r.battle_set_id=b.battle_set_id "
        "WHERE r.battle_set_id IS NULL ORDER BY b.battle_set_id LIMIT 256;";
    if (sqlite3_prepare_v2(
            analysis.db, pending_sql, -1, &pending.value, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(analysis.db);
        return false;
    }

    std::vector<std::string> failures;
    while (sqlite3_step(pending.value) == SQLITE_ROW) {
        PendingBattleRoute battle{};
        battle.battle_set_id = sqlite3_column_int64(pending.value, 0);
        battle.entry_savestate_id = sqlite3_column_int64(pending.value, 1);
        battle.battle_plan_id = sqlite3_column_int64(pending.value, 2);
        battle.workflow_instance_id = sqlite3_column_int64(pending.value, 3);
        const auto* fingerprint = sqlite3_column_text(pending.value, 4);
        battle.battle_plan_fingerprint = fingerprint
            ? reinterpret_cast<const char*>(fingerprint)
            : std::string{};

        const auto root = ResolveRootAuthority(
            execution.db, analysis.db, battle);
        if (!root) {
            failures.push_back(
                "battle_set=" + std::to_string(battle.battle_set_id) +
                " has no explicit TAS root provenance");
            continue;
        }
        std::string command_error;
        if (!analysis_db_->EnsureBattleRouteActivity({
                .battle_set_id = battle.battle_set_id,
                .entry_savestate_id = battle.entry_savestate_id,
                .battle_plan_id = battle.battle_plan_id,
                .root_establishment_attempt_id = *root,
                .activity_key = "battle-plan:" + battle.battle_plan_fingerprint,
                .default_label = "Battle plan " +
                    std::to_string(battle.battle_plan_id),
                .default_description =
                    "Battle activity grouped by its explicit TAS checkpoint authority.",
                .created_at_utc = types::UtcNow(),
            }, nullptr, &command_error)) {
            failures.push_back(
                "battle_set=" + std::to_string(battle.battle_set_id) +
                ": " + command_error);
        }
    }
    if (error_out) {
        std::ostringstream joined;
        for (std::size_t i = 0; i < failures.size(); ++i) {
            if (i != 0) joined << '\n';
            joined << failures[i];
        }
        *error_out = joined.str();
    }
    return true;
}

void TasRouteProjectionService::Run() {
    while (!stop_.load()) {
        std::string ignored;
        (void)RunOnce(&ignored);
        auto remaining = config_.poll_interval;
        while (!stop_.load() && remaining.count() > 0) {
            const auto slice = std::min(
                remaining, std::chrono::milliseconds{50});
            std::this_thread::sleep_for(slice);
            remaining -= slice;
        }
    }
}

} // namespace savor::db::analysis
