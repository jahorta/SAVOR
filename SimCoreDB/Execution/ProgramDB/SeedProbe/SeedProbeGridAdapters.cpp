#include "SeedProbeGridAdapters.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

#include "../../../Common/Types/UtcTimestamp.h"
#include "../../../../SimCore/Phases/RNGSeedDeltaMap.h"
#include "../../../../SimCore/Runner/Parallel/PRTypes.h"
#include "../../../../SimCore/Runner/Script/KeyRegistry.h"
#include "../../../../SimCore/Utils/Hex.h"
#include "SeedProbeContracts.h"

namespace simcore::db::execution::programdb::seedprobe {

namespace {

class GridToUniqueTransitionHandler final : public IWorkflowTransitionHandler {
public:
    WorkflowTransitionDecision EvaluateTransition(const WorkflowTransitionContext& context) const override {
        WorkflowTransitionDecision decision{};
        if (context.step_key != "Grid") {
            decision.should_advance = false;
            decision.blocked_reason = "unsupported_step_key";
            return decision;
        }

        decision.should_advance = true;
        decision.next_step_key = std::string("Unique");
        return decision;
    }
};

} // namespace

SeedProbeGridJobPersistenceAdapter::SeedProbeGridJobPersistenceAdapter(simcore::db::IExecutionDb* execution_db, SeedProbeGridBlueprintConfig blueprint, SeedProbeGridSpec grid)
    : execution_db_(execution_db)
    , blueprint_(std::move(blueprint))
    , grid_(grid)
    , fanout_(BuildFanout()) {
}

JobPersistenceRecord SeedProbeGridJobPersistenceAdapter::EncodeForQueueing(std::int64_t domain_ref_id) const {
    const std::int64_t probe_run_id = domain_ref_id;
    if (execution_db_ && probe_run_id > 0) {
        simcore::db::CreateJobSetCommand set_cmd{};
        set_cmd.program_kind = 3;
        set_cmd.purpose = "SeedProbe Grid";
        set_cmd.created_by = std::string("seedprobe_grid_adapter");
        set_cmd.expected_total = static_cast<std::int32_t>(fanout_.size());
        set_cmd.domain_ref_kind = std::string("sp_probe_run");
        set_cmd.domain_ref_id = probe_run_id;
        set_cmd.meta_note = std::string("phase=Grid");

        std::int64_t job_set_id = 0;
        std::string error;
        if (execution_db_->CreateJobSet(set_cmd, &job_set_id, &error) && job_set_id > 0) {
            for (const auto& entry : fanout_) {
                simcore::db::EnqueueJobCommand enqueue{};
                enqueue.job_set_id = job_set_id;
                enqueue.program_kind = 3;
                enqueue.program_version = blueprint_.program_version;
                enqueue.program_ref_kind = "sp_probe_run";
                enqueue.program_ref_id = probe_run_id;
                enqueue.fingerprint = FingerprintFor(blueprint_, probe_run_id, entry.frame_hex, "grid", entry.domain_ref_id);
                enqueue.priority = 0;
                enqueue.max_attempts = 3;
                std::int64_t ignored_job_id = 0;
                (void)execution_db_->EnqueueJob(enqueue, &ignored_job_id, &error);
            }
        }
    }

    const auto it = std::find_if(
        fanout_.begin(),
        fanout_.end(),
        [domain_ref_id](const GridFanoutEntry& entry) { return entry.domain_ref_id == domain_ref_id; });
    if (it != fanout_.end()) {
        return it->persistence;
    }

    JobPersistenceRecord fallback{};
    fallback.program_ref_kind = "sp_probe_run";
    fallback.program_ref_id = blueprint_.probe_id;
    fallback.program_version = blueprint_.program_version;
    fallback.fingerprint = FingerprintFor(blueprint_, probe_run_id, /*frame_hex*/"", "grid", domain_ref_id);
    return fallback;
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
    }
    catch (...) {
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
        + ";probe_run_id=" + std::to_string(probe_run_id)
        + ";family=" + family
        + ";grid_ref=" + std::to_string(grid_ref)
        + ";run_ms=" + std::to_string(blueprint.run_ms)
        + ";vi=" + std::to_string(blueprint.vi_stall_ms);
    if (!frame_hex.empty()) {
        fingerprint += ";frame=" + frame_hex;
    }
    return fingerprint;
}

std::vector<GridFanoutEntry> SeedProbeGridJobPersistenceAdapter::BuildFanout() const {
    std::vector<GridFanoutEntry> entries;

    const auto append_family = [this, &entries](const std::vector<simcore::GCInputFrame>& frames, const char* family) {
        for (const auto& frame : frames) {
            const auto frame_hex = frame.to_frame_hex();
            const auto next_ref = static_cast<std::int64_t>(entries.size()) + 1;

            JobPersistenceRecord record{};
            record.program_ref_kind = "sp_probe_run";
            record.program_ref_id = blueprint_.probe_id;
            record.program_version = blueprint_.program_version;
            record.fingerprint = FingerprintFor(blueprint_, blueprint_.probe_id, frame_hex, family, next_ref);

            entries.push_back(GridFanoutEntry{
                .domain_ref_id = next_ref,
                .frame = frame,
                .frame_hex = frame_hex,
                .persistence = std::move(record),
                });
        }
    };

    append_family(simcore::build_grid_main(grid_.samples_per_axis, grid_.min_value, grid_.max_value), "main");
    append_family(simcore::build_grid_cstick(grid_.samples_per_axis, grid_.min_value, grid_.max_value), "cstick");
    append_family(
        simcore::build_grid_trig(
            grid_.samples_per_axis,
            grid_.ignore_trigger_minmax ? 0 : grid_.min_value,
            grid_.ignore_trigger_minmax ? 255 : grid_.max_value,
            grid_.cap_trigger_top),
        "trigger");

    return entries;
}

SeedProbeRuntimeInitAdapter::SeedProbeRuntimeInitAdapter(simcore::db::IExecutionDb* execution_db, const simcore::db::IAnalysisDb* analysis_db)
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

SeedProbeGridResultMapper::SeedProbeGridResultMapper(simcore::db::IAnalysisDb* analysis_db, ContextLookupFn lookup_context)
    : analysis_db_(analysis_db)
    , lookup_context_(std::move(lookup_context)) {
}

std::string SeedProbeGridResultMapper::BuildResultIniFromPrResult(std::int64_t /*job_id*/, const simcore::PRResult& result) const {
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

ResultMapPayload SeedProbeGridResultMapper::MapPrimaryResult(std::int64_t job_id, const std::string& result_ini) const {
    ResultMapPayload payload{};

    if (analysis_db_ == nullptr || !lookup_context_) {
        payload.result_kind = "analysisseedprobe.unavailable";
        return payload;
    }

    const auto context = lookup_context_(job_id);
    if (!context.has_value()) {
        payload.result_kind = "analysisseedprobe.context_missing";
        return payload;
    }

    const auto parsed = ParseFrame(context->frame_hex);
    if (!parsed.has_value()) {
        payload.result_kind = "analysisseedprobe.grid_seed.invalid_frame";
        return payload;
    }

    const auto parsed_result = ResultsIni::from_section(IniDoc::parse(result_ini));
    const auto observed_seed = static_cast<std::int64_t>(parsed_result.rng_seed);
    const auto seed_delta = static_cast<std::int64_t>(observed_seed - static_cast<std::int64_t>(context->neutral_seed));

    if (context->phase == SeedProbeWorkflowPhase::Grid) {
        simcore::db::RecordSeedProbeGridSeedCommand cmd{};
        cmd.probe_result_id = context->probe_result_id;
        cmd.source_family = FamilyLabel(parsed->get_family());
        cmd.axis_xy_id = AxisXYId(*parsed);
        cmd.seed_value = observed_seed;
        cmd.seed_delta = seed_delta;
        cmd.recorded_at_utc = simcore::db::types::UtcNow();
        cmd.event_id = EventId(job_id, "grid");
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

std::string SeedProbeGridResultMapper::EventId(std::int64_t job_id, const char* phase_label) {
    return "seedprobe-job-" + std::to_string(job_id) + "-" + phase_label;
}

std::optional<simcore::GCInputFrame> SeedProbeGridResultMapper::ParseFrame(const std::string& frame_hex) {
    if (frame_hex.empty()) {
        return std::nullopt;
    }

    const auto bytes = hex_to_bytes(frame_hex);
    if (bytes.size() != sizeof(simcore::GCInputFrame)) {
        return std::nullopt;
    }

    simcore::GCInputFrame frame{};
    std::memcpy(&frame, bytes.data(), sizeof(simcore::GCInputFrame));
    return frame;
}

std::string SeedProbeGridResultMapper::FamilyLabel(std::uint8_t family) {
    switch (static_cast<simcore::SeedFamily>(family)) {
    case simcore::SeedFamily::Main: return "main";
    case simcore::SeedFamily::CStick: return "cstick";
    case simcore::SeedFamily::Triggers: return "trigger";
    }
    return "unknown";
}

std::int64_t SeedProbeGridResultMapper::AxisXYId(const simcore::GCInputFrame& frame) {
    const auto family = static_cast<simcore::SeedFamily>(frame.get_family());
    switch (family) {
    case simcore::SeedFamily::Main:
        return static_cast<std::int64_t>((static_cast<std::uint16_t>(frame.main_x) << 8) | frame.main_y);
    case simcore::SeedFamily::CStick:
        return static_cast<std::int64_t>((static_cast<std::uint16_t>(frame.c_x) << 8) | frame.c_y);
    case simcore::SeedFamily::Triggers:
        return static_cast<std::int64_t>((static_cast<std::uint16_t>(frame.trig_l) << 8) | frame.trig_r);
    }
    return 0;
}

ProgramKindDescriptor BuildSeedProbeGridDescriptor(
    simcore::db::IExecutionDb* execution_db,
    simcore::db::IAnalysisDb* analysis_db,
    SeedProbeGridBlueprintConfig blueprint,
    SeedProbeGridSpec grid,
    SeedProbeGridResultMapper::ContextLookupFn lookup_context) {
    ProgramKindDescriptor descriptor{};
    descriptor.program_kind = simcore::PK_SeedProbe;
    descriptor.program_name = "SeedProbe";
    descriptor.job_persistence = std::make_shared<SeedProbeGridJobPersistenceAdapter>(execution_db, std::move(blueprint), grid);
    descriptor.runtime_init = std::make_shared<SeedProbeRuntimeInitAdapter>(execution_db, analysis_db);
    descriptor.result_mapper = std::make_shared<SeedProbeGridResultMapper>(analysis_db, std::move(lookup_context));
    descriptor.workflow_transition = std::make_shared<GridToUniqueTransitionHandler>();
    descriptor.supports_workflow_orchestration = true;
    return descriptor;
}

} // namespace simcore::db::execution::programdb::seedprobe
