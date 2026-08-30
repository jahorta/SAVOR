#include "SqliteAuthoringDb.h"

#include <algorithm>
#include <chrono>
#include <charconv>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "../Common/Events/EventPayloadDispatch.h"
#include "../Common/Events/EventPayloadValidation.h"
#include "../Common/Events/OutboxEventIds.h"
#include "../../SavorCore/Utils/Hash.h"

namespace savor::db {

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

void BindOptionalInt64(sqlite3_stmt* st, int index, const std::optional<std::int64_t>& value) {
    if (value.has_value()) {
        sqlite3_bind_int64(st, index, *value);
    } else {
        sqlite3_bind_null(st, index);
    }
}

void BindOptionalInt(sqlite3_stmt* st, int index, const std::optional<int>& value) {
    if (value.has_value()) {
        sqlite3_bind_int(st, index, *value);
    } else {
        sqlite3_bind_null(st, index);
    }
}

void BindOptionalText(sqlite3_stmt* st, int index, const std::optional<std::string>& value) {
    if (value.has_value()) {
        sqlite3_bind_text(st, index, value->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(st, index);
    }
}

bool ValidFrameByte(std::int32_t value) {
    return value >= 0 && value <= 255;
}

std::string HashAuthoringInputSetFrames(const std::vector<AuthoringInputSetFrameCommand>& frames) {
    std::ostringstream out;
    out << "au.input_set.v1\n";
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto& frame = frames[i];
        out << i << ':'
            << frame.main_x << ','
            << frame.main_y << ','
            << frame.cstick_x << ','
            << frame.cstick_y << ','
            << frame.trigger_x << ','
            << frame.trigger_y << '\n';
    }
    const auto text = out.str();
    return hash::sha256(text.data(), text.size());
}



std::string_view AuthoringAggregateKindForEvent(std::string_view event_type) {
    if (event_type == "Authoring.SeedProbeSpecSaved.v1") return "seed_probe_spec";
    if (event_type == "Authoring.PlanSaved.v1") return "battle_plan";
    if (event_type == "Authoring.BattlePlanActionPresetSaved.v1"
        || event_type == "Authoring.BattlePlanActionPresetRenamed.v1") {
        return "battle_plan_action_preset";
    }
    if (event_type == "Authoring.WorkflowGraphSaved.v1") return "workflow_graph";
    return "authoring";
}

bool InsertAuthoringOutboxEvent(
    sqlite3* db,
    std::string_view event_type,
    std::string_view aggregate_id,
    std::string_view correlation_id,
    std::string_view causation_id,
    std::int64_t occurred_at_utc,
    std::int64_t payload_ref_id,
    std::string* error_out) {
    const auto aggregate_kind = AuthoringAggregateKindForEvent(event_type);
    constexpr int kMaxEventIdAttempts = 5;
    for (int attempt = 0; attempt < kMaxEventIdAttempts; ++attempt) {
        std::string event_id;
        if (!outbox::MakeDbOwnedEventId(
                db,
                "Authoring",
                event_type,
                "authoring_event",
                payload_ref_id,
                &event_id,
                error_out)) {
            return false;
        }

        Statement st;
        if (sqlite3_prepare_v2(
                db,
                "INSERT INTO au_outbox_message("
                "event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id) "
                "VALUES(?1,?2,1,'Authoring',?3,?4,?5,?6,?7,'authoring_event',?8);",
                -1,
                &st.st,
                nullptr)
            != SQLITE_OK) {
            if (error_out != nullptr) {
                *error_out = sqlite3_errmsg(db);
            }
            return false;
        }

        sqlite3_bind_text(st.st, 1, event_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 2, event_type.data(), static_cast<int>(event_type.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 3, aggregate_kind.data(), static_cast<int>(aggregate_kind.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 4, aggregate_id.data(), static_cast<int>(aggregate_id.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 5, correlation_id.data(), static_cast<int>(correlation_id.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 6, causation_id.data(), static_cast<int>(causation_id.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(st.st, 7, occurred_at_utc);
        sqlite3_bind_int64(st.st, 8, payload_ref_id);
        const auto rc = sqlite3_step(st.st);
        if (rc == SQLITE_DONE) {
            return true;
        }
        if (!outbox::IsUniqueConstraint(db)) {
            if (error_out != nullptr) {
                *error_out = sqlite3_errmsg(db);
            }
            return false;
        }
    }
    if (error_out != nullptr) {
        *error_out = "failed to generate a unique authoring outbox event id";
    }
    return false;
}

bool TryReadSeedProbeSpecPayload(sqlite3* db, std::int64_t seed_probe_spec_id, AuthoringPayloadRecord* out) {
    if (db == nullptr || out == nullptr || seed_probe_spec_id <= 0) {
        return false;
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
        out->seed_probe_spec_id = seed_probe_spec_id;
        return true;
    }

    return false;
}

std::optional<SeedProbeSpecSnapshot> LoadSeedProbeSpecByName(sqlite3* db, std::string_view name) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "SELECT s.seed_probe_spec_id,s.name,s.priority,"
            "g.min_value,g.max_value,g.cap_trigger_top,g.ignore_trigger_min_max,"
            "u.combo_attempts_per_target,u.combo_sampler_tries,s.auto_schedule_battle_run "
            "FROM au_seed_probe_spec s "
            "JOIN au_seed_probe_grid_spec g ON g.seed_probe_grid_spec_id=s.grid_spec_id "
            "JOIN au_seed_probe_unique_spec u ON u.seed_probe_unique_spec_id=s.unique_spec_id "
            "WHERE s.name=?1 LIMIT 1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_text(st.st, 1, name.data(), static_cast<int>(name.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    SeedProbeSpecSnapshot snapshot{};
    snapshot.seed_probe_spec_id = sqlite3_column_int64(st.st, 0);
    const auto* name_text = sqlite3_column_text(st.st, 1);
    snapshot.name = name_text == nullptr ? "" : reinterpret_cast<const char*>(name_text);
    snapshot.priority = sqlite3_column_int(st.st, 2);
    snapshot.min_value = sqlite3_column_int64(st.st, 3);
    snapshot.max_value = sqlite3_column_int64(st.st, 4);
    snapshot.cap_trigger_top = sqlite3_column_int(st.st, 5) != 0;
    snapshot.ignore_trigger_minmax = sqlite3_column_int(st.st, 6) != 0;
    snapshot.combo_attempts_per_target = sqlite3_column_int(st.st, 7);
    snapshot.combo_sampler_tries = sqlite3_column_int(st.st, 8);
    snapshot.auto_schedule_battle_run = sqlite3_column_int(st.st, 9) != 0;
    return snapshot;
}

bool SeedProbeSpecIdentityMatches(const SeedProbeSpecSnapshot& row, const SaveSeedProbeSpecCommand& command) {
    return row.name == command.name
        && row.priority == command.priority
        && row.min_value == command.min_value
        && row.max_value == command.max_value
        && row.cap_trigger_top == command.cap_trigger_top
        && row.ignore_trigger_minmax == command.ignore_trigger_minmax
        && row.combo_attempts_per_target == command.combo_attempts_per_target
        && row.combo_sampler_tries == command.combo_sampler_tries
        && row.auto_schedule_battle_run == command.auto_schedule_battle_run;
}

std::optional<SaveWorkflowGraphResult> LoadWorkflowGraphByHash(
    sqlite3* db,
    std::string_view graph_hash,
    std::string* graph_name_out) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "SELECT g.workflow_graph_id,r.workflow_graph_revision_id,g.name "
            "FROM au_workflow_graph_revision r "
            "JOIN au_workflow_graph g ON g.workflow_graph_id=r.workflow_graph_id "
            "WHERE r.graph_hash=?1 LIMIT 1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_text(st.st, 1, graph_hash.data(), static_cast<int>(graph_hash.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    SaveWorkflowGraphResult result{};
    result.workflow_graph_id = sqlite3_column_int64(st.st, 0);
    result.workflow_graph_revision_id = sqlite3_column_int64(st.st, 1);
    if (graph_name_out != nullptr) {
        const auto* name_text = sqlite3_column_text(st.st, 2);
        *graph_name_out = name_text == nullptr ? "" : reinterpret_cast<const char*>(name_text);
    }
    return result;
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

bool TryReadBattlePlanActionPresetPayload(sqlite3* db, std::int64_t action_preset_id, AuthoringPayloadRecord* out) {
    if (db == nullptr || out == nullptr || action_preset_id <= 0) {
        return false;
    }

    Statement st;
    constexpr const char* kSql =
        "SELECT action_preset_id "
        "FROM au_battle_plan_action_preset "
        "WHERE action_preset_id=?1;";
    if (sqlite3_prepare_v2(db, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(st.st, 1, action_preset_id);
    if (sqlite3_step(st.st) == SQLITE_ROW) {
        out->battle_plan_action_preset_id = sqlite3_column_int64(st.st, 0);
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
    const auto aggregate_id = std::to_string(workflow_graph_id);
    constexpr std::string_view kEventType = "Authoring.WorkflowGraphSaved.v1";
    constexpr int kMaxEventIdAttempts = 5;
    for (int attempt = 0; attempt < kMaxEventIdAttempts; ++attempt) {
        std::string event_id;
        if (!outbox::MakeDbOwnedEventId(
                db,
                "Authoring",
                kEventType,
                "authoring_event",
                workflow_graph_revision_id,
                &event_id,
                error_out)) {
            return false;
        }

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

        sqlite3_bind_text(st.st, 1, event_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 2, aggregate_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 3, command.correlation_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st.st, 4, command.causation_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st.st, 5, ToEpochMillis(command.created_at_utc));
        sqlite3_bind_int64(st.st, 6, workflow_graph_revision_id);
        const auto rc = sqlite3_step(st.st);
        if (rc == SQLITE_DONE) {
            return true;
        }
        if (!outbox::IsUniqueConstraint(db)) {
            if (error_out != nullptr) {
                *error_out = sqlite3_errmsg(db);
            }
            return false;
        }
    }
    if (error_out != nullptr) {
        *error_out = "failed to generate a unique workflow graph outbox event id";
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
    if (command.name.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    if (const auto existing = LoadSeedProbeSpecByName(db_, command.name); existing.has_value()) {
        if (!SeedProbeSpecIdentityMatches(*existing, command)) {
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            if (error_out) *error_out = "seed probe spec name already exists with different defining fields";
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
            *seed_probe_spec_id_out = existing->seed_probe_spec_id;
        }
        return true;
    }

    Statement insert_spec;
    Statement insert_grid;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO au_seed_probe_grid_spec("
            "min_value,max_value,cap_trigger_top,ignore_trigger_min_max) "
            "VALUES(?1,?2,?3,?4);",
            -1,
            &insert_grid.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(insert_grid.st, 1, command.min_value);
    sqlite3_bind_int64(insert_grid.st, 2, command.max_value);
    sqlite3_bind_int(insert_grid.st, 3, command.cap_trigger_top ? 1 : 0);
    sqlite3_bind_int(insert_grid.st, 4, command.ignore_trigger_minmax ? 1 : 0);

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
    // Schema compatibility shim: these NOT NULL legacy columns are otherwise
    // excluded from the authoring contract until the separate DB migration.
    sqlite3_bind_int64(insert_spec.st, 3, 0);
    sqlite3_bind_int64(insert_spec.st, 4, 0);
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
            "SELECT s.seed_probe_spec_id,s.name,s.priority,"
            "g.min_value,g.max_value,g.cap_trigger_top,g.ignore_trigger_min_max,"
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
    snapshot.min_value = sqlite3_column_int64(st.st, 3);
    snapshot.max_value = sqlite3_column_int64(st.st, 4);
    snapshot.cap_trigger_top = sqlite3_column_int(st.st, 5) != 0;
    snapshot.ignore_trigger_minmax = sqlite3_column_int(st.st, 6) != 0;
    snapshot.combo_attempts_per_target = sqlite3_column_int(st.st, 7);
    snapshot.combo_sampler_tries = sqlite3_column_int(st.st, 8);
    snapshot.auto_schedule_battle_run = sqlite3_column_int(st.st, 9) != 0;
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

bool SqliteAuthoringDb::EnsureAuthoringInputSet(
    const EnsureAuthoringInputSetCommand& command,
    std::int64_t* input_set_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.frames.empty()) {
        if (error_out) *error_out = "authoring input set requires at least one frame";
        return false;
    }
    for (const auto& frame : command.frames) {
        if (!ValidFrameByte(frame.main_x)
            || !ValidFrameByte(frame.main_y)
            || !ValidFrameByte(frame.cstick_x)
            || !ValidFrameByte(frame.cstick_y)
            || !ValidFrameByte(frame.trigger_x)
            || !ValidFrameByte(frame.trigger_y)) {
            if (error_out) *error_out = "authoring input set frame values must be between 0 and 255";
            return false;
        }
    }

    const auto content_hash = HashAuthoringInputSetFrames(command.frames);
    if (content_hash.empty()) {
        if (error_out) *error_out = "failed to hash authoring input set";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement existing;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT input_set_id FROM au_input_set WHERE content_hash=?1 LIMIT 1;",
            -1,
            &existing.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(existing.st, 1, content_hash.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(existing.st) == SQLITE_ROW) {
        if (input_set_id_out) {
            *input_set_id_out = sqlite3_column_int64(existing.st, 0);
        }
        if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
        return true;
    }

    Statement insert_set;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO au_input_set(name,content_hash,created_at_utc) VALUES(?1,?2,?3);",
            -1,
            &insert_set.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (command.name.empty()) sqlite3_bind_null(insert_set.st, 1);
    else sqlite3_bind_text(insert_set.st, 1, command.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_set.st, 2, content_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_set.st, 3, ToEpochMillis(command.created_at_utc));
    if (sqlite3_step(insert_set.st) != SQLITE_DONE) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    const auto input_set_id = sqlite3_last_insert_rowid(db_);

    for (int ordinal = 0; ordinal < static_cast<int>(command.frames.size()); ++ordinal) {
        const auto& frame = command.frames[static_cast<std::size_t>(ordinal)];
        Statement insert_frame;
        if (sqlite3_prepare_v2(
                db_,
                "INSERT INTO au_input_set_frame(input_set_id,ordinal,main_x,main_y,cstick_x,cstick_y,trigger_x,trigger_y) "
                "VALUES(?1,?2,?3,?4,?5,?6,?7,?8);",
                -1,
                &insert_frame.st,
                nullptr)
            != SQLITE_OK) {
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_int64(insert_frame.st, 1, input_set_id);
        sqlite3_bind_int(insert_frame.st, 2, ordinal);
        sqlite3_bind_int(insert_frame.st, 3, frame.main_x);
        sqlite3_bind_int(insert_frame.st, 4, frame.main_y);
        sqlite3_bind_int(insert_frame.st, 5, frame.cstick_x);
        sqlite3_bind_int(insert_frame.st, 6, frame.cstick_y);
        sqlite3_bind_int(insert_frame.st, 7, frame.trigger_x);
        sqlite3_bind_int(insert_frame.st, 8, frame.trigger_y);
        if (sqlite3_step(insert_frame.st) != SQLITE_DONE) {
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    if (input_set_id_out) {
        *input_set_id_out = input_set_id;
    }
    return true;
}

std::vector<AuthoringInputSetFrameSnapshot> SqliteAuthoringDb::ListAuthoringInputSetFrames(
    std::int64_t input_set_id) const {
    std::vector<AuthoringInputSetFrameSnapshot> out;
    if (db_ == nullptr || input_set_id <= 0) {
        return out;
    }
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT ordinal,main_x,main_y,cstick_x,cstick_y,trigger_x,trigger_y "
            "FROM au_input_set_frame WHERE input_set_id=?1 ORDER BY ordinal ASC;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return out;
    }
    sqlite3_bind_int64(st.st, 1, input_set_id);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        out.push_back(AuthoringInputSetFrameSnapshot{
            .ordinal = sqlite3_column_int(st.st, 0),
            .main_x = sqlite3_column_int(st.st, 1),
            .main_y = sqlite3_column_int(st.st, 2),
            .cstick_x = sqlite3_column_int(st.st, 3),
            .cstick_y = sqlite3_column_int(st.st, 4),
            .trigger_x = sqlite3_column_int(st.st, 5),
            .trigger_y = sqlite3_column_int(st.st, 6),
        });
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
    if (command.name.empty() || command.fingerprint.empty()) {
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
            "INSERT INTO au_battle_plan(name,description,fingerprint,created_at_utc) VALUES(?1,?2,?3,?4);",
            -1,
            &insert_plan.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_text(insert_plan.st, 1, command.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_plan.st, 2, command.description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_plan.st, 3, command.fingerprint.c_str(), -1, SQLITE_TRANSIENT);
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

bool SqliteAuthoringDb::SaveBattlePlanActionPreset(
    const SaveBattlePlanActionPresetCommand& command,
    std::int64_t* action_preset_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.name.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    Statement insert_preset;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO au_battle_plan_action_preset("
            "name,macro,target_kind,item_id,target_mask_bits,target_single_slot,target_same_as_actor_slot,flags,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9);",
            -1,
            &insert_preset.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    sqlite3_bind_text(insert_preset.st, 1, command.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(insert_preset.st, 2, static_cast<int>(command.macro));
    sqlite3_bind_int(insert_preset.st, 3, static_cast<int>(command.target_kind));
    BindOptionalInt(insert_preset.st, 4, command.item_id);
    BindOptionalInt(insert_preset.st, 5, command.target_mask_bits);
    BindOptionalInt(insert_preset.st, 6, command.target_single_slot);
    BindOptionalInt(insert_preset.st, 7, command.target_same_as_actor_slot);
    sqlite3_bind_int(insert_preset.st, 8, command.flags);
    sqlite3_bind_int64(insert_preset.st, 9, ToEpochMillis(command.created_at_utc));
    if (sqlite3_step(insert_preset.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    const auto action_preset_id = sqlite3_last_insert_rowid(db_);

    if (!InsertAuthoringOutboxEvent(
            db_,
            "Authoring.BattlePlanActionPresetSaved.v1",
            std::to_string(action_preset_id),
            command.correlation_id,
            command.causation_id,
            ToEpochMillis(command.created_at_utc),
            action_preset_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (action_preset_id_out) {
        *action_preset_id_out = action_preset_id;
    }
    return true;
}

bool SqliteAuthoringDb::RenameBattlePlanActionPreset(
    const RenameBattlePlanActionPresetCommand& command,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.action_preset_id <= 0 || command.name.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    Statement update_preset;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE au_battle_plan_action_preset "
            "SET name=?2, updated_at_utc=?3 "
            "WHERE action_preset_id=?1;",
            -1,
            &update_preset.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    sqlite3_bind_int64(update_preset.st, 1, command.action_preset_id);
    sqlite3_bind_text(update_preset.st, 2, command.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(update_preset.st, 3, ToEpochMillis(command.updated_at_utc));
    if (sqlite3_step(update_preset.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    if (sqlite3_changes(db_) <= 0) {
        if (error_out) *error_out = "battle plan action preset not found";
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (!InsertAuthoringOutboxEvent(
            db_,
            "Authoring.BattlePlanActionPresetRenamed.v1",
            std::to_string(command.action_preset_id),
            command.correlation_id,
            command.causation_id,
            ToEpochMillis(command.updated_at_utc),
            command.action_preset_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    return true;
}

std::optional<BattlePlanActionPresetSnapshot> SqliteAuthoringDb::GetBattlePlanActionPreset(
    std::int64_t action_preset_id) const {
    if (db_ == nullptr || action_preset_id <= 0) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT action_preset_id,name,macro,target_kind,item_id,target_mask_bits,target_single_slot,target_same_as_actor_slot,flags,created_at_utc,updated_at_utc "
            "FROM au_battle_plan_action_preset WHERE action_preset_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, action_preset_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    BattlePlanActionPresetSnapshot out{};
    out.action_preset_id = sqlite3_column_int64(st.st, 0);
    out.name = ColumnText(st.st, 1);
    out.macro = static_cast<BattlePlanActionMacro>(sqlite3_column_int(st.st, 2));
    out.target_kind = static_cast<BattlePlanTargetKind>(sqlite3_column_int(st.st, 3));
    out.item_id = ColumnIntOptional(st.st, 4);
    out.target_mask_bits = ColumnIntOptional(st.st, 5);
    out.target_single_slot = ColumnIntOptional(st.st, 6);
    out.target_same_as_actor_slot = ColumnIntOptional(st.st, 7);
    out.flags = sqlite3_column_int(st.st, 8);
    out.created_at_utc = FromEpochMillis(sqlite3_column_int64(st.st, 9));
    if (const auto updated = ColumnInt64Optional(st.st, 10); updated.has_value()) {
        out.updated_at_utc = FromEpochMillis(*updated);
    }
    return out;
}

std::vector<BattlePlanActionPresetSnapshot> SqliteAuthoringDb::ListBattlePlanActionPresets(
    int max_count) const {
    std::vector<BattlePlanActionPresetSnapshot> out;
    if (db_ == nullptr) {
        return out;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT action_preset_id FROM au_battle_plan_action_preset ORDER BY action_preset_id DESC LIMIT ?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return out;
    }
    sqlite3_bind_int(st.st, 1, std::max(1, max_count));
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        if (auto snapshot = GetBattlePlanActionPreset(sqlite3_column_int64(st.st, 0)); snapshot.has_value()) {
            out.push_back(std::move(*snapshot));
        }
    }
    return out;
}

bool SqliteAuthoringDb::SaveBattlePlanTurn(
    const SaveBattlePlanTurnCommand& command,
    std::int64_t* plan_turn_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.plan_id <= 0 || command.turn_index <= 0) {
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
            "INSERT INTO au_battle_plan_turn(plan_id,turn_index,default_predicate_group_revision_id) VALUES(?1,?2,?3) "
            "ON CONFLICT(plan_id, turn_index) DO UPDATE SET default_predicate_group_revision_id=excluded.default_predicate_group_revision_id;",
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
    if (command.default_predicate_group_revision_id.has_value()) {
        sqlite3_bind_int64(upsert_turn.st, 3,
            *command.default_predicate_group_revision_id);
    } else {
        sqlite3_bind_null(upsert_turn.st, 3);
    }
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
        if (action.action_preset_id <= 0) {
            if (error_out) *error_out = "battle plan action preset id is required";
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
        Statement insert_action;
        if (sqlite3_prepare_v2(
                db_,
                "INSERT INTO au_battle_plan_action("
                "plan_turn_id,actor_slot,action_preset_id,ordinal) "
                "VALUES(?1,?2,?3,?4);",
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
        sqlite3_bind_int64(insert_action.st, 3, action.action_preset_id);
        sqlite3_bind_int(insert_action.st, 4, action.ordinal);
        if (sqlite3_step(insert_action.st) != SQLITE_DONE) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
    }

    if (!InsertAuthoringOutboxEvent(
            db_,
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
            "SELECT plan_id,name,COALESCE(description,''),fingerprint FROM au_battle_plan WHERE plan_id=?1;",
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
    out.description = ColumnText(plan_st.st, 2);
    out.fingerprint = ColumnText(plan_st.st, 3);

    Statement turn_st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT plan_turn_id,plan_id,turn_index,default_predicate_group_revision_id FROM au_battle_plan_turn WHERE plan_id=?1 ORDER BY turn_index ASC;",
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
        turn.default_predicate_group_revision_id =
            ColumnInt64Optional(turn_st.st, 3);

        Statement action_st;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT a.plan_action_id,a.plan_turn_id,a.actor_slot,a.action_preset_id,a.ordinal,"
                "p.name,p.macro,p.target_kind,p.item_id,p.target_mask_bits,p.target_single_slot,p.target_same_as_actor_slot,p.flags,p.created_at_utc,p.updated_at_utc "
                "FROM au_battle_plan_action a "
                "JOIN au_battle_plan_action_preset p ON p.action_preset_id=a.action_preset_id "
                "WHERE a.plan_turn_id=?1 ORDER BY a.ordinal ASC, a.plan_action_id ASC;",
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
                action.action_preset_id = sqlite3_column_int64(action_st.st, 3);
                action.ordinal = sqlite3_column_int(action_st.st, 4);
                action.action_preset.action_preset_id = action.action_preset_id;
                action.action_preset.name = ColumnText(action_st.st, 5);
                action.action_preset.macro = static_cast<BattlePlanActionMacro>(sqlite3_column_int(action_st.st, 6));
                action.action_preset.target_kind = static_cast<BattlePlanTargetKind>(sqlite3_column_int(action_st.st, 7));
                action.action_preset.item_id = ColumnIntOptional(action_st.st, 8);
                action.action_preset.target_mask_bits = ColumnIntOptional(action_st.st, 9);
                action.action_preset.target_single_slot = ColumnIntOptional(action_st.st, 10);
                action.action_preset.target_same_as_actor_slot = ColumnIntOptional(action_st.st, 11);
                action.action_preset.flags = sqlite3_column_int(action_st.st, 12);
                action.action_preset.created_at_utc = FromEpochMillis(sqlite3_column_int64(action_st.st, 13));
                if (const auto updated = ColumnInt64Optional(action_st.st, 14); updated.has_value()) {
                    action.action_preset.updated_at_utc = FromEpochMillis(*updated);
                }
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

bool SqliteAuthoringDb::SaveWorkflowGraph(
    const SaveWorkflowGraphCommand& command,
    SaveWorkflowGraphResult* result_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.name.empty() || command.graph_hash.empty()) {
        if (error_out) *error_out = "workflow graph name and graph_hash are required";
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

    std::string existing_graph_name;
    if (const auto existing = LoadWorkflowGraphByHash(db_, command.graph_hash, &existing_graph_name); existing.has_value()) {
        if (existing_graph_name != command.name) {
            rollback();
            if (error_out) *error_out = "workflow graph hash already exists with a different graph name";
            return false;
        }
        if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            rollback();
            return false;
        }
        if (result_out) {
            *result_out = *existing;
        }
        return true;
    }

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
                "UPDATE au_workflow_graph SET name=?1, description=?2, hidden=COALESCE(?3, hidden) WHERE workflow_graph_id=?4;",
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
        if (command.hidden.has_value()) sqlite3_bind_int(update_graph.st, 3, *command.hidden ? 1 : 0);
        else sqlite3_bind_null(update_graph.st, 3);
        sqlite3_bind_int64(update_graph.st, 4, workflow_graph_id);
        if (sqlite3_step(update_graph.st) != SQLITE_DONE) {
            rollback();
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }
    } else {
        Statement insert_graph;
        if (sqlite3_prepare_v2(
                db_,
                "INSERT INTO au_workflow_graph(name,description,created_at_utc,hidden) "
                "VALUES(?1,?2,?3,?4);",
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
        sqlite3_bind_int(insert_graph.st, 4, command.hidden.value_or(false) ? 1 : 0);
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
            "workflow_graph_id,graph_version,graph_hash,parent_revision_id,execution_shape,expansion_kind,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7);",
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
    sqlite3_bind_text(insert_revision.st, 5, command.execution_shape.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_revision.st, 6, command.expansion_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_revision.st, 7, ToEpochMillis(command.created_at_utc));
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
            if (input.input_key.empty() || input.data_kind.empty() || input.ref_kind.empty()) {
                rollback();
                if (error_out) *error_out = "workflow graph input_key, data_kind, and ref_kind are required";
                return false;
            }
            Statement insert_input;
            if (sqlite3_prepare_v2(
                    db_,
                    "INSERT INTO au_workflow_graph_revision_node_input("
                    "workflow_graph_revision_node_id,input_key,data_kind,ref_kind,display_name,required,ordinal) "
                    "VALUES(?1,?2,?3,?4,?5,?6,?7);",
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
            sqlite3_bind_text(insert_input.st, 4, input.ref_kind.c_str(), -1, SQLITE_TRANSIENT);
            if (input.display_name.empty()) sqlite3_bind_null(insert_input.st, 5);
            else sqlite3_bind_text(insert_input.st, 5, input.display_name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(insert_input.st, 6, input.required ? 1 : 0);
            sqlite3_bind_int(insert_input.st, 7, input_ordinal);
            if (sqlite3_step(insert_input.st) != SQLITE_DONE) {
                rollback();
                if (error_out) *error_out = sqlite3_errmsg(db_);
                return false;
            }
        }

        for (int output_ordinal = 0; output_ordinal < static_cast<int>(node.possible_outputs.size()); ++output_ordinal) {
            const auto& output = node.possible_outputs[static_cast<std::size_t>(output_ordinal)];
            if (output.output_key.empty() || output.data_kind.empty() || output.ref_kind.empty()) {
                rollback();
                if (error_out) *error_out = "workflow graph output_key, data_kind, and ref_kind are required";
                return false;
            }
            Statement insert_output;
            if (sqlite3_prepare_v2(
                    db_,
                    "INSERT INTO au_workflow_graph_revision_node_output("
                    "workflow_graph_revision_node_id,output_key,data_kind,ref_kind,display_name,ordinal) "
                    "VALUES(?1,?2,?3,?4,?5,?6);",
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
            sqlite3_bind_text(insert_output.st, 4, output.ref_kind.c_str(), -1, SQLITE_TRANSIENT);
            if (output.display_name.empty()) sqlite3_bind_null(insert_output.st, 5);
            else sqlite3_bind_text(insert_output.st, 5, output.display_name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(insert_output.st, 6, output_ordinal);
            if (sqlite3_step(insert_output.st) != SQLITE_DONE) {
                rollback();
                if (error_out) *error_out = sqlite3_errmsg(db_);
                return false;
            }
        }

        for (int argument_ordinal = 0; argument_ordinal < static_cast<int>(node.arguments.size()); ++argument_ordinal) {
            const auto& argument = node.arguments[static_cast<std::size_t>(argument_ordinal)];
            if (argument.argument_key.empty() || argument.display_name.empty()
                || (argument.value_type != "integer" && argument.value_type != "text"
                    && argument.value_type != "boolean" && argument.value_type != "json"
                    && argument.value_type != "choice")
                || (argument.value_type == "choice" && argument.choices.empty())
                || (argument.value_type != "choice" && !argument.choices.empty())
                || (argument.binding_mode == SaveWorkflowGraphNodeArgumentCommand::BindingMode::Instance
                    && argument.constant_value.has_value())
                || (argument.binding_mode == SaveWorkflowGraphNodeArgumentCommand::BindingMode::Constant
                    && !argument.constant_value.has_value())) {
                rollback();
                if (error_out) *error_out = "workflow graph argument contract is invalid";
                return false;
            }
            Statement insert_argument;
            if (sqlite3_prepare_v2(db_,
                    "INSERT INTO au_workflow_graph_revision_node_argument("
                    "workflow_graph_revision_node_id,argument_key,display_name,value_type,required,default_value,minimum_integer,maximum_integer,binding_mode,constant_value,ordinal) "
                    "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11);",
                    -1, &insert_argument.st, nullptr) != SQLITE_OK) {
                rollback();
                if (error_out) *error_out = sqlite3_errmsg(db_);
                return false;
            }
            sqlite3_bind_int64(insert_argument.st, 1, workflow_graph_revision_node_id);
            sqlite3_bind_text(insert_argument.st, 2, argument.argument_key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_argument.st, 3, argument.display_name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_argument.st, 4, argument.value_type.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(insert_argument.st, 5, argument.required ? 1 : 0);
            if (argument.default_value) sqlite3_bind_text(insert_argument.st, 6, argument.default_value->c_str(), -1, SQLITE_TRANSIENT); else sqlite3_bind_null(insert_argument.st, 6);
            if (argument.minimum_integer) sqlite3_bind_int64(insert_argument.st, 7, *argument.minimum_integer); else sqlite3_bind_null(insert_argument.st, 7);
            if (argument.maximum_integer) sqlite3_bind_int64(insert_argument.st, 8, static_cast<sqlite3_int64>(*argument.maximum_integer)); else sqlite3_bind_null(insert_argument.st, 8);
            const char* binding_mode = argument.binding_mode ==
                    SaveWorkflowGraphNodeArgumentCommand::BindingMode::Constant
                ? "CONSTANT" : "INSTANCE";
            sqlite3_bind_text(insert_argument.st, 9, binding_mode, -1, SQLITE_STATIC);
            if (argument.constant_value) sqlite3_bind_text(insert_argument.st, 10,
                argument.constant_value->c_str(), -1, SQLITE_TRANSIENT);
            else sqlite3_bind_null(insert_argument.st, 10);
            sqlite3_bind_int(insert_argument.st, 11, argument_ordinal);
            if (sqlite3_step(insert_argument.st) != SQLITE_DONE) {
                rollback();
                if (error_out) *error_out = sqlite3_errmsg(db_);
                return false;
            }
            const auto workflow_graph_revision_node_argument_id = sqlite3_last_insert_rowid(db_);
            std::unordered_set<std::string> choice_values;
            for (int choice_ordinal = 0;
                 choice_ordinal < static_cast<int>(argument.choices.size());
                 ++choice_ordinal) {
                const auto& choice = argument.choices[static_cast<std::size_t>(choice_ordinal)];
                if (choice.value.empty() || choice.display_name.empty()
                    || !choice_values.emplace(choice.value).second) {
                    rollback();
                    if (error_out) *error_out = "workflow graph choice contract is invalid";
                    return false;
                }
                Statement insert_choice;
                if (sqlite3_prepare_v2(
                        db_,
                        "INSERT INTO au_workflow_graph_revision_node_argument_choice("
                        "workflow_graph_revision_node_argument_id,choice_value,display_name,ordinal) "
                        "VALUES(?1,?2,?3,?4);",
                        -1,
                        &insert_choice.st,
                        nullptr) != SQLITE_OK) {
                    rollback();
                    if (error_out) *error_out = sqlite3_errmsg(db_);
                    return false;
                }
                sqlite3_bind_int64(insert_choice.st, 1, workflow_graph_revision_node_argument_id);
                sqlite3_bind_text(insert_choice.st, 2, choice.value.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insert_choice.st, 3, choice.display_name.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(insert_choice.st, 4, choice_ordinal);
                if (sqlite3_step(insert_choice.st) != SQLITE_DONE) {
                    rollback();
                    if (error_out) *error_out = sqlite3_errmsg(db_);
                    return false;
                }
            }
        }

        for (int constraint_ordinal = 0; constraint_ordinal < static_cast<int>(node.argument_constraints.size()); ++constraint_ordinal) {
            const auto& constraint = node.argument_constraints[static_cast<std::size_t>(constraint_ordinal)];
            if (constraint.lesser_or_equal_key.empty() || constraint.greater_or_equal_key.empty() || constraint.message.empty()) {
                rollback();
                if (error_out) *error_out = "workflow graph argument constraint is invalid";
                return false;
            }
            Statement insert_constraint;
            if (sqlite3_prepare_v2(db_,
                    "INSERT INTO au_workflow_graph_revision_node_argument_constraint("
                    "workflow_graph_revision_node_id,lesser_or_equal_key,greater_or_equal_key,message,ordinal) "
                    "VALUES(?1,?2,?3,?4,?5);",
                    -1, &insert_constraint.st, nullptr) != SQLITE_OK) {
                rollback();
                if (error_out) *error_out = sqlite3_errmsg(db_);
                return false;
            }
            sqlite3_bind_int64(insert_constraint.st, 1, workflow_graph_revision_node_id);
            sqlite3_bind_text(insert_constraint.st, 2, constraint.lesser_or_equal_key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_constraint.st, 3, constraint.greater_or_equal_key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert_constraint.st, 4, constraint.message.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(insert_constraint.st, 5, constraint_ordinal);
            if (sqlite3_step(insert_constraint.st) != SQLITE_DONE) {
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
        const bool data_edge = edge.edge_kind == "DATA";
        const bool control_edge = edge.edge_kind == "CONTROL";
        if ((!data_edge && !control_edge)
            || (data_edge && (edge.output_key.empty() || edge.input_key.empty()))
            || (control_edge && (!edge.output_key.empty() || !edge.input_key.empty()))
            || from_it == node_id_by_key.end() || to_it == node_id_by_key.end()) {
            rollback();
            if (error_out) *error_out = "workflow graph edge references unknown node or empty port";
            return false;
        }
        if ((edge.guard_kind.has_value()
                && *edge.guard_kind != kWorkflowOutputPresentGuard)
            || (edge.guard_kind.has_value()
                && edge.guard_value.has_value())
            || (!edge.guard_kind.has_value()
                && edge.guard_value.has_value())) {
            rollback();
            if (error_out) {
                *error_out =
                    "workflow graph edge guard must be output_present with no value";
            }
            return false;
        }

        Statement insert_edge;
        if (sqlite3_prepare_v2(
                db_,
                "INSERT INTO au_workflow_graph_revision_edge("
                "workflow_graph_revision_id,from_revision_node_id,output_key,to_revision_node_id,input_key,edge_kind,guard_kind,guard_value,ordinal) "
                "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9);",
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
        if (data_edge) sqlite3_bind_text(insert_edge.st, 3, edge.output_key.c_str(), -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(insert_edge.st, 3);
        sqlite3_bind_int64(insert_edge.st, 4, to_it->second);
        if (data_edge) sqlite3_bind_text(insert_edge.st, 5, edge.input_key.c_str(), -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(insert_edge.st, 5);
        sqlite3_bind_text(insert_edge.st, 6, edge.edge_kind.c_str(), -1, SQLITE_TRANSIENT);
        if (edge.guard_kind.has_value()) sqlite3_bind_text(insert_edge.st, 7, edge.guard_kind->c_str(), -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(insert_edge.st, 7);
        if (edge.guard_value.has_value()) sqlite3_bind_text(insert_edge.st, 8, edge.guard_value->c_str(), -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(insert_edge.st, 8);
        sqlite3_bind_int(insert_edge.st, 9, edge_ordinal);
        if (sqlite3_step(insert_edge.st) != SQLITE_DONE) {
            const std::string sqlite_error = sqlite3_errmsg(db_);
            rollback();
            if (error_out) *error_out = "workflow graph edge insert failed: " + sqlite_error;
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

bool SqliteAuthoringDb::SetWorkflowGraphHidden(
    std::int64_t workflow_graph_id,
    bool hidden,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (workflow_graph_id <= 0) {
        if (error_out) *error_out = "workflow_graph_id must be > 0";
        return false;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE au_workflow_graph SET hidden=?1 WHERE workflow_graph_id=?2;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int(st.st, 1, hidden ? 1 : 0);
    sqlite3_bind_int64(st.st, 2, workflow_graph_id);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (sqlite3_changes(db_) == 0) {
        if (error_out) *error_out = "workflow graph not found";
        return false;
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
            "COALESCE(g.hidden,0),r.graph_version,r.graph_hash,r.execution_shape,r.expansion_kind "
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
    out.hidden = sqlite3_column_int(graph_st.st, 5) != 0;
    out.graph_version = sqlite3_column_int(graph_st.st, 6);
    out.graph_hash = ColumnText(graph_st.st, 7);
    out.execution_shape = ColumnText(graph_st.st, 8);
    out.expansion_kind = ColumnText(graph_st.st, 9);
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
                "SELECT input_key,data_kind,ref_kind,COALESCE(display_name,''),required "
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
                .ref_kind = ColumnText(input_st.st, 2),
                .display_name = ColumnText(input_st.st, 3),
                .required = sqlite3_column_int(input_st.st, 4) != 0,
            });
        }

        Statement output_st;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT output_key,data_kind,ref_kind,COALESCE(display_name,'') "
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
                .ref_kind = ColumnText(output_st.st, 2),
                .display_name = ColumnText(output_st.st, 3),
            });
        }

        Statement argument_st;
        if (sqlite3_prepare_v2(db_,
                "SELECT workflow_graph_revision_node_argument_id,argument_key,display_name,value_type,required,default_value,minimum_integer,maximum_integer,binding_mode,constant_value "
                "FROM au_workflow_graph_revision_node_argument WHERE workflow_graph_revision_node_id=?1 ORDER BY ordinal ASC, workflow_graph_revision_node_argument_id ASC;",
                -1, &argument_st.st, nullptr) != SQLITE_OK) return std::nullopt;
        sqlite3_bind_int64(argument_st.st, 1, node.workflow_graph_revision_node_id);
        while (sqlite3_step(argument_st.st) == SQLITE_ROW) {
            const auto argument_id = sqlite3_column_int64(argument_st.st, 0);
            const auto maximum = ColumnInt64Optional(argument_st.st, 7);
            WorkflowGraphNodeArgumentSnapshot argument{
                .argument_key = ColumnText(argument_st.st, 1),
                .display_name = ColumnText(argument_st.st, 2),
                .value_type = ColumnText(argument_st.st, 3),
                .required = sqlite3_column_int(argument_st.st, 4) != 0,
                .default_value = ColumnTextOptional(argument_st.st, 5),
                .minimum_integer = ColumnInt64Optional(argument_st.st, 6),
                .maximum_integer = maximum ? std::optional<std::uint64_t>(static_cast<std::uint64_t>(*maximum)) : std::nullopt,
                .binding_mode = ColumnText(argument_st.st, 8) == "CONSTANT"
                    ? SaveWorkflowGraphNodeArgumentCommand::BindingMode::Constant
                    : SaveWorkflowGraphNodeArgumentCommand::BindingMode::Instance,
                .constant_value = ColumnTextOptional(argument_st.st, 9),
            };
            Statement choice_st;
            if (sqlite3_prepare_v2(db_,
                    "SELECT choice_value,display_name FROM au_workflow_graph_revision_node_argument_choice "
                    "WHERE workflow_graph_revision_node_argument_id=?1 ORDER BY ordinal ASC,workflow_graph_revision_node_argument_choice_id ASC;",
                    -1, &choice_st.st, nullptr) != SQLITE_OK) return std::nullopt;
            sqlite3_bind_int64(choice_st.st, 1, argument_id);
            while (sqlite3_step(choice_st.st) == SQLITE_ROW) {
                argument.choices.push_back(WorkflowGraphNodeArgumentSnapshot::Choice{
                    .value = ColumnText(choice_st.st, 0),
                    .display_name = ColumnText(choice_st.st, 1),
                });
            }
            node.arguments.push_back(std::move(argument));
        }
        Statement constraint_st;
        if (sqlite3_prepare_v2(db_,
                "SELECT lesser_or_equal_key,greater_or_equal_key,message FROM au_workflow_graph_revision_node_argument_constraint "
                "WHERE workflow_graph_revision_node_id=?1 ORDER BY ordinal ASC, workflow_graph_revision_node_argument_constraint_id ASC;",
                -1, &constraint_st.st, nullptr) != SQLITE_OK) return std::nullopt;
        sqlite3_bind_int64(constraint_st.st, 1, node.workflow_graph_revision_node_id);
        while (sqlite3_step(constraint_st.st) == SQLITE_ROW) {
            node.argument_constraints.push_back(WorkflowGraphNodeArgumentConstraintSnapshot{
                .lesser_or_equal_key = ColumnText(constraint_st.st, 0),
                .greater_or_equal_key = ColumnText(constraint_st.st, 1),
                .message = ColumnText(constraint_st.st, 2),
            });
        }

        out.nodes.push_back(std::move(node));
    }

    Statement edge_st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT e.workflow_graph_revision_edge_id,fn.node_key,COALESCE(e.output_key,''),tn.node_key,COALESCE(e.input_key,''),e.edge_kind,e.guard_kind,e.guard_value "
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
            .edge_kind = ColumnText(edge_st.st, 5),
            .guard_kind = ColumnTextOptional(edge_st.st, 6),
            .guard_value = ColumnTextOptional(edge_st.st, 7),
        });
    }

    return out;
}

std::optional<WorkflowGraphSnapshot> SqliteAuthoringDb::GetWorkflowGraphByName(
    const std::string& name) const {
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT workflow_graph_id FROM au_workflow_graph WHERE name=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_text(st.st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    return GetWorkflowGraph(sqlite3_column_int64(st.st, 0));
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
            "COALESCE(g.hidden,0),r.graph_version,r.graph_hash,r.execution_shape,r.expansion_kind "
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
    out.hidden = sqlite3_column_int(graph_st.st, 5) != 0;
    out.graph_version = sqlite3_column_int(graph_st.st, 6);
    out.graph_hash = ColumnText(graph_st.st, 7);
    out.execution_shape = ColumnText(graph_st.st, 8);
    out.expansion_kind = ColumnText(graph_st.st, 9);
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
                "SELECT input_key,data_kind,ref_kind,COALESCE(display_name,''),required "
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
                .ref_kind = ColumnText(input_st.st, 2),
                .display_name = ColumnText(input_st.st, 3),
                .required = sqlite3_column_int(input_st.st, 4) != 0,
            });
        }

        Statement output_st;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT output_key,data_kind,ref_kind,COALESCE(display_name,'') "
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
                .ref_kind = ColumnText(output_st.st, 2),
                .display_name = ColumnText(output_st.st, 3),
            });
        }

        Statement argument_st;
        if (sqlite3_prepare_v2(db_,
                "SELECT workflow_graph_revision_node_argument_id,argument_key,display_name,value_type,required,default_value,minimum_integer,maximum_integer,binding_mode,constant_value "
                "FROM au_workflow_graph_revision_node_argument WHERE workflow_graph_revision_node_id=?1 ORDER BY ordinal ASC, workflow_graph_revision_node_argument_id ASC;",
                -1, &argument_st.st, nullptr) != SQLITE_OK) return std::nullopt;
        sqlite3_bind_int64(argument_st.st, 1, node.workflow_graph_revision_node_id);
        while (sqlite3_step(argument_st.st) == SQLITE_ROW) {
            const auto argument_id = sqlite3_column_int64(argument_st.st, 0);
            const auto maximum = ColumnInt64Optional(argument_st.st, 7);
            WorkflowGraphNodeArgumentSnapshot argument{
                .argument_key = ColumnText(argument_st.st, 1),
                .display_name = ColumnText(argument_st.st, 2),
                .value_type = ColumnText(argument_st.st, 3),
                .required = sqlite3_column_int(argument_st.st, 4) != 0,
                .default_value = ColumnTextOptional(argument_st.st, 5),
                .minimum_integer = ColumnInt64Optional(argument_st.st, 6),
                .maximum_integer = maximum ? std::optional<std::uint64_t>(static_cast<std::uint64_t>(*maximum)) : std::nullopt,
                .binding_mode = ColumnText(argument_st.st, 8) == "CONSTANT"
                    ? SaveWorkflowGraphNodeArgumentCommand::BindingMode::Constant
                    : SaveWorkflowGraphNodeArgumentCommand::BindingMode::Instance,
                .constant_value = ColumnTextOptional(argument_st.st, 9),
            };
            Statement choice_st;
            if (sqlite3_prepare_v2(db_,
                    "SELECT choice_value,display_name FROM au_workflow_graph_revision_node_argument_choice "
                    "WHERE workflow_graph_revision_node_argument_id=?1 ORDER BY ordinal ASC,workflow_graph_revision_node_argument_choice_id ASC;",
                    -1, &choice_st.st, nullptr) != SQLITE_OK) return std::nullopt;
            sqlite3_bind_int64(choice_st.st, 1, argument_id);
            while (sqlite3_step(choice_st.st) == SQLITE_ROW) {
                argument.choices.push_back(WorkflowGraphNodeArgumentSnapshot::Choice{
                    .value = ColumnText(choice_st.st, 0),
                    .display_name = ColumnText(choice_st.st, 1),
                });
            }
            node.arguments.push_back(std::move(argument));
        }
        Statement constraint_st;
        if (sqlite3_prepare_v2(db_,
                "SELECT lesser_or_equal_key,greater_or_equal_key,message FROM au_workflow_graph_revision_node_argument_constraint "
                "WHERE workflow_graph_revision_node_id=?1 ORDER BY ordinal ASC, workflow_graph_revision_node_argument_constraint_id ASC;",
                -1, &constraint_st.st, nullptr) != SQLITE_OK) return std::nullopt;
        sqlite3_bind_int64(constraint_st.st, 1, node.workflow_graph_revision_node_id);
        while (sqlite3_step(constraint_st.st) == SQLITE_ROW) {
            node.argument_constraints.push_back(WorkflowGraphNodeArgumentConstraintSnapshot{
                .lesser_or_equal_key = ColumnText(constraint_st.st, 0),
                .greater_or_equal_key = ColumnText(constraint_st.st, 1),
                .message = ColumnText(constraint_st.st, 2),
            });
        }

        out.nodes.push_back(std::move(node));
    }

    Statement edge_st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT e.workflow_graph_revision_edge_id,fn.node_key,COALESCE(e.output_key,''),tn.node_key,COALESCE(e.input_key,''),e.edge_kind,e.guard_kind,e.guard_value "
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
            .edge_kind = ColumnText(edge_st.st, 5),
            .guard_kind = ColumnTextOptional(edge_st.st, 6),
            .guard_value = ColumnTextOptional(edge_st.st, 7),
        });
    }

    return out;
}

std::vector<WorkflowGraphSnapshot> SqliteAuthoringDb::ListWorkflowGraphs(
    int max_count,
    bool include_hidden) const {
    std::vector<WorkflowGraphSnapshot> out;
    if (db_ == nullptr) {
        return out;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT workflow_graph_id FROM au_workflow_graph "
            "WHERE (?1<>0 OR COALESCE(hidden,0)=0) "
            "ORDER BY workflow_graph_id DESC LIMIT ?2;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return out;
    }
    sqlite3_bind_int(st.st, 1, include_hidden ? 1 : 0);
    sqlite3_bind_int(st.st, 2, std::max(1, max_count));
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
        if (event_type == "Authoring.SeedProbeSpecSaved.v1") {
            effective_ref_kind = "seed_probe_spec";
        }
        else if (event_type == "Authoring.WorkflowGraphSaved.v1") {
            effective_ref_kind = "workflow_graph";
        }
        else if (event_type == "Authoring.BattlePlanActionPresetSaved.v1"
            || event_type == "Authoring.BattlePlanActionPresetRenamed.v1") {
            effective_ref_kind = "battle_plan_action_preset";
        }
        else return std::nullopt;
    }

    AuthoringPayloadRecord payload{};
    if (effective_ref_kind == "seed_probe_spec") {
        if (!TryReadSeedProbeSpecPayload(db_, payload_ref_id, &payload)) {
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

    if (effective_ref_kind == "battle_plan_action_preset") {
        if (!TryReadBattlePlanActionPresetPayload(db_, payload_ref_id, &payload)) {
            return std::nullopt;
        }
        return payload;
    }

    return std::nullopt;
}

} // namespace savor::db
