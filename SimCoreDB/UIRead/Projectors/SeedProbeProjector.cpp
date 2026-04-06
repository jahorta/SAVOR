#include "SeedProbeProjector.h"

#include "ProjectorContract.h"

namespace simcore::db::uiread::projectors {

SeedProbeProjector::SeedProbeProjector(sqlite3* db)
    : db_(db) {
}

bool SeedProbeProjector::ProjectAll(std::string* error_out) {
    char* err = nullptr;
    constexpr const char* kSql =
        "BEGIN IMMEDIATE;"
        "INSERT INTO ui_seed_probe_summary(probe_run_id,probe_set_id,status,neutral_seed_value,grid_count,unique_count,requested_at_utc,completed_at_utc) "
        "SELECT r.probe_run_id,r.probe_set_id,r.status,res.neutral_seed_value,res.grid_count,res.unique_count,r.requested_at_utc,r.completed_at_utc "
        "FROM sp_probe_run r "
        "LEFT JOIN sp_probe_result res ON res.probe_run_id=r.probe_run_id "
        "ON CONFLICT(probe_run_id) DO UPDATE SET "
        "probe_set_id=excluded.probe_set_id,status=excluded.status,neutral_seed_value=excluded.neutral_seed_value,"
        "grid_count=excluded.grid_count,unique_count=excluded.unique_count,requested_at_utc=excluded.requested_at_utc,completed_at_utc=excluded.completed_at_utc;"
        "DELETE FROM ui_seed_probe_delta_point;"
        "INSERT INTO ui_seed_probe_delta_point(delta_point_id,probe_run_id,source_family,axis_x,axis_y,seed_value,seed_delta) "
        "SELECT g.grid_seed_id,res.probe_run_id,g.source_family,xy.x,xy.y,g.seed_value,g.seed_delta "
        "FROM sp_grid_seed g "
        "JOIN sp_probe_result res ON res.probe_result_id=g.probe_result_id "
        "JOIN sp_axis_xy xy ON xy.axis_xy_id=g.axis_xy_id "
        "ON CONFLICT(probe_run_id,source_family,axis_x,axis_y,seed_value) DO UPDATE SET seed_delta=excluded.seed_delta;"
        "DELETE FROM ui_seed_probe_unique_value;"
        "INSERT INTO ui_seed_probe_unique_value(unique_value_id,probe_run_id,seed_value,seed_delta,main_x,main_y,cstick_x,cstick_y,trigger_x,trigger_y) "
        "SELECT u.unique_seed_id,res.probe_run_id,u.seed_value,u.seed_delta,m.x,m.y,c.x,c.y,t.x,t.y "
        "FROM sp_unique_seed u "
        "JOIN sp_probe_result res ON res.probe_result_id=u.probe_result_id "
        "JOIN sp_input_frame f ON f.input_frame_id=u.input_frame_id "
        "JOIN sp_axis_xy m ON m.axis_xy_id=f.main_axis_xy_id "
        "JOIN sp_axis_xy c ON c.axis_xy_id=f.cstick_axis_xy_id "
        "JOIN sp_axis_xy t ON t.axis_xy_id=f.trigger_axis_xy_id;"
        "COMMIT;";
    if (sqlite3_exec(db_, kSql, nullptr, nullptr, &err) != SQLITE_OK) {
        if (error_out) *error_out = err ? err : sqlite3_errmsg(db_);
        sqlite3_free(err);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    return true;
}

bool SeedProbeProjector::ProjectFromOutbox(const std::string& projector_name, int max_batch_size, std::string* error_out, int max_attempts) {
    if (!ValidateProjectorContractInputs(projector_name, max_batch_size, max_attempts, error_out)) {
        return false;
    }

    const auto project_all = [this](const events::EventEnvelope&, std::string* handler_error) {
        return ProjectAll(handler_error);
    };

    const std::vector<events::OutboxRelayDispatchBinding> bindings{
        { { "AnalysisSeedProbe.SetCreated.v1", 1 }, project_all },
        { { "AnalysisSeedProbe.RunRequested.v1", 1 }, project_all },
        { { "AnalysisSeedProbe.NeutralSeedRecorded.v1", 1 }, project_all },
        { { "AnalysisSeedProbe.GridSeedRecorded.v1", 1 }, project_all },
        { { "AnalysisSeedProbe.UniqueSeedRecorded.v1", 1 }, project_all },
        { { "AnalysisSeedProbe.EncounterProjectionRecorded.v1", 1 }, project_all },
        { { "AnalysisSeedProbe.RunCompleted.v1", 1 }, project_all },
    };

    return RunProjectorRelay(
        db_,
        projector_name,
        {
            .db = db_,
            .outbox_table = "sp_outbox_message",
            .context_name = "AnalysisSeedProbe",
            .aggregate_kind = "probe_run",
            .payload_ref_kind = "",
            .max_attempts = max_attempts,
        },
        bindings,
        max_batch_size,
        error_out);
}

} // namespace simcore::db::uiread::projectors
