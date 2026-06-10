#include "SeedProbeUniqueAdapters.h"

#include <algorithm>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#include "../../Execution/Jobs/JobEventOrchestration.h"
#include "../../../Common/Types/UtcTimestamp.h"
#include "../../../../SimCore/Phases/RNGSeedDeltaMap.h"
#include "../../../../SimCore/Phases/Programs/SeedProbe/SeedProbePayload.h"
#include "../../../../SimCore/Runner/Parallel/PRTypes.h"
#include "../../../../SimCore/Runner/Script/KeyRegistry.h"

namespace simcore::db::execution::programdb::seedprobe {
namespace {

std::optional<std::int64_t> ParseExpectedDeltaFromFingerprint(const std::string& fingerprint) {
    constexpr const char* token = ";expected_delta=";
    const auto pos = fingerprint.find(token);
    if (pos == std::string::npos) {
        return std::nullopt;
    }

    const std::size_t begin = pos + std::char_traits<char>::length(token);
    std::size_t end = fingerprint.find(';', begin);
    if (end == std::string::npos) {
        end = fingerprint.size();
    }

    const auto segment = fingerprint.substr(begin, end - begin);
    try {
        return std::stoll(segment);
    }
    catch (...) {
        return std::nullopt;
    }
}

std::int64_t AxisXYId(std::uint8_t x, std::uint8_t y) {
    return static_cast<std::int64_t>((static_cast<std::uint16_t>(x) << 8) | y);
}

std::string BuildUniqueFingerprint(
    const SeedProbeGridBlueprintConfig& blueprint,
    std::int64_t probe_run_id,
    const std::string& frame_hex = {},
    std::optional<std::int64_t> probe_result_id = std::nullopt,
    std::optional<std::int64_t> expected_delta = std::nullopt) {
    std::string fingerprint = "PK=3;PV=" + std::to_string(blueprint.program_version)
        + ";phase=unique;probe_run_id=" + std::to_string(probe_run_id)
        + ";run_ms=" + std::to_string(blueprint.run_ms)
        + ";vi=" + std::to_string(blueprint.vi_stall_ms);
    if (probe_result_id.has_value()) {
        fingerprint += ";probe_result_id=" + std::to_string(*probe_result_id);
    }
    if (expected_delta.has_value()) {
        fingerprint += ";expected_delta=" + std::to_string(*expected_delta);
    }
    if (!frame_hex.empty()) {
        fingerprint += ";frame=" + frame_hex;
    }
    return fingerprint;
}

std::string ApplyTerminalJobState(
    simcore::db::IExecutionDb* execution_db,
    std::int64_t job_id,
    const std::optional<std::string>& terminal_state) {
    std::ostringstream event;
    event << "[seedprobe-job-terminal-state] stage=AppendLifecycleEvent phase=Unique"
          << " job=" << job_id
          << " terminal_state=" << terminal_state.value_or("");
    if (execution_db == nullptr || execution_db->JobCommandService() == nullptr || job_id <= 0) {
        event << " ok=false error=execution_db_unavailable";
        return event.str();
    }

    std::string error;
    const bool ok = execution_db->JobCommandService()->AppendLifecycleEvent(
        {
            .kind = simcore::db::execution::jobs::JobLifecycleEventKind::JobCompleted,
            .job_id = job_id,
            .terminal_state = terminal_state,
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

    if (context.step_key == "Grid" && context.failed_total > 0) {
        decision.should_advance = false;
        decision.blocked_reason = "seedprobe.grid.step_has_failures";
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
    simcore::db::IAuthoringDb* authoring_db,
    SeedProbeGridBlueprintConfig blueprint,
    UniqueIni unique_ini)
    : execution_db_(execution_db)
    , analysis_db_(analysis_db)
    , authoring_db_(authoring_db)
    , blueprint_(std::move(blueprint))
    , unique_ini_(unique_ini) {
}

WorkflowStepScheduleResult SeedProbeUniqueJobPersistenceAdapter::EncodeForQueueing(std::int64_t domain_ref_id) const {
    WorkflowStepScheduleResult scheduled{};
    const auto resolved_blueprint = ResolveBlueprintForRun(domain_ref_id);
    const auto resolved_unique = ResolveUniqueSpecForRun(domain_ref_id);
    auto& persisted = scheduled.persistence;
    persisted.program_ref_kind = "sp_probe_run";
    persisted.program_ref_id = domain_ref_id;
    persisted.program_version = resolved_blueprint.program_version;
    persisted.fingerprint = BuildUniqueFingerprint(resolved_blueprint, domain_ref_id);

    if (execution_db_ == nullptr || analysis_db_ == nullptr || domain_ref_id <= 0) {
        return scheduled;
    }

    const auto neutral_seed = analysis_db_->LookupSeedProbeNeutralSeed(domain_ref_id);
    const auto grid_rows = analysis_db_->ListSeedProbeGridSeeds(domain_ref_id);
    if (!neutral_seed.has_value() || grid_rows.empty()) {
        return scheduled;
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

    auto planned = simcore::PlanJCTComboSamples(
        grid,
        static_cast<std::uint32_t>(std::max(resolved_unique.combo_attempts_per_target, 1)),
        static_cast<std::uint32_t>(std::max(resolved_unique.combo_sampler_tries, 1)));

    std::int64_t root_job_set_id = 0;
    std::string error;
    if (!execution_db_->CreateJobSet(
            {
                .program_kind = 3,
                .purpose = "SeedProbe Unique",
                .created_by = std::string("seedprobe_unique_adapter"),
                .expected_total = static_cast<int>(planned.singletons.size() + planned.samples.size()),
                .domain_ref_kind = std::string("sp_probe_run"),
                .domain_ref_id = domain_ref_id,
                .meta_note = std::string("phase=Unique"),
            },
            &root_job_set_id,
            &error)
        || root_job_set_id <= 0) {
        return scheduled;
    }
    scheduled.root_job_set_id = root_job_set_id;

    std::int64_t planned_frames = 0;
    std::int64_t child_sets_created = 0;
    std::int64_t child_sets_failed = 0;
    std::int64_t child_expected_total = 0;
    std::int64_t jobs_enqueued = 0;
    std::int64_t jobs_failed = 0;
    for (const auto& singleton : planned.singletons) {
        std::int64_t child_job_set_id = 0;
        constexpr int child_expected = 1;
        planned_frames += child_expected;
        child_expected_total += child_expected;
        if (!execution_db_->CreateJobSet(
                {
                    .parent_job_set_id = root_job_set_id,
                    .program_kind = 3,
                    .purpose = "SeedProbe Unique Singleton",
                    .created_by = std::string("seedprobe_unique_adapter"),
                    .expected_total = child_expected,
                    .domain_ref_kind = std::string("sp_probe_run"),
                    .domain_ref_id = domain_ref_id,
                    .meta_note = std::string("expected_delta=") + std::to_string(singleton.target_delta)
                        + ";seed=" + std::to_string(singleton.seed)
                        + ";source=singleton",
                },
                &child_job_set_id,
                &error)
            || child_job_set_id <= 0) {
            ++child_sets_failed;
            jobs_failed += child_expected;
            continue;
        }
        ++child_sets_created;

        auto frame = singleton.frame;
        auto frame_hex = frame.to_frame_hex();
        simcore::db::EnqueueJobCommand enqueue{};
        enqueue.job_set_id = child_job_set_id;
        enqueue.program_kind = 3;
        enqueue.program_version = resolved_blueprint.program_version;
        enqueue.program_ref_kind = "sp_probe_run";
        enqueue.program_ref_id = domain_ref_id;
        enqueue.fingerprint = BuildUniqueFingerprint(
            resolved_blueprint,
            domain_ref_id,
            frame_hex,
            probe_result_id,
            singleton.target_delta);
        enqueue.priority = 1;
        enqueue.max_attempts = 2;
        std::int64_t job_id = 0;
        if (execution_db_->EnqueueJob(enqueue, &job_id, &error) && job_id > 0) {
            ++jobs_enqueued;
        } else {
            ++jobs_failed;
        }
    }

    for (auto& sample : planned.samples) {
        std::int64_t child_job_set_id = 0;
        const auto child_expected = static_cast<int>(sample.frames.size());
        planned_frames += child_expected;
        child_expected_total += child_expected;
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
            ++child_sets_failed;
            jobs_failed += child_expected;
            continue;
        }
        ++child_sets_created;

        for (auto& frame : sample.frames) {
            auto frame_hex = frame.to_frame_hex();
            simcore::db::EnqueueJobCommand enqueue{};
            enqueue.job_set_id = child_job_set_id;
            enqueue.program_kind = 3;
            enqueue.program_version = resolved_blueprint.program_version;
            enqueue.program_ref_kind = "sp_probe_run";
            enqueue.program_ref_id = domain_ref_id;
            enqueue.fingerprint = BuildUniqueFingerprint(
                resolved_blueprint,
                domain_ref_id,
                frame_hex,
                probe_result_id,
                sample.target_delta);
            enqueue.priority = 0;
            enqueue.max_attempts = 2;
            std::int64_t job_id = 0;
            if (execution_db_->EnqueueJob(enqueue, &job_id, &error) && job_id > 0) {
                ++jobs_enqueued;
            } else {
                ++jobs_failed;
            }
        }
    }

    std::ostringstream event;
    event << "[seedprobe-unique-enqueue-summary]"
          << " root_job_set=" << root_job_set_id
          << " combo_attempts_per_target=" << resolved_unique.combo_attempts_per_target
          << " combo_sampler_tries=" << resolved_unique.combo_sampler_tries
          << " planned_singletons=" << planned.singletons.size()
          << " planned_samples=" << planned.samples.size()
          << " planned_frames=" << planned_frames
          << " child_sets_created=" << child_sets_created
          << " child_sets_failed=" << child_sets_failed
          << " child_expected_total=" << child_expected_total
          << " jobs_enqueued=" << jobs_enqueued
          << " jobs_failed=" << jobs_failed;
    if (!error.empty()) {
        event << " last_error=" << error;
    }
    scheduled.event_lines.push_back(event.str());

    return scheduled;
}

std::int64_t SeedProbeUniqueJobPersistenceAdapter::DecodeDomainRefId(const JobPersistenceRecord& persisted) const {
    return persisted.program_ref_id;
}

SeedProbeGridBlueprintConfig SeedProbeUniqueJobPersistenceAdapter::ResolveBlueprintForRun(std::int64_t probe_run_id) const {
    auto resolved = blueprint_;
    const auto timing = resolve_timing_from_authoring_spec(analysis_db_, authoring_db_, probe_run_id);
    if (timing.has_value()) {
        resolved.run_ms = timing->run_ms;
        resolved.vi_stall_ms = timing->vi_stall_ms;
    }
    return resolved;
}

UniqueIni SeedProbeUniqueJobPersistenceAdapter::ResolveUniqueSpecForRun(std::int64_t probe_run_id) const {
    auto resolved = unique_ini_;
    if (analysis_db_ == nullptr || authoring_db_ == nullptr || probe_run_id <= 0) {
        return resolved;
    }

    const auto probe_run = analysis_db_->GetSeedProbeRun(probe_run_id);
    if (!probe_run.has_value()) {
        return resolved;
    }

    const auto spec = authoring_db_->GetSeedProbeSpec(probe_run->seed_probe_spec_id);
    if (!spec.has_value()) {
        return resolved;
    }

    if (spec->combo_attempts_per_target > 0) {
        resolved.combo_attempts_per_target = spec->combo_attempts_per_target;
    }
    if (spec->combo_sampler_tries > 0) {
        resolved.combo_sampler_tries = spec->combo_sampler_tries;
    }
    return resolved;
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

std::optional<simcore::PSJob> SeedProbeUniqueRuntimeInitAdapter::MaterializePsJob(
    std::int64_t job_id,
    const RuntimeInitRequest& /*request*/) const {
    if (execution_db_ == nullptr) {
        return std::nullopt;
    }
    const auto job_row = execution_db_->GetJob(job_id);
    if (!job_row.has_value()) {
        return std::nullopt;
    }

    simcore::PSJob job{};
    const auto spec = build_encode_spec_from_fingerprint(job_row->fingerprint);
    if (!simcore::seedprobe::encode_payload(spec, job.payload)) {
        return std::nullopt;
    }
    return job;
}

SeedProbeUniqueResultMapper::SeedProbeUniqueResultMapper(simcore::db::IExecutionDb* execution_db, simcore::db::IAnalysisDb* analysis_db)
    : execution_db_(execution_db)
    , analysis_db_(analysis_db) {
}

std::string SeedProbeUniqueResultMapper::BuildResultIniFromPrResult(std::int64_t /*job_id*/, const simcore::PRResult& result) const {
    ResultsIni out{};
    out.w_err = result.ps.w_err;
    if (out.w_err == 0) {
        result.ps.ctx.get(simcore::keys::core::DW_RUN_OUTCOME_CODE, out.dw_err);
    }
    if (result.ps.ok) {
        result.ps.ctx.get(simcore::keys::seed::RNG_SEED, out.rng_seed);
        result.ps.ctx.get(simcore::keys::core::VI_FIRST, out.vi_start);
        result.ps.ctx.get(simcore::keys::core::VI_LAST, out.vi_end);
    }
    IniDoc ini;
    return out.append_section(ini).to_string_sorted();
}

ResultMapPayload SeedProbeUniqueResultMapper::MapPrimaryResult(std::int64_t job_id, const std::string& result_ini) const {
    const auto parsed = ResultsIni::from_section(IniDoc::parse(result_ini));

    ResultMapPayload payload{};
    payload.result_kind = "analysisseedprobe.unique.unavailable";
    if (execution_db_ == nullptr || analysis_db_ == nullptr) {
        payload.event_lines.push_back(ApplyTerminalJobState(execution_db_, job_id, std::optional<std::string>("FAILED")));
        return payload;
    }
    const auto job = execution_db_->GetJob(job_id);
    if (!job.has_value() || job->program_ref_kind != "sp_probe_run") {
        payload.event_lines.push_back(ApplyTerminalJobState(execution_db_, job_id, std::optional<std::string>("FAILED")));
        return payload;
    }

    const auto neutral_seed = analysis_db_->LookupSeedProbeNeutralSeed(job->program_ref_id);
    if (!neutral_seed.has_value()) {
        payload.result_kind = "analysisseedprobe.unique.missing_neutral";
        payload.event_lines.push_back(ApplyTerminalJobState(execution_db_, job_id, std::optional<std::string>("FAILED")));
        return payload;
    }
    const bool failed = parsed.w_err != 0 || parsed.dw_err != 0;
    if (failed) {
        payload.result_kind = "analysisseedprobe.unique.failure";
        payload.event_lines.push_back(ApplyTerminalJobState(execution_db_, job_id, std::optional<std::string>("FAILED")));
        return payload;
    }

    const auto observed_delta = static_cast<std::int64_t>(parsed.rng_seed) - *neutral_seed;
    const auto expected_delta = ParseExpectedDeltaFromFingerprint(job->fingerprint);
    const bool matched_expected_delta = expected_delta.has_value() && observed_delta == *expected_delta;

    const auto probe_result_id = analysis_db_->LookupSeedProbeResultId(job->program_ref_id);
    if (!probe_result_id.has_value()) {
        payload.result_kind = "analysisseedprobe.unique.missing_probe_result";
        payload.event_lines.push_back(ApplyTerminalJobState(execution_db_, job_id, std::optional<std::string>("FAILED")));
        return payload;
    }

    std::string error;
    const auto input_frame = parse_frame_hex_or_null(fingerprint_value(job->fingerprint, "frame"));
    if (!input_frame.has_value()) {
        payload.result_kind = "analysisseedprobe.unique.missing_frame";
        payload.event_lines.push_back(ApplyTerminalJobState(execution_db_, job_id, std::optional<std::string>("FAILED")));
        return payload;
    }

    std::int64_t input_frame_id = 0;
    if (!analysis_db_->EnsureSeedProbeInputFrame(
            AxisXYId(input_frame->main_x, input_frame->main_y),
            AxisXYId(input_frame->c_x, input_frame->c_y),
            AxisXYId(input_frame->trig_l, input_frame->trig_r),
            &input_frame_id,
            &error)
        || input_frame_id <= 0) {
        payload.result_kind = "analysisseedprobe.unique.input_frame_error";
        payload.event_lines.push_back(ApplyTerminalJobState(execution_db_, job_id, std::optional<std::string>("FAILED")));
        return payload;
    }

    RecordSeedProbeUniqueSeedCommand cmd{};
    cmd.probe_result_id = *probe_result_id;
    cmd.input_frame_id = input_frame_id;
    cmd.seed_value = parsed.rng_seed;
    cmd.seed_delta = observed_delta;
    cmd.recorded_at_utc = simcore::db::types::UtcNow();
    cmd.event_id = "seedprobe-result-" + std::to_string(*probe_result_id)
        + "-job-" + std::to_string(job_id)
        + "-unique";
    cmd.correlation_id = "seedprobe-run-" + std::to_string(job->program_ref_id);
    cmd.causation_id = "job-" + std::to_string(job_id);

    bool inserted = false;
    std::int64_t unique_seed_id = 0;
    if (!analysis_db_->EnsureSeedProbeUniqueSeedDelta(cmd, &inserted, &unique_seed_id, &error)) {
        payload.result_kind = "analysisseedprobe.unique.error";
        payload.event_lines.push_back(ApplyTerminalJobState(execution_db_, job_id, std::optional<std::string>("FAILED")));
        return payload;
    }

    if (inserted) {
        payload.result_kind = "analysisseedprobe.unique.winner";
        payload.result_ref_id = unique_seed_id;
        payload.event_lines.push_back(ApplyTerminalJobState(execution_db_, job_id, std::optional<std::string>("SUCCEEDED_WINNER")));
    } else {
        payload.result_kind = "analysisseedprobe.unique.superseded";
        payload.result_ref_id = unique_seed_id;
        payload.event_lines.push_back(ApplyTerminalJobState(execution_db_, job_id, std::optional<std::string>("SUPERSEDED")));
    }
    payload.output_key = "unique_input_frames";
    payload.output_data_kind = "analysis.input_frame_set_id";
    payload.output_ref_kind = job->program_ref_kind;
    payload.output_ref_id = job->program_ref_id;

    if (matched_expected_delta) {
        int rows_superseded = 0;
        (void)execution_db_->MarkQueuedJobsSuperseded(job->job_set_id, job_id, nullptr, &rows_superseded);
        std::ostringstream event;
        event << "[seedprobe-superseded] job_set=" << job->job_set_id
              << " job=" << job_id
              << " expected_delta=" << *expected_delta
              << " observed_delta=" << observed_delta
              << " superseded=" << rows_superseded;
        payload.event_lines.push_back(event.str());
    }
    return payload;
}

std::optional<ResultArtifactRef> SeedProbeUniqueResultMapper::MapPrimaryArtifact(std::int64_t /*job_id*/) const {
    return std::nullopt;
}

ProgramKindDescriptor BuildSeedProbeUniqueDescriptor(
    simcore::db::IExecutionDb* execution_db,
    simcore::db::IAnalysisDb* analysis_db,
    SeedProbeGridBlueprintConfig blueprint,
    UniqueIni unique_ini,
    SeedProbeUniqueTransitionHandler::CompletionGateFn completion_gate,
    simcore::db::IAuthoringDb* authoring_db) {
    if (!completion_gate) {
        completion_gate = [](const WorkflowTransitionContext&) { return true; };
    }

    ProgramKindDescriptor descriptor{};
    descriptor.program_kind = simcore::PK_SeedProbe;
    descriptor.program_name = "SeedProbe";
    descriptor.job_persistence = std::make_shared<SeedProbeUniqueJobPersistenceAdapter>(
        execution_db,
        analysis_db,
        authoring_db,
        std::move(blueprint),
        unique_ini);
    descriptor.runtime_init = std::make_shared<SeedProbeUniqueRuntimeInitAdapter>(execution_db, analysis_db);
    descriptor.result_mapper = std::make_shared<SeedProbeUniqueResultMapper>(execution_db, analysis_db);
    descriptor.workflow_transition = std::make_shared<SeedProbeUniqueTransitionHandler>(std::move(completion_gate));
    descriptor.supports_workflow_orchestration = true;
    return descriptor;
}

} // namespace simcore::db::execution::programdb::seedprobe
