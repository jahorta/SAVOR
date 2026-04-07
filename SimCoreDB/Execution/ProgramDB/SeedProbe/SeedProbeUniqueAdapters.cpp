#include "SeedProbeUniqueAdapters.h"

#include <algorithm>
#include <set>
#include <string>
#include <utility>

#include "../../../Common/Types/UtcTimestamp.h"
#include "../../../../SimCore/Phases/RNGSeedDeltaMap.h"

namespace simcore::db::execution::programdb::seedprobe {

SeedProbeUniqueTransitionHandler::SeedProbeUniqueTransitionHandler(CompletionGateFn completion_gate)
    : completion_gate_(std::move(completion_gate)) {
}

WorkflowTransitionDecision SeedProbeUniqueTransitionHandler::EvaluateTransition(const WorkflowTransitionContext& context) const {
    WorkflowTransitionDecision decision{};

    if (context.step_key == "Unique") {
        decision.should_advance = true;
        decision.next_step_key = "Done";
        return decision;
    }

    if (context.step_key != "Grid") {
        decision.should_advance = true;
        return decision;
    }

    if (!completion_gate_ || !completion_gate_(context)) {
        decision.should_advance = false;
        decision.next_step_key = "Unique";
        decision.blocked_reason = "Grid completion gate not satisfied";
        return decision;
    }

    decision.should_advance = true;
    decision.next_step_key = "Unique";
    return decision;
}

SeedProbeUniqueJobPersistenceAdapter::SeedProbeUniqueJobPersistenceAdapter(
    simcore::db::IExecutionDb* execution_db,
    simcore::db::IAnalysisDb* analysis_db,
    SeedProbeGridBlueprintConfig blueprint,
    UniqueIni unique_ini)
    : execution_db_(execution_db)
    , analysis_db_(analysis_db)
    , blueprint_(std::move(blueprint))
    , unique_ini_(unique_ini) {
}

JobPersistenceRecord SeedProbeUniqueJobPersistenceAdapter::EncodeForQueueing(std::int64_t domain_ref_id) const {
    JobPersistenceRecord persisted{};
    persisted.program_ref_kind = "sp_probe_run";
    persisted.program_ref_id = domain_ref_id;
    persisted.program_version = blueprint_.program_version;
    persisted.fingerprint = "PK=3;phase=unique;probe_run_id=" + std::to_string(domain_ref_id);

    if (execution_db_ == nullptr || analysis_db_ == nullptr || domain_ref_id <= 0) {
        return persisted;
    }

    const auto neutral_seed = analysis_db_->LookupSeedProbeNeutralSeed(domain_ref_id);
    const auto grid_rows = analysis_db_->ListSeedProbeGridSeeds(domain_ref_id);
    if (!neutral_seed.has_value() || grid_rows.empty()) {
        return persisted;
    }

    simcore::RandSeedProbeResult grid{};
    grid.base_seed = static_cast<std::uint32_t>(*neutral_seed);
    std::set<std::int64_t> distinct_deltas;
    std::int64_t probe_result_id = 0;
    for (const auto& row : grid_rows) {
        probe_result_id = std::max(probe_result_id, row.probe_result_id);
        distinct_deltas.insert(row.seed_delta);
        simcore::RandSeedProbeEntry entry{};
        entry.delta = row.seed_delta;
        entry.seed = static_cast<std::uint32_t>(row.seed_value);
        entry.ok = true;
        if (row.source_family == "main" || row.source_family == "MAIN") {
            entry.family = simcore::SeedFamily::Main;
        } else if (row.source_family == "cstick" || row.source_family == "CSTICK") {
            entry.family = simcore::SeedFamily::CStick;
        } else {
            entry.family = simcore::SeedFamily::Triggers;
        }
        entry.x = static_cast<std::uint8_t>(row.axis_x);
        entry.y = static_cast<std::uint8_t>(row.axis_y);
        grid.entries.push_back(entry);
    }

    const auto planned = simcore::PlanJCTComboSamples(
        grid,
        static_cast<std::uint32_t>(std::max(unique_ini_.combo_attempts_per_target, 1)),
        static_cast<std::uint32_t>(std::max(unique_ini_.combo_sampler_tries, 1)));

    std::int64_t root_job_set_id = 0;
    std::string error;
    if (!execution_db_->CreateJobSet(
            {
                .program_kind = 3,
                .purpose = "SeedProbe Unique",
                .created_by = std::string("seedprobe_unique_adapter"),
                .expected_total = static_cast<int>(planned.samples.size()),
                .domain_ref_kind = std::string("sp_probe_run"),
                .domain_ref_id = domain_ref_id,
                .meta_note = std::string("phase=Unique"),
            },
            &root_job_set_id,
            &error)
        || root_job_set_id <= 0) {
        return persisted;
    }

    for (const auto& sample : planned.samples) {
        std::int64_t child_job_set_id = 0;
        const auto child_expected = static_cast<int>(sample.frames.size());
        if (!execution_db_->CreateJobSet(
                {
                    .parent_job_set_id = root_job_set_id,
                    .program_kind = 3,
                    .purpose = "SeedProbe Unique Delta",
                    .created_by = std::string("seedprobe_unique_adapter"),
                    .expected_total = child_expected,
                    .domain_ref_kind = std::string("sp_probe_run"),
                    .domain_ref_id = domain_ref_id,
                    .meta_note = std::string("expected_delta=") + std::to_string(sample.target_delta),
                },
                &child_job_set_id,
                &error)
            || child_job_set_id <= 0) {
            continue;
        }

        for (const auto& frame : sample.frames) {
            const auto frame_hex = frame.to_frame_hex();
            simcore::db::EnqueueJobCommand enqueue{};
            enqueue.job_set_id = child_job_set_id;
            enqueue.program_kind = 3;
            enqueue.program_version = blueprint_.program_version;
            enqueue.program_ref_kind = "sp_probe_run";
            enqueue.program_ref_id = domain_ref_id;
            enqueue.fingerprint = "PK=3;phase=unique;probe_run_id=" + std::to_string(domain_ref_id)
                + ";probe_result_id=" + std::to_string(probe_result_id)
                + ";expected_delta=" + std::to_string(sample.target_delta)
                + ";frame=" + frame_hex;
            enqueue.priority = 0;
            enqueue.max_attempts = 2;
            std::int64_t ignored_job_id = 0;
            (void)execution_db_->EnqueueJob(enqueue, &ignored_job_id, &error);
        }
    }

    return persisted;
}

std::int64_t SeedProbeUniqueJobPersistenceAdapter::DecodeDomainRefId(const JobPersistenceRecord& persisted) const {
    return persisted.program_ref_id;
}

SeedProbeUniqueRuntimeInitAdapter::SeedProbeUniqueRuntimeInitAdapter(
    simcore::db::IExecutionDb* execution_db,
    const simcore::db::IAnalysisDb* analysis_db)
    : execution_db_(execution_db)
    , analysis_db_(analysis_db) {
}

RuntimeInitRequest SeedProbeUniqueRuntimeInitAdapter::BuildRuntimeInit(std::int64_t job_id) const {
    RuntimeInitRequest request{};
    request.savestate_ref_kind = "analysis_savestate";
    request.bootstrap_profile = "seedprobe.unique";
    if (execution_db_ == nullptr || analysis_db_ == nullptr) {
        return request;
    }
    const auto job = execution_db_->GetJob(job_id);
    if (!job.has_value() || job->program_ref_kind != "sp_probe_run") {
        return request;
    }
    const auto savestate_id = analysis_db_->LookupSeedProbeRunSavestateId(job->program_ref_id);
    if (savestate_id.has_value()) {
        request.savestate_ref_id = *savestate_id;
    }
    return request;
}

SeedProbeUniqueResultMapper::SeedProbeUniqueResultMapper(simcore::db::IExecutionDb* execution_db, simcore::db::IAnalysisDb* analysis_db)
    : execution_db_(execution_db)
    , analysis_db_(analysis_db) {
}

ResultMapPayload SeedProbeUniqueResultMapper::MapPrimaryResult(std::int64_t job_id, const std::string& result_ini) const {
    ResultMapPayload payload{};
    payload.result_kind = "analysisseedprobe.unique.unavailable";
    if (execution_db_ == nullptr || analysis_db_ == nullptr) {
        return payload;
    }
    const auto job = execution_db_->GetJob(job_id);
    if (!job.has_value() || job->program_ref_kind != "sp_probe_run") {
        return payload;
    }

    const auto parsed = ResultsIni::from_section(IniDoc::parse(result_ini));
    const auto neutral_seed = analysis_db_->LookupSeedProbeNeutralSeed(job->program_ref_id);
    if (!neutral_seed.has_value()) {
        payload.result_kind = "analysisseedprobe.unique.missing_neutral";
        return payload;
    }
    const auto observed_delta = static_cast<std::int64_t>(parsed.rng_seed) - *neutral_seed;

    const bool seen = analysis_db_->HasSeedProbeUniqueSeedDelta(job->program_ref_id, observed_delta);
    if (seen) {
        payload.result_kind = "analysisseedprobe.unique.duplicate";
        (void)execution_db_->MarkQueuedJobsSuperseded(job->job_set_id, job_id, nullptr);
        return payload;
    }

    const auto probe_result_id = analysis_db_->LookupSeedProbeResultId(job->program_ref_id);
    if (!probe_result_id.has_value()) {
        payload.result_kind = "analysisseedprobe.unique.missing_probe_result";
        return payload;
    }

    RecordSeedProbeUniqueSeedCommand cmd{};
    cmd.probe_result_id = *probe_result_id;
    cmd.input_frame_id = 1;
    cmd.seed_value = parsed.rng_seed;
    cmd.seed_delta = observed_delta;
    cmd.recorded_at_utc = simcore::db::types::UtcNow();
    cmd.event_id = "seedprobe-unique-" + std::to_string(job_id);
    cmd.correlation_id = "seedprobe-run-" + std::to_string(job->program_ref_id);
    cmd.causation_id = "job-" + std::to_string(job_id);

    std::int64_t unique_seed_id = 0;
    std::string error;
    if (!analysis_db_->RecordSeedProbeUniqueSeed(cmd, &unique_seed_id, &error)) {
        payload.result_kind = "analysisseedprobe.unique.error";
        return payload;
    }

    payload.result_kind = "analysisseedprobe.unique.winner";
    payload.result_ref_id = unique_seed_id;
    (void)execution_db_->MarkQueuedJobsSuperseded(job->job_set_id, job_id, nullptr);
    return payload;
}

std::optional<ResultArtifactRef> SeedProbeUniqueResultMapper::MapPrimaryArtifact(std::int64_t /*job_id*/) const {
    return std::nullopt;
}

} // namespace simcore::db::execution::programdb::seedprobe
