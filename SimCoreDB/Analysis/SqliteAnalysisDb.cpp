#include "SqliteAnalysisDb.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>

#include "../Common/Events/EventPayloadDispatch.h"
#include "../Common/Events/EventPayloadValidation.h"

namespace simcore::db::analysis {

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

std::optional<types::UtcTimePoint> ColumnTimeOptional(sqlite3_stmt* st, int index) {
    if (sqlite3_column_type(st, index) == SQLITE_NULL) {
        return std::nullopt;
    }
    return types::UtcTimePoint(std::chrono::milliseconds(sqlite3_column_int64(st, index)));
}

types::UtcTimePoint ColumnTime(sqlite3_stmt* st, int index) {
    return types::UtcTimePoint(std::chrono::milliseconds(sqlite3_column_int64(st, index)));
}

std::string ColumnText(sqlite3_stmt* st, int index) {
    const auto* text = sqlite3_column_text(st, index);
    if (text == nullptr) {
        return "";
    }
    return std::string(
        reinterpret_cast<const char*>(text),
        static_cast<std::size_t>(sqlite3_column_bytes(st, index)));
}

std::optional<std::string> ColumnTextOptional(sqlite3_stmt* st, int index) {
    if (sqlite3_column_type(st, index) == SQLITE_NULL) {
        return std::nullopt;
    }
    return ColumnText(st, index);
}

std::string ColumnBlob(sqlite3_stmt* st, int index) {
    const auto* blob = sqlite3_column_blob(st, index);
    if (blob == nullptr) {
        return "";
    }
    return std::string(
        static_cast<const char*>(blob),
        static_cast<std::size_t>(sqlite3_column_bytes(st, index)));
}



enum class OutboxMode {
    None,
    AnalysisSpine,
    SplitSeedProbeBattle,
};

bool TableExists(sqlite3* db, std::string_view table_name) {
    if (db == nullptr) {
        return false;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1 LIMIT 1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(st.st, 1, table_name.data(), static_cast<int>(table_name.size()), SQLITE_TRANSIENT);
    return sqlite3_step(st.st) == SQLITE_ROW;
}

OutboxMode ResolveOutboxMode(sqlite3* db) {
    if (TableExists(db, "asp_outbox_message")) {
        return OutboxMode::AnalysisSpine;
    }

    const bool has_sp = TableExists(db, "sp_outbox_message");
    const bool has_ab = TableExists(db, "ab_outbox_message");
    if (has_sp || has_ab) {
        return OutboxMode::SplitSeedProbeBattle;
    }

    return OutboxMode::None;
}

bool InsertSeedProbeOutboxEvent(
    sqlite3* db,
    std::string_view event_id,
    std::string_view event_type,
    std::string_view aggregate_kind,
    std::string_view aggregate_id,
    std::string_view correlation_id,
    std::string_view causation_id,
    std::int64_t occurred_at_utc,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id,
    std::string* error_out) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "INSERT INTO sp_outbox_message("
            "event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id) "
            "VALUES(?1,?2,1,'AnalysisSeedProbe',?3,?4,?5,?6,?7,?8,?9);",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) {
            *error_out = sqlite3_errmsg(db);
        }
        return false;
    }

    sqlite3_bind_text(st.st, 1, event_id.data(), static_cast<int>(event_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 2, event_type.data(), static_cast<int>(event_type.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 3, aggregate_kind.data(), static_cast<int>(aggregate_kind.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 4, aggregate_id.data(), static_cast<int>(aggregate_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 5, correlation_id.data(), static_cast<int>(correlation_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 6, causation_id.data(), static_cast<int>(causation_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 7, occurred_at_utc);
    sqlite3_bind_text(st.st, 8, payload_ref_kind.data(), static_cast<int>(payload_ref_kind.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 9, payload_ref_id);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db);
        }
        return false;
    }
    return true;
}

bool InsertBattleOutboxEvent(
    sqlite3* db,
    std::string_view event_id,
    std::string_view event_type,
    std::string_view aggregate_kind,
    std::string_view aggregate_id,
    std::string_view correlation_id,
    std::string_view causation_id,
    std::int64_t occurred_at_utc,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id,
    std::string* error_out) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "INSERT INTO ab_outbox_message("
            "event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id) "
            "VALUES(?1,?2,1,'AnalysisBattle',?3,?4,?5,?6,?7,?8,?9);",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) {
            *error_out = sqlite3_errmsg(db);
        }
        return false;
    }

    sqlite3_bind_text(st.st, 1, event_id.data(), static_cast<int>(event_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 2, event_type.data(), static_cast<int>(event_type.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 3, aggregate_kind.data(), static_cast<int>(aggregate_kind.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 4, aggregate_id.data(), static_cast<int>(aggregate_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 5, correlation_id.data(), static_cast<int>(correlation_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st.st, 6, causation_id.data(), static_cast<int>(causation_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 7, occurred_at_utc);
    sqlite3_bind_text(st.st, 8, payload_ref_kind.data(), static_cast<int>(payload_ref_kind.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.st, 9, payload_ref_id);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db);
        }
        return false;
    }
    return true;
}

std::optional<std::int64_t> ProbeRunIdForResult(sqlite3* db, std::int64_t probe_result_id) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "SELECT probe_run_id FROM sp_probe_result WHERE probe_result_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, probe_result_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    return sqlite3_column_int64(st.st, 0);
}

std::optional<std::int64_t> BattleSetIdForWave(sqlite3* db, std::int64_t wave_id) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "SELECT battle_set_id FROM ab_turn_wave WHERE wave_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, wave_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    return sqlite3_column_int64(st.st, 0);
}

std::optional<std::int64_t> BattleSetIdForSelectionPool(sqlite3* db, std::int64_t selection_pool_id) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "SELECT battle_set_id FROM ab_selection_pool WHERE selection_pool_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, selection_pool_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    return sqlite3_column_int64(st.st, 0);
}

std::optional<std::int64_t> BattleSetIdForTurnJob(sqlite3* db, std::int64_t turn_job_id) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "SELECT w.battle_set_id "
            "FROM ab_turn_job j "
            "JOIN ab_turn_wave w ON w.wave_id=j.wave_id "
            "WHERE j.turn_job_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, turn_job_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    return sqlite3_column_int64(st.st, 0);
}

