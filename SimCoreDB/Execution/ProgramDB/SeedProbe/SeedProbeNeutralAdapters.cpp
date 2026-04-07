#include "SeedProbeNeutralAdapters.h"

#include "../../../../Common/Types/UtcTimestamp.h"
#include "../../../../SimCore/Runner/IPC/Wire.h"
#include "SeedProbeContracts.h"

namespace simcore::db::execution::programdb::seedprobe {

namespace {

constexpr std::int32_t kProgramVersion = 1;
constexpr const char* kProgramRefKind = "seed_probe";
constexpr const char* kSavestateRefKind = "savestate";
constexpr const char* kNeutralBootstrapProfile = "seedprobe.neutral.required_savestate";
constexpr const char* kNeutralResultKind = "seedprobe.neutral_seed";

std::string BuildNeutralFingerprint(std::int64_t probe_id) {
    return fingerprint_for(probe_id, serialize_frame_hex(simcore::GCInputFrame{}), 0, 0);
}

} // namespace

NeutralProbeJobPersistenceAdapter::NeutralProbeJobPersistenceAdapter(
    simcore::db::IExecutionDb* execution_db,
    simcore::db::IAnalysisDb* analysis_db)
    : execution_db_(execution_db)
    , analysis_db_(analysis_db) {
}

JobPersistenceRecord NeutralProbeJobPersistenceAdapter::EncodeForQueueing(std::int64_t domain_ref_id) const {
    JobPersistenceRecord persisted{};
    persisted.program_ref_kind = kProgramRefKind;
    persisted.program_version = kProgramVersion;
    persisted.fingerprint = BuildNeutralFingerprint(domain_ref_id);

    std::int64_t probe_run_id = 0;
    if (analysis_db_ != nullptr) {
        std::string analysis_error;
        if (analysis_db_->CreateSeedProbeRunForSet(domain_ref_id, &probe_run_id, &analysis_error)) {
            persisted.program_ref_id = probe_run_id;
            persisted.fingerprint = BuildNeutralFingerprint(probe_run_id);
        } else {
            persisted.program_ref_id = domain_ref_id;
        }
    } else {
        persisted.program_ref_id = domain_ref_id;
    }

    if (execution_db_ != nullptr) {
        std::string error;
        std::int64_t job_set_id = 0;
        (void)execution_db_->CreateJobSet(
            {
                .program_kind = static_cast<std::int32_t>(simcore::PK_SeedProbe),
                .purpose = "SeedProbe Neutral",
                .created_by = "seedprobe.neutral.adapters",
                .created_at_utc = simcore::db::types::UtcNow().time_since_epoch().count(),
                .expected_total = 1,
                .domain_ref_kind = std::string("sp_probe_run"),
                .domain_ref_id = persisted.program_ref_id,
                .meta_note = std::string("phase=neutral"),
            },
            &job_set_id,
            &error);
        if (job_set_id > 0) {
            (void)execution_db_->EnqueueJob(
                {
                    .job_set_id = job_set_id,
                    .program_kind = static_cast<std::int32_t>(simcore::PK_SeedProbe),
                    .program_version = persisted.program_version,
                    .program_ref_kind = "seed_probe_run",
                    .program_ref_id = persisted.program_ref_id,
                    .fingerprint = persisted.fingerprint,
                    .priority = 0,
                    .attempts = 0,
                    .max_attempts = 1,
                    .queued_at_utc = simcore::db::types::UtcNow().time_since_epoch().count(),
                },
                nullptr,
                &error);
        }
    }
    return persisted;
}

std::int64_t NeutralProbeJobPersistenceAdapter::DecodeDomainRefId(const JobPersistenceRecord& persisted) const {
    return persisted.program_ref_id;
}

RequiredSavestateRuntimeInitAdapter::RequiredSavestateRuntimeInitAdapter(
    simcore::db::IExecutionDb* execution_db,
    simcore::db::IAnalysisDb* analysis_db)
    : execution_db_(execution_db)
    , analysis_db_(analysis_db) {
}

RuntimeInitRequest RequiredSavestateRuntimeInitAdapter::BuildRuntimeInit(std::int64_t job_id) const {
    RuntimeInitRequest request{};
    request.bootstrap_profile = kNeutralBootstrapProfile;

    if (execution_db_ == nullptr || analysis_db_ == nullptr) {
        return request;
    }
    const auto job = execution_db_->GetJobRecord(job_id);
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

ResultMapPayload NeutralSeedResultMapper::MapPrimaryResult(std::int64_t job_id, const std::string& result_ini) const {
    ResultMapPayload payload{};
    payload.result_kind = kNeutralResultKind;

    if (execution_db_ == nullptr || analysis_db_ == nullptr) {
        return payload;
    }

    const auto job = execution_db_->GetJobRecord(job_id);
    if (!job.has_value()) {
        return payload;
    }
    const auto probe_run = analysis_db_->GetSeedProbeRun(job->program_ref_id);
    if (!probe_run.has_value()) {
        return payload;
    }

    const auto parsed = ResultsIni::from_section(IniDoc::parse(result_ini));
    std::string error;
    if (analysis_db_->SetSeedProbeRunNeutralSeed(probe_run->probe_run_id, parsed.rng_seed, &error)) {
        payload.result_ref_id = probe_run->probe_run_id;
    }
    return payload;
}

std::optional<ResultArtifactRef> NeutralSeedResultMapper::MapPrimaryArtifact(std::int64_t) const {
    return std::nullopt;
}

NeutralSeedResultMapper::NeutralSeedResultMapper(
    simcore::db::IExecutionDb* execution_db,
    simcore::db::IAnalysisDb* analysis_db)
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

    decision.should_advance = true;
    decision.next_step_key = std::string("Grid");
    return decision;
}

ProgramKindDescriptor BuildSeedProbeNeutralDescriptor(
    simcore::db::IExecutionDb* execution_db,
    simcore::db::IAnalysisDb* analysis_db) {
    ProgramKindDescriptor descriptor{};
    descriptor.program_kind = simcore::PK_SeedProbe;
    descriptor.program_name = "SeedProbe";
    descriptor.job_persistence = std::make_shared<NeutralProbeJobPersistenceAdapter>(execution_db, analysis_db);
    descriptor.runtime_init = std::make_shared<RequiredSavestateRuntimeInitAdapter>(execution_db, analysis_db);
    descriptor.result_mapper = std::make_shared<NeutralSeedResultMapper>(execution_db, analysis_db);
    descriptor.workflow_transition = std::make_shared<NeutralToGridTransitionHandler>();
    descriptor.supports_workflow_orchestration = true;
    return descriptor;
}

} // namespace simcore::db::execution::programdb::seedprobe
