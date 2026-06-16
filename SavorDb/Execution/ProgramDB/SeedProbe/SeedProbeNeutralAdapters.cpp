#include "SeedProbeNeutralAdapters.h"

#include <sstream>

#include "../../Jobs/JobEventOrchestration.h"
#include "../../../Common/Types/UtcTimestamp.h"
#include "../../../../SavorCore/Runner/IPC/Wire.h"
#include "../../../../SavorCore/Runner/Parallel/PRTypes.h"
#include "../../../../SavorCore/Runner/Script/CtxRegistry.h"
#include "../../../../SavorCore/Phases/Programs/SeedProbe/SeedProbePayload.h"
#include "SeedProbeContracts.h"

namespace savor::db::execution::programdb::seedprobe {

namespace {

constexpr std::int32_t kProgramVersion = 1;
constexpr const char* kProgramRefKind = "seed_probe";
constexpr const char* kSavestateRefKind = "savestate";
constexpr const char* kNeutralBootstrapProfile = "seedprobe.neutral.required_savestate";
constexpr const char* kNeutralResultKind = "seedprobe.neutral_seed";

std::string BuildNeutralFingerprint(std::int64_t probe_id, SeedProbeTimingConfig timing) {
    return fingerprint_for(probe_id, savor::GCInputFrame{}.to_frame_hex(), timing.run_ms, timing.vi_stall_ms);
}

std::string ApplyTerminalJobStateFromResults(
    savor::db::IExecutionDb* execution_db,
    std::int64_t job_id,
    const ResultsIni& parsed) {
    const bool failed = parsed.w_err != 0 || parsed.dw_err != 0;
    const char* terminal_state = failed ? "FAILED" : "SUCCEEDED";
    std::ostringstream event;
    event << "[seedprobe-job-terminal-state] stage=AppendLifecycleEvent phase=Neutral"
          << " job=" << job_id
          << " terminal_state=" << terminal_state;
    if (execution_db == nullptr || execution_db->JobCommandService() == nullptr || job_id <= 0) {
        event << " ok=false error=execution_db_unavailable";
        return event.str();
    }

    std::string error;
    const bool ok = execution_db->JobCommandService()->AppendLifecycleEvent(
        {
            .kind = savor::db::execution::jobs::JobLifecycleEventKind::JobCompleted,
            .job_id = job_id,
            .terminal_state = failed ? std::optional<std::string>("FAILED") : std::optional<std::string>("SUCCEEDED"),
            .requested_by = "seedprobe_result_mapper",
        },
        &error);
    event << " ok=" << (ok ? "true" : "false");
    if (!ok) {
        event << " error=" << error;
    }
    return event.str();
}

} // namespace

NeutralProbeJobPersistenceAdapter::NeutralProbeJobPersistenceAdapter(
    savor::db::IExecutionDb* execution_db,
    savor::db::IAnalysisDb* analysis_db,
    savor::db::IAuthoringDb* authoring_db)
    : execution_db_(execution_db)
    , analysis_db_(analysis_db)
    , authoring_db_(authoring_db) {
}

WorkflowStepScheduleResult NeutralProbeJobPersistenceAdapter::EncodeForQueueing(std::int64_t domain_ref_id) const {
    WorkflowStepScheduleResult scheduled{};
    auto& persisted = scheduled.persistence;
    persisted.program_ref_kind = kProgramRefKind;
    persisted.program_version = kProgramVersion;
    const auto timing = resolve_timing_from_authoring_spec(analysis_db_, authoring_db_, domain_ref_id).value_or(SeedProbeTimingConfig{});
    persisted.fingerprint = BuildNeutralFingerprint(domain_ref_id, timing);
    persisted.program_ref_id = domain_ref_id;
    std::int64_t probe_run_id = domain_ref_id;

    if (execution_db_ != nullptr) {
        std::string error;
        std::int64_t job_set_id = 0;
        (void)execution_db_->CreateJobSet(
            {
                .program_kind = static_cast<std::int32_t>(savor::PK_SeedProbe),
                .purpose = "SeedProbe Neutral",
                .created_by = "seedprobe.neutral.adapters",
                .created_at_utc = savor::db::types::UtcNow().time_since_epoch().count(),
                .expected_total = 1,
                .domain_ref_kind = std::string("sp_probe_run"),
                .domain_ref_id = persisted.program_ref_id,
                .meta_note = std::string("phase=neutral"),
            },
            &job_set_id,
            &error);
        if (job_set_id > 0) {
            scheduled.root_job_set_id = job_set_id;
            (void)execution_db_->EnqueueJob(
                {
                    .job_set_id = job_set_id,
                    .program_kind = static_cast<std::int32_t>(savor::PK_SeedProbe),
                    .program_version = persisted.program_version,
                    .program_ref_kind = "seed_probe_run",
                    .program_ref_id = persisted.program_ref_id,
                    .fingerprint = persisted.fingerprint,
                    .priority = 0,
                    .max_attempts = 1,
                    .pending_until_workflow_materialized = true,
                },
                nullptr,
                &error);
        }
    }
    return scheduled;
}

std::int64_t NeutralProbeJobPersistenceAdapter::DecodeDomainRefId(const JobPersistenceRecord& persisted) const {
    return persisted.program_ref_id;
}

RequiredSavestateRuntimeInitAdapter::RequiredSavestateRuntimeInitAdapter(
    savor::db::IExecutionDb* execution_db,
    savor::db::IAnalysisDb* analysis_db)
    : execution_db_(execution_db)
    , analysis_db_(analysis_db) {
}

RuntimeInitRequest RequiredSavestateRuntimeInitAdapter::BuildRuntimeInit(std::int64_t job_id) const {
    RuntimeInitRequest request{};
    request.bootstrap_profile = kNeutralBootstrapProfile;

    if (execution_db_ == nullptr || analysis_db_ == nullptr) {
        return request;
    }
    const auto job = execution_db_->GetJob(job_id);
    if (!job.has_value()) {
        return request;
    }
    const auto probe_run = analysis_db_->GetSeedProbeRun(job->program_ref_id);
    if (!probe_run.has_value()) {
        return request;
    }

    request.savestate_ref_kind = kSavestateRefKind;
    request.savestate_ref_id = probe_run->entry_savestate_id;
    return request;
}

std::optional<savor::PSJob> RequiredSavestateRuntimeInitAdapter::MaterializePsJob(
    std::int64_t job_id,
    const RuntimeInitRequest& /*request*/) const {
    if (execution_db_ == nullptr) {
        return std::nullopt;
    }
    const auto job_row = execution_db_->GetJob(job_id);
    if (!job_row.has_value()) {
        return std::nullopt;
    }

    savor::PSJob job{};
    const auto spec = build_encode_spec_from_fingerprint(job_row->fingerprint);
    if (!savor::seedprobe::encode_payload(spec, job.payload)) {
        return std::nullopt;
    }
    return job;
}

ResultMapPayload NeutralSeedResultMapper::MapPrimaryResult(std::int64_t job_id, const std::string& result_ini) const {
    ResultMapPayload payload{};
    payload.result_kind = kNeutralResultKind;
    const auto parsed = ResultsIni::from_section(IniDoc::parse(result_ini));
    payload.event_lines.push_back(ApplyTerminalJobStateFromResults(execution_db_, job_id, parsed));

    if (execution_db_ == nullptr || analysis_db_ == nullptr) {
        return payload;
    }

    const auto job = execution_db_->GetJob(job_id);
    if (!job.has_value()) {
        return payload;
    }
    const auto probe_run = analysis_db_->GetSeedProbeRun(job->program_ref_id);
    if (!probe_run.has_value()) {
        return payload;
    }

    std::string error;
    if (analysis_db_->SetSeedProbeRunNeutralSeed(probe_run->probe_run_id, parsed.rng_seed, &error)) {
        payload.result_ref_id = probe_run->probe_run_id;
    }
    return payload;
}

std::string NeutralSeedResultMapper::BuildResultIniFromPrResult(std::int64_t /*job_id*/, const savor::PRResult& result) const {
    ResultsIni out{};
    out.w_err = result.ps.w_err;
    if (out.w_err == 0) {
        result.ps.ctx.get(savor::context::key::core::DW_RUN_OUTCOME_CODE, out.dw_err);
    }
    if (result.ps.ok) {
        result.ps.ctx.get(savor::context::key::seed::RNG_SEED, out.rng_seed);
        result.ps.ctx.get(savor::context::key::core::VI_FIRST, out.vi_start);
        result.ps.ctx.get(savor::context::key::core::VI_LAST, out.vi_end);
    }
    IniDoc ini;
    return out.append_section(ini).to_string_sorted();
}

std::optional<ResultArtifactRef> NeutralSeedResultMapper::MapPrimaryArtifact(std::int64_t) const {
    return std::nullopt;
}

NeutralSeedResultMapper::NeutralSeedResultMapper(
    savor::db::IExecutionDb* execution_db,
    savor::db::IAnalysisDb* analysis_db)
    : execution_db_(execution_db)
    , analysis_db_(analysis_db) {
}

WorkflowTransitionDecision NeutralToGridTransitionHandler::EvaluateTransition(const WorkflowTransitionContext& context) const {
    WorkflowTransitionDecision decision{};

    if (context.step_key != "Neutral") {
        decision.should_advance = false;
        decision.blocked_reason = "unsupported_step_key";
        return decision;
    }

    if (context.failed_total > 0) {
        decision.should_advance = false;
        decision.blocked_reason = "seedprobe.neutral.step_has_failures";
        return decision;
    }

    decision.should_advance = true;
    decision.next_step_key = std::string("Grid");
    return decision;
}

ProgramKindDescriptor BuildSeedProbeNeutralDescriptor(
    savor::db::IExecutionDb* execution_db,
    savor::db::IAnalysisDb* analysis_db,
    savor::db::IAuthoringDb* authoring_db) {
    ProgramKindDescriptor descriptor{};
    descriptor.program_kind = savor::PK_SeedProbe;
    descriptor.program_name = "SeedProbe";
    descriptor.job_persistence = std::make_shared<NeutralProbeJobPersistenceAdapter>(execution_db, analysis_db, authoring_db);
    descriptor.runtime_init = std::make_shared<RequiredSavestateRuntimeInitAdapter>(execution_db, analysis_db);
    descriptor.result_mapper = std::make_shared<NeutralSeedResultMapper>(execution_db, analysis_db);
    descriptor.workflow_transition = std::make_shared<NeutralToGridTransitionHandler>();
    descriptor.supports_workflow_orchestration = true;
    return descriptor;
}

} // namespace savor::db::execution::programdb::seedprobe