std::optional<std::int64_t> TerminalFollowupIdForTurnJob(sqlite3* db, std::int64_t turn_job_id) {
    Statement st;
    if (sqlite3_prepare_v2(
            db,
            "SELECT terminal_followup_id FROM ab_terminal_followup WHERE turn_job_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, turn_job_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    return sqlite3_column_int64(st.st, 0);
}

events::EventEnvelope ReadEnvelope(sqlite3_stmt* st, int column_offset = 0) {
    events::EventEnvelope envelope{};

    const auto* event_id = sqlite3_column_text(st, column_offset + 0);
    const auto* event_type = sqlite3_column_text(st, column_offset + 1);
    const auto event_version = sqlite3_column_int(st, column_offset + 2);
    const auto* context_name = sqlite3_column_text(st, column_offset + 3);
    const auto* aggregate_kind = sqlite3_column_text(st, column_offset + 4);
    const auto* aggregate_id = sqlite3_column_text(st, column_offset + 5);
    const auto* correlation_id = sqlite3_column_text(st, column_offset + 6);
    const auto* causation_id = sqlite3_column_text(st, column_offset + 7);
    const auto occurred_at_utc = sqlite3_column_int64(st, column_offset + 8);
    const auto* payload_ref_kind = sqlite3_column_text(st, column_offset + 9);
    const auto payload_ref_id = sqlite3_column_int64(st, column_offset + 10);

    envelope.event_id = event_id == nullptr ? "" : reinterpret_cast<const char*>(event_id);
    envelope.event_type = event_type == nullptr ? "" : reinterpret_cast<const char*>(event_type);
    envelope.event_version = event_version;
    envelope.context_name = context_name == nullptr ? "" : reinterpret_cast<const char*>(context_name);
    envelope.aggregate_kind = aggregate_kind == nullptr ? "" : reinterpret_cast<const char*>(aggregate_kind);
    envelope.aggregate_id = aggregate_id == nullptr ? "" : reinterpret_cast<const char*>(aggregate_id);
    envelope.correlation_id = correlation_id == nullptr ? "" : reinterpret_cast<const char*>(correlation_id);
    envelope.causation_id = causation_id == nullptr ? "" : reinterpret_cast<const char*>(causation_id);
    envelope.occurred_at_utc = types::UtcTimePoint(std::chrono::milliseconds(occurred_at_utc));
    envelope.payload_ref_kind = payload_ref_kind == nullptr ? "" : reinterpret_cast<const char*>(payload_ref_kind);
    envelope.payload_ref_id = payload_ref_id;

    return envelope;
}

std::optional<events::AnalysisSeedProbePayloadView> ResolveSeedProbeByKind(
    const SqliteSeedProbePayloadRowResolver& resolver,
    int event_version,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) {
    if (event_version != 1 || payload_ref_id <= 0) {
        return std::nullopt;
    }

    events::AnalysisSeedProbePayloadView record{};

    if (payload_ref_kind == "probe_set") {
        const auto view = resolver.ResolveSeedProbeSetCreated(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.probe_set_id = view->probe_set_id;
        return record;
    }
    if (payload_ref_kind == "probe_run") {
        const auto view = resolver.ResolveSeedProbeRunRequested(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.probe_set_id = view->probe_set_id;
        record.probe_run_id = view->probe_run_id;
        return record;
    }
    if (payload_ref_kind == "neutral_seed") {
        const auto view = resolver.ResolveSeedProbeNeutralSeedRecorded(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.probe_run_id = view->probe_run_id;
        record.probe_result_id = view->probe_result_id;
        return record;
    }
    if (payload_ref_kind == "grid_seed") {
        const auto view = resolver.ResolveSeedProbeGridSeedRecorded(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.probe_run_id = view->probe_run_id;
        record.probe_result_id = view->probe_result_id;
        return record;
    }
    if (payload_ref_kind == "unique_seed") {
        const auto view = resolver.ResolveSeedProbeUniqueSeedRecorded(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.probe_run_id = view->probe_run_id;
        record.probe_result_id = view->probe_result_id;
        return record;
    }
    if (payload_ref_kind == "encounter_projection") {
        const auto view = resolver.ResolveSeedProbeEncounterProjectionRecorded(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.probe_run_id = view->probe_run_id;
        return record;
    }
    if (payload_ref_kind == "probe_result") {
        const auto view = resolver.ResolveSeedProbeRunCompleted(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.probe_run_id = view->probe_run_id;
        record.probe_result_id = view->probe_result_id;
        return record;
    }

    return std::nullopt;
}

std::optional<events::AnalysisBattlePayloadView> ResolveBattleByKind(
    const SqliteBattlePayloadRowResolver& resolver,
    int event_version,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) {
    if (event_version != 1 || payload_ref_id <= 0) {
        return std::nullopt;
    }

    events::AnalysisBattlePayloadView record{};

    if (payload_ref_kind == "battle_set") {
        const auto view = resolver.ResolveBattleSetCreated(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.battle_set_id = view->battle_set_id;
        return record;
    }
    if (payload_ref_kind == "seed_candidate") {
        const auto view = resolver.ResolveBattleSeedCandidateAdded(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.battle_set_id = view->battle_set_id;
        return record;
    }
    if (payload_ref_kind == "turn_wave") {
        const auto view = resolver.ResolveBattleTurnWaveCreated(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.battle_set_id = view->battle_set_id;
        record.wave_id = view->wave_id;
        return record;
    }
    if (payload_ref_kind == "turn_job") {
        const auto view = resolver.ResolveBattleTurnJobRecorded(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.battle_set_id = view->battle_set_id;
        record.wave_id = view->wave_id;
        record.turn_job_id = view->turn_job_id;
        return record;
    }
    if (payload_ref_kind == "selection_pool") {
        const auto view = resolver.ResolveBattleSelectionPoolCreated(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.battle_set_id = view->battle_set_id;
        return record;
    }
    if (payload_ref_kind == "selection_decision") {
        const auto view = resolver.ResolveBattleSelectionDecisionRecorded(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.battle_set_id = view->battle_set_id;
        record.turn_job_id = view->turn_job_id;
        return record;
    }
    if (payload_ref_kind == "terminal_followup") {
        const auto view = resolver.ResolveBattleTerminalFollowupUpdated(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.battle_set_id = view->battle_set_id;
        record.turn_job_id = view->turn_job_id;
        return record;
    }

    return std::nullopt;
}

std::optional<events::AnalysisSpinePayloadView> ResolveSpineByKind(
    const SqliteAnalysisSpinePayloadRowResolver& resolver,
    int event_version,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) {
    if (event_version != 1 || payload_ref_id <= 0) {
        return std::nullopt;
    }

    events::AnalysisSpinePayloadView record{};

    if (payload_ref_kind == "run") {
        const auto view = resolver.ResolveSpineRunCreated(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.run_id = view->run_id;
        return record;
    }
    if (payload_ref_kind == "state_ref") {
        const auto view = resolver.ResolveSpineStateRefRegistered(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.run_id = view->run_id;
        record.state_ref_id = view->state_ref_id;
        return record;
    }
    if (payload_ref_kind == "lineage_edge") {
        const auto view = resolver.ResolveSpineLineageEdgeAdded(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.run_id = view->child_run_id;
        record.lineage_edge_id = view->lineage_edge_id;
        return record;
    }
    if (payload_ref_kind == "artifact_ref") {
        const auto view = resolver.ResolveSpineArtifactLinked(payload_ref_kind, payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }
        record.run_id = view->run_id;
        record.artifact_ref_id = view->artifact_ref_id;
        return record;
    }

    return std::nullopt;
}

} // namespace

SqliteAnalysisDb::SqliteAnalysisDb(sqlite3* db)
    : db_(db)
    , seed_probe_row_resolver_(db_)
    , battle_row_resolver_(db_)
    , spine_row_resolver_(db_) {
}

std::optional<std::int64_t> SqliteAnalysisDb::LookupSeedProbeRunSavestateId(std::int64_t probe_run_id) const {
    if (db_ == nullptr || probe_run_id <= 0) {
        return std::nullopt;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT entry_savestate_id FROM sp_probe_run WHERE probe_run_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(st.st, 1, probe_run_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    return sqlite3_column_int64(st.st, 0);
}

std::optional<std::int64_t> SqliteAnalysisDb::LookupSeedProbeResultId(std::int64_t probe_run_id) const {
    if (db_ == nullptr || probe_run_id <= 0) {
        return std::nullopt;
    }
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT probe_result_id FROM sp_probe_result WHERE probe_run_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, probe_run_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    return sqlite3_column_int64(st.st, 0);
}

std::optional<std::int64_t> SqliteAnalysisDb::LookupSeedProbeNeutralSeed(std::int64_t probe_run_id) const {
    if (db_ == nullptr || probe_run_id <= 0) {
        return std::nullopt;
    }
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT neutral_seed_value FROM sp_probe_result WHERE probe_run_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, probe_run_id);
    if (sqlite3_step(st.st) != SQLITE_ROW || sqlite3_column_type(st.st, 0) == SQLITE_NULL) {
        return std::nullopt;
    }
    return sqlite3_column_int64(st.st, 0);
}

std::vector<SeedProbeGridSeedRow> SqliteAnalysisDb::ListSeedProbeGridSeeds(std::int64_t probe_run_id) const {
    std::vector<SeedProbeGridSeedRow> rows;
    if (db_ == nullptr || probe_run_id <= 0) {
        return rows;
    }
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT g.grid_seed_id,g.probe_result_id,g.source_family,a.x,a.y,g.seed_value,g.seed_delta "
            "FROM sp_grid_seed g "
            "JOIN sp_axis_xy a ON a.axis_xy_id=g.axis_xy_id "
            "JOIN sp_probe_result r ON r.probe_result_id=g.probe_result_id "
            "WHERE r.probe_run_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_int64(st.st, 1, probe_run_id);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        rows.push_back(SeedProbeGridSeedRow{
            .grid_seed_id = sqlite3_column_int64(st.st, 0),
            .probe_result_id = sqlite3_column_int64(st.st, 1),
            .source_family = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 2)),
            .axis_x = sqlite3_column_int(st.st, 3),
            .axis_y = sqlite3_column_int(st.st, 4),
            .seed_value = sqlite3_column_int64(st.st, 5),
            .seed_delta = sqlite3_column_int64(st.st, 6),
            });
    }
    return rows;
}

std::vector<SeedProbeUniqueSeedRow> SqliteAnalysisDb::ListSeedProbeUniqueSeeds(std::int64_t probe_run_id) const {
    std::vector<SeedProbeUniqueSeedRow> rows;
    if (db_ == nullptr || probe_run_id <= 0) {
        return rows;
    }
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT u.unique_seed_id,u.probe_result_id,u.seed_value,u.seed_delta,"
            "m.x,m.y,c.x,c.y,t.x,t.y "
            "FROM sp_unique_seed u "
            "JOIN sp_probe_result r ON r.probe_result_id=u.probe_result_id "
            "JOIN sp_input_frame f ON f.input_frame_id=u.input_frame_id "
            "JOIN sp_axis_xy m ON m.axis_xy_id=f.main_axis_xy_id "
            "JOIN sp_axis_xy c ON c.axis_xy_id=f.cstick_axis_xy_id "
            "JOIN sp_axis_xy t ON t.axis_xy_id=f.trigger_axis_xy_id "
            "WHERE r.probe_run_id=?1 "
            "ORDER BY u.seed_value ASC, u.unique_seed_id ASC;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_int64(st.st, 1, probe_run_id);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        rows.push_back(SeedProbeUniqueSeedRow{
            .unique_seed_id = sqlite3_column_int64(st.st, 0),
            .probe_result_id = sqlite3_column_int64(st.st, 1),
            .seed_value = sqlite3_column_int64(st.st, 2),
            .seed_delta = sqlite3_column_int64(st.st, 3),
            .main_x = sqlite3_column_int(st.st, 4),
            .main_y = sqlite3_column_int(st.st, 5),
            .cstick_x = sqlite3_column_int(st.st, 6),
            .cstick_y = sqlite3_column_int(st.st, 7),
            .trigger_x = sqlite3_column_int(st.st, 8),
            .trigger_y = sqlite3_column_int(st.st, 9),
            });
    }
    return rows;
}

std::optional<SeedProbeUniqueSeedRow> SqliteAnalysisDb::GetSeedProbeUniqueSeed(std::int64_t unique_seed_id) const {
    if (db_ == nullptr || unique_seed_id <= 0) {
        return std::nullopt;
    }
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT u.unique_seed_id,u.probe_result_id,u.seed_value,u.seed_delta,"
            "m.x,m.y,c.x,c.y,t.x,t.y "
            "FROM sp_unique_seed u "
            "JOIN sp_input_frame f ON f.input_frame_id=u.input_frame_id "
            "JOIN sp_axis_xy m ON m.axis_xy_id=f.main_axis_xy_id "
            "JOIN sp_axis_xy c ON c.axis_xy_id=f.cstick_axis_xy_id "
            "JOIN sp_axis_xy t ON t.axis_xy_id=f.trigger_axis_xy_id "
            "WHERE u.unique_seed_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, unique_seed_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    return SeedProbeUniqueSeedRow{
        .unique_seed_id = sqlite3_column_int64(st.st, 0),
        .probe_result_id = sqlite3_column_int64(st.st, 1),
        .seed_value = sqlite3_column_int64(st.st, 2),
        .seed_delta = sqlite3_column_int64(st.st, 3),
        .main_x = sqlite3_column_int(st.st, 4),
        .main_y = sqlite3_column_int(st.st, 5),
        .cstick_x = sqlite3_column_int(st.st, 6),
        .cstick_y = sqlite3_column_int(st.st, 7),
        .trigger_x = sqlite3_column_int(st.st, 8),
        .trigger_y = sqlite3_column_int(st.st, 9),
    };
}

bool SqliteAnalysisDb::EnsureSeedProbeInputFrame(
    std::int64_t main_axis_xy_id,
    std::int64_t cstick_axis_xy_id,
    std::int64_t trigger_axis_xy_id,
    std::int64_t* input_frame_id_out,
    std::string* error_out) {
    if (input_frame_id_out != nullptr) {
        *input_frame_id_out = 0;
    }
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (main_axis_xy_id < 0 || cstick_axis_xy_id < 0 || trigger_axis_xy_id < 0) {
        if (error_out) *error_out = "axis ids must be non-negative";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    const auto rollback = [&]() {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    };

    const auto ensure_axis = [&](std::int64_t axis_xy_id, std::string* axis_error) -> bool {
        const auto axis_x = static_cast<int>((static_cast<std::uint64_t>(axis_xy_id) >> 8) & 0xff);
        const auto axis_y = static_cast<int>(static_cast<std::uint64_t>(axis_xy_id) & 0xff);

        Statement st;
        if (sqlite3_prepare_v2(
                db_,
                "INSERT OR IGNORE INTO sp_axis_xy(axis_xy_id,x,y) VALUES(?1,?2,?3);",
                -1,
                &st.st,
                nullptr)
            != SQLITE_OK) {
            if (axis_error) *axis_error = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_int64(st.st, 1, axis_xy_id);
        sqlite3_bind_int(st.st, 2, axis_x);
        sqlite3_bind_int(st.st, 3, axis_y);
        if (sqlite3_step(st.st) != SQLITE_DONE) {
            if (axis_error) *axis_error = sqlite3_errmsg(db_);
            return false;
        }
        return true;
    };

    if (!ensure_axis(main_axis_xy_id, error_out)
        || !ensure_axis(cstick_axis_xy_id, error_out)
        || !ensure_axis(trigger_axis_xy_id, error_out)) {
        rollback();
        return false;
    }

    Statement insert_frame;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT OR IGNORE INTO sp_input_frame(main_axis_xy_id,cstick_axis_xy_id,trigger_axis_xy_id) "
            "VALUES(?1,?2,?3);",
            -1,
            &insert_frame.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        rollback();
        return false;
    }
    sqlite3_bind_int64(insert_frame.st, 1, main_axis_xy_id);
    sqlite3_bind_int64(insert_frame.st, 2, cstick_axis_xy_id);
    sqlite3_bind_int64(insert_frame.st, 3, trigger_axis_xy_id);
    if (sqlite3_step(insert_frame.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        rollback();
        return false;
    }

    Statement select_frame;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT input_frame_id FROM sp_input_frame "
            "WHERE main_axis_xy_id=?1 AND cstick_axis_xy_id=?2 AND trigger_axis_xy_id=?3 "
            "LIMIT 1;",
            -1,
            &select_frame.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        rollback();
        return false;
    }
    sqlite3_bind_int64(select_frame.st, 1, main_axis_xy_id);
    sqlite3_bind_int64(select_frame.st, 2, cstick_axis_xy_id);
    sqlite3_bind_int64(select_frame.st, 3, trigger_axis_xy_id);
    if (sqlite3_step(select_frame.st) != SQLITE_ROW) {
        if (error_out) *error_out = "input frame could not be resolved";
        rollback();
        return false;
    }

    const auto input_frame_id = sqlite3_column_int64(select_frame.st, 0);
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        rollback();
        return false;
    }

    if (input_frame_id_out != nullptr) {
        *input_frame_id_out = input_frame_id;
    }
    return true;
}

bool SqliteAnalysisDb::EnsureSeedProbeUniqueSeedDelta(
    const RecordSeedProbeUniqueSeedCommand& command,
    bool* inserted_out,
    std::int64_t* unique_seed_id_out,
    std::string* error_out) {
    if (inserted_out) {
        *inserted_out = false;
    }
    if (unique_seed_id_out) {
        *unique_seed_id_out = 0;
    }

    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.probe_result_id <= 0 || command.input_frame_id <= 0 || command.event_id.empty()) {
        if (error_out) *error_out = "required command fields are missing";
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
            "SELECT unique_seed_id "
            "FROM sp_unique_seed "
            "WHERE probe_result_id=?1 AND seed_delta=?2 "
            "LIMIT 1;",
            -1,
            &existing.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(existing.st, 1, command.probe_result_id);
    sqlite3_bind_int64(existing.st, 2, command.seed_delta);
    if (sqlite3_step(existing.st) == SQLITE_ROW) {
        if (unique_seed_id_out) {
            *unique_seed_id_out = sqlite3_column_int64(existing.st, 0);
        }
        if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
            if (error_out) {
                *error_out = sqlite3_errmsg(db_);
            }
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
        return true;
    }

    Statement insert_unique;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO sp_unique_seed(probe_result_id,input_frame_id,seed_value,seed_delta,recorded_at_utc) "
            "VALUES(?1,?2,?3,?4,?5);",
            -1,
            &insert_unique.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(insert_unique.st, 1, command.probe_result_id);
    sqlite3_bind_int64(insert_unique.st, 2, command.input_frame_id);
    sqlite3_bind_int64(insert_unique.st, 3, command.seed_value);
    sqlite3_bind_int64(insert_unique.st, 4, command.seed_delta);
    sqlite3_bind_int64(insert_unique.st, 5, command.recorded_at_utc.time_since_epoch().count());
    if (sqlite3_step(insert_unique.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto unique_seed_id = sqlite3_last_insert_rowid(db_);
    if (inserted_out) {
        *inserted_out = true;
    }
    if (unique_seed_id_out) {
        *unique_seed_id_out = unique_seed_id;
    }

    const auto probe_run_id = ProbeRunIdForResult(db_, command.probe_result_id);
    if (!probe_run_id.has_value()) {
        if (error_out) *error_out = "probe_result_id does not resolve to probe_run";
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (!InsertSeedProbeOutboxEvent(
            db_,
            command.event_id,
            "AnalysisSeedProbe.UniqueSeedRecorded.v1",
            "probe_run",
            std::to_string(probe_run_id.value()),
            command.correlation_id,
            command.causation_id,
            command.recorded_at_utc.time_since_epoch().count(),
            "unique_seed",
            unique_seed_id,
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

    return true;
}

bool SqliteAnalysisDb::CreateSeedProbeSet(
    const CreateSeedProbeSetCommand& command,
    std::int64_t* probe_set_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.name.empty()
        || command.probe_flavor.empty()
        || command.breakpoint_policy_name.empty()
        || command.segment_source_kind.empty()
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

    Statement insert_set;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO sp_probe_set(name,probe_flavor,breakpoint_policy_name,dungeon_segment_file_num,dungeon_segment_file_letter,dungeon_segment_code,segment_source_kind,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8);",
            -1,
            &insert_set.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_text(insert_set.st, 1, command.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_set.st, 2, command.probe_flavor.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_set.st, 3, command.breakpoint_policy_name.c_str(), -1, SQLITE_TRANSIENT);
    if (command.dungeon_segment_file_num.has_value()) sqlite3_bind_int64(insert_set.st, 4, command.dungeon_segment_file_num.value());
    else sqlite3_bind_null(insert_set.st, 4);
    if (command.dungeon_segment_file_letter.has_value()) sqlite3_bind_text(insert_set.st, 5, command.dungeon_segment_file_letter->c_str(), -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(insert_set.st, 5);
    if (command.dungeon_segment_code.has_value()) sqlite3_bind_text(insert_set.st, 6, command.dungeon_segment_code->c_str(), -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(insert_set.st, 6);
    sqlite3_bind_text(insert_set.st, 7, command.segment_source_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_set.st, 8, command.created_at_utc.time_since_epoch().count());
    if (sqlite3_step(insert_set.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto probe_set_id = sqlite3_last_insert_rowid(db_);
    if (!InsertSeedProbeOutboxEvent(
            db_,
            command.event_id,
            "AnalysisSeedProbe.SetCreated.v1",
            "probe_set",
            std::to_string(probe_set_id),
            command.correlation_id,
            command.causation_id,
            command.created_at_utc.time_since_epoch().count(),
            "probe_set",
            probe_set_id,
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

    if (probe_set_id_out) {
        *probe_set_id_out = probe_set_id;
    }
    return true;
}

bool SqliteAnalysisDb::RequestSeedProbeRun(
    const RequestSeedProbeRunCommand& command,
    std::int64_t* probe_run_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.probe_set_id <= 0
        || command.entry_savestate_id <= 0
        || command.seed_probe_spec_id <= 0
        || command.status.empty()
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

    Statement insert_run;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO sp_probe_run(probe_set_id,entry_savestate_id,seed_probe_spec_id,codec_version,status,requested_at_utc,completed_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,NULL);",
            -1,
            &insert_run.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(insert_run.st, 1, command.probe_set_id);
    sqlite3_bind_int64(insert_run.st, 2, command.entry_savestate_id);
    sqlite3_bind_int64(insert_run.st, 3, command.seed_probe_spec_id);
    sqlite3_bind_int(insert_run.st, 4, command.codec_version);
    sqlite3_bind_text(insert_run.st, 5, command.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_run.st, 6, command.requested_at_utc.time_since_epoch().count());
    if (sqlite3_step(insert_run.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto probe_run_id = sqlite3_last_insert_rowid(db_);

    Statement insert_result;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO sp_probe_result(probe_run_id,neutral_seed_value,grid_count,unique_count,result_status,recorded_at_utc) "
            "VALUES(?1,NULL,0,0,'pending',?2);",
            -1,
            &insert_result.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    sqlite3_bind_int64(insert_result.st, 1, probe_run_id);
    sqlite3_bind_int64(insert_result.st, 2, command.requested_at_utc.time_since_epoch().count());
    if (sqlite3_step(insert_result.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto aggregate_id = std::to_string(probe_run_id);
    if (!InsertSeedProbeOutboxEvent(
            db_,
            command.event_id,
            "AnalysisSeedProbe.RunRequested.v1",
            "probe_run",
            aggregate_id,
            command.correlation_id,
            command.causation_id,
            command.requested_at_utc.time_since_epoch().count(),
            "probe_run",
            probe_run_id,
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

    if (probe_run_id_out) {
        *probe_run_id_out = probe_run_id;
    }
    return true;
}

bool SqliteAnalysisDb::CreateSeedProbeRunForSet(
    std::int64_t probe_set_id,
    std::int64_t* probe_run_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (probe_set_id <= 0) {
        if (error_out) *error_out = "probe_set_id must be > 0";
        return false;
    }
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    const auto now = types::UtcNow().time_since_epoch().count();
    Statement insert_run;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO sp_probe_run(probe_set_id,entry_savestate_id,seed_probe_spec_id,codec_version,status,requested_at_utc,completed_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,NULL);",
            -1,
            &insert_run.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    sqlite3_bind_int64(insert_run.st, 1, probe_set_id);
    sqlite3_bind_int64(insert_run.st, 2, 0); // runtime init will fail fast until caller hydrates this run with a concrete savestate
    sqlite3_bind_int64(insert_run.st, 3, probe_set_id); // neutral queueing path binds spec to set id
    sqlite3_bind_int(insert_run.st, 4, 1);
    sqlite3_bind_text(insert_run.st, 5, "queued", -1, SQLITE_STATIC);
    sqlite3_bind_int64(insert_run.st, 6, now);
    if (sqlite3_step(insert_run.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto probe_run_id = sqlite3_last_insert_rowid(db_);
    Statement insert_result;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO sp_probe_result(probe_run_id,neutral_seed_value,grid_count,unique_count,result_status,recorded_at_utc) "
            "VALUES(?1,NULL,0,0,'pending',?2);",
            -1,
            &insert_result.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    sqlite3_bind_int64(insert_result.st, 1, probe_run_id);
    sqlite3_bind_int64(insert_result.st, 2, now);
    if (sqlite3_step(insert_result.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
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
    if (probe_run_id_out) {
        *probe_run_id_out = probe_run_id;
    }
    return true;
}

std::optional<SeedProbeRunSnapshot> SqliteAnalysisDb::GetSeedProbeRun(std::int64_t probe_run_id) const {
    if (db_ == nullptr || probe_run_id <= 0) {
        return std::nullopt;
    }
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT probe_run_id, probe_set_id, seed_probe_spec_id, entry_savestate_id, codec_version, status, "
            "requested_at_utc, completed_at_utc "
            "FROM sp_probe_run WHERE probe_run_id=?1 LIMIT 1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, probe_run_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }

    SeedProbeRunSnapshot snapshot{};
    snapshot.probe_run_id = sqlite3_column_int64(st.st, 0);
    snapshot.probe_set_id = sqlite3_column_int64(st.st, 1);
    snapshot.seed_probe_spec_id = sqlite3_column_int64(st.st, 2);
    snapshot.entry_savestate_id = sqlite3_column_int64(st.st, 3);
    snapshot.codec_version = sqlite3_column_int(st.st, 4);
    snapshot.status = reinterpret_cast<const char*>(sqlite3_column_text(st.st, 5));
    snapshot.requested_at_utc = types::UtcTimePoint(std::chrono::milliseconds(sqlite3_column_int64(st.st, 6)));
    if (sqlite3_column_type(st.st, 7) != SQLITE_NULL) {
        snapshot.completed_at_utc = types::UtcTimePoint(std::chrono::milliseconds(sqlite3_column_int64(st.st, 7)));
    }
    return snapshot;
}

bool SqliteAnalysisDb::SetSeedProbeRunNeutralSeed(
    std::int64_t probe_run_id,
    std::int64_t neutral_seed_value,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (probe_run_id <= 0) {
        if (error_out) *error_out = "probe_run_id must be > 0";
        return false;
    }
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }
    const auto now = types::UtcNow().time_since_epoch().count();

    Statement get_result;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT probe_result_id FROM sp_probe_result WHERE probe_run_id=?1 LIMIT 1;",
            -1,
            &get_result.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    sqlite3_bind_int64(get_result.st, 1, probe_run_id);
    auto get_result_step = sqlite3_step(get_result.st);
    std::int64_t probe_result_id = 0;
    if (get_result_step == SQLITE_ROW) {
        probe_result_id = sqlite3_column_int64(get_result.st, 0);
    } else if (get_result_step == SQLITE_DONE) {
        Statement insert_result;
        if (sqlite3_prepare_v2(
                db_,
                "INSERT INTO sp_probe_result(probe_run_id,neutral_seed_value,grid_count,unique_count,result_status,recorded_at_utc) "
                "VALUES(?1,NULL,0,0,'pending',?2);",
                -1,
                &insert_result.st,
                nullptr)
            != SQLITE_OK) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
        sqlite3_bind_int64(insert_result.st, 1, probe_run_id);
        sqlite3_bind_int64(insert_result.st, 2, now);
        if (sqlite3_step(insert_result.st) != SQLITE_DONE) {
            if (error_out != nullptr) {
                *error_out = sqlite3_errmsg(db_);
            }
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
        probe_result_id = sqlite3_last_insert_rowid(db_);
    } else {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    Statement update_result;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE sp_probe_result SET neutral_seed_value=?2, result_status='completed', recorded_at_utc=?3 WHERE probe_result_id=?1;",
            -1,
            &update_result.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    sqlite3_bind_int64(update_result.st, 1, probe_result_id);
    sqlite3_bind_int64(update_result.st, 2, neutral_seed_value);
    sqlite3_bind_int64(update_result.st, 3, now);
    if (sqlite3_step(update_result.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    Statement update_run;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE sp_probe_run SET status='completed', completed_at_utc=?2 WHERE probe_run_id=?1;",
            -1,
            &update_run.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    sqlite3_bind_int64(update_run.st, 1, probe_run_id);
    sqlite3_bind_int64(update_run.st, 2, now);
    if (sqlite3_step(update_run.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
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
    return true;
}

bool SqliteAnalysisDb::SetSeedProbeRunEntrySavestate(
    const SetSeedProbeRunEntrySavestateCommand& command,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.probe_run_id <= 0 || command.entry_savestate_id <= 0) {
        if (error_out) *error_out = "probe_run_id and entry_savestate_id must be > 0";
        return false;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE sp_probe_run SET entry_savestate_id=?2 WHERE probe_run_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, command.probe_run_id);
    sqlite3_bind_int64(st.st, 2, command.entry_savestate_id);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (sqlite3_changes(db_) <= 0) {
        if (error_out) *error_out = "seed probe run not found";
        return false;
    }
    return true;
}

bool SqliteAnalysisDb::RecordSeedProbeNeutralSeed(
    const RecordSeedProbeNeutralSeedCommand& command,
    std::int64_t* neutral_seed_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.probe_result_id <= 0 || command.source_kind.empty() || command.event_id.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    const auto probe_run_id = ProbeRunIdForResult(db_, command.probe_result_id);
    if (!probe_run_id.has_value()) {
        if (error_out) *error_out = "probe_result_id does not resolve to probe_run";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement insert_neutral;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO sp_neutral_seed(probe_result_id,neutral_seed_value,source_kind,recorded_at_utc) "
            "VALUES(?1,?2,?3,?4);",
            -1,
            &insert_neutral.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(insert_neutral.st, 1, command.probe_result_id);
    sqlite3_bind_int64(insert_neutral.st, 2, command.neutral_seed_value);
    sqlite3_bind_text(insert_neutral.st, 3, command.source_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_neutral.st, 4, command.recorded_at_utc.time_since_epoch().count());

    if (sqlite3_step(insert_neutral.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto neutral_seed_id = sqlite3_last_insert_rowid(db_);
    if (!InsertSeedProbeOutboxEvent(
            db_,
            command.event_id,
            "AnalysisSeedProbe.NeutralSeedRecorded.v1",
            "probe_run",
            std::to_string(probe_run_id.value()),
            command.correlation_id,
            command.causation_id,
            command.recorded_at_utc.time_since_epoch().count(),
            "neutral_seed",
            neutral_seed_id,
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

    if (neutral_seed_id_out) {
        *neutral_seed_id_out = neutral_seed_id;
    }
    return true;
}

bool SqliteAnalysisDb::RecordSeedProbeGridSeed(
    const RecordSeedProbeGridSeedCommand& command,
    std::int64_t* grid_seed_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.probe_result_id <= 0 || command.axis_xy_id < 0 || command.source_family.empty() || command.event_id.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    const auto probe_run_id = ProbeRunIdForResult(db_, command.probe_result_id);
    if (!probe_run_id.has_value()) {
        if (error_out) *error_out = "probe_result_id does not resolve to probe_run";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement existing_event;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT payload_ref_id FROM sp_outbox_message "
            "WHERE event_id=?1 "
            "  AND event_type='AnalysisSeedProbe.GridSeedRecorded.v1' "
            "  AND payload_ref_kind='grid_seed' "
            "LIMIT 1;",
            -1,
            &existing_event.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(existing_event.st, 1, command.event_id.c_str(), -1, SQLITE_TRANSIENT);
    const auto existing_event_rc = sqlite3_step(existing_event.st);
    if (existing_event_rc == SQLITE_ROW) {
        if (grid_seed_id_out) {
            *grid_seed_id_out = sqlite3_column_int64(existing_event.st, 0);
        }
        if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
            if (error_out != nullptr) {
                *error_out = sqlite3_errmsg(db_);
            }
            (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
        return true;
    }
    if (existing_event_rc != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto axis_x = static_cast<int>((static_cast<std::uint64_t>(command.axis_xy_id) >> 8) & 0xff);
    const auto axis_y = static_cast<int>(static_cast<std::uint64_t>(command.axis_xy_id) & 0xff);

    Statement insert_axis;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT OR IGNORE INTO sp_axis_xy(axis_xy_id,x,y) "
            "VALUES(?1,?2,?3);",
            -1,
            &insert_axis.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(insert_axis.st, 1, command.axis_xy_id);
    sqlite3_bind_int(insert_axis.st, 2, axis_x);
    sqlite3_bind_int(insert_axis.st, 3, axis_y);
    if (sqlite3_step(insert_axis.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    Statement insert_grid;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO sp_grid_seed(probe_result_id,source_family,axis_xy_id,seed_value,seed_delta,recorded_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6);",
            -1,
            &insert_grid.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(insert_grid.st, 1, command.probe_result_id);
    sqlite3_bind_text(insert_grid.st, 2, command.source_family.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_grid.st, 3, command.axis_xy_id);
    sqlite3_bind_int64(insert_grid.st, 4, command.seed_value);
    sqlite3_bind_int64(insert_grid.st, 5, command.seed_delta);
    sqlite3_bind_int64(insert_grid.st, 6, command.recorded_at_utc.time_since_epoch().count());
    if (sqlite3_step(insert_grid.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto grid_seed_id = sqlite3_last_insert_rowid(db_);
    if (!InsertSeedProbeOutboxEvent(
            db_,
            command.event_id,
            "AnalysisSeedProbe.GridSeedRecorded.v1",
            "probe_run",
            std::to_string(probe_run_id.value()),
            command.correlation_id,
            command.causation_id,
            command.recorded_at_utc.time_since_epoch().count(),
            "grid_seed",
            grid_seed_id,
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

    if (grid_seed_id_out) {
        *grid_seed_id_out = grid_seed_id;
    }
    return true;
}

bool SqliteAnalysisDb::RecordSeedProbeUniqueSeed(
    const RecordSeedProbeUniqueSeedCommand& command,
    std::int64_t* unique_seed_id_out,
    std::string* error_out) {
    bool inserted = false;
    return EnsureSeedProbeUniqueSeedDelta(command, &inserted, unique_seed_id_out, error_out) && inserted;
}

bool SqliteAnalysisDb::RecordSeedProbeEncounterProjection(
    const RecordSeedProbeEncounterProjectionCommand& command,
    std::int64_t* encounter_projection_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.probe_run_id <= 0 || command.encounter_id.empty() || command.event_id.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement insert_projection;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO sp_encounter_projection(probe_run_id,seed_value,option_ordinal,encounter_id,encounter_frame,stutter_step_at,movement_required,recorded_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8);",
            -1,
            &insert_projection.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(insert_projection.st, 1, command.probe_run_id);
    sqlite3_bind_int64(insert_projection.st, 2, command.seed_value);
    sqlite3_bind_int(insert_projection.st, 3, command.option_ordinal);
    sqlite3_bind_text(insert_projection.st, 4, command.encounter_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_projection.st, 5, command.encounter_frame);
    if (command.stutter_step_at.has_value()) {
        sqlite3_bind_int64(insert_projection.st, 6, command.stutter_step_at.value());
    } else {
        sqlite3_bind_null(insert_projection.st, 6);
    }
    sqlite3_bind_int(insert_projection.st, 7, command.movement_required ? 1 : 0);
    sqlite3_bind_int64(insert_projection.st, 8, command.recorded_at_utc.time_since_epoch().count());
    if (sqlite3_step(insert_projection.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto encounter_projection_id = sqlite3_last_insert_rowid(db_);
    if (!InsertSeedProbeOutboxEvent(
            db_,
            command.event_id,
            "AnalysisSeedProbe.EncounterProjectionRecorded.v1",
            "probe_run",
            std::to_string(command.probe_run_id),
            command.correlation_id,
            command.causation_id,
            command.recorded_at_utc.time_since_epoch().count(),
            "encounter_projection",
            encounter_projection_id,
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

    if (encounter_projection_id_out) {
        *encounter_projection_id_out = encounter_projection_id;
    }
    return true;
}

bool SqliteAnalysisDb::CompleteSeedProbeRun(
    const CompleteSeedProbeRunCommand& command,
    std::int64_t* probe_result_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.probe_run_id <= 0 || command.result_status.empty() || command.run_status.empty() || command.event_id.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement insert_result;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO sp_probe_result(probe_run_id,neutral_seed_value,grid_count,unique_count,result_status,recorded_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6);",
            -1,
            &insert_result.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(insert_result.st, 1, command.probe_run_id);
    if (command.neutral_seed_value.has_value()) sqlite3_bind_int64(insert_result.st, 2, command.neutral_seed_value.value());
    else sqlite3_bind_null(insert_result.st, 2);
    sqlite3_bind_int(insert_result.st, 3, command.grid_count);
    sqlite3_bind_int(insert_result.st, 4, command.unique_count);
    sqlite3_bind_text(insert_result.st, 5, command.result_status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_result.st, 6, command.recorded_at_utc.time_since_epoch().count());
    if (sqlite3_step(insert_result.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    Statement update_run;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE sp_probe_run SET status=?2, completed_at_utc=?3 WHERE probe_run_id=?1;",
            -1,
            &update_run.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(update_run.st, 1, command.probe_run_id);
    sqlite3_bind_text(update_run.st, 2, command.run_status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(update_run.st, 3, command.completed_at_utc.time_since_epoch().count());
    if (sqlite3_step(update_run.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto probe_result_id = sqlite3_last_insert_rowid(db_);
    if (!InsertSeedProbeOutboxEvent(
            db_,
            command.event_id,
            "AnalysisSeedProbe.RunCompleted.v1",
            "probe_run",
            std::to_string(command.probe_run_id),
            command.correlation_id,
            command.causation_id,
            command.recorded_at_utc.time_since_epoch().count(),
            "probe_result",
            probe_result_id,
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

    if (probe_result_id_out) {
        *probe_result_id_out = probe_result_id;
    }
    return true;
}

bool SqliteAnalysisDb::CreateBattleSet(
    const CreateBattleSetCommand& command,
    std::int64_t* battle_set_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.name.empty()
        || command.entry_savestate_id <= 0
        || command.battle_run_spec_id <= 0
        || command.explorer_settings_id <= 0
        || command.status == BattleSetStatus::Unknown
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

    Statement insert_set;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO ab_battle_set(name,entry_savestate_id,battle_run_spec_id,explorer_settings_id,status,created_at_utc,completed_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,NULL);",
            -1,
            &insert_set.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_text(insert_set.st, 1, command.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_set.st, 2, command.entry_savestate_id);
    sqlite3_bind_int64(insert_set.st, 3, command.battle_run_spec_id);
    sqlite3_bind_int64(insert_set.st, 4, command.explorer_settings_id);
    const auto status = ToDbString(command.status);
    sqlite3_bind_text(insert_set.st, 5, status.data(), static_cast<int>(status.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_set.st, 6, command.created_at_utc.time_since_epoch().count());
    if (sqlite3_step(insert_set.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto battle_set_id = sqlite3_last_insert_rowid(db_);
    if (!InsertBattleOutboxEvent(
            db_,
            command.event_id,
            "AnalysisBattle.BattleSetCreated.v1",
            "battle_set",
            std::to_string(battle_set_id),
            command.correlation_id,
            command.causation_id,
            command.created_at_utc.time_since_epoch().count(),
            "battle_set",
            battle_set_id,
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

    if (battle_set_id_out) {
        *battle_set_id_out = battle_set_id;
    }
    return true;
}

bool SqliteAnalysisDb::AddBattleSeedCandidate(
    const AddBattleSeedCandidateCommand& command,
    std::int64_t* seed_candidate_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.battle_set_id <= 0
        || command.source_kind == BattleSeedCandidateSourceKind::Unknown
        || command.candidate_status == BattleSeedCandidateStatus::Unknown
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

    Statement insert_candidate;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO ab_seed_candidate(battle_set_id,source_unique_seed_id,seed_value,source_kind,candidate_status,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6);",
            -1,
            &insert_candidate.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(insert_candidate.st, 1, command.battle_set_id);
    if (command.source_unique_seed_id.has_value()) sqlite3_bind_int64(insert_candidate.st, 2, command.source_unique_seed_id.value());
    else sqlite3_bind_null(insert_candidate.st, 2);
    sqlite3_bind_int64(insert_candidate.st, 3, command.seed_value);
    const auto source_kind = ToDbString(command.source_kind);
    const auto candidate_status = ToDbString(command.candidate_status);
    sqlite3_bind_text(insert_candidate.st, 4, source_kind.data(), static_cast<int>(source_kind.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(insert_candidate.st, 5, candidate_status.data(), static_cast<int>(candidate_status.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_candidate.st, 6, command.created_at_utc.time_since_epoch().count());
    if (sqlite3_step(insert_candidate.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto seed_candidate_id = sqlite3_last_insert_rowid(db_);
    if (!InsertBattleOutboxEvent(
            db_,
            command.event_id,
            "AnalysisBattle.SeedCandidateAdded.v1",
            "battle_set",
            std::to_string(command.battle_set_id),
            command.correlation_id,
            command.causation_id,
            command.created_at_utc.time_since_epoch().count(),
            "seed_candidate",
            seed_candidate_id,
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

    if (seed_candidate_id_out) {
        *seed_candidate_id_out = seed_candidate_id;
    }
    return true;
}

bool SqliteAnalysisDb::CreateBattleTurnWave(
    const CreateBattleTurnWaveCommand& command,
    std::int64_t* wave_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.battle_set_id <= 0
        || command.seed_candidate_id <= 0
        || command.status == BattleTurnWaveStatus::Unknown
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

    Statement insert_wave;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO ab_turn_wave(battle_set_id,turn_index,context_probe_id,parent_wave_id,parent_turn_job_id,seed_candidate_id,selection_pool_id,status,created_at_utc,completed_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10);",
            -1,
            &insert_wave.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(insert_wave.st, 1, command.battle_set_id);
    sqlite3_bind_int(insert_wave.st, 2, command.turn_index);
    if (command.context_probe_id.has_value()) sqlite3_bind_int64(insert_wave.st, 3, command.context_probe_id.value());
    else sqlite3_bind_null(insert_wave.st, 3);
    if (command.parent_wave_id.has_value()) sqlite3_bind_int64(insert_wave.st, 4, command.parent_wave_id.value());
    else sqlite3_bind_null(insert_wave.st, 4);
    if (command.parent_turn_job_id.has_value()) sqlite3_bind_int64(insert_wave.st, 5, command.parent_turn_job_id.value());
    else sqlite3_bind_null(insert_wave.st, 5);
    sqlite3_bind_int64(insert_wave.st, 6, command.seed_candidate_id);
    if (command.selection_pool_id.has_value()) sqlite3_bind_int64(insert_wave.st, 7, command.selection_pool_id.value());
    else sqlite3_bind_null(insert_wave.st, 7);
    const auto status = ToDbString(command.status);
    sqlite3_bind_text(insert_wave.st, 8, status.data(), static_cast<int>(status.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_wave.st, 9, command.created_at_utc.time_since_epoch().count());
    if (command.completed_at_utc.has_value()) sqlite3_bind_int64(insert_wave.st, 10, command.completed_at_utc->time_since_epoch().count());
    else sqlite3_bind_null(insert_wave.st, 10);
    if (sqlite3_step(insert_wave.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto wave_id = sqlite3_last_insert_rowid(db_);
    if (!InsertBattleOutboxEvent(
            db_,
            command.event_id,
            "AnalysisBattle.TurnWaveCreated.v1",
            "battle_set",
            std::to_string(command.battle_set_id),
            command.correlation_id,
            command.causation_id,
            command.created_at_utc.time_since_epoch().count(),
            "turn_wave",
            wave_id,
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

    if (wave_id_out) {
        *wave_id_out = wave_id;
    }
    return true;
}

bool SqliteAnalysisDb::CreateBattleContextProbe(
    const CreateBattleContextProbeCommand& command,
    std::int64_t* context_probe_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.source_savestate_id <= 0 || command.probe_status == BattleContextProbeStatus::Unknown || command.event_id.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    std::optional<std::int64_t> battle_set_id;
    if (command.wave_id > 0) {
        battle_set_id = BattleSetIdForWave(db_, command.wave_id);
        if (!battle_set_id.has_value()) {
            if (error_out) *error_out = "wave_id does not resolve to battle_set";
            return false;
        }
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    Statement insert_probe;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO ab_battle_context_probe(wave_id,source_savestate_id,exec_job_id,probe_status,context_blob,context_version,recorded_at_utc,created_at_utc) "
            "VALUES(?1,?2,NULL,?3,NULL,NULL,NULL,?4);",
            -1,
            &insert_probe.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    if (command.wave_id > 0) sqlite3_bind_int64(insert_probe.st, 1, command.wave_id);
    else sqlite3_bind_null(insert_probe.st, 1);
    sqlite3_bind_int64(insert_probe.st, 2, command.source_savestate_id);
    const auto probe_status = ToDbString(command.probe_status);
    sqlite3_bind_text(insert_probe.st, 3, probe_status.data(), static_cast<int>(probe_status.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_probe.st, 4, command.created_at_utc.time_since_epoch().count());
    if (sqlite3_step(insert_probe.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto context_probe_id = sqlite3_last_insert_rowid(db_);
    if (!InsertBattleOutboxEvent(
            db_,
            command.event_id,
            "AnalysisBattle.ContextProbeCreated.v1",
            battle_set_id.has_value() ? "battle_set" : "battle_context_probe",
            battle_set_id.has_value() ? std::to_string(*battle_set_id) : std::to_string(context_probe_id),
            command.correlation_id,
            command.causation_id,
            command.created_at_utc.time_since_epoch().count(),
            "battle_context_probe",
            context_probe_id,
            error_out)) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (context_probe_id_out) *context_probe_id_out = context_probe_id;
    return true;
}

bool SqliteAnalysisDb::SetBattleContextProbeExecJobId(
    std::int64_t context_probe_id,
    std::int64_t exec_job_id,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (context_probe_id <= 0 || exec_job_id <= 0) {
        if (error_out) *error_out = "context_probe_id and exec_job_id are required";
        return false;
    }
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE ab_battle_context_probe SET exec_job_id=?2, probe_status='RUNNING' WHERE context_probe_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, context_probe_id);
    sqlite3_bind_int64(st.st, 2, exec_job_id);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    return sqlite3_changes(db_) > 0;
}

bool SqliteAnalysisDb::CompleteBattleContextProbe(
    const CompleteBattleContextProbeCommand& command,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.exec_job_id <= 0 || command.probe_status == BattleContextProbeStatus::Unknown) {
        if (error_out) *error_out = "exec_job_id and probe_status are required";
        return false;
    }
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE ab_battle_context_probe SET probe_status=?2,context_blob=?3,context_version=?4,recorded_at_utc=?5 WHERE exec_job_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, command.exec_job_id);
    const auto probe_status = ToDbString(command.probe_status);
    sqlite3_bind_text(st.st, 2, probe_status.data(), static_cast<int>(probe_status.size()), SQLITE_TRANSIENT);
    if (command.context_blob.has_value()) {
        sqlite3_bind_blob(
            st.st,
            3,
            command.context_blob->data(),
            static_cast<int>(command.context_blob->size()),
            SQLITE_TRANSIENT);
    }
    else sqlite3_bind_null(st.st, 3);
    if (command.context_version.has_value()) sqlite3_bind_int(st.st, 4, *command.context_version);
    else sqlite3_bind_null(st.st, 4);
    sqlite3_bind_int64(st.st, 5, command.recorded_at_utc.time_since_epoch().count());
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    return sqlite3_changes(db_) > 0;
}

bool SqliteAnalysisDb::RecordBattleTurnJob(
    const RecordBattleTurnJobCommand& command,
    std::int64_t* turn_job_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.wave_id <= 0 || command.plan_id <= 0 || command.job_state == BattleTurnJobState::Unknown || command.event_id.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    const auto battle_set_id = BattleSetIdForWave(db_, command.wave_id);
    if (!battle_set_id.has_value()) {
        if (error_out) *error_out = "wave_id does not resolve to battle_set";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement insert_job;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO ab_turn_job(wave_id,exec_job_id,plan_id,fake_attacks_this_turn,fake_attacks_used_before,job_state,started_at_utc,ended_at_utc,has_results,vi_start,vi_end,delta_vi,rng_seed,battle_outcome,plan_materialize_err,pred_passed,pred_total,pred_abort_run,output_savestate_id,applied_input_artifact_id,result_context_blob_base64,result_context_version,recorded_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22,?23);",
            -1,
            &insert_job.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(insert_job.st, 1, command.wave_id);
    if (command.exec_job_id.has_value()) sqlite3_bind_int64(insert_job.st, 2, command.exec_job_id.value());
    else sqlite3_bind_null(insert_job.st, 2);
    sqlite3_bind_int64(insert_job.st, 3, command.plan_id);
    sqlite3_bind_int(insert_job.st, 4, command.fake_attacks_this_turn);
    sqlite3_bind_int(insert_job.st, 5, command.fake_attacks_used_before);
    const auto job_state = ToDbString(command.job_state);
    sqlite3_bind_text(insert_job.st, 6, job_state.data(), static_cast<int>(job_state.size()), SQLITE_TRANSIENT);
    if (command.started_at_utc.has_value()) sqlite3_bind_int64(insert_job.st, 7, command.started_at_utc->time_since_epoch().count());
    else sqlite3_bind_null(insert_job.st, 7);
    if (command.ended_at_utc.has_value()) sqlite3_bind_int64(insert_job.st, 8, command.ended_at_utc->time_since_epoch().count());
    else sqlite3_bind_null(insert_job.st, 8);
    sqlite3_bind_int(insert_job.st, 9, command.has_results ? 1 : 0);
    if (command.vi_start.has_value()) sqlite3_bind_int(insert_job.st, 10, command.vi_start.value());
    else sqlite3_bind_null(insert_job.st, 10);
    if (command.vi_end.has_value()) sqlite3_bind_int(insert_job.st, 11, command.vi_end.value());
    else sqlite3_bind_null(insert_job.st, 11);
    if (command.delta_vi.has_value()) sqlite3_bind_int(insert_job.st, 12, command.delta_vi.value());
    else sqlite3_bind_null(insert_job.st, 12);
    if (command.rng_seed.has_value()) sqlite3_bind_int64(insert_job.st, 13, command.rng_seed.value());
    else sqlite3_bind_null(insert_job.st, 13);
    if (command.battle_outcome.has_value()) sqlite3_bind_int(insert_job.st, 14, static_cast<int>(*command.battle_outcome));
    else sqlite3_bind_null(insert_job.st, 14);
    if (command.plan_materialize_err.has_value()) sqlite3_bind_int(insert_job.st, 15, command.plan_materialize_err.value());
    else sqlite3_bind_null(insert_job.st, 15);
    if (command.pred_passed.has_value()) sqlite3_bind_int(insert_job.st, 16, command.pred_passed.value());
    else sqlite3_bind_null(insert_job.st, 16);
    if (command.pred_total.has_value()) sqlite3_bind_int(insert_job.st, 17, command.pred_total.value());
    else sqlite3_bind_null(insert_job.st, 17);
    if (command.pred_abort_run.has_value()) sqlite3_bind_int(insert_job.st, 18, command.pred_abort_run.value());
    else sqlite3_bind_null(insert_job.st, 18);
    if (command.output_savestate_id.has_value()) sqlite3_bind_int64(insert_job.st, 19, command.output_savestate_id.value());
    else sqlite3_bind_null(insert_job.st, 19);
    if (command.applied_input_artifact_id.has_value()) sqlite3_bind_int64(insert_job.st, 20, command.applied_input_artifact_id.value());
    else sqlite3_bind_null(insert_job.st, 20);
    if (command.result_context_blob_base64.has_value()) sqlite3_bind_text(insert_job.st, 21, command.result_context_blob_base64->c_str(), -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(insert_job.st, 21);
    if (command.result_context_version.has_value()) sqlite3_bind_int(insert_job.st, 22, *command.result_context_version);
    else sqlite3_bind_null(insert_job.st, 22);
    if (command.recorded_at_utc.has_value()) sqlite3_bind_int64(insert_job.st, 23, command.recorded_at_utc->time_since_epoch().count());
    else sqlite3_bind_null(insert_job.st, 23);

    if (sqlite3_step(insert_job.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto turn_job_id = sqlite3_last_insert_rowid(db_);
    const auto occurred_at_utc = command.recorded_at_utc.has_value()
        ? command.recorded_at_utc->time_since_epoch().count()
        : std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (!InsertBattleOutboxEvent(
            db_,
            command.event_id,
            "AnalysisBattle.TurnJobRecorded.v1",
            "battle_set",
            std::to_string(battle_set_id.value()),
            command.correlation_id,
            command.causation_id,
            occurred_at_utc,
            "turn_job",
            turn_job_id,
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

    if (turn_job_id_out) {
        *turn_job_id_out = turn_job_id;
    }
    return true;
}

bool SqliteAnalysisDb::SetBattleTurnJobExecJobId(
    std::int64_t turn_job_id,
    std::int64_t exec_job_id,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (turn_job_id <= 0 || exec_job_id <= 0) {
        if (error_out) *error_out = "turn_job_id and exec_job_id are required";
        return false;
    }
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE ab_turn_job SET exec_job_id=?2 WHERE turn_job_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, turn_job_id);
    sqlite3_bind_int64(st.st, 2, exec_job_id);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    return sqlite3_changes(db_) > 0;
}

bool SqliteAnalysisDb::UpdateBattleTurnJobResult(
    const RecordBattleTurnJobCommand& command,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (!command.exec_job_id.has_value() || *command.exec_job_id <= 0 || command.job_state == BattleTurnJobState::Unknown) {
        if (error_out) *error_out = "exec_job_id and job_state are required";
        return false;
    }

    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE ab_turn_job SET "
            "job_state=?2,ended_at_utc=?3,has_results=?4,vi_start=?5,vi_end=?6,delta_vi=?7,rng_seed=?8,"
            "battle_outcome=?9,plan_materialize_err=?10,pred_passed=?11,pred_total=?12,pred_abort_run=?13,"
            "output_savestate_id=?14,applied_input_artifact_id=?15,result_context_blob_base64=?16,result_context_version=?17,recorded_at_utc=?18 "
            "WHERE exec_job_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, *command.exec_job_id);
    const auto job_state = ToDbString(command.job_state);
    sqlite3_bind_text(st.st, 2, job_state.data(), static_cast<int>(job_state.size()), SQLITE_TRANSIENT);
    if (command.ended_at_utc.has_value()) sqlite3_bind_int64(st.st, 3, command.ended_at_utc->time_since_epoch().count());
    else sqlite3_bind_null(st.st, 3);
    sqlite3_bind_int(st.st, 4, command.has_results ? 1 : 0);
    if (command.vi_start.has_value()) sqlite3_bind_int(st.st, 5, *command.vi_start); else sqlite3_bind_null(st.st, 5);
    if (command.vi_end.has_value()) sqlite3_bind_int(st.st, 6, *command.vi_end); else sqlite3_bind_null(st.st, 6);
    if (command.delta_vi.has_value()) sqlite3_bind_int(st.st, 7, *command.delta_vi); else sqlite3_bind_null(st.st, 7);
    if (command.rng_seed.has_value()) sqlite3_bind_int64(st.st, 8, *command.rng_seed); else sqlite3_bind_null(st.st, 8);
    if (command.battle_outcome.has_value()) sqlite3_bind_int(st.st, 9, static_cast<int>(*command.battle_outcome)); else sqlite3_bind_null(st.st, 9);
    if (command.plan_materialize_err.has_value()) sqlite3_bind_int(st.st, 10, *command.plan_materialize_err); else sqlite3_bind_null(st.st, 10);
    if (command.pred_passed.has_value()) sqlite3_bind_int(st.st, 11, *command.pred_passed); else sqlite3_bind_null(st.st, 11);
    if (command.pred_total.has_value()) sqlite3_bind_int(st.st, 12, *command.pred_total); else sqlite3_bind_null(st.st, 12);
    if (command.pred_abort_run.has_value()) sqlite3_bind_int(st.st, 13, *command.pred_abort_run); else sqlite3_bind_null(st.st, 13);
    if (command.output_savestate_id.has_value()) sqlite3_bind_int64(st.st, 14, *command.output_savestate_id); else sqlite3_bind_null(st.st, 14);
    if (command.applied_input_artifact_id.has_value()) sqlite3_bind_int64(st.st, 15, *command.applied_input_artifact_id); else sqlite3_bind_null(st.st, 15);
    if (command.result_context_blob_base64.has_value()) sqlite3_bind_text(st.st, 16, command.result_context_blob_base64->c_str(), -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(st.st, 16);
    if (command.result_context_version.has_value()) sqlite3_bind_int(st.st, 17, *command.result_context_version);
    else sqlite3_bind_null(st.st, 17);
    if (command.recorded_at_utc.has_value()) sqlite3_bind_int64(st.st, 18, command.recorded_at_utc->time_since_epoch().count());
    else sqlite3_bind_null(st.st, 18);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    return sqlite3_changes(db_) > 0;
}

bool SqliteAnalysisDb::CreateBattleSelectionPool(
    const CreateBattleSelectionPoolCommand& command,
    std::int64_t* selection_pool_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.battle_set_id <= 0
        || command.pool_name.empty()
        || command.criterion_kind == BattleSelectionCriterionKind::Unknown
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

    Statement insert_pool;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO ab_selection_pool(battle_set_id,turn_index,pool_name,criterion_kind,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5);",
            -1,
            &insert_pool.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(insert_pool.st, 1, command.battle_set_id);
    sqlite3_bind_int(insert_pool.st, 2, command.turn_index);
    sqlite3_bind_text(insert_pool.st, 3, command.pool_name.c_str(), -1, SQLITE_TRANSIENT);
    const auto criterion_kind = ToDbString(command.criterion_kind);
    sqlite3_bind_text(insert_pool.st, 4, criterion_kind.data(), static_cast<int>(criterion_kind.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_pool.st, 5, command.created_at_utc.time_since_epoch().count());
    if (sqlite3_step(insert_pool.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto selection_pool_id = sqlite3_last_insert_rowid(db_);
    if (!InsertBattleOutboxEvent(
            db_,
            command.event_id,
            "AnalysisBattle.SelectionPoolCreated.v1",
            "battle_set",
            std::to_string(command.battle_set_id),
            command.correlation_id,
            command.causation_id,
            command.created_at_utc.time_since_epoch().count(),
            "selection_pool",
            selection_pool_id,
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

    if (selection_pool_id_out) {
        *selection_pool_id_out = selection_pool_id;
    }
    return true;
}

bool SqliteAnalysisDb::EnsureBattleSelectionPool(
    const CreateBattleSelectionPoolCommand& command,
    std::int64_t* selection_pool_id_out,
    std::string* error_out) {
    if (selection_pool_id_out != nullptr) {
        *selection_pool_id_out = 0;
    }
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.battle_set_id <= 0 || command.pool_name.empty()) {
        if (error_out) *error_out = "battle_set_id and pool_name are required";
        return false;
    }

    Statement existing;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT selection_pool_id FROM ab_selection_pool WHERE battle_set_id=?1 AND turn_index=?2 AND pool_name=?3;",
            -1,
            &existing.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(existing.st, 1, command.battle_set_id);
    sqlite3_bind_int(existing.st, 2, command.turn_index);
    sqlite3_bind_text(existing.st, 3, command.pool_name.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(existing.st) == SQLITE_ROW) {
        if (selection_pool_id_out) *selection_pool_id_out = sqlite3_column_int64(existing.st, 0);
        return true;
    }

    return CreateBattleSelectionPool(command, selection_pool_id_out, error_out);
}

bool SqliteAnalysisDb::RecordBattleSelectionDecision(
    const RecordBattleSelectionDecisionCommand& command,
    std::int64_t* selection_decision_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.selection_pool_id <= 0 || command.turn_job_id <= 0 || command.decision_kind == BattleSelectionDecisionKind::Unknown || command.event_id.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    const auto battle_set_id = BattleSetIdForSelectionPool(db_, command.selection_pool_id);
    if (!battle_set_id.has_value()) {
        if (error_out) *error_out = "selection_pool_id does not resolve to battle_set";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement insert_decision;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT OR IGNORE INTO ab_selection_decision(selection_pool_id,turn_job_id,decision_kind,decision_reason,created_at_utc) "
            "VALUES(?1,?2,?3,?4,?5);",
            -1,
            &insert_decision.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(insert_decision.st, 1, command.selection_pool_id);
    sqlite3_bind_int64(insert_decision.st, 2, command.turn_job_id);
    const auto decision_kind = ToDbString(command.decision_kind);
    sqlite3_bind_text(insert_decision.st, 3, decision_kind.data(), static_cast<int>(decision_kind.size()), SQLITE_TRANSIENT);
    if (command.decision_reason.has_value()) sqlite3_bind_text(insert_decision.st, 4, command.decision_reason->c_str(), -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(insert_decision.st, 4);
    sqlite3_bind_int64(insert_decision.st, 5, command.created_at_utc.time_since_epoch().count());
    if (sqlite3_step(insert_decision.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    std::int64_t selection_decision_id = sqlite3_last_insert_rowid(db_);
    const bool inserted_decision = sqlite3_changes(db_) > 0;
    if (!inserted_decision) {
        Statement existing;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT selection_decision_id FROM ab_selection_decision WHERE selection_pool_id=?1 AND turn_job_id=?2;",
                -1,
                &existing.st,
                nullptr)
            == SQLITE_OK) {
            sqlite3_bind_int64(existing.st, 1, command.selection_pool_id);
            sqlite3_bind_int64(existing.st, 2, command.turn_job_id);
            if (sqlite3_step(existing.st) == SQLITE_ROW) {
                selection_decision_id = sqlite3_column_int64(existing.st, 0);
            }
        }
    }
    if (inserted_decision && !InsertBattleOutboxEvent(
            db_,
            command.event_id,
            "AnalysisBattle.SelectionDecisionRecorded.v1",
            "battle_set",
            std::to_string(battle_set_id.value()),
            command.correlation_id,
            command.causation_id,
            command.created_at_utc.time_since_epoch().count(),
            "selection_decision",
            selection_decision_id,
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

    if (selection_decision_id_out) {
        *selection_decision_id_out = selection_decision_id;
    }
    return true;
}

bool SqliteAnalysisDb::UpsertBattleTerminalFollowup(
    const UpsertBattleTerminalFollowupCommand& command,
    std::int64_t* terminal_followup_id_out,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (command.turn_job_id <= 0 || command.manual_followup_status == BattleManualFollowupStatus::Unknown || command.event_id.empty()) {
        if (error_out) *error_out = "required command fields are missing";
        return false;
    }

    const auto battle_set_id = BattleSetIdForTurnJob(db_, command.turn_job_id);
    if (!battle_set_id.has_value()) {
        if (error_out) *error_out = "turn_job_id does not resolve to battle_set";
        return false;
    }

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        return false;
    }

    Statement upsert_followup;
    if (sqlite3_prepare_v2(
            db_,
            "INSERT INTO ab_terminal_followup(turn_job_id,is_victory,manual_followup_status,recorded_dtm_artifact_id,recorded_dtmini_artifact_id,recorded_sav_artifact_id,note,updated_at_utc) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8) "
            "ON CONFLICT(turn_job_id) DO UPDATE SET "
            "is_victory=excluded.is_victory,"
            "manual_followup_status=excluded.manual_followup_status,"
            "recorded_dtm_artifact_id=excluded.recorded_dtm_artifact_id,"
            "recorded_dtmini_artifact_id=excluded.recorded_dtmini_artifact_id,"
            "recorded_sav_artifact_id=excluded.recorded_sav_artifact_id,"
            "note=excluded.note,"
            "updated_at_utc=excluded.updated_at_utc;",
            -1,
            &upsert_followup.st,
            nullptr)
        != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }

    sqlite3_bind_int64(upsert_followup.st, 1, command.turn_job_id);
    sqlite3_bind_int(upsert_followup.st, 2, command.is_victory ? 1 : 0);
    const auto manual_followup_status = ToDbString(command.manual_followup_status);
    sqlite3_bind_text(upsert_followup.st, 3, manual_followup_status.data(), static_cast<int>(manual_followup_status.size()), SQLITE_TRANSIENT);
    if (command.recorded_dtm_artifact_id.has_value()) sqlite3_bind_int64(upsert_followup.st, 4, command.recorded_dtm_artifact_id.value());
    else sqlite3_bind_null(upsert_followup.st, 4);
    if (command.recorded_dtmini_artifact_id.has_value()) sqlite3_bind_int64(upsert_followup.st, 5, command.recorded_dtmini_artifact_id.value());
    else sqlite3_bind_null(upsert_followup.st, 5);
    if (command.recorded_sav_artifact_id.has_value()) sqlite3_bind_int64(upsert_followup.st, 6, command.recorded_sav_artifact_id.value());
    else sqlite3_bind_null(upsert_followup.st, 6);
    if (command.note.has_value()) sqlite3_bind_text(upsert_followup.st, 7, command.note->c_str(), -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(upsert_followup.st, 7);
    sqlite3_bind_int64(upsert_followup.st, 8, command.updated_at_utc.time_since_epoch().count());
    if (sqlite3_step(upsert_followup.st) != SQLITE_DONE) {
        if (error_out != nullptr) {
            *error_out = sqlite3_errmsg(db_);
        }
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    const auto terminal_followup_id = TerminalFollowupIdForTurnJob(db_, command.turn_job_id);
    if (!terminal_followup_id.has_value()) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        if (error_out) *error_out = "turn_job terminal_followup_id could not be resolved";
        return false;
    }

    if (!InsertBattleOutboxEvent(
            db_,
            command.event_id,
            "AnalysisBattle.TerminalFollowupUpdated.v1",
            "battle_set",
            std::to_string(battle_set_id.value()),
            command.correlation_id,
            command.causation_id,
            command.updated_at_utc.time_since_epoch().count(),
            "terminal_followup",
            terminal_followup_id.value(),
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

    if (terminal_followup_id_out) {
        *terminal_followup_id_out = terminal_followup_id.value();
    }
    return true;
}

bool SqliteAnalysisDb::UpdateBattleSetStatus(
    std::int64_t battle_set_id,
    BattleSetStatus status,
    std::optional<types::UtcTimePoint> completed_at_utc,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (battle_set_id <= 0 || status == BattleSetStatus::Unknown) {
        if (error_out) *error_out = "battle_set_id and status are required";
        return false;
    }
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE ab_battle_set SET status=?2, completed_at_utc=?3 WHERE battle_set_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, battle_set_id);
    const auto status_text = ToDbString(status);
    sqlite3_bind_text(st.st, 2, status_text.data(), static_cast<int>(status_text.size()), SQLITE_TRANSIENT);
    if (completed_at_utc.has_value()) sqlite3_bind_int64(st.st, 3, completed_at_utc->time_since_epoch().count());
    else sqlite3_bind_null(st.st, 3);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    return sqlite3_changes(db_) > 0;
}

bool SqliteAnalysisDb::UpdateBattleTurnWaveStatus(
    std::int64_t wave_id,
    BattleTurnWaveStatus status,
    std::optional<types::UtcTimePoint> completed_at_utc,
    std::string* error_out) {
    if (db_ == nullptr) {
        if (error_out) *error_out = "database handle is null";
        return false;
    }
    if (wave_id <= 0 || status == BattleTurnWaveStatus::Unknown) {
        if (error_out) *error_out = "wave_id and status are required";
        return false;
    }
    Statement st;
    if (sqlite3_prepare_v2(
            db_,
            "UPDATE ab_turn_wave SET status=?2, completed_at_utc=?3 WHERE wave_id=?1;",
            -1,
            &st.st,
            nullptr)
        != SQLITE_OK) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.st, 1, wave_id);
    const auto status_text = ToDbString(status);
    sqlite3_bind_text(st.st, 2, status_text.data(), static_cast<int>(status_text.size()), SQLITE_TRANSIENT);
    if (completed_at_utc.has_value()) sqlite3_bind_int64(st.st, 3, completed_at_utc->time_since_epoch().count());
    else sqlite3_bind_null(st.st, 3);
    if (sqlite3_step(st.st) != SQLITE_DONE) {
        if (error_out) *error_out = sqlite3_errmsg(db_);
        return false;
    }
    return sqlite3_changes(db_) > 0;
}

std::optional<BattleSetSnapshot> SqliteAnalysisDb::GetBattleSet(std::int64_t battle_set_id) const {
    if (db_ == nullptr || battle_set_id <= 0) {
        return std::nullopt;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT battle_set_id,name,entry_savestate_id,battle_run_spec_id,explorer_settings_id,status,created_at_utc,completed_at_utc "
        "FROM ab_battle_set WHERE battle_set_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, battle_set_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    BattleSetSnapshot out{};
    out.battle_set_id = sqlite3_column_int64(st.st, 0);
    out.name = ColumnText(st.st, 1);
    out.entry_savestate_id = sqlite3_column_int64(st.st, 2);
    out.battle_run_spec_id = sqlite3_column_int64(st.st, 3);
    out.explorer_settings_id = sqlite3_column_int64(st.st, 4);
    out.status = ParseBattleSetStatus(ColumnText(st.st, 5));
    out.created_at_utc = ColumnTime(st.st, 6);
    out.completed_at_utc = ColumnTimeOptional(st.st, 7);
    return out;
}

std::vector<BattleSeedCandidateRow> SqliteAnalysisDb::ListBattleSeedCandidates(std::int64_t battle_set_id) const {
    std::vector<BattleSeedCandidateRow> rows;
    if (db_ == nullptr || battle_set_id <= 0) {
        return rows;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT seed_candidate_id,battle_set_id,source_unique_seed_id,seed_value,source_kind,candidate_status,created_at_utc "
        "FROM ab_seed_candidate WHERE battle_set_id=?1 ORDER BY seed_candidate_id ASC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_int64(st.st, 1, battle_set_id);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        BattleSeedCandidateRow row{};
        row.seed_candidate_id = sqlite3_column_int64(st.st, 0);
        row.battle_set_id = sqlite3_column_int64(st.st, 1);
        row.source_unique_seed_id = ColumnInt64Optional(st.st, 2);
        row.seed_value = sqlite3_column_int64(st.st, 3);
        row.source_kind = ParseBattleSeedCandidateSourceKind(ColumnText(st.st, 4));
        row.candidate_status = ParseBattleSeedCandidateStatus(ColumnText(st.st, 5));
        row.created_at_utc = ColumnTime(st.st, 6);
        rows.push_back(std::move(row));
    }
    return rows;
}

std::optional<BattleSeedCandidateRow> SqliteAnalysisDb::GetBattleSeedCandidate(std::int64_t seed_candidate_id) const {
    if (db_ == nullptr || seed_candidate_id <= 0) {
        return std::nullopt;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT seed_candidate_id,battle_set_id,source_unique_seed_id,seed_value,source_kind,candidate_status,created_at_utc "
        "FROM ab_seed_candidate WHERE seed_candidate_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, seed_candidate_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    BattleSeedCandidateRow row{};
    row.seed_candidate_id = sqlite3_column_int64(st.st, 0);
    row.battle_set_id = sqlite3_column_int64(st.st, 1);
    row.source_unique_seed_id = ColumnInt64Optional(st.st, 2);
    row.seed_value = sqlite3_column_int64(st.st, 3);
    row.source_kind = ParseBattleSeedCandidateSourceKind(ColumnText(st.st, 4));
    row.candidate_status = ParseBattleSeedCandidateStatus(ColumnText(st.st, 5));
    row.created_at_utc = ColumnTime(st.st, 6);
    return row;
}

std::optional<BattleTurnWaveSnapshot> SqliteAnalysisDb::GetBattleTurnWave(std::int64_t wave_id) const {
    if (db_ == nullptr || wave_id <= 0) {
        return std::nullopt;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT wave_id,battle_set_id,turn_index,context_probe_id,parent_wave_id,parent_turn_job_id,seed_candidate_id,selection_pool_id,status,created_at_utc,completed_at_utc "
        "FROM ab_turn_wave WHERE wave_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, wave_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    BattleTurnWaveSnapshot out{};
    out.wave_id = sqlite3_column_int64(st.st, 0);
    out.battle_set_id = sqlite3_column_int64(st.st, 1);
    out.turn_index = sqlite3_column_int(st.st, 2);
    out.context_probe_id = ColumnInt64Optional(st.st, 3);
    out.parent_wave_id = ColumnInt64Optional(st.st, 4);
    out.parent_turn_job_id = ColumnInt64Optional(st.st, 5);
    out.seed_candidate_id = sqlite3_column_int64(st.st, 6);
    out.selection_pool_id = ColumnInt64Optional(st.st, 7);
    out.status = ParseBattleTurnWaveStatus(ColumnText(st.st, 8));
    out.created_at_utc = ColumnTime(st.st, 9);
    out.completed_at_utc = ColumnTimeOptional(st.st, 10);
    return out;
}

std::vector<BattleTurnWaveSnapshot> SqliteAnalysisDb::ListBattleTurnWaves(std::int64_t battle_set_id) const {
    std::vector<BattleTurnWaveSnapshot> rows;
    if (db_ == nullptr || battle_set_id <= 0) {
        return rows;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT wave_id,battle_set_id,turn_index,context_probe_id,parent_wave_id,parent_turn_job_id,seed_candidate_id,selection_pool_id,status,created_at_utc,completed_at_utc "
        "FROM ab_turn_wave WHERE battle_set_id=?1 ORDER BY turn_index ASC, wave_id ASC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_int64(st.st, 1, battle_set_id);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        BattleTurnWaveSnapshot row{};
        row.wave_id = sqlite3_column_int64(st.st, 0);
        row.battle_set_id = sqlite3_column_int64(st.st, 1);
        row.turn_index = sqlite3_column_int(st.st, 2);
        row.context_probe_id = ColumnInt64Optional(st.st, 3);
        row.parent_wave_id = ColumnInt64Optional(st.st, 4);
        row.parent_turn_job_id = ColumnInt64Optional(st.st, 5);
        row.seed_candidate_id = sqlite3_column_int64(st.st, 6);
        row.selection_pool_id = ColumnInt64Optional(st.st, 7);
        row.status = ParseBattleTurnWaveStatus(ColumnText(st.st, 8));
        row.created_at_utc = ColumnTime(st.st, 9);
        row.completed_at_utc = ColumnTimeOptional(st.st, 10);
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<BattleTurnWaveSnapshot> SqliteAnalysisDb::ListBattleTurnWavesForContextProbe(std::int64_t context_probe_id) const {
    std::vector<BattleTurnWaveSnapshot> rows;
    if (db_ == nullptr || context_probe_id <= 0) {
        return rows;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT wave_id,battle_set_id,turn_index,context_probe_id,parent_wave_id,parent_turn_job_id,seed_candidate_id,selection_pool_id,status,created_at_utc,completed_at_utc "
        "FROM ab_turn_wave WHERE context_probe_id=?1 ORDER BY turn_index ASC, wave_id ASC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_int64(st.st, 1, context_probe_id);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        BattleTurnWaveSnapshot row{};
        row.wave_id = sqlite3_column_int64(st.st, 0);
        row.battle_set_id = sqlite3_column_int64(st.st, 1);
        row.turn_index = sqlite3_column_int(st.st, 2);
        row.context_probe_id = ColumnInt64Optional(st.st, 3);
        row.parent_wave_id = ColumnInt64Optional(st.st, 4);
        row.parent_turn_job_id = ColumnInt64Optional(st.st, 5);
        row.seed_candidate_id = sqlite3_column_int64(st.st, 6);
        row.selection_pool_id = ColumnInt64Optional(st.st, 7);
        row.status = ParseBattleTurnWaveStatus(ColumnText(st.st, 8));
        row.created_at_utc = ColumnTime(st.st, 9);
        row.completed_at_utc = ColumnTimeOptional(st.st, 10);
        rows.push_back(std::move(row));
    }
    return rows;
}

namespace {
BattleContextProbeSnapshot ReadBattleContextProbe(sqlite3_stmt* st) {
    BattleContextProbeSnapshot row{};
    row.context_probe_id = sqlite3_column_int64(st, 0);
    row.wave_id = sqlite3_column_int64(st, 1);
    row.source_savestate_id = sqlite3_column_int64(st, 2);
    row.exec_job_id = ColumnInt64Optional(st, 3);
    row.probe_status = ParseBattleContextProbeStatus(ColumnText(st, 4));
    if (sqlite3_column_type(st, 5) != SQLITE_NULL) {
        row.context_blob = ColumnBlob(st, 5);
    }
    row.context_version = ColumnIntOptional(st, 6);
    row.recorded_at_utc = ColumnTimeOptional(st, 7);
    row.created_at_utc = ColumnTime(st, 8);
    return row;
}

BattleTurnJobSnapshot ReadBattleTurnJob(sqlite3_stmt* st) {
    BattleTurnJobSnapshot row{};
    row.turn_job_id = sqlite3_column_int64(st, 0);
    row.wave_id = sqlite3_column_int64(st, 1);
    row.exec_job_id = ColumnInt64Optional(st, 2);
    row.plan_id = sqlite3_column_int64(st, 3);
    row.fake_attacks_this_turn = sqlite3_column_int(st, 4);
    row.fake_attacks_used_before = sqlite3_column_int(st, 5);
    row.job_state = ParseBattleTurnJobState(ColumnText(st, 6));
    row.started_at_utc = ColumnTimeOptional(st, 7);
    row.ended_at_utc = ColumnTimeOptional(st, 8);
    row.has_results = sqlite3_column_int(st, 9) != 0;
    row.vi_start = ColumnIntOptional(st, 10);
    row.vi_end = ColumnIntOptional(st, 11);
    row.delta_vi = ColumnIntOptional(st, 12);
    row.rng_seed = ColumnInt64Optional(st, 13);
    if (const auto value = ColumnIntOptional(st, 14); value.has_value()) {
        row.battle_outcome = static_cast<BattleTurnOutcome>(*value);
    }
    row.plan_materialize_err = ColumnIntOptional(st, 15);
    row.pred_passed = ColumnIntOptional(st, 16);
    row.pred_total = ColumnIntOptional(st, 17);
    row.pred_abort_run = ColumnIntOptional(st, 18);
    row.output_savestate_id = ColumnInt64Optional(st, 19);
    row.applied_input_artifact_id = ColumnInt64Optional(st, 20);
    row.result_context_blob_base64 = ColumnTextOptional(st, 21);
    row.result_context_version = ColumnIntOptional(st, 22);
    row.recorded_at_utc = ColumnTimeOptional(st, 23);
    return row;
}
} // namespace

std::optional<BattleContextProbeSnapshot> SqliteAnalysisDb::GetBattleContextProbe(std::int64_t context_probe_id) const {
    if (db_ == nullptr || context_probe_id <= 0) {
        return std::nullopt;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT context_probe_id,wave_id,source_savestate_id,exec_job_id,probe_status,context_blob,context_version,recorded_at_utc,created_at_utc "
        "FROM ab_battle_context_probe WHERE context_probe_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, context_probe_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    return ReadBattleContextProbe(st.st);
}

std::optional<BattleContextProbeSnapshot> SqliteAnalysisDb::GetBattleContextProbeForExecJob(std::int64_t exec_job_id) const {
    if (db_ == nullptr || exec_job_id <= 0) {
        return std::nullopt;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT context_probe_id,wave_id,source_savestate_id,exec_job_id,probe_status,context_blob,context_version,recorded_at_utc,created_at_utc "
        "FROM ab_battle_context_probe WHERE exec_job_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, exec_job_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    return ReadBattleContextProbe(st.st);
}

std::optional<BattleContextProbeSnapshot> SqliteAnalysisDb::GetLatestBattleContextForWave(std::int64_t wave_id) const {
    if (db_ == nullptr || wave_id <= 0) {
        return std::nullopt;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT context_probe_id,wave_id,source_savestate_id,exec_job_id,probe_status,context_blob,context_version,recorded_at_utc,created_at_utc "
        "FROM ab_battle_context_probe WHERE wave_id=?1 AND probe_status='SUCCEEDED' AND context_blob IS NOT NULL "
        "ORDER BY COALESCE(recorded_at_utc, created_at_utc) DESC, context_probe_id DESC LIMIT 1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, wave_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    return ReadBattleContextProbe(st.st);
}

std::optional<BattleTurnJobSnapshot> SqliteAnalysisDb::GetBattleTurnJobForExecJob(std::int64_t exec_job_id) const {
    if (db_ == nullptr || exec_job_id <= 0) {
        return std::nullopt;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT turn_job_id,wave_id,exec_job_id,plan_id,fake_attacks_this_turn,fake_attacks_used_before,job_state,"
        "started_at_utc,ended_at_utc,has_results,vi_start,vi_end,delta_vi,rng_seed,battle_outcome,plan_materialize_err,"
        "pred_passed,pred_total,pred_abort_run,output_savestate_id,applied_input_artifact_id,result_context_blob_base64,result_context_version,recorded_at_utc "
        "FROM ab_turn_job WHERE exec_job_id=?1;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_int64(st.st, 1, exec_job_id);
    if (sqlite3_step(st.st) != SQLITE_ROW) {
        return std::nullopt;
    }
    return ReadBattleTurnJob(st.st);
}

std::vector<BattleTurnJobSnapshot> SqliteAnalysisDb::ListBattleTurnJobsForWave(std::int64_t wave_id) const {
    std::vector<BattleTurnJobSnapshot> rows;
    if (db_ == nullptr || wave_id <= 0) {
        return rows;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT turn_job_id,wave_id,exec_job_id,plan_id,fake_attacks_this_turn,fake_attacks_used_before,job_state,"
        "started_at_utc,ended_at_utc,has_results,vi_start,vi_end,delta_vi,rng_seed,battle_outcome,plan_materialize_err,"
        "pred_passed,pred_total,pred_abort_run,output_savestate_id,applied_input_artifact_id,result_context_blob_base64,result_context_version,recorded_at_utc "
        "FROM ab_turn_job WHERE wave_id=?1 ORDER BY turn_job_id ASC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_int64(st.st, 1, wave_id);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        rows.push_back(ReadBattleTurnJob(st.st));
    }
    return rows;
}

std::vector<BattleTurnJobSnapshot> SqliteAnalysisDb::ListBattleTurnJobsForBattleTurn(
    std::int64_t battle_set_id,
    int turn_index) const {
    std::vector<BattleTurnJobSnapshot> rows;
    if (db_ == nullptr || battle_set_id <= 0 || turn_index <= 0) {
        return rows;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT j.turn_job_id,j.wave_id,j.exec_job_id,j.plan_id,j.fake_attacks_this_turn,j.fake_attacks_used_before,j.job_state,"
        "j.started_at_utc,j.ended_at_utc,j.has_results,j.vi_start,j.vi_end,j.delta_vi,j.rng_seed,j.battle_outcome,j.plan_materialize_err,"
        "j.pred_passed,j.pred_total,j.pred_abort_run,j.output_savestate_id,j.applied_input_artifact_id,j.result_context_blob_base64,j.result_context_version,j.recorded_at_utc "
        "FROM ab_turn_job j "
        "JOIN ab_turn_wave w ON w.wave_id=j.wave_id "
        "WHERE w.battle_set_id=?1 AND w.turn_index=?2 "
        "ORDER BY j.turn_job_id ASC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_int64(st.st, 1, battle_set_id);
    sqlite3_bind_int(st.st, 2, turn_index);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        rows.push_back(ReadBattleTurnJob(st.st));
    }
    return rows;
}

std::vector<BattleSelectionDecisionRow> SqliteAnalysisDb::ListBattleSelectionDecisionsForPool(
    std::int64_t selection_pool_id) const {
    std::vector<BattleSelectionDecisionRow> rows;
    if (db_ == nullptr || selection_pool_id <= 0) {
        return rows;
    }
    Statement st;
    constexpr const char* kSql =
        "SELECT selection_decision_id,selection_pool_id,turn_job_id,decision_kind,decision_reason,created_at_utc "
        "FROM ab_selection_decision WHERE selection_pool_id=?1 ORDER BY selection_decision_id ASC;";
    if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
        return rows;
    }
    sqlite3_bind_int64(st.st, 1, selection_pool_id);
    while (sqlite3_step(st.st) == SQLITE_ROW) {
        BattleSelectionDecisionRow row{};
        row.selection_decision_id = sqlite3_column_int64(st.st, 0);
        row.selection_pool_id = sqlite3_column_int64(st.st, 1);
        row.turn_job_id = sqlite3_column_int64(st.st, 2);
        row.decision_kind = ParseBattleSelectionDecisionKind(ColumnText(st.st, 3));
        const auto* reason = sqlite3_column_text(st.st, 4);
        row.decision_reason = reason ? std::optional<std::string>(reinterpret_cast<const char*>(reason)) : std::nullopt;
        row.created_at_utc = ColumnTime(st.st, 5);
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<events::EventEnvelope> SqliteAnalysisDb::ReadUnpublishedOutboxBatch(
    std::int64_t after_outbox_id,
    int max_batch_size) {
    std::vector<events::EventEnvelope> batch;
    if (db_ == nullptr || max_batch_size <= 0) {
        return batch;
    }

    const auto mode = ResolveOutboxMode(db_);
    if (mode == OutboxMode::None) {
        return batch;
    }

    Statement st;
    if (mode == OutboxMode::AnalysisSpine) {
        constexpr auto* kSql =
            "SELECT event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,"
            "correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id "
            "FROM asp_outbox_message "
            "WHERE outbox_id > ?1 "
            "AND published_at_utc IS NULL "
            "ORDER BY outbox_id ASC LIMIT ?2;";

        if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
            return batch;
        }

        sqlite3_bind_int64(st.st, 1, after_outbox_id);
        sqlite3_bind_int(st.st, 2, max_batch_size);

        while (sqlite3_step(st.st) == SQLITE_ROW) {
            batch.push_back(ReadEnvelope(st.st));
        }

        return batch;
    }

    const bool has_sp = TableExists(db_, "sp_outbox_message");
    const bool has_ab = TableExists(db_, "ab_outbox_message");
    if (!has_sp && !has_ab) {
        return batch;
    }

    std::string sql =
        "SELECT event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,"
        "correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id "
        "FROM (";
    if (has_sp) {
        sql +=
            "SELECT (outbox_id * 2) AS synthetic_outbox_id, "
            "event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,"
            "correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id "
            "FROM sp_outbox_message WHERE published_at_utc IS NULL ";
    }
    if (has_sp && has_ab) {
        sql += "UNION ALL ";
    }
    if (has_ab) {
        sql +=
            "SELECT (outbox_id * 2) + 1 AS synthetic_outbox_id, "
            "event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,"
            "correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id "
            "FROM ab_outbox_message WHERE published_at_utc IS NULL";
    }
    sql += ") q WHERE q.synthetic_outbox_id > ?1 ORDER BY q.synthetic_outbox_id ASC LIMIT ?2;";

    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st.st, nullptr) != SQLITE_OK) {
        return batch;
    }

    sqlite3_bind_int64(st.st, 1, after_outbox_id);
    sqlite3_bind_int(st.st, 2, max_batch_size);

    while (sqlite3_step(st.st) == SQLITE_ROW) {
        batch.push_back(ReadEnvelope(st.st));
    }

    return batch;
}

bool SqliteAnalysisDb::MarkOutboxPublished(
    std::int64_t outbox_id,
    types::UtcTimePoint published_at_utc) {
    if (db_ == nullptr || outbox_id <= 0) {
        return false;
    }

    const auto mode = ResolveOutboxMode(db_);
    if (mode == OutboxMode::None) {
        return false;
    }

    Statement st;
    if (mode == OutboxMode::AnalysisSpine) {
        constexpr auto* kSql =
            "UPDATE asp_outbox_message "
            "SET published_at_utc=?2 "
            "WHERE outbox_id=?1;";
        if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
            return false;
        }

        sqlite3_bind_int64(st.st, 1, outbox_id);
        sqlite3_bind_int64(st.st, 2, published_at_utc.time_since_epoch().count());
        return sqlite3_step(st.st) == SQLITE_DONE;
    }

    const bool is_ab = (outbox_id % 2) == 1;
    const auto table_outbox_id = outbox_id / 2;
    if (table_outbox_id <= 0) {
        return false;
    }

    if ((is_ab && !TableExists(db_, "ab_outbox_message")) || (!is_ab && !TableExists(db_, "sp_outbox_message"))) {
        return false;
    }

    const auto* sql = is_ab
        ? "UPDATE ab_outbox_message SET published_at_utc=?2 WHERE outbox_id=?1;"
        : "UPDATE sp_outbox_message SET published_at_utc=?2 WHERE outbox_id=?1;";

    if (sqlite3_prepare_v2(db_, sql, -1, &st.st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(st.st, 1, table_outbox_id);
    sqlite3_bind_int64(st.st, 2, published_at_utc.time_since_epoch().count());
    return sqlite3_step(st.st) == SQLITE_DONE;
}

bool SqliteAnalysisDb::MarkOutboxPublishFailure(
    std::int64_t outbox_id,
    std::string_view last_error) {
    if (db_ == nullptr || outbox_id <= 0) {
        return false;
    }

    const auto mode = ResolveOutboxMode(db_);
    if (mode == OutboxMode::None) {
        return false;
    }

    Statement st;
    if (mode == OutboxMode::AnalysisSpine) {
        constexpr auto* kSql =
            "UPDATE asp_outbox_message "
            "SET attempt_count=attempt_count+1, last_error=?2 "
            "WHERE outbox_id=?1;";
        if (sqlite3_prepare_v2(db_, kSql, -1, &st.st, nullptr) != SQLITE_OK) {
            return false;
        }

        sqlite3_bind_int64(st.st, 1, outbox_id);
        sqlite3_bind_text(st.st, 2, last_error.data(), static_cast<int>(last_error.size()), SQLITE_TRANSIENT);
        return sqlite3_step(st.st) == SQLITE_DONE;
    }

    const bool is_ab = (outbox_id % 2) == 1;
    const auto table_outbox_id = outbox_id / 2;
    if (table_outbox_id <= 0) {
        return false;
    }

    if ((is_ab && !TableExists(db_, "ab_outbox_message")) || (!is_ab && !TableExists(db_, "sp_outbox_message"))) {
        return false;
    }

    const auto* sql = is_ab
        ? "UPDATE ab_outbox_message SET attempt_count=attempt_count+1, last_error=?2 WHERE outbox_id=?1;"
        : "UPDATE sp_outbox_message SET attempt_count=attempt_count+1, last_error=?2 WHERE outbox_id=?1;";

    if (sqlite3_prepare_v2(db_, sql, -1, &st.st, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(st.st, 1, table_outbox_id);
    sqlite3_bind_text(st.st, 2, last_error.data(), static_cast<int>(last_error.size()), SQLITE_TRANSIENT);
    return sqlite3_step(st.st) == SQLITE_DONE;
}

retention::OutboxRetentionPreview SqliteAnalysisDb::PreviewOutboxRetention(
    const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
    types::UtcTimePoint now_utc,
    const retention::OutboxRetentionPolicy& policy) const {
    std::int64_t max_outbox_id = 0;
    Statement st;
    const auto mode = ResolveOutboxMode(db_);
    if (mode == OutboxMode::AnalysisSpine) {
        if (sqlite3_prepare_v2(db_, "SELECT COALESCE(MAX(outbox_id), 0) FROM asp_outbox_message;", -1, &st.st, nullptr)
                == SQLITE_OK
            && sqlite3_step(st.st) == SQLITE_ROW) {
            max_outbox_id = sqlite3_column_int64(st.st, 0);
        }
    }
    else if (mode == OutboxMode::SplitSeedProbeBattle) {
        if (sqlite3_prepare_v2(
                db_,
                "SELECT MAX(v) FROM ("
                "SELECT COALESCE(MAX(outbox_id),0) AS v FROM sp_outbox_message "
                "UNION ALL "
                "SELECT COALESCE(MAX(outbox_id),0) AS v FROM ab_outbox_message"
                ");",
                -1,
                &st.st,
                nullptr)
                == SQLITE_OK
            && sqlite3_step(st.st) == SQLITE_ROW) {
            max_outbox_id = sqlite3_column_int64(st.st, 0);
        }
    }
    return retention::BuildOutboxRetentionPreview(max_outbox_id, subscriptions, now_utc, policy);
}

bool SqliteAnalysisDb::PurgeOutboxThroughRetentionFloor(
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

    int deleted = 0;
    const auto mode = ResolveOutboxMode(db_);
    auto delete_from = [&](const char* table_name, int limit) -> bool {
        Statement st;
        std::string sql =
            "DELETE FROM " + std::string(table_name)
            + " WHERE outbox_id IN (SELECT outbox_id FROM " + std::string(table_name)
            + " WHERE published_at_utc IS NOT NULL AND outbox_id < ?1 ORDER BY outbox_id ASC LIMIT ?2);";
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st.st, nullptr) != SQLITE_OK) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_int64(st.st, 1, preview.safe_purge_floor_outbox_id.value());
        sqlite3_bind_int(st.st, 2, limit);
        if (sqlite3_step(st.st) != SQLITE_DONE) {
            if (error_out) *error_out = sqlite3_errmsg(db_);
            return false;
        }
        deleted += sqlite3_changes(db_);
        return true;
    };

    if (mode == OutboxMode::AnalysisSpine) {
        if (!delete_from("asp_outbox_message", max_rows)) return false;
    }
    else if (mode == OutboxMode::SplitSeedProbeBattle) {
        const int sp_limit = std::max(1, max_rows / 2);
        const int ab_limit = std::max(1, max_rows - sp_limit);
        if (TableExists(db_, "sp_outbox_message") && !delete_from("sp_outbox_message", sp_limit)) return false;
        if (TableExists(db_, "ab_outbox_message") && !delete_from("ab_outbox_message", ab_limit)) return false;
    }

    if (rows_deleted_out) *rows_deleted_out = deleted;
    return true;
}

std::optional<SeedProbePayloadRecord> SqliteAnalysisDb::ResolveSeedProbePayload(
    int event_version,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    return ResolveSeedProbeByKind(seed_probe_row_resolver_, event_version, payload_ref_kind, payload_ref_id);
}

std::optional<SeedProbePayloadRecord> SqliteAnalysisDb::ResolveSeedProbePayload(
    const events::EventEnvelope& envelope) const {
    if (!events::ValidateAnalysisSeedProbePayloadV1(envelope)) {
        return std::nullopt;
    }

    const auto contract = events::ResolvePayloadResolverContract(envelope.event_type, envelope.event_version);
    if (!contract.has_value() || contract.value() != events::PayloadResolverContract::AnalysisSeedProbeV1) {
        return std::nullopt;
    }

    if (envelope.event_type == "AnalysisSeedProbe.SetCreated.v1") {
        const auto view = seed_probe_row_resolver_.ResolveSeedProbeSetCreated(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        SeedProbePayloadRecord record{};
        record.probe_set_id = view->probe_set_id;
        return record;
    }
    if (envelope.event_type == "AnalysisSeedProbe.RunRequested.v1") {
        const auto view = seed_probe_row_resolver_.ResolveSeedProbeRunRequested(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        SeedProbePayloadRecord record{};
        record.probe_set_id = view->probe_set_id;
        record.probe_run_id = view->probe_run_id;
        return record;
    }
    if (envelope.event_type == "AnalysisSeedProbe.NeutralSeedRecorded.v1") {
        const auto view = seed_probe_row_resolver_.ResolveSeedProbeNeutralSeedRecorded(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        SeedProbePayloadRecord record{};
        record.probe_run_id = view->probe_run_id;
        record.probe_result_id = view->probe_result_id;
        return record;
    }
    if (envelope.event_type == "AnalysisSeedProbe.GridSeedRecorded.v1") {
        const auto view = seed_probe_row_resolver_.ResolveSeedProbeGridSeedRecorded(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        SeedProbePayloadRecord record{};
        record.probe_run_id = view->probe_run_id;
        record.probe_result_id = view->probe_result_id;
        return record;
    }
    if (envelope.event_type == "AnalysisSeedProbe.UniqueSeedRecorded.v1") {
        const auto view = seed_probe_row_resolver_.ResolveSeedProbeUniqueSeedRecorded(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        SeedProbePayloadRecord record{};
        record.probe_run_id = view->probe_run_id;
        record.probe_result_id = view->probe_result_id;
        return record;
    }
    if (envelope.event_type == "AnalysisSeedProbe.EncounterProjectionRecorded.v1") {
        const auto view = seed_probe_row_resolver_.ResolveSeedProbeEncounterProjectionRecorded(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        SeedProbePayloadRecord record{};
        record.probe_run_id = view->probe_run_id;
        return record;
    }
    if (envelope.event_type == "AnalysisSeedProbe.RunCompleted.v1") {
        const auto view = seed_probe_row_resolver_.ResolveSeedProbeRunCompleted(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        SeedProbePayloadRecord record{};
        record.probe_run_id = view->probe_run_id;
        record.probe_result_id = view->probe_result_id;
        return record;
    }

    return std::nullopt;
}

std::optional<BattlePayloadRecord> SqliteAnalysisDb::ResolveBattlePayload(
    int event_version,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    return ResolveBattleByKind(battle_row_resolver_, event_version, payload_ref_kind, payload_ref_id);
}

std::optional<BattlePayloadRecord> SqliteAnalysisDb::ResolveBattlePayload(
    const events::EventEnvelope& envelope) const {
    if (!events::ValidateAnalysisBattlePayloadV1(envelope)) {
        return std::nullopt;
    }

    const auto contract = events::ResolvePayloadResolverContract(envelope.event_type, envelope.event_version);
    if (!contract.has_value() || contract.value() != events::PayloadResolverContract::AnalysisBattleV1) {
        return std::nullopt;
    }

    if (envelope.event_type == "AnalysisBattle.BattleSetCreated.v1") {
        const auto view = battle_row_resolver_.ResolveBattleSetCreated(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        BattlePayloadRecord record{};
        record.battle_set_id = view->battle_set_id;
        return record;
    }
    if (envelope.event_type == "AnalysisBattle.SeedCandidateAdded.v1") {
        const auto view = battle_row_resolver_.ResolveBattleSeedCandidateAdded(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        BattlePayloadRecord record{};
        record.battle_set_id = view->battle_set_id;
        return record;
    }
    if (envelope.event_type == "AnalysisBattle.TurnWaveCreated.v1") {
        const auto view = battle_row_resolver_.ResolveBattleTurnWaveCreated(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        BattlePayloadRecord record{};
        record.battle_set_id = view->battle_set_id;
        record.wave_id = view->wave_id;
        return record;
    }
    if (envelope.event_type == "AnalysisBattle.TurnJobRecorded.v1") {
        const auto view = battle_row_resolver_.ResolveBattleTurnJobRecorded(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        BattlePayloadRecord record{};
        record.battle_set_id = view->battle_set_id;
        record.wave_id = view->wave_id;
        record.turn_job_id = view->turn_job_id;
        return record;
    }
    if (envelope.event_type == "AnalysisBattle.SelectionPoolCreated.v1") {
        const auto view = battle_row_resolver_.ResolveBattleSelectionPoolCreated(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        BattlePayloadRecord record{};
        record.battle_set_id = view->battle_set_id;
        return record;
    }
    if (envelope.event_type == "AnalysisBattle.SelectionDecisionRecorded.v1") {
        const auto view = battle_row_resolver_.ResolveBattleSelectionDecisionRecorded(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        BattlePayloadRecord record{};
        record.battle_set_id = view->battle_set_id;
        record.turn_job_id = view->turn_job_id;
        return record;
    }
    if (envelope.event_type == "AnalysisBattle.TerminalFollowupUpdated.v1") {
        const auto view = battle_row_resolver_.ResolveBattleTerminalFollowupUpdated(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        BattlePayloadRecord record{};
        record.battle_set_id = view->battle_set_id;
        record.turn_job_id = view->turn_job_id;
        return record;
    }

    return std::nullopt;
}

std::optional<SpinePayloadRecord> SqliteAnalysisDb::ResolveSpinePayload(
    int event_version,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    return ResolveSpineByKind(spine_row_resolver_, event_version, payload_ref_kind, payload_ref_id);
}

std::optional<SpinePayloadRecord> SqliteAnalysisDb::ResolveSpinePayload(
    const events::EventEnvelope& envelope) const {
    if (!events::ValidateAnalysisSpinePayloadV1(envelope)) {
        return std::nullopt;
    }

    const auto contract = events::ResolvePayloadResolverContract(envelope.event_type, envelope.event_version);
    if (!contract.has_value() || contract.value() != events::PayloadResolverContract::AnalysisSpineV1) {
        return std::nullopt;
    }

    if (envelope.event_type == "AnalysisSpine.RunCreated.v1") {
        const auto view = spine_row_resolver_.ResolveSpineRunCreated(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        SpinePayloadRecord record{};
        record.run_id = view->run_id;
        return record;
    }
    if (envelope.event_type == "AnalysisSpine.StateRefRegistered.v1") {
        const auto view = spine_row_resolver_.ResolveSpineStateRefRegistered(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        SpinePayloadRecord record{};
        record.run_id = view->run_id;
        record.state_ref_id = view->state_ref_id;
        return record;
    }
    if (envelope.event_type == "AnalysisSpine.LineageEdgeAdded.v1") {
        const auto view = spine_row_resolver_.ResolveSpineLineageEdgeAdded(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        SpinePayloadRecord record{};
        record.run_id = view->child_run_id;
        record.lineage_edge_id = view->lineage_edge_id;
        return record;
    }
    if (envelope.event_type == "AnalysisSpine.ArtifactLinked.v1") {
        const auto view = spine_row_resolver_.ResolveSpineArtifactLinked(envelope.payload_ref_kind, envelope.payload_ref_id);
        if (!view.has_value()) {
            return std::nullopt;
        }

        SpinePayloadRecord record{};
        record.run_id = view->run_id;
        record.artifact_ref_id = view->artifact_ref_id;
        return record;
    }

    return std::nullopt;
}

} // namespace simcore::db::analysis
