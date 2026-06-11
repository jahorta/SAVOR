#include "SeedProbeGridAdapters.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include "../../Jobs/JobEventOrchestration.h"
#include "../../../Common/Types/UtcTimestamp.h"
#include "../../../../SavorCore/Phases/RNGSeedDeltaMap.h"
#include "../../../../SavorCore/Phases/Programs/SeedProbe/SeedProbePayload.h"
#include "../../../../SavorCore/Runner/Parallel/PRTypes.h"
#include "../../../../SavorCore/Runner/Script/CtxRegistry.h"
#include "../../../../SavorCore/Utils/Hex.h"
#include "SeedProbeContracts.h"

namespace savor::db::execution::programdb::seedprobe {

namespace {

std::uint8_t ClampToU8(std::int64_t value, std::uint8_t fallback) {
    if (value < 0) {
        return fallback;
    }
    if (value > 255) {
        return 255;
    }
    return static_cast<std::uint8_t>(value);
}

class GridToUniqueTransitionHandler final : public IWorkflowTransitionHandler {
public:
    WorkflowTransitionDecision EvaluateTransition(const WorkflowTransitionContext& context) const override {
        WorkflowTransitionDecision decision{};
        if (context.step_key == "Grid") {
            if (context.failed_total > 0) {
                decision.should_advance = false;
                decision.blocked_reason = "seedprobe.grid.step_has_failures";
                return decision;
            }
            decision.should_advance = true;
            decision.next_step_key = std::string("Unique");
            return decision;
        }

        const std::string suffix = "/Grid";
        if (context.step_key.size() <= suffix.size()
            || context.step_key.compare(context.step_key.size() - suffix.size(), suffix.size(), suffix) != 0) {
            decision.should_advance = false;
            decision.blocked_reason = "unsupported_step_key";
            return decision;
        }

        if (context.failed_total > 0) {
            decision.should_advance = false;
            decision.blocked_reason = "seedprobe.grid.step_has_failures";
            return decision;
        }

        decision.should_advance = true;
        decision.spawn_steps.push_back(
            WorkflowTransitionDecision::DynamicStep{
                .step_key = context.step_key.substr(0, context.step_key.size() - suffix.size()) + "/Unique",
                .step_kind = "seedprobe.unique",
                .input_ref_kind = context.input_ref_kind,
                .input_ref_id = context.input_ref_id,
                .priority = 0,
                .max_attempts = 1,
            });
        return decision;
    }
};

std::string ApplyTerminalJobStateFromResults(
    savor::db::IExecutionDb* execution_db,
    std::int64_t job_id,
    const ResultsIni& parsed) {
    const bool failed = parsed.w_err != 0 || parsed.dw_err != 0;
    const char* terminal_state = failed ? "FAILED" : "SUCCEEDED";
    std::ostringstream event;
    event << "[seedprobe-job-terminal-state] stage=AppendLifecycleEvent phase=Grid"
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

SeedProbeGridJobPersistenceAdapter::SeedProbeGridJobPersistenceAdapter(
    savor::db::IExecutionDb* execution_db,
    savor::db::IAnalysisDb* analysis_db,
    savor::db::IAuthoringDb* authoring_db,
    SeedProbeGridBlueprintConfig blueprint,
    SeedProbeGridSpec grid)
    : execution_db_(execution_db)
    , analysis_db_(analysis_db)
    , authoring_db_(authoring_db)
    , blueprint_(std::move(blueprint))
    , grid_(grid)
    , fanout_(BuildFanout(grid_, blueprint_)) {
}

WorkflowStepScheduleResult SeedProbeGridJobPersistenceAdapter::EncodeForQueueing(std::int64_t domain_ref_id) const {
    WorkflowStepScheduleResult scheduled{};
    const std::int64_t probe_run_id = domain_ref_id;
    const auto resolved_blueprint = ResolveBlueprintForRun(probe_run_id);
    const auto resolved_grid = ResolveGridSpecForRun(probe_run_id);
    const auto fanout = BuildFanout(resolved_grid, resolved_blueprint);

    if (execution_db_ != nullptr && probe_run_id > 0) {
        savor::db::CreateJobSetCommand set_cmd{};
        set_cmd.program_kind = static_cast<std::int32_t>(savor::PK_SeedProbe);
        set_cmd.purpose = "SeedProbe Grid";
        set_cmd.created_by = std::string("seedprobe_grid_adapter");
        set_cmd.expected_total = static_cast<std::int32_t>(fanout.size());
        set_cmd.domain_ref_kind = std::string("sp_probe_run");
        set_cmd.domain_ref_id = probe_run_id;
        set_cmd.meta_note = std::string("phase=Grid");

        std::int64_t job_set_id = 0;
        std::string error;
        if (execution_db_->CreateJobSet(set_cmd, &job_set_id, &error) && job_set_id > 0) {
            scheduled.root_job_set_id = job_set_id;
            for (const auto& entry : fanout) {
                savor::db::EnqueueJobCommand enqueue{};
                enqueue.job_set_id = job_set_id;
                enqueue.program_kind = static_cast<std::int32_t>(savor::PK_SeedProbe);
                enqueue.program_version = resolved_blueprint.program_version;
                enqueue.program_ref_kind = "sp_probe_run";
                enqueue.program_ref_id = probe_run_id;
                enqueue.fingerprint = FingerprintFor(
                    resolved_blueprint,
                    probe_run_id,
                    entry.frame_hex,
                    entry.family.c_str(),
                    entry.domain_ref_id);
                enqueue.priority = 0;
                enqueue.max_attempts = 3;
                enqueue.input_ini = "";
                (void)execution_db_->EnqueueJob(enqueue, nullptr, &error);
            }
        }
    }

    JobPersistenceRecord persisted{};
    persisted.program_ref_kind = "sp_probe_run";
    persisted.program_ref_id = probe_run_id;
    persisted.program_version = resolved_blueprint.program_version;
    persisted.fingerprint = FingerprintFor(resolved_blueprint, probe_run_id, /*frame_hex*/ "", "grid", domain_ref_id);
    scheduled.persistence = std::move(persisted);
    return scheduled;
}

std::int64_t SeedProbeGridJobPersistenceAdapter::DecodeDomainRefId(const JobPersistenceRecord& persisted) const {
    constexpr const char* token = ";grid_ref=";
    const auto pos = persisted.fingerprint.find(token);
    if (pos == std::string::npos) {
        return 0;
    }

    const std::size_t begin = pos + std::char_traits<char>::length(token);
    std::size_t end = persisted.fingerprint.find(';', begin);
    if (end == std::string::npos) {
        end = persisted.fingerprint.size();
    }

    const auto segment = persisted.fingerprint.substr(begin, end - begin);
    try {
        return std::stoll(segment);
    } catch (...) {
        return 0;
    }
}

const std::vector<GridFanoutEntry>& SeedProbeGridJobPersistenceAdapter::Fanout() const {
    return fanout_;
}

std::string SeedProbeGridJobPersistenceAdapter::FingerprintFor(
    const SeedProbeGridBlueprintConfig& blueprint,
    std::int64_t probe_run_id,
    const std::string& frame_hex,
    const char* family,
    std::int64_t grid_ref) {
    std::string fingerprint = "PK=3;PV=" + std::to_string(blueprint.program_version)
        + ";phase=grid;probe_run_id=" + std::to_string(probe_run_id)
        + ";family=" + family
        + ";grid_ref=" + std::to_string(grid_ref)
        + ";run_ms=" + std::to_string(blueprint.run_ms)
        + ";vi=" + std::to_string(blueprint.vi_stall_ms);
    if (!frame_hex.empty()) {
        fingerprint += ";frame=" + frame_hex;
    }
    return fingerprint;
}

std::vector<GridFanoutEntry> SeedProbeGridJobPersistenceAdapter::BuildFanout(
    const SeedProbeGridSpec& grid,
    const SeedProbeGridBlueprintConfig& blueprint) const {
    std::vector<GridFanoutEntry> entries;
    const int samples_per_axis = std::max(grid.samples_per_axis, 1);
    entries.reserve(static_cast<std::size_t>(samples_per_axis * samples_per_axis * 3));

    auto add_family = [&entries, &blueprint](std::vector<savor::GCInputFrame> frames, const char* family) {
        for (auto& frame : frames) {
            auto frame_hex = frame.to_frame_hex();
            const auto next_ref = static_cast<std::int64_t>(entries.size()) + 1;

            JobPersistenceRecord record{};
            record.program_ref_kind = "sp_probe_run";
            record.program_ref_id = blueprint.probe_id;
            record.program_version = blueprint.program_version;
            record.fingerprint = FingerprintFor(blueprint, blueprint.probe_id, frame_hex, family, next_ref);

            entries.push_back(GridFanoutEntry{
                .domain_ref_id = next_ref,
                .frame = frame,
                .frame_hex = frame_hex,
                .family = family,
                .persistence = std::move(record),
            });
        }
    };

    add_family(savor::build_grid_main(samples_per_axis, grid.min_value, grid.max_value), "main");
    add_family(savor::build_grid_cstick(samples_per_axis, grid.min_value, grid.max_value), "cstick");
    add_family(
        savor::build_grid_trig(
            samples_per_axis,
            grid.ignore_trigger_minmax ? 0 : grid.min_value,
            grid.ignore_trigger_minmax ? 255 : grid.max_value,
            grid.cap_trigger_top),
        "trigger");
    return entries;
}

SeedProbeGridBlueprintConfig SeedProbeGridJobPersistenceAdapter::ResolveBlueprintForRun(std::int64_t probe_run_id) const {
    auto resolved = blueprint_;
    resolved.probe_id = probe_run_id;
    const auto timing = resolve_timing_from_authoring_spec(analysis_db_, authoring_db_, probe_run_id);
    if (timing.has_value()) {
        resolved.run_ms = timing->run_ms;
        resolved.vi_stall_ms = timing->vi_stall_ms;
    }
    return resolved;
}

SeedProbeGridSpec SeedProbeGridJobPersistenceAdapter::ResolveGridSpecForRun(std::int64_t probe_run_id) const {
    auto resolved = grid_;
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

    if (spec->samples_per_axis > 0) {
        resolved.samples_per_axis = spec->samples_per_axis;
    }
    resolved.min_value = ClampToU8(spec->min_value, resolved.min_value);
    resolved.max_value = ClampToU8(spec->max_value, resolved.max_value);
    resolved.cap_trigger_top = spec->cap_trigger_top;
    resolved.ignore_trigger_minmax = spec->ignore_trigger_minmax;
    return resolved;
}

SeedProbeRuntimeInitAdapter::SeedProbeRuntimeInitAdapter(
    savor::db::IExecutionDb* execution_db,
    const savor::db::IAnalysisDb* analysis_db)
    : execution_db_(execution_db)
    , analysis_db_(analysis_db) {
}

RuntimeInitRequest SeedProbeRuntimeInitAdapter::BuildRuntimeInit(std::int64_t job_id) const {
    RuntimeInitRequest request{};
    request.savestate_ref_kind = "analysis_savestate";

    if (execution_db_ == nullptr || analysis_db_ == nullptr) {
        return request;
    }

    const auto job = execution_db_->GetJob(job_id);
    if (!job.has_value()) {
        return request;
    }

    if (job->program_ref_kind != "sp_probe_run") {
        return request;
    }

    const auto savestate_id = analysis_db_->LookupSeedProbeRunSavestateId(job->program_ref_id);
    if (!savestate_id.has_value()) {
        return request;
    }

    request.savestate_ref_id = *savestate_id;
    request.bootstrap_profile = "seedprobe.grid";
    return request;
}

std::optional<savor::PSJob> SeedProbeRuntimeInitAdapter::MaterializePsJob(
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

SeedProbeGridResultMapper::SeedProbeGridResultMapper(
    savor::db::IExecutionDb* execution_db,
    savor::db::IAnalysisDb* analysis_db)
    : execution_db_(execution_db)
    , analysis_db_(analysis_db) {
}

std::string SeedProbeGridResultMapper::BuildResultIniFromPrResult(std::int64_t /*job_id*/, const savor::PRResult& result) const {
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

ResultMapPayload SeedProbeGridResultMapper::MapPrimaryResult(std::int64_t job_id, const std::string& result_ini) const {
    ResultMapPayload payload{};
    const auto parsed_result = ResultsIni::from_section(IniDoc::parse(result_ini));
    payload.event_lines.push_back(ApplyTerminalJobStateFromResults(execution_db_, job_id, parsed_result));

    if (analysis_db_ == nullptr) {
        payload.result_kind = "analysisseedprobe.unavailable";
        return payload;
    }

    const auto context = ResolveContextFromJob(job_id);
    if (!context.has_value()) {
        payload.result_kind = "analysisseedprobe.context_missing";
        return payload;
    }

    const auto parsed = ParseFrame(context->frame_hex);
    if (!parsed.has_value()) {
        payload.result_kind = "analysisseedprobe.grid_seed.invalid_frame";
        return payload;
    }

    const auto observed_seed = static_cast<std::int64_t>(parsed_result.rng_seed);
    const auto seed_delta = static_cast<std::int64_t>(observed_seed - context->neutral_seed);

    if (context->phase == SeedProbeWorkflowPhase::Grid) {
        savor::db::RecordSeedProbeGridSeedCommand cmd{};
        cmd.probe_result_id = context->probe_result_id;
        cmd.source_family = NormalizeFamilyLabel(context->source_family.empty()
            ? FamilyLabel(static_cast<savor::ElementFamily>(parsed->get_family()))
            : context->source_family);
        cmd.axis_xy_id = AxisXYId(*parsed, cmd.source_family);
        cmd.seed_value = observed_seed;
        cmd.seed_delta = seed_delta;
        cmd.recorded_at_utc = savor::db::types::UtcNow();
        cmd.correlation_id = context->correlation_id;
        cmd.causation_id = context->causation_id;

        std::int64_t grid_seed_id = 0;
        std::string error;
        if (!analysis_db_->RecordSeedProbeGridSeed(cmd, &grid_seed_id, &error)) {
            payload.result_kind = "analysisseedprobe.grid_seed.error";
            return payload;
        }

        payload.result_kind = "analysisseedprobe.grid_seed";
        payload.result_ref_id = grid_seed_id;
        return payload;
    }

    if (context->phase == SeedProbeWorkflowPhase::Unique) {
        payload.result_kind = "analysisseedprobe.unique.deferred";
        return payload;
    }

    payload.result_kind = "analysisseedprobe.noop";
    return payload;
}

std::optional<ResultArtifactRef> SeedProbeGridResultMapper::MapPrimaryArtifact(std::int64_t /*job_id*/) const {
    return std::nullopt;
}

bool SeedProbeGridResultMapper::ShouldRequeueOnFailure(SeedProbeWorkflowPhase phase) {
    return phase == SeedProbeWorkflowPhase::Grid;
}

std::optional<GridResultContext> SeedProbeGridResultMapper::ResolveContextFromJob(std::int64_t job_id) const {
    if (execution_db_ == nullptr || analysis_db_ == nullptr || job_id <= 0) {
        return std::nullopt;
    }

    const auto job = execution_db_->GetJob(job_id);
    if (!job.has_value() || job->program_ref_kind != "sp_probe_run") {
        return std::nullopt;
    }

    const auto probe_result_id = analysis_db_->LookupSeedProbeResultId(job->program_ref_id);
    const auto neutral_seed = analysis_db_->LookupSeedProbeNeutralSeed(job->program_ref_id);
    const auto frame_hex = fingerprint_value(job->fingerprint, "frame");
    if (!probe_result_id.has_value() || !neutral_seed.has_value() || !frame_hex.has_value() || frame_hex->empty()) {
        return std::nullopt;
    }

    GridResultContext context{};
    context.phase = fingerprint_value(job->fingerprint, "phase").value_or("grid") == "unique"
        ? SeedProbeWorkflowPhase::Unique
        : SeedProbeWorkflowPhase::Grid;
    context.probe_result_id = *probe_result_id;
    context.neutral_seed = *neutral_seed;
    context.observed_seed = 0;
    context.frame_hex = *frame_hex;
    context.source_family = NormalizeFamilyLabel(fingerprint_value(job->fingerprint, "family").value_or(""));
    context.correlation_id = "seedprobe-run-" + std::to_string(job->program_ref_id);
    context.causation_id = "job-" + std::to_string(job_id);
    if (const auto expected_delta = fingerprint_value(job->fingerprint, "expected_delta"); expected_delta.has_value()) {
        try {
            context.expected_delta = std::stoll(*expected_delta);
        } catch (...) {
            context.expected_delta = 0;
        }
    }
    return context;
}

std::optional<savor::GCInputFrame> SeedProbeGridResultMapper::ParseFrame(const std::string& frame_hex) {
    if (frame_hex.empty()) {
        return std::nullopt;
    }

    const auto bytes = hex_to_bytes(frame_hex);
    if (bytes.size() != sizeof(savor::GCInputFrame)) {
        return std::nullopt;
    }

    savor::GCInputFrame frame{};
    std::memcpy(&frame, bytes.data(), sizeof(savor::GCInputFrame));
    return frame;
}

std::string SeedProbeGridResultMapper::FamilyLabel(savor::ElementFamily family) {
    switch (family) {
    case savor::ElementFamily::Main: return "MAIN";
    case savor::ElementFamily::CStick: return "CSTICK";
    case savor::ElementFamily::Triggers: return "TRIGGER";
    case savor::ElementFamily::Neutral: return "NEUTRAL";
    }
    return "UNKNOWN";
}

std::string SeedProbeGridResultMapper::NormalizeFamilyLabel(const std::string& family) {
    std::string normalized;
    normalized.reserve(family.size());
    for (const char ch : family) {
        normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
    if (normalized == "MAIN" || normalized == "JSTICK") {
        return "MAIN";
    }
    if (normalized == "CSTICK" || normalized == "C-STICK") {
        return "CSTICK";
    }
    if (normalized == "TRIGGER" || normalized == "TRIGGERS") {
        return "TRIGGER";
    }
    if (normalized == "NEUTRAL") {
        return "NEUTRAL";
    }
    return normalized;
}

std::int64_t SeedProbeGridResultMapper::AxisXYId(const savor::GCInputFrame& frame, const std::string& source_family) {
    const auto family = NormalizeFamilyLabel(source_family.empty()
        ? FamilyLabel(static_cast<savor::ElementFamily>(frame.get_family()))
        : source_family);
    if (family == "MAIN") {
        return static_cast<std::int64_t>((static_cast<std::uint16_t>(frame.main_x) << 8) | frame.main_y);
    }
    if (family == "CSTICK") {
        return static_cast<std::int64_t>((static_cast<std::uint16_t>(frame.c_x) << 8) | frame.c_y);
    }
    if (family == "TRIGGER") {
        return static_cast<std::int64_t>((static_cast<std::uint16_t>(frame.trig_l) << 8) | frame.trig_r);
    }
    return 0;
}

ProgramKindDescriptor BuildSeedProbeGridDescriptor(
    savor::db::IExecutionDb* execution_db,
    savor::db::IAnalysisDb* analysis_db,
    SeedProbeGridBlueprintConfig blueprint,
    SeedProbeGridSpec grid,
    savor::db::IAuthoringDb* authoring_db) {
    ProgramKindDescriptor descriptor{};
    descriptor.program_kind = savor::PK_SeedProbe;
    descriptor.program_name = "SeedProbe";
    descriptor.job_persistence = std::make_shared<SeedProbeGridJobPersistenceAdapter>(
        execution_db,
        analysis_db,
        authoring_db,
        std::move(blueprint),
        grid);
    descriptor.runtime_init = std::make_shared<SeedProbeRuntimeInitAdapter>(execution_db, analysis_db);
    descriptor.result_mapper = std::make_shared<SeedProbeGridResultMapper>(execution_db, analysis_db);
    descriptor.workflow_transition = std::make_shared<GridToUniqueTransitionHandler>();
    descriptor.supports_workflow_orchestration = true;
    return descriptor;
}

} // namespace savor::db::execution::programdb::seedprobe
