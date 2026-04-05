#pragma once

#include <cstdint>

namespace simcore::db::events {

// Typed v1 payload view for Execution workflow/job events.
struct ExecutionWorkflowJobPayloadView {
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::int64_t workflow_edge_id = 0;
    std::int64_t job_set_id = 0;
    std::int64_t job_id = 0;
};

// Typed v1 payload view for AnalysisSeedProbe events.
struct AnalysisSeedProbePayloadView {
    std::int64_t probe_set_id = 0;
    std::int64_t probe_run_id = 0;
    std::int64_t probe_result_id = 0;
};

// Typed v1 payload view for AnalysisBattle events.
struct AnalysisBattlePayloadView {
    std::int64_t battle_set_id = 0;
    std::int64_t wave_id = 0;
    std::int64_t turn_job_id = 0;
};

// Typed v1 payload view for State artifact events.
struct StateArtifactPayloadView {
    std::int64_t artifact_id = 0;
    std::int64_t savestate_id = 0;
    std::int64_t tas_variant_id = 0;
};

// Typed v1 payload view for Archive package events.
struct ArchivePackagePayloadView {
    std::int64_t archive_package_id = 0;
    std::int64_t archive_item_id = 0;
    std::int64_t rehydrate_request_id = 0;
};

} // namespace simcore::db::events
