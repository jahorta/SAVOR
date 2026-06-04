#include "SqliteAuthoringDb.h"

#include <chrono>
#include <string>

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
        "SELECT template_id, COALESCE(seed_probe_spec_id, 0), COALESCE(battle_run_spec_id, 0) "
        "FROM au_template "
        "WHERE template_id=?1;";
    if (sqlite3_prepare_v2(db, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(st.st, 1, template_id);

    if (sqlite3_step(st.st) == SQLITE_ROW) {
        out->template_id = sqlite3_column_int64(st.st, 0);
        out->seed_probe_spec_id = sqlite3_column_int64(st.st, 1);
        out->battle_run_spec_id = sqlite3_column_int64(st.st, 2);
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
        "SELECT template_id, COALESCE(seed_probe_spec_id, 0), COALESCE(battle_run_spec_id, 0) "
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
        out->battle_run_spec_id = sqlite3_column_int64(st.st, 2);
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
        out->battle_run_spec_id = 0;
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
        "SELECT template_id, COALESCE(seed_probe_spec_id, 0), COALESCE(battle_run_spec_id, 0) "
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
        out->battle_run_spec_id = sqlite3_column_int64(st.st, 2);
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
        out->battle_run_spec_id = battle_run_spec_id;
        return true;
    }

    return false;
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

bool SqliteAuthoringDb::SaveTasSpec(
    const SaveTasSpecCommand& command,
    std::int64_t* tas_spec_id_out,
    std::int64_t* tas_spec_base_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.base_name.empty() || command.base_dtm_artifact_id <= 0 || command.event_id.empty()) {
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

bool SqliteAuthoringDb::SavePredicateSpec(
    const SavePredicateSpecCommand& command,
    std::int64_t* predicate_spec_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.name.empty()
        || command.breakpoint_name.empty()
        || command.lhs_kind.empty()
        || command.rhs_kind.empty()
        || command.cmp_op.empty()
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
            "name,breakpoint_name,lhs_kind,lhs_value,rhs_kind,rhs_value,cmp_op,flag_mask,value_mask,abort_on_fail,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11);",
            -1,
            &insert_spec.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_text(insert_spec.st, 1, command.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_spec.st, 2, command.breakpoint_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_spec.st, 3, command.lhs_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_spec.st, 4, command.lhs_value);
    sqlite3_bind_text(insert_spec.st, 5, command.rhs_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_spec.st, 6, command.rhs_value);
    sqlite3_bind_text(insert_spec.st, 7, command.cmp_op.c_str(), -1, SQLITE_TRANSIENT);
    if (command.flag_mask.has_value()) {
        sqlite3_bind_int64(insert_spec.st, 8, command.flag_mask.value());
    } else {
        sqlite3_bind_null(insert_spec.st, 8);
    }
    if (command.value_mask.has_value()) {
        sqlite3_bind_int64(insert_spec.st, 9, command.value_mask.value());
    } else {
        sqlite3_bind_null(insert_spec.st, 9);
    }
    sqlite3_bind_int(insert_spec.st, 10, command.abort_on_fail ? 1 : 0);
    sqlite3_bind_int64(insert_spec.st, 11, ToEpochMillis(command.created_at_utc));

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

    return std::nullopt;
}

} // namespace simcore::db
