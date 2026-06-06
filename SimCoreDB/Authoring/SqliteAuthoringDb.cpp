#include "SqliteAuthoringDb.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <unordered_map>

#include "../Common/Events/EventPayloadDispatch.h"
#include "../Common/Events/EventPayloadValidation.h"

namespace simcore::db {

namespace {

struct Statement {
    Statement() {}
    ~Statement() {
        if (st != nullptr) {
            sqlite3_finalize(st);
        }
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    sqlite3_stmt* st = nullptr;
};

std::int64_t ToEpochMillis(types::UtcTimePoint value) {
    return value.time_since_epoch().count();
}

types::UtcTimePoint FromEpochMillis(std::int64_t value) {
    return types::UtcTimePoint{ std::chrono::milliseconds(value) };
}

std::optional<std::int64_t> ColumnInt64Optional(sqlite3_stmt* st, int index) {
    if (sqlite3_column_type(st, index) == SQLITE_NULL) {
        return std::nullopt;
    }
    return sqlite3_column_int64(st, index);
}

std::optional<int> ColumnIntOptional(sqlite3_stmt* st, int index) {
    if (sqlite3_column_type(st, index) == SQLITE_NULL) {
        return std::nullopt;
    }
    return sqlite3_column_int(st, index);
}

std::string ColumnText(sqlite3_stmt* st, int index) {
    const auto* text = sqlite3_column_text(st, index);
    return text ? reinterpret_cast<const char*>(text) : "";
}

std::optional<std::string> ColumnTextOptional(sqlite3_stmt* st, int index) {
    if (sqlite3_column_type(st, index) == SQLITE_NULL) {
        return std::nullopt;
    }
    return ColumnText(st, index);
}

std::vector<std::uint8_t> ColumnBlob(sqlite3_stmt* st, int index) {
    const auto* blob = sqlite3_column_blob(st, index);
    const int size = sqlite3_column_bytes(st, index);
    if (blob == nullptr || size <= 0) {
        return {};
    }
    const auto* first = static_cast<const std::uint8_t*>(blob);
    return std::vector<std::uint8_t>(first, first + size);
}



bool InsertAuthoringOutboxEvent(
    sqlite3* db,
    std::string_view event_id,
    std::string_view event_type,
    std::string_view aggregate_id,
    std::string_view correlation_id,
    std::string_view causation_id,
    std::int64_t occurred_at_utc,
    std::int64_t payload_ref_id,
    std::string* error_out) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "INSERT INTO au_outbox_message("
            "event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id) "
            "VALUES(?1,?2,1,'Authoring','template',?3,?4,?5,?6,'authoring_event',?7);",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db);
        }
        return false;
    }

    sqlite3_bind_text(st.st, 1, event_id.data(), static_cast<int>(event_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 2, event_type.data(), static_cast<int>(event_type.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 3, aggregate_id.data(), static_cast<int>(aggregate_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 4, correlation_id.data(), static_cast<int>(correlation_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 5, causation_id.data(), static_cast<int>(causation_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 6, occurred_at_utc);
    sqlite3_bind_int64(st.st, 7, payload_ref_id);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db);
        }
        return false;
    }
    return true;
}

bool TryReadTemplatePayload(sqlite3* db, std::int64_t template_id, AuthoringPayloadRecord* out) {
    if (db == nullptr || out == nullptr || template_id <= 0) {
        return false;
    }

    Statement st;
    constexpr const char* kSql =
        "SELECT template_id, COALESCE(seed_probe_spec_id, 0), COALESCE(tas_spec_id, 0), "
        "COALESCE(battle_run_spec_id, 0), COALESCE(explorer_settings_id, 0) "
        "FROM au_template "
        "WHERE template_id=?1;";
    if (sqlite3_prepare_v2(db, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(st.st, 1, template_id);

    if (sqlite3_step(st.st) == SQLITE_ROW) {
        out->template_id = sqlite3_column_int64(st.st, 0);
        out->seed_probe_spec_id = sqlite3_column_int64(st.st, 1);
        out->tas_spec_id = sqlite3_column_int64(st.st, 2);
        out->battle_run_spec_id = sqlite3_column_int64(st.st, 3);
        out->explorer_settings_id = sqlite3_column_int64(st.st, 4);
        return true;
    }

    return false;
}

bool TryReadTemplateBySeedProbeSpec(sqlite3* db, std::int64_t seed_probe_spec_id, AuthoringPayloadRecord* out) {
    if (db == nullptr || out == nullptr || seed_probe_spec_id <= 0) {
        return false;
    }

    Statement st;
    constexpr const char* kSql =
        "SELECT template_id, COALESCE(seed_probe_spec_id, 0), COALESCE(tas_spec_id, 0), "
        "COALESCE(battle_run_spec_id, 0), COALESCE(explorer_settings_id, 0) "
        "FROM au_template "
        "WHERE seed_probe_spec_id=?1 "
        "ORDER BY template_id DESC "
        "LIMIT 1;";
    if (sqlite3_prepare_v2(db, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(st.st, 1, seed_probe_spec_id);

    if (sqlite3_step(st.st) == SQLITE_ROW) {
        out->template_id = sqlite3_column_int64(st.st, 0);
        out->seed_probe_spec_id = sqlite3_column_int64(st.st, 1);
        out->tas_spec_id = sqlite3_column_int64(st.st, 2);
        out->battle_run_spec_id = sqlite3_column_int64(st.st, 3);
        out->explorer_settings_id = sqlite3_column_int64(st.st, 4);
        return true;
    }

    Statement exists_st;
    constexpr const char* kExistsSql =
        "SELECT seed_probe_spec_id "
        "FROM au_seed_probe_spec "
        "WHERE seed_probe_spec_id=?1;";
    if (sqlite3_prepare_v2(db, kExistsSql, -1, &exists_st.st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(exists_st.st, 1, seed_probe_spec_id);
    if (sqlite3_step(exists_st.st) == SQLITE_ROW) {
        out->template_id = 0;
        out->seed_probe_spec_id = seed_probe_spec_id;
        out->tas_spec_id = 0;
        out->battle_run_spec_id = 0;
        out->explorer_settings_id = 0;
        return true;
    }

    return false;
}

bool TryReadTemplateByBattleRunSpec(sqlite3* db, std::int64_t battle_run_spec_id, AuthoringPayloadRecord* out) {
    if (db == nullptr || out == nullptr || battle_run_spec_id <= 0) {
        return false;
    }

    Statement st;
    constexpr const char* kSql =
        "SELECT template_id, COALESCE(seed_probe_spec_id, 0), COALESCE(tas_spec_id, 0), "
        "COALESCE(battle_run_spec_id, 0), COALESCE(explorer_settings_id, 0) "
        "FROM au_template "
        "WHERE battle_run_spec_id=?1 "
        "ORDER BY template_id DESC "
        "LIMIT 1;";
    if (sqlite3_prepare_v2(db, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(st.st, 1, battle_run_spec_id);

    if (sqlite3_step(st.st) == SQLITE_ROW) {
        out->template_id = sqlite3_column_int64(st.st, 0);
        out->seed_probe_spec_id = sqlite3_column_int64(st.st, 1);
        out->tas_spec_id = sqlite3_column_int64(st.st, 2);
        out->battle_run_spec_id = sqlite3_column_int64(st.st, 3);
        out->explorer_settings_id = sqlite3_column_int64(st.st, 4);
        return true;
    }

    Statement exists_st;
    constexpr const char* kExistsSql =
        "SELECT battle_run_spec_id "
        "FROM au_battle_run_spec "
        "WHERE battle_run_spec_id=?1;";
    if (sqlite3_prepare_v2(db, kExistsSql, -1, &exists_st.st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(exists_st.st, 1, battle_run_spec_id);
    if (sqlite3_step(exists_st.st) == SQLITE_ROW) {
        out->template_id = 0;
        out->seed_probe_spec_id = 0;
        out->tas_spec_id = 0;
        out->battle_run_spec_id = battle_run_spec_id;
        out->explorer_settings_id = 0;
        return true;
    }

    return false;
}

bool TryReadWorkflowGraphPayload(sqlite3* db, std::int64_t workflow_graph_revision_id, AuthoringPayloadRecord* out) {
    if (db == nullptr || out == nullptr || workflow_graph_revision_id <= 0) {
        return false;
    }

    Statement st;
    constexpr const char* kSql =
        "SELECT workflow_graph_id, workflow_graph_revision_id "
        "FROM au_workflow_graph_revision "
        "WHERE workflow_graph_revision_id=?1;";
    if (sqlite3_prepare_v2(db, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(st.st, 1, workflow_graph_revision_id);
    if (sqlite3_step(st.st) == SQLITE_ROW) {
        out->workflow_graph_id = sqlite3_column_int64(st.st, 0);
        out->workflow_graph_revision_id = sqlite3_column_int64(st.st, 1);
        return true;
    }

    return false;
}

bool InsertWorkflowGraphOutboxEvent(
    sqlite3* db,
    const SaveWorkflowGraphCommand& command,
    std::int64_t workflow_graph_id,
    std::int64_t workflow_graph_revision_id,
    std::string* error_out) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "INSERT INTO au_outbox_message("
            "event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id) "
            "VALUES(?1,'Authoring.WorkflowGraphSaved.v1',1,'Authoring','workflow_graph',?2,?3,?4,?5,'authoring_event',?6);",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db);
        }
        return false;
    }

    const auto aggregate_id = std::to_string(workflow_graph_id);
    sqlite3_bind_text(st.st, 1, command.event_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 2, aggregate_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 3, command.correlation_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 4, command.causation_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 5, ToEpochMillis(command.created_at_utc));
    sqlite3_bind_int64(st.st, 6, workflow_graph_revision_id);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db);
        }
        return false;
    }

    return true;
}

} // namespace

SqliteAuthoringDb::SqliteAuthoringDb(sqlite3* db)
    : db_(db) {
}

bool SqliteAuthoringDb::SaveSeedProbeSpec(
    const SaveSeedProbeSpecCommand& command,
    std::int64_t* seed_probe_spec_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.name.empty() || command.event_id.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement insert_spec;
    Statement insert_grid;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO au_seed_probe_grid_spec("
            "samples_per_axis,min_value,max_value,cap_trigger_top,ignore_trigger_min_max) "
            "VALUES(?1,?2,?3,?4,?5);",
            -1,
            &insert_grid.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int(insert_grid.st, 1, command.samples_per_axis);
    sqlite3_bind_int64(insert_grid.st, 2, command.min_value);
    sqlite3_bind_int64(insert_grid.st, 3, command.max_value);
    sqlite3_bind_int(insert_grid.st, 4, command.cap_trigger_top ? 1 : 0);
    sqlite3_bind_int(insert_grid.st, 5, command.ignore_trigger_minmax ? 1 : 0);

    if (sqlite3_step(insert_grid.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto grid_spec_id = sqlite3_last_insert_rowid(db_);

    Statement insert_unique;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO au_seed_probe_unique_spec("
            "combo_attempts_per_target,combo_sampler_tries) "
            "VALUES(?1,?2);",
            -1,
            &insert_unique.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int(insert_unique.st, 1, command.combo_attempts_per_target);
    sqlite3_bind_int(insert_unique.st, 2, command.combo_sampler_tries);

    if (sqlite3_step(insert_unique.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto unique_spec_id = sqlite3_last_insert_rowid(db_);

    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO au_seed_probe_spec("
            "name,priority,run_ms,vi_stall_ms,grid_spec_id,unique_spec_id,auto_schedule_battle_run,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8);",
            -1,
            &insert_spec.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_text(insert_spec.st, 1, command.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(insert_spec.st, 2, command.priority);
    sqlite3_bind_int64(insert_spec.st, 3, command.run_ms);
    sqlite3_bind_int64(insert_spec.st, 4, command.vi_stall_ms);
    sqlite3_bind_int64(insert_spec.st, 5, grid_spec_id);
    sqlite3_bind_int64(insert_spec.st, 6, unique_spec_id);
    sqlite3_bind_int(insert_spec.st, 7, command.auto_schedule_battle_run ? 1 : 0);
    sqlite3_bind_int64(insert_spec.st, 8, ToEpochMillis(command.created_at_utc));

    if (sqlite3_step(insert_spec.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto seed_probe_spec_id = sqlite3_last_insert_rowid(db_);
    if (!InsertAuthoringOutboxEvent(
            db_,
            command.event_id,
            "Authoring.SeedProbeSpecSaved.v1",
            std::to_string(seed_probe_spec_id),
            command.correlation_id,
            command.causation_id,
            ToEpochMillis(command.created_at_utc),
            seed_probe_spec_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (seed_probe_spec_id_out) {
        *seed_probe_spec_id_out = seed_probe_spec_id;
    }

    return true;
}

std::optional<SeedProbeSpecSnapshot> SqliteAuthoringDb::GetSeedProbeSpec(std::int64_t seed_probe_spec_id) const {
    if (db_ == nullptr || seed_probe_spec_id <= 0) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT s.seed_probe_spec_id,s.name,s.priority,s.run_ms,s.vi_stall_ms,"
            "g.samples_per_axis,g.min_value,g.max_value,g.cap_trigger_top,g.ignore_trigger_min_max,"
            "u.combo_attempts_per_target,u.combo_sampler_tries,s.auto_schedule_battle_run "
            "FROM au_seed_probe_spec s "
            "JOIN au_seed_probe_grid_spec g ON g.seed_probe_grid_spec_id=s.grid_spec_id "
            "JOIN au_seed_probe_unique_spec u ON u.seed_probe_unique_spec_id=s.unique_spec_id "
            "WHERE s.seed_probe_spec_id=?1 LIMIT 1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, seed_probe_spec_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    SeedProbeSpecSnapshot snapshot{};
    snapshot.seed_probe_spec_id = sqlite3_column_int64(st.st, 0);
    if (const auto* text = sqlite3_column_text(st.st, 1)) {
        snapshot.name = reinterpret_cast<const char*>(text);
    }
    snapshot.priority = sqlite3_column_int(st.st, 2);
    snapshot.run_ms = sqlite3_column_int64(st.st, 3);
    snapshot.vi_stall_ms = sqlite3_column_int64(st.st, 4);
    snapshot.samples_per_axis = sqlite3_column_int(st.st, 5);
    snapshot.min_value = sqlite3_column_int64(st.st, 6);
    snapshot.max_value = sqlite3_column_int64(st.st, 7);
    snapshot.cap_trigger_top = sqlite3_column_int(st.st, 8) != 0;
    snapshot.ignore_trigger_minmax = sqlite3_column_int(st.st, 9) != 0;
    snapshot.combo_attempts_per_target = sqlite3_column_int(st.st, 10);
    snapshot.combo_sampler_tries = sqlite3_column_int(st.st, 11);
    snapshot.auto_schedule_battle_run = sqlite3_column_int(st.st, 12) != 0;
    return snapshot;
}

std::vector<SeedProbeSpecSnapshot> SqliteAuthoringDb::ListSeedProbeSpecs(
    int max_count) const {
    std::vector<SeedProbeSpecSnapshot> out;
    if (db_ == nullptr) {
        return out;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT seed_probe_spec_id FROM au_seed_probe_spec ORDER BY seed_probe_spec_id DESC LIMIT ?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return out;
    }
    sqlite3_bind_int(st.st, 1, std::max(1, max_count));
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        if (auto snapshot = GetSeedProbeSpec(sqlite3_column_int64(st.st, 0)); snapshot.has_value()) {
            out.push_back(std::move(*snapshot));
        }
    }
    return out;
}

bool SqliteAuthoringDb::SaveTasSpec(
    const SaveTasSpecCommand& command,
    std::int64_t* tas_spec_id_out,
    std::int64_t* tas_spec_base_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.base_name.empty() || command.event_id.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement insert_base;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO au_tas_spec_base("
            "name,priority,run_ms,vi_stall_ms,headroom_x10,progress_enable,auto_queue_seeds,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8);",
            -1,
            &insert_base.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_text(insert_base.st, 1, command.base_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(insert_base.st, 2, command.priority);
    sqlite3_bind_int64(insert_base.st, 3, command.run_ms);
    sqlite3_bind_int64(insert_base.st, 4, command.vi_stall_ms);
    sqlite3_bind_int(insert_base.st, 5, command.headroom_x10);
    sqlite3_bind_int(insert_base.st, 6, command.progress_enable ? 1 : 0);
    sqlite3_bind_int(insert_base.st, 7, command.auto_queue_seeds ? 1 : 0);
    sqlite3_bind_int64(insert_base.st, 8, ToEpochMillis(command.created_at_utc));

    if (sqlite3_step(insert_base.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto tas_spec_base_id = sqlite3_last_insert_rowid(db_);

    Statement insert_spec;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO au_tas_spec(tas_spec_base_id,base_dtm_artifact_id,rtc_low,rtc_high,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5);",
            -1,
            &insert_spec.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(insert_spec.st, 1, tas_spec_base_id);
    sqlite3_bind_int64(insert_spec.st, 2, command.base_dtm_artifact_id);
    sqlite3_bind_int64(insert_spec.st, 3, command.rtc_low);
    sqlite3_bind_int64(insert_spec.st, 4, command.rtc_high);
    sqlite3_bind_int64(insert_spec.st, 5, ToEpochMillis(command.created_at_utc));

    if (sqlite3_step(insert_spec.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto tas_spec_id = sqlite3_last_insert_rowid(db_);
    if (!InsertAuthoringOutboxEvent(
            db_,
            command.event_id,
            "Authoring.TasSpecSaved.v1",
            std::to_string(tas_spec_id),
            command.correlation_id,
            command.causation_id,
            ToEpochMillis(command.created_at_utc),
            tas_spec_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (tas_spec_id_out) {
        *tas_spec_id_out = tas_spec_id;
    }
    if (tas_spec_base_id_out) {
        *tas_spec_base_id_out = tas_spec_base_id;
    }

    return true;
}

std::optional<TasSpecSnapshot> SqliteAuthoringDb::GetTasSpec(
    std::int64_t tas_spec_id) const {
    if (db_ == nullptr || tas_spec_id <= 0) {
        return std::nullopt;
    }

    Statement st;
    constexpr const char* kSql =
        "SELECT s.tas_spec_id, b.tas_spec_base_id, b.name, b.priority, b.run_ms, b.vi_stall_ms, "
        "b.headroom_x10, b.progress_enable, b.auto_queue_seeds, "
        "s.base_dtm_artifact_id, s.rtc_low, s.rtc_high "
        "FROM au_tas_spec s "
        "JOIN au_tas_spec_base b ON b.tas_spec_base_id=s.tas_spec_base_id "
        "WHERE s.tas_spec_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, tas_spec_id);

    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    TasSpecSnapshot snapshot{};
    snapshot.tas_spec_id = sqlite3_column_int64(st.st, 0);
    snapshot.tas_spec_base_id = sqlite3_column_int64(st.st, 1);
    snapshot.base_name = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 2));
    snapshot.priority = sqlite3_column_int(st.st, 3);
    snapshot.run_ms = sqlite3_column_int64(st.st, 4);
    snapshot.vi_stall_ms = sqlite3_column_int64(st.st, 5);
    snapshot.headroom_x10 = sqlite3_column_int(st.st, 6);
    snapshot.progress_enable = sqlite3_column_int(st.st, 7) != 0;
    snapshot.auto_queue_seeds = sqlite3_column_int(st.st, 8) != 0;
    snapshot.base_dtm_artifact_id = sqlite3_column_int64(st.st, 9);
    snapshot.rtc_low = sqlite3_column_int64(st.st, 10);
    snapshot.rtc_high = sqlite3_column_int64(st.st, 11);
    return snapshot;
}

std::vector<TasSpecSnapshot> SqliteAuthoringDb::ListTasSpecs(
    int max_count) const {
    std::vector<TasSpecSnapshot> out;
    if (db_ == nullptr) {
        return out;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT tas_spec_id FROM au_tas_spec ORDER BY tas_spec_id DESC LIMIT ?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return out;
    }
    sqlite3_bind_int(st.st, 1, std::max(1, max_count));
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        if (auto snapshot = GetTasSpec(sqlite3_column_int64(st.st, 0)); snapshot.has_value()) {
            out.push_back(std::move(*snapshot));
        }
    }
    return out;
}

bool SqliteAuthoringDb::SaveBattleRunSpec(
    const SaveBattleRunSpecCommand& command,
    std::int64_t* battle_run_spec_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.name.empty() || command.event_id.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement insert_spec;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO au_battle_run_spec("
            "name,priority,run_ms,vi_stall_ms,progress_enable,use_single_turn_runner,auto_wave_trigger_enable,min_fake_attacks,max_fake_attacks,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10);",
            -1,
            &insert_spec.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_text(insert_spec.st, 1, command.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(insert_spec.st, 2, command.priority);
    sqlite3_bind_int64(insert_spec.st, 3, command.run_ms);
    sqlite3_bind_int64(insert_spec.st, 4, command.vi_stall_ms);
    sqlite3_bind_int(insert_spec.st, 5, command.progress_enable ? 1 : 0);
    sqlite3_bind_int(insert_spec.st, 6, command.use_single_turn_runner ? 1 : 0);
    sqlite3_bind_int(insert_spec.st, 7, command.auto_wave_trigger_enable ? 1 : 0);
    sqlite3_bind_int(insert_spec.st, 8, command.min_fake_attacks);
    sqlite3_bind_int(insert_spec.st, 9, command.max_fake_attacks);
    sqlite3_bind_int64(insert_spec.st, 10, ToEpochMillis(command.created_at_utc));

    if (sqlite3_step(insert_spec.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto battle_run_spec_id = sqlite3_last_insert_rowid(db_);
    if (!InsertAuthoringOutboxEvent(
            db_,
            command.event_id,
            "Authoring.BattleRunSpecSaved.v1",
            std::to_string(battle_run_spec_id),
            command.correlation_id,
            command.causation_id,
            ToEpochMillis(command.created_at_utc),
            battle_run_spec_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (battle_run_spec_id_out) {
        *battle_run_spec_id_out = battle_run_spec_id;
    }

    return true;
}

std::optional<BattleRunSpecSnapshot> SqliteAuthoringDb::GetBattleRunSpec(
    std::int64_t battle_run_spec_id) const {
    if (db_ == nullptr || battle_run_spec_id <= 0) {
        return std::nullopt;
    }

    Statement st;
    constexpr const char* kSql =
        "SELECT battle_run_spec_id,name,priority,run_ms,vi_stall_ms,progress_enable,use_single_turn_runner,"
        "auto_wave_trigger_enable,min_fake_attacks,max_fake_attacks "
        "FROM au_battle_run_spec WHERE battle_run_spec_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, battle_run_spec_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    BattleRunSpecSnapshot out{};
    out.battle_run_spec_id = sqlite3_column_int64(st.st, 0);
    out.name = ColumnText(st.st, 1);
    out.priority = sqlite3_column_int(st.st, 2);
    out.run_ms = sqlite3_column_int64(st.st, 3);
    out.vi_stall_ms = sqlite3_column_int64(st.st, 4);
    out.progress_enable = sqlite3_column_int(st.st, 5) != 0;
    out.use_single_turn_runner = sqlite3_column_int(st.st, 6) != 0;
    out.auto_wave_trigger_enable = sqlite3_column_int(st.st, 7) != 0;
    out.min_fake_attacks = sqlite3_column_int(st.st, 8);
    out.max_fake_attacks = sqlite3_column_int(st.st, 9);
    return out;
}

std::vector<BattleRunSpecSnapshot> SqliteAuthoringDb::ListBattleRunSpecs(
    int max_count) const {
    std::vector<BattleRunSpecSnapshot> out;
    if (db_ == nullptr) {
        return out;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT battle_run_spec_id FROM au_battle_run_spec ORDER BY battle_run_spec_id DESC LIMIT ?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return out;
    }
    sqlite3_bind_int(st.st, 1, std::max(1, max_count));
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        if (auto snapshot = GetBattleRunSpec(sqlite3_column_int64(st.st, 0)); snapshot.has_value()) {
            out.push_back(std::move(*snapshot));
        }
    }
    return out;
}

bool SqliteAuthoringDb::SavePlan(
    const SavePlanCommand& command,
    std::int64_t* plan_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.name.empty() || command.fingerprint.empty() || command.num_turns < 0 || command.event_id.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement insert_plan;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO au_battle_plan(name,fingerprint,num_turns,created_at_utc) VALUES(?1,?2,?3,?4);",
            -1,
            &insert_plan.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_text(insert_plan.st, 1, command.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_plan.st, 2, command.fingerprint.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(insert_plan.st, 3, command.num_turns);
    sqlite3_bind_int64(insert_plan.st, 4, ToEpochMillis(command.created_at_utc));

    if (sqlite3_step(insert_plan.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto plan_id = sqlite3_last_insert_rowid(db_);
    if (!InsertAuthoringOutboxEvent(
            db_,
            command.event_id,
            "Authoring.PlanSaved.v1",
            std::to_string(plan_id),
            command.correlation_id,
            command.causation_id,
            ToEpochMillis(command.created_at_utc),
            plan_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (plan_id_out) {
        *plan_id_out = plan_id;
    }

    return true;
}

bool SqliteAuthoringDb::SaveBattlePlanTurn(
    const SaveBattlePlanTurnCommand& command,
    std::int64_t* plan_turn_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.plan_id <= 0 || command.turn_index < 0 || command.event_id.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    Statement upsert_turn;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO au_battle_plan_turn(plan_id,turn_index) VALUES(?1,?2) "
            "ON CONFLICT(plan_id, turn_index) DO NOTHING;",
            -1,
            &upsert_turn.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    sqlite3_bind_int64(upsert_turn.st, 1, command.plan_id);
    sqlite3_bind_int(upsert_turn.st, 2, command.turn_index);
    if (sqlite3_step(upsert_turn.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    Statement read_turn;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT plan_turn_id FROM au_battle_plan_turn WHERE plan_id=?1 AND turn_index=?2;",
            -1,
            &read_turn.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    sqlite3_bind_int64(read_turn.st, 1, command.plan_id);
    sqlite3_bind_int(read_turn.st, 2, command.turn_index);
    if (sqlite3_step(read_turn.st) != SQLITE_ROW) {
        if (error_out) *error_out = "battle plan turn not found after upsert";
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    const auto plan_turn_id = sqlite3_column_int64(read_turn.st, 0);

    if (command.replace_existing_actions) {
        Statement del;
        if (sqlite3_prepare_v2(
                db_,
                "DELETE FROM au_battle_plan_action WHERE plan_turn_id=?1;",
                -1,
                &del.st,
                nullptr)
            != SQLITE_OK) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
        sqlite3_bind_int64(del.st, 1, plan_turn_id);
        if (sqlite3_step(del.st) != SQLITE_DONE) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
    }

    for (const auto& action : command.actions) {
        Statement insert_action;
        if (sqlite3_prepare_v2(
                db_,
                "INSERT INTO au_battle_plan_action("
                "plan_turn_id,actor_slot,macro,target_kind,target_slot,target_mask_bits,target_single_slot,target_same_as_actor_slot,target_expr_ini,item_id,ordinal) "
                "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11);",
                -1,
                &insert_action.st,
                nullptr)
            != SQLITE_OK) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
        sqlite3_bind_int64(insert_action.st, 1, plan_turn_id);
        sqlite3_bind_int(insert_action.st, 2, action.actor_slot);
        sqlite3_bind_int(insert_action.st, 3, static_cast<int>(action.macro));
        sqlite3_bind_int(insert_action.st, 4, static_cast<int>(action.target_kind));
        if (action.target_slot.has_value()) sqlite3_bind_int(insert_action.st, 5, *action.target_slot);
        else sqlite3_bind_null(insert_action.st, 5);
        if (action.target_mask_bits.has_value()) sqlite3_bind_int(insert_action.st, 6, *action.target_mask_bits);
        else sqlite3_bind_null(insert_action.st, 6);
        if (action.target_single_slot.has_value()) sqlite3_bind_int(insert_action.st, 7, *action.target_single_slot);
        else sqlite3_bind_null(insert_action.st, 7);
        if (action.target_same_as_actor_slot.has_value()) sqlite3_bind_int(insert_action.st, 8, *action.target_same_as_actor_slot);
        else sqlite3_bind_null(insert_action.st, 8);
        if (action.target_expr_ini.has_value()) sqlite3_bind_text(insert_action.st, 9, action.target_expr_ini->c_str(), -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(insert_action.st, 9);
        if (action.item_id.has_value()) sqlite3_bind_int(insert_action.st, 10, *action.item_id);
        else sqlite3_bind_null(insert_action.st, 10);
        sqlite3_bind_int(insert_action.st, 11, action.ordinal);
        if (sqlite3_step(insert_action.st) != SQLITE_DONE) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
    }

    if (!InsertAuthoringOutboxEvent(
            db_,
            command.event_id,
            "Authoring.PlanSaved.v1",
            std::to_string(command.plan_id),
            command.correlation_id,
            command.causation_id,
            ToEpochMillis(command.created_at_utc),
            command.plan_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (plan_turn_id_out) {
        *plan_turn_id_out = plan_turn_id;
    }
    return true;
}

std::optional<BattlePlanSnapshot> SqliteAuthoringDb::GetBattlePlan(
    std::int64_t plan_id) const {
    if (db_ == nullptr || plan_id <= 0) {
        return std::nullopt;
    }

    Statement plan_st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT plan_id,name,fingerprint,num_turns FROM au_battle_plan WHERE plan_id=?1;",
            -1,
            &plan_st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(plan_st.st, 1, plan_id);
    if (sqlite3_step(plan_st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    BattlePlanSnapshot out{};
    out.plan_id = sqlite3_column_int64(plan_st.st, 0);
    out.name = ColumnText(plan_st.st, 1);
    out.fingerprint = ColumnText(plan_st.st, 2);
    out.num_turns = sqlite3_column_int(plan_st.st, 3);

    Statement turn_st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT plan_turn_id, plan_id, turn_index FROM au_battle_plan_turn WHERE plan_id=?1 ORDER BY turn_index ASC;",
            -1,
            &turn_st.st,
            nullptr)
        != SQLITE_OK) {
        return out;
    }
    sqlite3_bind_int64(turn_st.st, 1, plan_id);
    while (sqlite3_step(turn_st.st) == SQLITE_ROW) {
        BattlePlanTurnSnapshot turn{};
        turn.plan_turn_id = sqlite3_column_int64(turn_st.st, 0);
        turn.plan_id = sqlite3_column_int64(turn_st.st, 1);
        turn.turn_index = sqlite3_column_int(turn_st.st, 2);

        Statement action_st;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT plan_action_id,plan_turn_id,actor_slot,macro,target_kind,target_slot,target_mask_bits,target_single_slot,target_same_as_actor_slot,target_expr_ini,item_id,ordinal "
                "FROM au_battle_plan_action WHERE plan_turn_id=?1 ORDER BY ordinal ASC, plan_action_id ASC;",
                -1,
                &action_st.st,
                nullptr)
            == SQLITE_OK) {
            sqlite3_bind_int64(action_st.st, 1, turn.plan_turn_id);
            while (sqlite3_step(action_st.st) == SQLITE_ROW) {
                BattlePlanActionSnapshot action{};
                action.plan_action_id = sqlite3_column_int64(action_st.st, 0);
                action.plan_turn_id = sqlite3_column_int64(action_st.st, 1);
                action.actor_slot = sqlite3_column_int(action_st.st, 2);
                action.macro = static_cast<BattlePlanActionMacro>(sqlite3_column_int(action_st.st, 3));
                action.target_kind = static_cast<BattlePlanTargetKind>(sqlite3_column_int(action_st.st, 4));
                action.target_slot = ColumnIntOptional(action_st.st, 5);
                action.target_mask_bits = ColumnIntOptional(action_st.st, 6);
                action.target_single_slot = ColumnIntOptional(action_st.st, 7);
                action.target_same_as_actor_slot = ColumnIntOptional(action_st.st, 8);
                if (sqlite3_column_type(action_st.st, 9) != SQLITE_NULL) {
                    action.target_expr_ini = ColumnText(action_st.st, 9);
                }
                action.item_id = ColumnIntOptional(action_st.st, 10);
                action.ordinal = sqlite3_column_int(action_st.st, 11);
                turn.actions.push_back(std::move(action));
            }
        }
        out.turns.push_back(std::move(turn));
    }

    return out;
}

std::vector<BattlePlanSnapshot> SqliteAuthoringDb::ListBattlePlans(
    int max_count) const {
    std::vector<BattlePlanSnapshot> out;
    if (db_ == nullptr) {
        return out;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT plan_id FROM au_battle_plan ORDER BY plan_id DESC LIMIT ?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return out;
    }
    sqlite3_bind_int(st.st, 1, std::max(1, max_count));
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        if (auto snapshot = GetBattlePlan(sqlite3_column_int64(st.st, 0)); snapshot.has_value()) {
            out.push_back(std::move(*snapshot));
        }
    }
    return out;
}

bool SqliteAuthoringDb::EnsureAddressProgram(
    const EnsureAddressProgramCommand& command,
    std::int64_t* address_program_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.program_version <= 0 || command.prog_bytes.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    Statement select_existing;
    constexpr const char* kSelectSql =
        "SELECT address_program_id FROM au_address_program "
        "WHERE program_version=?1 AND prog_bytes=?2 "
        "AND COALESCE(derived_buffer_version,0)=COALESCE(?3,0) "
        "AND COALESCE(derived_buffer_schema_hash,'')=COALESCE(?4,'') "
        "AND COALESCE(soa_structs_hash,'')=COALESCE(?5,'') "
        "LIMIT 1;";
    if (sqlite3_prepare_v2(db_, kSelectSql, -1, &select_existing.st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int(select_existing.st, 1, command.program_version);
    sqlite3_bind_blob(
        select_existing.st,
        2,
        command.prog_bytes.data(),
        static_cast<int>(command.prog_bytes.size()),
        SQLITE_TRANSIENT);
    if (command.derived_buffer_version.has_value()) sqlite3_bind_int(select_existing.st, 3, *command.derived_buffer_version);
    else sqlite3_bind_null(select_existing.st, 3);
    if (command.derived_buffer_schema_hash.has_value()) {
        sqlite3_bind_text(select_existing.st, 4, command.derived_buffer_schema_hash->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(select_existing.st, 4);
    }
    if (command.soa_structs_hash.has_value()) {
        sqlite3_bind_text(select_existing.st, 5, command.soa_structs_hash->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(select_existing.st, 5);
    }

    if (sqlite3_step(select_existing.st) == SQLITE_ROW) {
        if (address_program_id_out) {
            *address_program_id_out = sqlite3_column_int64(select_existing.st, 0);
        }
        return true;
    }

    Statement insert_program;
    constexpr const char* kInsertSql =
        "INSERT INTO au_address_program("
        "program_version,prog_bytes,derived_buffer_version,derived_buffer_schema_hash,soa_structs_hash,description) "
        "VALUES(?1,?2,?3,?4,?5,?6);";
    if (sqlite3_prepare_v2(db_, kInsertSql, -1, &insert_program.st, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int(insert_program.st, 1, command.program_version);
    sqlite3_bind_blob(
        insert_program.st,
        2,
        command.prog_bytes.data(),
        static_cast<int>(command.prog_bytes.size()),
        SQLITE_TRANSIENT);
    if (command.derived_buffer_version.has_value()) sqlite3_bind_int(insert_program.st, 3, *command.derived_buffer_version);
    else sqlite3_bind_null(insert_program.st, 3);
    if (command.derived_buffer_schema_hash.has_value()) {
        sqlite3_bind_text(insert_program.st, 4, command.derived_buffer_schema_hash->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(insert_program.st, 4);
    }
    if (command.soa_structs_hash.has_value()) {
        sqlite3_bind_text(insert_program.st, 5, command.soa_structs_hash->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(insert_program.st, 5);
    }
    if (command.description.empty()) sqlite3_bind_null(insert_program.st, 6);
    else sqlite3_bind_text(insert_program.st, 6, command.description.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(insert_program.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    if (address_program_id_out) {
        *address_program_id_out = sqlite3_last_insert_rowid(db_);
    }
    return true;
}

std::optional<AddressProgramSnapshot> SqliteAuthoringDb::GetAddressProgram(
    std::int64_t address_program_id) const {
    if (db_ == nullptr || address_program_id <= 0) {
        return std::nullopt;
    }

    Statement st;
    constexpr const char* kSql =
        "SELECT address_program_id,program_version,prog_bytes,derived_buffer_version,"
        "derived_buffer_schema_hash,soa_structs_hash,description "
        "FROM au_address_program WHERE address_program_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, address_program_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    AddressProgramSnapshot out{};
    out.address_program_id = sqlite3_column_int64(st.st, 0);
    out.program_version = sqlite3_column_int(st.st, 1);
    out.prog_bytes = ColumnBlob(st.st, 2);
    out.derived_buffer_version = ColumnIntOptional(st.st, 3);
    const auto* derived_hash = sqlite3_column_text(st.st, 4);
    out.derived_buffer_schema_hash = derived_hash
        ? std::optional<std::string>(reinterpret_cast<const char*>(derived_hash))
        : std::nullopt;
    const auto* structs_hash = sqlite3_column_text(st.st, 5);
    out.soa_structs_hash = structs_hash
        ? std::optional<std::string>(reinterpret_cast<const char*>(structs_hash))
        : std::nullopt;
    out.description = ColumnText(st.st, 6);
    return out;
}

bool SqliteAuthoringDb::SavePredicateSpec(
    const SavePredicateSpecCommand& command,
    std::int64_t* predicate_spec_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.name.empty()
        || command.breakpoint_id == 0
        || bp::BPRegistry::find(command.breakpoint_id) == nullptr
        || command.lhs_kind == PredicateOperandKind::Unknown
        || command.rhs_kind == PredicateOperandKind::Unknown
        || (command.width != 1 && command.width != 2 && command.width != 4 && command.width != 8)
        || command.event_id.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement insert_spec;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO au_predicate_spec("
            "name,breakpoint_name,breakpoint_id,lhs_kind,lhs_value,rhs_kind,rhs_value,cmp_op,width,flag_mask,value_mask,"
            "lhs_address_program_id,rhs_address_program_id,abort_on_fail,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15);",
            -1,
            &insert_spec.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_text(insert_spec.st, 1, command.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_spec.st, 2, bp::BPRegistry::name(command.breakpoint_id), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(insert_spec.st, 3, static_cast<int>(command.breakpoint_id));
    const auto lhs_kind = ToDbString(command.lhs_kind);
    const auto rhs_kind = ToDbString(command.rhs_kind);
    const auto cmp_op = ToDbString(command.cmp_op);
    sqlite3_bind_text(insert_spec.st, 4, lhs_kind.data(), static_cast<int>(lhs_kind.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_spec.st, 5, command.lhs_value);
    sqlite3_bind_text(insert_spec.st, 6, rhs_kind.data(), static_cast<int>(rhs_kind.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_spec.st, 7, command.rhs_value);
    sqlite3_bind_text(insert_spec.st, 8, cmp_op.data(), static_cast<int>(cmp_op.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int(insert_spec.st, 9, command.width);
    if (command.flag_mask.has_value()) {
        sqlite3_bind_int64(insert_spec.st, 10, command.flag_mask.value());
    } else {
        sqlite3_bind_null(insert_spec.st, 10);
    }
    if (command.value_mask.has_value()) {
        sqlite3_bind_int64(insert_spec.st, 11, command.value_mask.value());
    } else {
        sqlite3_bind_null(insert_spec.st, 11);
    }
    if (command.lhs_address_program_id.has_value()) {
        sqlite3_bind_int64(insert_spec.st, 12, *command.lhs_address_program_id);
    } else {
        sqlite3_bind_null(insert_spec.st, 12);
    }
    if (command.rhs_address_program_id.has_value()) {
        sqlite3_bind_int64(insert_spec.st, 13, *command.rhs_address_program_id);
    } else {
        sqlite3_bind_null(insert_spec.st, 13);
    }
    sqlite3_bind_int(insert_spec.st, 14, command.abort_on_fail ? 1 : 0);
    sqlite3_bind_int64(insert_spec.st, 15, ToEpochMillis(command.created_at_utc));

    if (sqlite3_step(insert_spec.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto predicate_spec_id = sqlite3_last_insert_rowid(db_);
    if (!InsertAuthoringOutboxEvent(
            db_,
            command.event_id,
            "Authoring.PredicateSpecSaved.v1",
            std::to_string(predicate_spec_id),
            command.correlation_id,
            command.causation_id,
            ToEpochMillis(command.created_at_utc),
            predicate_spec_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (predicate_spec_id_out) {
        *predicate_spec_id_out = predicate_spec_id;
    }

    return true;
}

std::optional<PredicateSpecSnapshot> SqliteAuthoringDb::GetPredicateSpec(
    std::int64_t predicate_spec_id) const {
    if (db_ == nullptr || predicate_spec_id <= 0) {
        return std::nullopt;
    }

    Statement st;
    constexpr const char* kSql =
        "SELECT predicate_spec_id,name,breakpoint_id,lhs_kind,lhs_value,rhs_kind,rhs_value,cmp_op,"
        "width,flag_mask,value_mask,lhs_address_program_id,rhs_address_program_id,abort_on_fail "
        "FROM au_predicate_spec WHERE predicate_spec_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, predicate_spec_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    PredicateSpecSnapshot out{};
    out.predicate_spec_id = sqlite3_column_int64(st.st, 0);
    out.name = ColumnText(st.st, 1);
    out.breakpoint_id = static_cast<BPKey>(sqlite3_column_int(st.st, 2));
    out.lhs_kind = ParsePredicateOperandKind(ColumnText(st.st, 3));
    out.lhs_value = sqlite3_column_int64(st.st, 4);
    out.rhs_kind = ParsePredicateOperandKind(ColumnText(st.st, 5));
    out.rhs_value = sqlite3_column_int64(st.st, 6);
    out.cmp_op = ParsePredicateComparisonOp(ColumnText(st.st, 7));
    out.width = sqlite3_column_int(st.st, 8);
    out.flag_mask = ColumnInt64Optional(st.st, 9);
    out.value_mask = ColumnInt64Optional(st.st, 10);
    out.lhs_address_program_id = ColumnInt64Optional(st.st, 11);
    out.rhs_address_program_id = ColumnInt64Optional(st.st, 12);
    out.abort_on_fail = sqlite3_column_int(st.st, 13) != 0;
    return out;
}

std::vector<PredicateSpecSnapshot> SqliteAuthoringDb::ListPredicateSpecs(
    int max_count) const {
    std::vector<PredicateSpecSnapshot> out;
    if (db_ == nullptr) {
        return out;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT predicate_spec_id FROM au_predicate_spec ORDER BY predicate_spec_id DESC LIMIT ?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return out;
    }
    sqlite3_bind_int(st.st, 1, std::max(1, max_count));
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        if (auto snapshot = GetPredicateSpec(sqlite3_column_int64(st.st, 0)); snapshot.has_value()) {
            out.push_back(std::move(*snapshot));
        }
    }
    return out;
}

bool SqliteAuthoringDb::SavePredicateSet(
    const SavePredicateSetCommand& command,
    std::int64_t* predicate_set_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    Statement insert_set;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO au_predicate_set(created_at_utc) VALUES(?1);",
            -1,
            &insert_set.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    sqlite3_bind_int64(insert_set.st, 1, ToEpochMillis(command.created_at_utc));
    if (sqlite3_step(insert_set.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    const auto predicate_set_id = sqlite3_last_insert_rowid(db_);

    for (int ordinal = 0; ordinal < static_cast<int>(command.predicate_spec_ids.size()); ++ordinal) {
        Statement insert_item;
        if (sqlite3_prepare_v2(
                db_,
                "INSERT INTO au_predicate_set_item(predicate_set_id,predicate_spec_id,ordinal) VALUES(?1,?2,?3);",
                -1,
                &insert_item.st,
                nullptr)
            != SQLITE_OK) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
        sqlite3_bind_int64(insert_item.st, 1, predicate_set_id);
        sqlite3_bind_int64(insert_item.st, 2, command.predicate_spec_ids[ordinal]);
        sqlite3_bind_int(insert_item.st, 3, ordinal);
        if (sqlite3_step(insert_item.st) != SQLITE_DONE) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    if (predicate_set_id_out) {
        *predicate_set_id_out = predicate_set_id;
    }
    return true;
}

std::optional<PredicateSetSnapshot> SqliteAuthoringDb::GetPredicateSet(
    std::int64_t predicate_set_id) const {
    if (db_ == nullptr || predicate_set_id <= 0) {
        return std::nullopt;
    }

    Statement exists;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT predicate_set_id FROM au_predicate_set WHERE predicate_set_id=?1;",
            -1,
            &exists.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(exists.st, 1, predicate_set_id);
    if (sqlite3_step(exists.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    PredicateSetSnapshot out{};
    out.predicate_set_id = predicate_set_id;

    Statement st;
    constexpr const char* kSql =
        "SELECT ps.predicate_spec_id,ps.name,ps.breakpoint_id,ps.lhs_kind,ps.lhs_value,ps.rhs_kind,ps.rhs_value,"
        "ps.cmp_op,ps.width,ps.flag_mask,ps.value_mask,ps.lhs_address_program_id,ps.rhs_address_program_id,ps.abort_on_fail "
        "FROM au_predicate_set_item item "
        "JOIN au_predicate_spec ps ON ps.predicate_spec_id=item.predicate_spec_id "
        "WHERE item.predicate_set_id=?1 ORDER BY item.ordinal ASC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return out;
    }
    sqlite3_bind_int64(st.st, 1, predicate_set_id);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        PredicateSpecSnapshot pred{};
        pred.predicate_spec_id = sqlite3_column_int64(st.st, 0);
        pred.name = ColumnText(st.st, 1);
        pred.breakpoint_id = static_cast<BPKey>(sqlite3_column_int(st.st, 2));
        pred.lhs_kind = ParsePredicateOperandKind(ColumnText(st.st, 3));
        pred.lhs_value = sqlite3_column_int64(st.st, 4);
        pred.rhs_kind = ParsePredicateOperandKind(ColumnText(st.st, 5));
        pred.rhs_value = sqlite3_column_int64(st.st, 6);
        pred.cmp_op = ParsePredicateComparisonOp(ColumnText(st.st, 7));
        pred.width = sqlite3_column_int(st.st, 8);
        pred.flag_mask = ColumnInt64Optional(st.st, 9);
        pred.value_mask = ColumnInt64Optional(st.st, 10);
        pred.lhs_address_program_id = ColumnInt64Optional(st.st, 11);
        pred.rhs_address_program_id = ColumnInt64Optional(st.st, 12);
        pred.abort_on_fail = sqlite3_column_int(st.st, 13) != 0;
        out.predicates.push_back(std::move(pred));
    }
    return out;
}

std::vector<PredicateSetSnapshot> SqliteAuthoringDb::ListPredicateSets(
    int max_count) const {
    std::vector<PredicateSetSnapshot> out;
    if (db_ == nullptr) {
        return out;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT predicate_set_id FROM au_predicate_set ORDER BY predicate_set_id DESC LIMIT ?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return out;
    }
    sqlite3_bind_int(st.st, 1, std::max(1, max_count));
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        if (auto snapshot = GetPredicateSet(sqlite3_column_int64(st.st, 0)); snapshot.has_value()) {
            out.push_back(std::move(*snapshot));
        }
    }
    return out;
}

bool SqliteAuthoringDb::SaveExplorerSettings(
    const SaveExplorerSettingsCommand& command,
    std::int64_t* explorer_settings_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.name.empty() || command.event_id.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement insert_settings;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO au_explorer_settings(name,description,default_plan_id,default_predicate_set_id,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5);",
            -1,
            &insert_settings.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_text(insert_settings.st, 1, command.name.c_str(), -1, SQLITE_TRANSIENT);
    if (command.description.empty()) sqlite3_bind_null(insert_settings.st, 2);
    else sqlite3_bind_text(insert_settings.st, 2, command.description.c_str(), -1, SQLITE_TRANSIENT);
    if (command.default_plan_id.has_value()) sqlite3_bind_int64(insert_settings.st, 3, command.default_plan_id.value());
    else sqlite3_bind_null(insert_settings.st, 3);
    if (command.default_predicate_set_id.has_value()) {
        sqlite3_bind_int64(insert_settings.st, 4, command.default_predicate_set_id.value());
    } else {
        sqlite3_bind_null(insert_settings.st, 4);
    }
    sqlite3_bind_int64(insert_settings.st, 5, ToEpochMillis(command.created_at_utc));

    if (sqlite3_step(insert_settings.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto explorer_settings_id = sqlite3_last_insert_rowid(db_);
    if (!InsertAuthoringOutboxEvent(
            db_,
            command.event_id,
            "Authoring.SettingsSaved.v1",
            std::to_string(explorer_settings_id),
            command.correlation_id,
            command.causation_id,
            ToEpochMillis(command.created_at_utc),
            explorer_settings_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (explorer_settings_id_out) {
        *explorer_settings_id_out = explorer_settings_id;
    }

    return true;
}

std::optional<ExplorerSettingsSnapshot> SqliteAuthoringDb::GetExplorerSettings(
    std::int64_t explorer_settings_id) const {
    if (db_ == nullptr || explorer_settings_id <= 0) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT explorer_settings_id,name,description,default_plan_id,default_predicate_set_id "
            "FROM au_explorer_settings WHERE explorer_settings_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, explorer_settings_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    ExplorerSettingsSnapshot out{};
    out.explorer_settings_id = sqlite3_column_int64(st.st, 0);
    out.name = ColumnText(st.st, 1);
    out.description = ColumnText(st.st, 2);
    out.default_plan_id = ColumnInt64Optional(st.st, 3);
    out.default_predicate_set_id = ColumnInt64Optional(st.st, 4);
    return out;
}

std::vector<ExplorerSettingsSnapshot> SqliteAuthoringDb::ListExplorerSettings(
    int max_count) const {
    std::vector<ExplorerSettingsSnapshot> out;
    if (db_ == nullptr) {
        return out;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT explorer_settings_id FROM au_explorer_settings ORDER BY explorer_settings_id DESC LIMIT ?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return out;
    }
    sqlite3_bind_int(st.st, 1, std::max(1, max_count));
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        if (auto snapshot = GetExplorerSettings(sqlite3_column_int64(st.st, 0)); snapshot.has_value()) {
            out.push_back(std::move(*snapshot));
        }
    }
    return out;
}

bool SqliteAuthoringDb::SaveTemplate(
    const SaveTemplateCommand& command,
    std::int64_t* template_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.name.empty() || command.event_id.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement insert_template;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO au_template("
            "name,description,seed_probe_spec_id,tas_spec_id,battle_run_spec_id,explorer_settings_id,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7);",
            -1,
            &insert_template.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_text(insert_template.st, 1, command.name.c_str(), -1, SQLITE_TRANSIENT);
    if (command.description.empty()) sqlite3_bind_null(insert_template.st, 2);
    else sqlite3_bind_text(insert_template.st, 2, command.description.c_str(), -1, SQLITE_TRANSIENT);
    if (command.seed_probe_spec_id.has_value()) sqlite3_bind_int64(insert_template.st, 3, command.seed_probe_spec_id.value());
    else sqlite3_bind_null(insert_template.st, 3);
    if (command.tas_spec_id.has_value()) sqlite3_bind_int64(insert_template.st, 4, command.tas_spec_id.value());
    else sqlite3_bind_null(insert_template.st, 4);
    if (command.battle_run_spec_id.has_value()) sqlite3_bind_int64(insert_template.st, 5, command.battle_run_spec_id.value());
    else sqlite3_bind_null(insert_template.st, 5);
    if (command.explorer_settings_id.has_value()) sqlite3_bind_int64(insert_template.st, 6, command.explorer_settings_id.value());
    else sqlite3_bind_null(insert_template.st, 6);
    sqlite3_bind_int64(insert_template.st, 7, ToEpochMillis(command.created_at_utc));

    if (sqlite3_step(insert_template.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto template_id = sqlite3_last_insert_rowid(db_);
    if (!InsertAuthoringOutboxEvent(
            db_,
            command.event_id,
            "Authoring.TemplateSaved.v1",
            std::to_string(template_id),
            command.correlation_id,
            command.causation_id,
            ToEpochMillis(command.created_at_utc),
            template_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (template_id_out) {
        *template_id_out = template_id;
    }

    return true;
}

std::optional<TemplateSnapshot> SqliteAuthoringDb::GetTemplate(
    std::int64_t template_id) const {
    if (db_ == nullptr || template_id <= 0) {
        return std::nullopt;
    }

    Statement st;
    constexpr const char* kSql =
        "SELECT template_id, name, description, seed_probe_spec_id, tas_spec_id, battle_run_spec_id, explorer_settings_id "
        "FROM au_template "
        "WHERE template_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, template_id);

    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    TemplateSnapshot snapshot{};
    snapshot.template_id = sqlite3_column_int64(st.st, 0);
    snapshot.name = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 1));
    snapshot.description = ColumnTextOptional(st.st, 2).value_or("");
    snapshot.seed_probe_spec_id = ColumnInt64Optional(st.st, 3);
    snapshot.tas_spec_id = ColumnInt64Optional(st.st, 4);
    snapshot.battle_run_spec_id = ColumnInt64Optional(st.st, 5);
    snapshot.explorer_settings_id = ColumnInt64Optional(st.st, 6);
    return snapshot;
}

std::vector<TemplateSnapshot> SqliteAuthoringDb::ListTemplates(
    int max_count) const {
    std::vector<TemplateSnapshot> out;
    if (db_ == nullptr) {
        return out;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT template_id FROM au_template ORDER BY template_id DESC LIMIT ?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return out;
    }
    sqlite3_bind_int(st.st, 1, std::max(1, max_count));
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        if (auto snapshot = GetTemplate(sqlite3_column_int64(st.st, 0)); snapshot.has_value()) {
            out.push_back(std::move(*snapshot));
        }
    }
    return out;
}

bool SqliteAuthoringDb::SaveWorkflowGraph(
    const SaveWorkflowGraphCommand& command,
    SaveWorkflowGraphResult* result_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.name.empty() || command.graph_hash.empty() || command.event_id.empty()) {
        if (error_out) *error_out = "workflow graph name, graph_hash, and event_id are required";
        return false;
    }
    if (command.nodes.empty()) {
        if (error_out) *error_out = "workflow graph must contain at least one node";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    const auto rollback = [&]() { (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr); };

    std::int64_t workflow_graph_id = command.workflow_graph_id.value_or(0);
    std::optional<std::int64_t> parent_revision_id = command.parent_revision_id;

    if (workflow_graph_id > 0) {
        Statement read_graph;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT active_revision_id FROM au_workflow_graph WHERE workflow_graph_id=?1;",
                -1,
                &read_graph.st,
                nullptr)
            != SQLITE_OK) {
            rollback();
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_int64(read_graph.st, 1, workflow_graph_id);
        if (sqlite3_step(read_graph.st) != SQLITE_ROW) {
            rollback();
            if (error_out) *error_out = "workflow graph not found";
            return false;
        }
        if (!parent_revision_id.has_value()) {
            parent_revision_id = ColumnInt64Optional(read_graph.st, 0);
        }

        Statement update_graph;
        if (sqlite3_prepare_v2(
                db_,
                "UPDATE au_workflow_graph SET name=?1, description=?2 WHERE workflow_graph_id=?3;",
                -1,
                &update_graph.st,
                nullptr)
            != SQLITE_OK) {
            rollback();
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_text(update_graph.st, 1, command.name.c_str(), -1, SQLITE_TRANSIENT);
        if (command.description.empty()) sqlite3_bind_null(update_graph.st, 2);
        else sqlite3_bind_text(update_graph.st, 2, command.description.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(update_graph.st, 3, workflow_graph_id);
        if (sqlite3_step(update_graph.st) != SQLITE_DONE) {
            rollback();
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }
    } else {
        Statement insert_graph;
        if (sqlite3_prepare_v2(
                db_,
                "INSERT INTO au_workflow_graph(name,description,created_at_utc) "
                "VALUES(?1,?2,?3);",
                -1,
                &insert_graph.st,
                nullptr)
            != SQLITE_OK) {
            rollback();
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }

        sqlite3_bind_text(insert_graph.st, 1, command.name.c_str(), -1, SQLITE_TRANSIENT);
        if (command.description.empty()) sqlite3_bind_null(insert_graph.st, 2);
        else sqlite3_bind_text(insert_graph.st, 2, command.description.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert_graph.st, 3, ToEpochMillis(command.created_at_utc));
        if (sqlite3_step(insert_graph.st) != SQLITE_DONE) {
            rollback();
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }

        workflow_graph_id = sqlite3_last_insert_rowid(db_);
    }

    int graph_version = command.graph_version;
    if (graph_version <= 0) {
        Statement version_st;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT COALESCE(MAX(graph_version),0)+1 FROM au_workflow_graph_revision WHERE workflow_graph_id=?1;",
                -1,
                &version_st.st,
                nullptr)
            != SQLITE_OK) {
            rollback();
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_int64(version_st.st, 1, workflow_graph_id);
        if (sqlite3_step(version_st.st) == SQLITE_ROW) {
            graph_version = sqlite3_column_int(version_st.st, 0);
        } else {
            graph_version = 1;
        }
    }

    Statement insert_revision;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO au_workflow_graph_revision("
            "workflow_graph_id,graph_version,graph_hash,parent_revision_id,status,created_at_utc) "
            "VALUES(?1,?2,?3,?4,'active',?5);",
            -1,
            &insert_revision.st,
            nullptr)
        != SQLITE_OK) {
        rollback();
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(insert_revision.st, 1, workflow_graph_id);
    sqlite3_bind_int(insert_revision.st, 2, graph_version);
    sqlite3_bind_text(insert_revision.st, 3, command.graph_hash.c_str(), -1, SQLITE_TRANSIENT);
    if (parent_revision_id.has_value()) sqlite3_bind_int64(insert_revision.st, 4, parent_revision_id.value());
    else sqlite3_bind_null(insert_revision.st, 4);
    sqlite3_bind_int64(insert_revision.st, 5, ToEpochMillis(command.created_at_utc));
    if (sqlite3_step(insert_revision.st) != SQLITE_DONE) {
        rollback();
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    const auto workflow_graph_revision_id = sqlite3_last_insert_rowid(db_);
    std::unordered_map<std::string, std::int64_t> node_id_by_key;
    node_id_by_key.reserve(command.nodes.size());

    for (int node_ordinal = 0; node_ordinal < static_cast<int>(command.nodes.size()); ++node_ordinal) {
        const auto& node = command.nodes[static_cast<std::size_t>(node_ordinal)];
        if (node.node_key.empty() || node.unit_kind.empty()) {
            rollback();
            if (error_out) *error_out = "workflow graph node_key and unit_kind are required";
            return false;
        }
        if (node_id_by_key.find(node.node_key) != node_id_by_key.end()) {
            rollback();
            if (error_out) *error_out = "duplicate workflow graph node_key: " + node.node_key;
            return false;
        }

        Statement insert_node;
        if (sqlite3_prepare_v2(
                db_,
                "INSERT INTO au_workflow_graph_revision_node("
                "workflow_graph_revision_id,node_key,unit_kind,display_name,authored_ref_kind,authored_ref_id,ordinal) "
                "VALUES(?1,?2,?3,?4,?5,?6,?7);",
                -1,
                &insert_node.st,
                nullptr)
            != SQLITE_OK) {
            rollback();
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }

        sqlite3_bind_int64(insert_node.st, 1, workflow_graph_revision_id);
        sqlite3_bind_text(insert_node.st, 2, node.node_key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert_node.st, 3, node.unit_kind.c_str(), -1, SQLITE_TRANSIENT);
        if (node.display_name.empty()) sqlite3_bind_null(insert_node.st, 4);
        else sqlite3_bind_text(insert_node.st, 4, node.display_name.c_str(), -1, SQLITE_TRANSIENT);
        if (node.authored_ref_kind.has_value()) sqlite3_bind_text(insert_node.st, 5, node.authored_ref_kind->c_str(), -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(insert_node.st, 5);
        if (node.authored_ref_id.has_value()) sqlite3_bind_int64(insert_node.st, 6, node.authored_ref_id.value());
        else sqlite3_bind_null(insert_node.st, 6);
        sqlite3_bind_int(insert_node.st, 7, node_ordinal);
        if (sqlite3_step(insert_node.st) != SQLITE_DONE) {
            rollback();
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }

        const auto workflow_graph_revision_node_id = sqlite3_last_insert_rowid(db_);
        node_id_by_key.emplace(node.node_key, workflow_graph_revision_node_id);

        for (int input_ordinal = 0; input_ordinal < static_cast<int>(node.inputs.size()); ++input_ordinal) {
            const auto& input = node.inputs[static_cast<std::size_t>(input_ordinal)];
            if (input.input_key.empty() || input.data_kind.empty()) {
                rollback();
                if (error_out) *error_out = "workflow graph input_key and data_kind are required";
                return false;
            }
            Statement insert_input;
            if (sqlite3_prepare_v2(
                    db_,
                    "INSERT INTO au_workflow_graph_revision_node_input("
                    "workflow_graph_revision_node_id,input_key,data_kind,display_name,required,ordinal) "
                    "VALUES(?1,?2,?3,?4,?5,?6);",
                    -1,
                    &insert_input.st,
                    nullptr)
                != SQLITE_OK) {
                rollback();
                if (error_out) *error_out = sqlite3_errmsg(db_);
                return false;
            }
            sqlite3_bind_int64(insert_input.st, 1, workflow_graph_revision_node_id);
            sqlite3_bind_text(insert_input.st, 2, input.input_key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_input.st, 3, input.data_kind.c_str(), -1, SQLITE_TRANSIENT);
            if (input.display_name.empty()) sqlite3_bind_null(insert_input.st, 4);
            else sqlite3_bind_text(insert_input.st, 4, input.display_name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(insert_input.st, 5, input.required ? 1 : 0);
            sqlite3_bind_int(insert_input.st, 6, input_ordinal);
            if (sqlite3_step(insert_input.st) != SQLITE_DONE) {
                rollback();
                if (error_out) *error_out = sqlite3_errmsg(db_);
                return false;
            }
        }

        for (int output_ordinal = 0; output_ordinal < static_cast<int>(node.possible_outputs.size()); ++output_ordinal) {
            const auto& output = node.possible_outputs[static_cast<std::size_t>(output_ordinal)];
            if (output.output_key.empty() || output.data_kind.empty()) {
                rollback();
                if (error_out) *error_out = "workflow graph output_key and data_kind are required";
                return false;
            }
            Statement insert_output;
            if (sqlite3_prepare_v2(
                    db_,
                    "INSERT INTO au_workflow_graph_revision_node_output("
                    "workflow_graph_revision_node_id,output_key,data_kind,display_name,ordinal) "
                    "VALUES(?1,?2,?3,?4,?5);",
                    -1,
                    &insert_output.st,
                    nullptr)
                != SQLITE_OK) {
                rollback();
                if (error_out) *error_out = sqlite3_errmsg(db_);
                return false;
            }
            sqlite3_bind_int64(insert_output.st, 1, workflow_graph_revision_node_id);
            sqlite3_bind_text(insert_output.st, 2, output.output_key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_output.st, 3, output.data_kind.c_str(), -1, SQLITE_TRANSIENT);
            if (output.display_name.empty()) sqlite3_bind_null(insert_output.st, 4);
            else sqlite3_bind_text(insert_output.st, 4, output.display_name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(insert_output.st, 5, output_ordinal);
            if (sqlite3_step(insert_output.st) != SQLITE_DONE) {
                rollback();
                if (error_out) *error_out = sqlite3_errmsg(db_);
                return false;
            }
        }
    }

    for (int edge_ordinal = 0; edge_ordinal < static_cast<int>(command.edges.size()); ++edge_ordinal) {
        const auto& edge = command.edges[static_cast<std::size_t>(edge_ordinal)];
        const auto from_it = node_id_by_key.find(edge.from_node_key);
        const auto to_it = node_id_by_key.find(edge.to_node_key);
        if (edge.output_key.empty() || edge.input_key.empty() || from_it == node_id_by_key.end() || to_it == node_id_by_key.end()) {
            rollback();
            if (error_out) *error_out = "workflow graph edge references unknown node or empty port";
            return false;
        }

        Statement insert_edge;
        if (sqlite3_prepare_v2(
                db_,
                "INSERT INTO au_workflow_graph_revision_edge("
                "workflow_graph_revision_id,from_revision_node_id,output_key,to_revision_node_id,input_key,guard_kind,guard_value,ordinal) "
                "VALUES(?1,?2,?3,?4,?5,?6,?7,?8);",
                -1,
                &insert_edge.st,
                nullptr)
            != SQLITE_OK) {
            rollback();
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_int64(insert_edge.st, 1, workflow_graph_revision_id);
        sqlite3_bind_int64(insert_edge.st, 2, from_it->second);
        sqlite3_bind_text(insert_edge.st, 3, edge.output_key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert_edge.st, 4, to_it->second);
        sqlite3_bind_text(insert_edge.st, 5, edge.input_key.c_str(), -1, SQLITE_TRANSIENT);
        if (edge.guard_kind.has_value()) sqlite3_bind_text(insert_edge.st, 6, edge.guard_kind->c_str(), -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(insert_edge.st, 6);
        if (edge.guard_value.has_value()) sqlite3_bind_text(insert_edge.st, 7, edge.guard_value->c_str(), -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(insert_edge.st, 7);
        sqlite3_bind_int(insert_edge.st, 8, edge_ordinal);
        if (sqlite3_step(insert_edge.st) != SQLITE_DONE) {
            rollback();
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }
    }

    if (command.make_active) {
        Statement update_active;
        if (sqlite3_prepare_v2(
                db_,
                "UPDATE au_workflow_graph SET active_revision_id=?1 WHERE workflow_graph_id=?2;",
                -1,
                &update_active.st,
                nullptr)
            != SQLITE_OK) {
            rollback();
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_int64(update_active.st, 1, workflow_graph_revision_id);
        sqlite3_bind_int64(update_active.st, 2, workflow_graph_id);
        if (sqlite3_step(update_active.st) != SQLITE_DONE) {
            rollback();
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }
    }

    if (!InsertWorkflowGraphOutboxEvent(db_, command, workflow_graph_id, workflow_graph_revision_id, error_out)) {
        rollback();
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        rollback();
        return false;
    }

    if (result_out) {
        result_out->workflow_graph_id = workflow_graph_id;
        result_out->workflow_graph_revision_id = workflow_graph_revision_id;
    }
    return true;
}

std::optional<WorkflowGraphSnapshot> SqliteAuthoringDb::GetWorkflowGraph(
    std::int64_t workflow_graph_id) const {
    if (db_ == nullptr || workflow_graph_id <= 0) {
        return std::nullopt;
    }

    Statement graph_st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT g.workflow_graph_id,r.workflow_graph_revision_id,r.parent_revision_id,g.name,COALESCE(g.description,''),"
            "r.graph_version,r.graph_hash,r.status "
            "FROM au_workflow_graph g "
            "JOIN au_workflow_graph_revision r ON r.workflow_graph_revision_id=g.active_revision_id "
            "WHERE g.workflow_graph_id=?1;",
            -1,
            &graph_st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(graph_st.st, 1, workflow_graph_id);
    if (sqlite3_step(graph_st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    WorkflowGraphSnapshot out{};
    out.workflow_graph_id = sqlite3_column_int64(graph_st.st, 0);
    out.workflow_graph_revision_id = sqlite3_column_int64(graph_st.st, 1);
    out.parent_revision_id = ColumnInt64Optional(graph_st.st, 2);
    out.name = ColumnText(graph_st.st, 3);
    out.description = ColumnText(graph_st.st, 4);
    out.graph_version = sqlite3_column_int(graph_st.st, 5);
    out.graph_hash = ColumnText(graph_st.st, 6);
    out.status = ColumnText(graph_st.st, 7);

    Statement node_st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT workflow_graph_revision_node_id,node_key,unit_kind,COALESCE(display_name,''),authored_ref_kind,authored_ref_id "
            "FROM au_workflow_graph_revision_node WHERE workflow_graph_revision_id=?1 ORDER BY ordinal ASC, workflow_graph_revision_node_id ASC;",
            -1,
            &node_st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(node_st.st, 1, out.workflow_graph_revision_id);

    while (sqlite3_step(node_st.st) == SQLITE_ROW) {
        WorkflowGraphNodeSnapshot node{};
        node.workflow_graph_revision_node_id = sqlite3_column_int64(node_st.st, 0);
        node.node_key = ColumnText(node_st.st, 1);
        node.unit_kind = ColumnText(node_st.st, 2);
        node.display_name = ColumnText(node_st.st, 3);
        node.authored_ref_kind = ColumnTextOptional(node_st.st, 4);
        node.authored_ref_id = ColumnInt64Optional(node_st.st, 5);

        Statement input_st;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT input_key,data_kind,COALESCE(display_name,''),required "
                "FROM au_workflow_graph_revision_node_input WHERE workflow_graph_revision_node_id=?1 ORDER BY ordinal ASC, workflow_graph_revision_node_input_id ASC;",
                -1,
                &input_st.st,
                nullptr)
            != SQLITE_OK) {
            return std::nullopt;
        }
        sqlite3_bind_int64(input_st.st, 1, node.workflow_graph_revision_node_id);
        while (sqlite3_step(input_st.st) == SQLITE_ROW) {
            node.inputs.push_back(WorkflowGraphNodeInputSnapshot{
                .input_key = ColumnText(input_st.st, 0),
                .data_kind = ColumnText(input_st.st, 1),
                .display_name = ColumnText(input_st.st, 2),
                .required = sqlite3_column_int(input_st.st, 3) != 0,
            });
        }

        Statement output_st;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT output_key,data_kind,COALESCE(display_name,'') "
                "FROM au_workflow_graph_revision_node_output WHERE workflow_graph_revision_node_id=?1 ORDER BY ordinal ASC, workflow_graph_revision_node_output_id ASC;",
                -1,
                &output_st.st,
                nullptr)
            != SQLITE_OK) {
            return std::nullopt;
        }
        sqlite3_bind_int64(output_st.st, 1, node.workflow_graph_revision_node_id);
        while (sqlite3_step(output_st.st) == SQLITE_ROW) {
            node.possible_outputs.push_back(WorkflowGraphNodeOutputSnapshot{
                .output_key = ColumnText(output_st.st, 0),
                .data_kind = ColumnText(output_st.st, 1),
                .display_name = ColumnText(output_st.st, 2),
            });
        }

        out.nodes.push_back(std::move(node));
    }

    Statement edge_st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT e.workflow_graph_revision_edge_id,fn.node_key,e.output_key,tn.node_key,e.input_key,e.guard_kind,e.guard_value "
            "FROM au_workflow_graph_revision_edge e "
            "JOIN au_workflow_graph_revision_node fn ON fn.workflow_graph_revision_node_id=e.from_revision_node_id "
            "JOIN au_workflow_graph_revision_node tn ON tn.workflow_graph_revision_node_id=e.to_revision_node_id "
            "WHERE e.workflow_graph_revision_id=?1 ORDER BY e.ordinal ASC, e.workflow_graph_revision_edge_id ASC;",
            -1,
            &edge_st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(edge_st.st, 1, out.workflow_graph_revision_id);
    while (sqlite3_step(edge_st.st) == SQLITE_ROW) {
        out.edges.push_back(WorkflowGraphEdgeSnapshot{
            .workflow_graph_revision_edge_id = sqlite3_column_int64(edge_st.st, 0),
            .from_node_key = ColumnText(edge_st.st, 1),
            .output_key = ColumnText(edge_st.st, 2),
            .to_node_key = ColumnText(edge_st.st, 3),
            .input_key = ColumnText(edge_st.st, 4),
            .guard_kind = ColumnTextOptional(edge_st.st, 5),
            .guard_value = ColumnTextOptional(edge_st.st, 6),
        });
    }

    return out;
}

std::optional<WorkflowGraphSnapshot> SqliteAuthoringDb::GetWorkflowGraphRevision(
    std::int64_t workflow_graph_revision_id) const {
    if (db_ == nullptr || workflow_graph_revision_id <= 0) {
        return std::nullopt;
    }

    Statement graph_st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT g.workflow_graph_id,r.workflow_graph_revision_id,r.parent_revision_id,g.name,COALESCE(g.description,''),"
            "r.graph_version,r.graph_hash,r.status "
            "FROM au_workflow_graph_revision r "
            "JOIN au_workflow_graph g ON g.workflow_graph_id=r.workflow_graph_id "
            "WHERE r.workflow_graph_revision_id=?1;",
            -1,
            &graph_st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(graph_st.st, 1, workflow_graph_revision_id);
    if (sqlite3_step(graph_st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    WorkflowGraphSnapshot out{};
    out.workflow_graph_id = sqlite3_column_int64(graph_st.st, 0);
    out.workflow_graph_revision_id = sqlite3_column_int64(graph_st.st, 1);
    out.parent_revision_id = ColumnInt64Optional(graph_st.st, 2);
    out.name = ColumnText(graph_st.st, 3);
    out.description = ColumnText(graph_st.st, 4);
    out.graph_version = sqlite3_column_int(graph_st.st, 5);
    out.graph_hash = ColumnText(graph_st.st, 6);
    out.status = ColumnText(graph_st.st, 7);

    Statement node_st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT workflow_graph_revision_node_id,node_key,unit_kind,COALESCE(display_name,''),authored_ref_kind,authored_ref_id "
            "FROM au_workflow_graph_revision_node WHERE workflow_graph_revision_id=?1 ORDER BY ordinal ASC, workflow_graph_revision_node_id ASC;",
            -1,
            &node_st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(node_st.st, 1, out.workflow_graph_revision_id);

    while (sqlite3_step(node_st.st) == SQLITE_ROW) {
        WorkflowGraphNodeSnapshot node{};
        node.workflow_graph_revision_node_id = sqlite3_column_int64(node_st.st, 0);
        node.node_key = ColumnText(node_st.st, 1);
        node.unit_kind = ColumnText(node_st.st, 2);
        node.display_name = ColumnText(node_st.st, 3);
        node.authored_ref_kind = ColumnTextOptional(node_st.st, 4);
        node.authored_ref_id = ColumnInt64Optional(node_st.st, 5);

        Statement input_st;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT input_key,data_kind,COALESCE(display_name,''),required "
                "FROM au_workflow_graph_revision_node_input WHERE workflow_graph_revision_node_id=?1 ORDER BY ordinal ASC, workflow_graph_revision_node_input_id ASC;",
                -1,
                &input_st.st,
                nullptr)
            != SQLITE_OK) {
            return std::nullopt;
        }
        sqlite3_bind_int64(input_st.st, 1, node.workflow_graph_revision_node_id);
        while (sqlite3_step(input_st.st) == SQLITE_ROW) {
            node.inputs.push_back(WorkflowGraphNodeInputSnapshot{
                .input_key = ColumnText(input_st.st, 0),
                .data_kind = ColumnText(input_st.st, 1),
                .display_name = ColumnText(input_st.st, 2),
                .required = sqlite3_column_int(input_st.st, 3) != 0,
            });
        }

        Statement output_st;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT output_key,data_kind,COALESCE(display_name,'') "
                "FROM au_workflow_graph_revision_node_output WHERE workflow_graph_revision_node_id=?1 ORDER BY ordinal ASC, workflow_graph_revision_node_output_id ASC;",
                -1,
                &output_st.st,
                nullptr)
            != SQLITE_OK) {
            return std::nullopt;
        }
        sqlite3_bind_int64(output_st.st, 1, node.workflow_graph_revision_node_id);
        while (sqlite3_step(output_st.st) == SQLITE_ROW) {
            node.possible_outputs.push_back(WorkflowGraphNodeOutputSnapshot{
                .output_key = ColumnText(output_st.st, 0),
                .data_kind = ColumnText(output_st.st, 1),
                .display_name = ColumnText(output_st.st, 2),
            });
        }

        out.nodes.push_back(std::move(node));
    }

    Statement edge_st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT e.workflow_graph_revision_edge_id,fn.node_key,e.output_key,tn.node_key,e.input_key,e.guard_kind,e.guard_value "
            "FROM au_workflow_graph_revision_edge e "
            "JOIN au_workflow_graph_revision_node fn ON fn.workflow_graph_revision_node_id=e.from_revision_node_id "
            "JOIN au_workflow_graph_revision_node tn ON tn.workflow_graph_revision_node_id=e.to_revision_node_id "
            "WHERE e.workflow_graph_revision_id=?1 ORDER BY e.ordinal ASC, e.workflow_graph_revision_edge_id ASC;",
            -1,
            &edge_st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(edge_st.st, 1, out.workflow_graph_revision_id);
    while (sqlite3_step(edge_st.st) == SQLITE_ROW) {
        out.edges.push_back(WorkflowGraphEdgeSnapshot{
            .workflow_graph_revision_edge_id = sqlite3_column_int64(edge_st.st, 0),
            .from_node_key = ColumnText(edge_st.st, 1),
            .output_key = ColumnText(edge_st.st, 2),
            .to_node_key = ColumnText(edge_st.st, 3),
            .input_key = ColumnText(edge_st.st, 4),
            .guard_kind = ColumnTextOptional(edge_st.st, 5),
            .guard_value = ColumnTextOptional(edge_st.st, 6),
        });
    }

    return out;
}

std::vector<WorkflowGraphSnapshot> SqliteAuthoringDb::ListWorkflowGraphs(
    int max_count) const {
    std::vector<WorkflowGraphSnapshot> out;
    if (db_ == nullptr) {
        return out;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT workflow_graph_id FROM au_workflow_graph ORDER BY workflow_graph_id DESC LIMIT ?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return out;
    }
    sqlite3_bind_int(st.st, 1, std::max(1, max_count));
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        if (auto snapshot = GetWorkflowGraph(sqlite3_column_int64(st.st, 0)); snapshot.has_value()) {
            out.push_back(std::move(*snapshot));
        }
    }
    return out;
}

std::vector<events::EventEnvelope> SqliteAuthoringDb::ReadUnpublishedOutboxBatch(
    std::int64_t after_outbox_id,
    int max_batch_size) {
    if (db_ == nullptr || max_batch_size <= 0) {
        return {};
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "SELECT outbox_id, event_id, event_type, event_version, context_name, aggregate_kind, aggregate_id, "
        "COALESCE(correlation_id, ''), COALESCE(causation_id, ''), occurred_at_utc, payload_ref_kind, payload_ref_id "
        "FROM au_outbox_message "
        "WHERE outbox_id>?1 AND published_at_utc IS NULL "
        "ORDER BY outbox_id ASC "
        "LIMIT ?2;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return {};
    }

    sqlite3_bind_int64(st, 1, after_outbox_id);
    sqlite3_bind_int(st, 2, max_batch_size);

    std::vector<events::EventEnvelope> batch;
    batch.reserve(static_cast<std::size_t>(max_batch_size));

    while (sqlite3_step(st) == SQLITE_ROW) {
        events::EventEnvelope envelope;

        const auto* event_id = sqlite3_column_text(st, 1);
        const auto* event_type = sqlite3_column_text(st, 2);
        const auto* context_name = sqlite3_column_text(st, 4);
        const auto* aggregate_kind = sqlite3_column_text(st, 5);
        const auto* aggregate_id = sqlite3_column_text(st, 6);
        const auto* correlation_id = sqlite3_column_text(st, 7);
        const auto* causation_id = sqlite3_column_text(st, 8);
        const auto* payload_ref_kind = sqlite3_column_text(st, 10);

        envelope.event_id = event_id == nullptr ? std::string{} : reinterpret_cast<const char*>(event_id);
        envelope.event_type = event_type == nullptr ? std::string{} : reinterpret_cast<const char*>(event_type);
        envelope.event_version = sqlite3_column_int(st, 3);
        envelope.context_name = context_name == nullptr ? std::string{} : reinterpret_cast<const char*>(context_name);
        envelope.aggregate_kind = aggregate_kind == nullptr ? std::string{} : reinterpret_cast<const char*>(aggregate_kind);
        envelope.aggregate_id = aggregate_id == nullptr ? std::string{} : reinterpret_cast<const char*>(aggregate_id);
        envelope.correlation_id = correlation_id == nullptr ? std::string{} : reinterpret_cast<const char*>(correlation_id);
        envelope.causation_id = causation_id == nullptr ? std::string{} : reinterpret_cast<const char*>(causation_id);
        envelope.occurred_at_utc = FromEpochMillis(sqlite3_column_int64(st, 9));
        envelope.payload_ref_kind = payload_ref_kind == nullptr ? std::string{} : reinterpret_cast<const char*>(payload_ref_kind);
        envelope.payload_ref_id = sqlite3_column_int64(st, 11);

        batch.push_back(std::move(envelope));
    }

    sqlite3_finalize(st);
    return batch;
}

bool SqliteAuthoringDb::MarkOutboxPublished(
    std::int64_t outbox_id,
    types::UtcTimePoint published_at_utc) {
    if (db_ == nullptr || outbox_id <= 0) {
        return false;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "UPDATE au_outbox_message "
        "SET published_at_utc=?2 "
        "WHERE outbox_id=?1;";

    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(st, 1, outbox_id);
    sqlite3_bind_int64(st, 2, ToEpochMillis(published_at_utc));

    const auto rc = sqlite3_step(st);
    sqlite3_finalize(st);

    return rc == SQLITE_DONE && sqlite3_changes(db_) > 0;
}

bool SqliteAuthoringDb::MarkOutboxPublishFailure(
    std::int64_t outbox_id,
    std::string_view last_error) {
    if (db_ == nullptr || outbox_id <= 0) {
        return false;
    }

    sqlite3_stmt* st = nullptr;
    constexpr const char* kSql =
        "UPDATE au_outbox_message "
        "SET attempt_count=attempt_count+1, last_error=?2 "
        "WHERE outbox_id=?1;";

    if (sqlite3_prepare_v2(db_, kSql, -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(st, 1, outbox_id);
    sqlite3_bind_text(st, 2, last_error.data(), static_cast<int>(last_error.size()), SQLITE_TRANSIENT);

    const auto rc = sqlite3_step(st);
    sqlite3_finalize(st);

    return rc == SQLITE_DONE && sqlite3_changes(db_) > 0;
}

retention::OutboxRetentionPreview SqliteAuthoringDb::PreviewOutboxRetention(
    const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
    types::UtcTimePoint now_utc,
    const retention::OutboxRetentionPolicy& policy) const {
    std::int64_t max_outbox_id = 0;
    sqlite3_stmt* st = nullptr;
    if (db_ != nullptr
        && sqlite3_prepare_v2(db_, "SELECT COALESCE(MAX(outbox_id), 0) FROM au_outbox_message;", -1, &st, nullptr)
            == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            max_outbox_id = sqlite3_column_int64(st, 0);
        }
    }
    sqlite3_finalize(st);
    return retention::BuildOutboxRetentionPreview(max_outbox_id, subscriptions, now_utc, policy);
}

bool SqliteAuthoringDb::PurgeOutboxThroughRetentionFloor(
    const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
    types::UtcTimePoint now_utc,
    const retention::OutboxRetentionPolicy& policy,
    int max_rows,
    int* rows_deleted_out,
    std::string* error_out) {
    if (rows_deleted_out) *rows_deleted_out = 0;
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (max_rows <= 0) {
        if (error_out) *error_out = "max_rows must be > 0";
        return false;
    }

    const auto preview = PreviewOutboxRetention(subscriptions, now_utc, policy);
    if (preview.IsPurgeBlocked()) {
        if (error_out) *error_out = "purge blocked by required paused/error subscriptions";
        return false;
    }
    if (!preview.safe_purge_floor_outbox_id.has_value()) {
        if (error_out) *error_out = "safe purge floor unavailable";
        return false;
    }

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "DELETE FROM au_outbox_message "
            "WHERE outbox_id IN ("
            "  SELECT outbox_id FROM au_outbox_message "
            "  WHERE published_at_utc IS NOT NULL AND outbox_id < ?1 "
            "  ORDER BY outbox_id ASC LIMIT ?2"
            ");",
            -1,
            &st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(st, 1, preview.safe_purge_floor_outbox_id.value());
    sqlite3_bind_int(st, 2, max_rows);
    const auto rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    if (rows_deleted_out) *rows_deleted_out = sqlite3_changes(db_);
    return true;
}

std::optional<AuthoringPayloadRecord> SqliteAuthoringDb::ResolveAuthoringPayload(
    const events::EventEnvelope& envelope) const {
    if (!events::ValidateV1PayloadRef(envelope, "Authoring", "authoring_event")) {
        return std::nullopt;
    }

    return ResolveAuthoringPayload(
        envelope.event_type,
        envelope.event_version,
        envelope.payload_ref_kind,
        envelope.payload_ref_id);
}

std::optional<AuthoringPayloadRecord> SqliteAuthoringDb::ResolveAuthoringPayload(
    std::string_view event_type,
    int event_version,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    if (db_ == nullptr) {
        return std::nullopt;
    }

    const auto contract = events::ResolvePayloadResolverContract(event_type, event_version);
    if (!contract.has_value() || contract.value() != events::PayloadResolverContract::AuthoringV1) {
        return std::nullopt;
    }

    if (payload_ref_kind.empty() || payload_ref_id <= 0) {
        return std::nullopt;
    }

    std::string_view effective_ref_kind = payload_ref_kind;
    if (payload_ref_kind == "authoring_event") {
        if (event_type == "Authoring.TemplateSaved.v1") {
            effective_ref_kind = "template";
        }
        else if (event_type == "Authoring.SeedProbeSpecSaved.v1") {
            effective_ref_kind = "seed_probe_spec";
        }
        else if (event_type == "Authoring.BattleRunSpecSaved.v1") {
            effective_ref_kind = "battle_run_spec";
        }
        else if (event_type == "Authoring.WorkflowGraphSaved.v1") {
            effective_ref_kind = "workflow_graph";
        }
        else {
            effective_ref_kind = "template";
        }
    }

    AuthoringPayloadRecord payload{};
    if (effective_ref_kind == "template") {
        if (!TryReadTemplatePayload(db_, payload_ref_id, &payload)) {
            return std::nullopt;
        }
        return payload;
    }

    if (effective_ref_kind == "seed_probe_spec") {
        if (!TryReadTemplateBySeedProbeSpec(db_, payload_ref_id, &payload)) {
            return std::nullopt;
        }
        return payload;
    }

    if (effective_ref_kind == "battle_run_spec") {
        if (!TryReadTemplateByBattleRunSpec(db_, payload_ref_id, &payload)) {
            return std::nullopt;
        }
        return payload;
    }

    if (effective_ref_kind == "workflow_graph") {
        if (!TryReadWorkflowGraphPayload(db_, payload_ref_id, &payload)) {
            return std::nullopt;
        }
        return payload;
    }

    return std::nullopt;
}

} // namespace simcore::db
