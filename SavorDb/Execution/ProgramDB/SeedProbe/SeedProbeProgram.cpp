#include "SeedProbeProgram.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "SeedProbeExecutionAdapters.h"
#include "SeedProbeJobSpec.h"
#include "../WorksetDerivedStateBinding.h"
#include "../WorksetObservationBinding.h"
#include "../../IExecutionDb.h"
#include "../../../Analysis/IAnalysisDb.h"
#include "../../../Authoring/IAuthoringDb.h"
#include "../../../Common/Types/UtcTimestamp.h"
#include "../../../State/IStateDb.h"
#include "../../../../SavorCore/Phases/Programs/SeedProbe/SeedProbeModule.h"
#include "../../../../SavorCore/Phases/RNGSeedDeltaMap.h"
#include "../../../../SavorCore/Runner/Runtime/ProgramKind.h"
#include "../../../../SavorCore/Runner/Runtime/RuntimeTypes.h"
#include "../../../../SavorCore/Utils/Hash.h"

namespace savor::db::execution::programdb::seedprobe {
namespace {

constexpr std::int32_t kProgramVersion =
    runtime::seedprobe::ProgramVersion;
constexpr std::int32_t kJobSpecVersion = 1;
constexpr std::string_view kRunRefKind = "sp_probe_run";
constexpr std::string_view kRootPurpose = "SEEDPROBE_SURVEY";
constexpr std::string_view kSearchPurpose = "SEEDPROBE_SEARCH";
constexpr std::string_view kConfirmPurpose = "SEEDPROBE_CONFIRM";
constexpr std::string_view kCreatedBy = "seedprobe_program_kind";

const WorksetObservationDefaultsV1& ObservationDefaults()
{
    static const WorksetObservationDefaultsV1 defaults{};
    return defaults;
}

std::filesystem::path WorkingRoot(const std::filesystem::path& configured)
{
    return configured.empty()
        ? std::filesystem::temp_directory_path() / "savor-seedprobe"
        : configured;
}

struct SurveyObservation {
    SeedProbeJobSpec job;
    SeedProbeResultRow result;
    AnalysisInputSetFrameRow frame;
    SeedFamily family = SeedFamily::Neutral;
};

struct PublishedJobSet {
    std::int64_t job_set_id = 0;
    bool created = false;
};

std::int64_t NowMs() {
    return types::UtcNow().time_since_epoch().count();
}

std::string Fingerprint(
    std::string_view materialization_key,
    const SeedProbeJobSpec& spec) {
    const auto input = EncodeSeedProbeJobSpec(spec);
    const auto canonical =
        std::string(materialization_key) + "\n" + input;
    return "PK=1;PV=2;seedprobe="
        + hash::sha256(canonical.data(), canonical.size());
}

std::string PlanSha256(const std::vector<SeedProbeJobSpec>& specs) {
    std::string canonical;
    for (const auto& spec : specs) {
        const auto encoded = EncodeSeedProbeJobSpec(spec);
        canonical.append(std::to_string(encoded.size()));
        canonical.push_back(':');
        canonical.append(encoded);
        canonical.push_back('\n');
    }
    return hash::sha256(canonical.data(), canonical.size());
}

std::string DynamicStepKey(
    std::string_view graph_node_key,
    std::string_view stage,
    const std::vector<SeedProbeJobSpec>& specs) {
    return std::string(graph_node_key) + "/" + std::string(stage) + "/"
        + PlanSha256(specs);
}

GCInputFrame NeutralFrame() {
    GCInputFrame frame{};
    frame.buttons = 0;
    frame.main_x = 128;
    frame.main_y = 128;
    frame.c_x = 128;
    frame.c_y = 128;
    frame.trig_l = 0;
    frame.trig_r = 0;
    return frame;
}

GCInputFrame ToFrame(const AnalysisInputSetFrameRow& row) {
    GCInputFrame frame = NeutralFrame();
    frame.main_x = static_cast<std::uint8_t>(row.main_x);
    frame.main_y = static_cast<std::uint8_t>(row.main_y);
    frame.c_x = static_cast<std::uint8_t>(row.cstick_x);
    frame.c_y = static_cast<std::uint8_t>(row.cstick_y);
    frame.trig_l = static_cast<std::uint8_t>(row.trigger_x);
    frame.trig_r = static_cast<std::uint8_t>(row.trigger_y);
    return frame;
}

std::int64_t AxisId(std::uint8_t x, std::uint8_t y) {
    return (static_cast<std::int64_t>(x) << 8)
        | static_cast<std::int64_t>(y);
}

std::optional<SeedFamily> ClassifySurveyFrame(
    const AnalysisInputSetFrameRow& row) {
    const bool main =
        row.main_x != 128 || row.main_y != 128;
    const bool cstick =
        row.cstick_x != 128 || row.cstick_y != 128;
    const bool triggers =
        row.trigger_x != 0 || row.trigger_y != 0;
    const auto changed =
        static_cast<int>(main)
        + static_cast<int>(cstick)
        + static_cast<int>(triggers);
    if (changed == 0) {
        return SeedFamily::Neutral;
    }
    if (changed != 1) {
        return std::nullopt;
    }
    if (main) {
        return SeedFamily::Main;
    }
    if (cstick) {
        return SeedFamily::CStick;
    }
    return SeedFamily::Triggers;
}

std::optional<std::int64_t> ResolveEntrySavestate(
    const ProgramJobMaterializationContext& context) {
    if (!context.graph.has_value()) {
        return context.step.domain_ref_id > 0
            ? std::optional<std::int64_t>(
                  context.step.domain_ref_id)
            : std::nullopt;
    }
    for (const auto& binding :
         context.graph->input_bindings) {
        if (binding.input_key == "entry_savestate"
            && binding.data_kind == "state.movie_inactive_savestate_id"
            && binding.ref_kind == "state.savestate"
            && binding.ref_id > 0) {
            return binding.ref_id;
        }
    }
    return std::nullopt;
}

std::optional<std::int64_t> ResolveAuthoredSpec(
    const ProgramJobMaterializationContext& context) {
    if (!context.graph.has_value()
        || !context.graph->authored_ref_id.has_value()
        || context.graph->authored_ref_kind
            != std::optional<std::string>("seed_probe_spec")) {
        return std::nullopt;
    }
    return context.graph->authored_ref_id;
}

int ResolveSamplesPerAxis(
    const ProgramJobMaterializationContext& context) {
    if (context.graph.has_value()) {
        for (const auto& argument : context.graph->arguments) {
            if (argument.argument_key == "samples_per_axis"
                && argument.integer_value.has_value()) {
                return static_cast<int>(
                    *argument.integer_value);
            }
        }
    }
    return 1;
}

bool IsBusinessFinal(std::string_view state) {
    return state == "SUCCEEDED"
        || state == "SUCCEEDED_WINNER"
        || state == "SUPERSEDED"
        || state == "SUCCEEDED_DUPLICATE"
        || state == "FAILED" || state == "INTERRUPTED" || state == "CANCELED";
}

std::int32_t SignedDelta(
    std::uint32_t seed,
    std::uint32_t neutral) {
    return savor::wrapped_seed_delta(seed, neutral);
}

std::optional<SeedProbeJobSpec> JobSpecFor(
    IExecutionDb* execution_db,
    std::int64_t job_id) {
    const auto job = execution_db != nullptr
        ? execution_db->GetExecutionJob(job_id)
        : std::nullopt;
    if (!job.has_value()) {
        return std::nullopt;
    }
    return DecodeSeedProbeJobSpec(job->input_ini);
}

std::string JoinIds(
    const std::vector<std::int64_t>& values) {
    std::ostringstream out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << values[i];
    }
    return out.str();
}

class SeedProbeMaterializer final : public IProgramJobMaterializer,
                                    public IWorkflowTransitionHandler {
public:
    SeedProbeMaterializer(
        IExecutionDb* execution_db,
        IStateDb* state_db,
        IAnalysisDb* analysis_db,
        IAuthoringDb* authoring_db,
        SeedProbeProgramConfig config)
        : execution_db_(execution_db)
        , state_db_(state_db)
        , analysis_db_(analysis_db)
        , authoring_db_(authoring_db)
        , config_(std::move(config)) {
        phase_ = runtime::seedprobe::
            SeedProbeFullPhaseDefinitionV2();
        if (!phase_ || !phase_->identity()) {
            module_error_ =
                "SeedProbe Full Phase definition is unavailable";
        }
    }

    bool Materialize(
        const ProgramJobMaterializationContext& context,
        WorkflowStepScheduleResult* result_out,
        std::string* error_out) const override {
        if (result_out == nullptr) {
            return Fail(
                "SeedProbe schedule result output is required",
                error_out);
        }
        *result_out = {};
        if (!DependenciesReady(error_out)) {
            return false;
        }
        if (context.step.step_kind == "seedprobe.search"
            || context.step.step_kind == "seedprobe.confirm") {
            return MaterializeContinuationStage(context, result_out, error_out);
        }
        if (context.step.step_kind != "seedprobe.survey") {
            return Fail("unsupported SeedProbe workflow step kind", error_out);
        }
        const auto entry_savestate =
            ResolveEntrySavestate(context);
        const auto authored_spec = ResolveAuthoredSpec(context);
        const auto samples_per_axis =
            ResolveSamplesPerAxis(context);
        if (!entry_savestate.has_value()
            || !authored_spec.has_value()
            || samples_per_axis <= 0
            || samples_per_axis > 64) {
            return Fail(
                "SeedProbe requires an entry savestate, authored "
                "seed_probe_spec, and samples_per_axis in [1,64]",
                error_out);
        }
        const auto spec =
            authoring_db_->GetSeedProbeSpec(*authored_spec);
        if (!spec.has_value()
            || spec->min_value < 0
            || spec->max_value > 255
            || spec->min_value > spec->max_value
            || spec->combo_attempts_per_target < 0
            || spec->combo_sampler_tries < 0) {
            return Fail(
                "SeedProbe authored specification is invalid",
                error_out);
        }
        const auto savestate =
            state_db_->GetSavestate(*entry_savestate);
        if (!savestate.has_value() || !savestate->is_complete
            || savestate->playback_state != SavestatePlaybackState::MovieInactive
            || savestate->dtm_artifact_id.has_value()) {
            return Fail(
                "SeedProbe entry savestate is not a complete movie-inactive state",
                error_out);
        }

        const auto root_key =
            "seedprobe.step."
            + std::to_string(context.step.workflow_step_id)
            + "." + context.step.step_key;
        std::int64_t probe_run_id = 0;
        std::int64_t job_set_id = 0;
        const auto existing =
            execution_db_->GetJobSetByMaterializationKey(
                root_key);
        if (existing.has_value()) {
            if (existing->purpose != kRootPurpose
                || existing->domain_ref_kind
                    != std::optional<std::string>(
                        std::string(kRunRefKind))
                || !existing->domain_ref_id.has_value()) {
                return Fail(
                    "SeedProbe Survey materialization key conflicts "
                    "with a different job set",
                    error_out);
            }
            job_set_id = existing->job_set_id;
            probe_run_id = *existing->domain_ref_id;
        } else {
            constexpr std::string_view flavor = "ENTRY_QUALIFIED";
            constexpr std::string_view policy = "seedprobe.entry_pc.v1";
            std::int64_t probe_set_id = 0;
            if (!analysis_db_->CreateSeedProbeSet(
                    {
                        .name =
                            std::string("seedprobe.") +
                            std::string(flavor) + "." +
                            std::string(policy),
                        .probe_flavor = std::string(flavor),
                        .breakpoint_policy_name = std::string(policy),
                        .segment_source_kind =
                            "workflow_program_kind",
                        .created_at_utc = types::UtcNow(),
                        .correlation_id =
                            "seedprobe-step-"
                            + std::to_string(
                                context.step.workflow_step_id),
                        .causation_id =
                            "workflow-"
                            + std::to_string(
                                context.step.workflow_instance_id),
                    },
                    &probe_set_id,
                    error_out)) {
                return false;
            }
            if (!analysis_db_->RequestSeedProbeRun(
                    {
                        .materialization_key = root_key,
                        .probe_set_id = probe_set_id,
                        .entry_savestate_id = *entry_savestate,
                        .seed_probe_spec_id = *authored_spec,
                        .launch_samples_per_axis =
                            samples_per_axis,
                        .codec_version = kProgramVersion,
                        .status = SeedProbeRunStatus::Survey,
                        .requested_at_utc = types::UtcNow(),
                        .correlation_id =
                            "seedprobe-step-"
                            + std::to_string(
                                context.step.workflow_step_id),
                        .causation_id =
                            "workflow-"
                            + std::to_string(
                                context.step.workflow_instance_id),
                    },
                    &probe_run_id,
                    error_out)) {
                return false;
            }
        }

        const auto run =
            analysis_db_->GetSeedProbeRun(probe_run_id);
        if (!run.has_value()
            || run->materialization_key != root_key
            || run->entry_savestate_id != *entry_savestate
            || run->seed_probe_spec_id != *authored_spec
            || run->launch_samples_per_axis
                != samples_per_axis) {
            return Fail(
                "SeedProbe run does not match the workflow request",
                error_out);
        }

        const auto frames = phase_->PlanSurvey({
            .samples_per_axis = samples_per_axis,
            .min_value = static_cast<std::int32_t>(spec->min_value),
            .max_value = static_cast<std::int32_t>(spec->max_value),
            .ignore_trigger_minmax = spec->ignore_trigger_minmax,
            .cap_trigger_top = spec->cap_trigger_top,
        });
        std::vector<SeedProbeJobSpec> jobs;
        jobs.reserve(frames.size());
        std::int32_t ordinal = 0;
        for (const auto& frame : frames) {
            std::int64_t input_frame_id = 0;
            if (!analysis_db_->EnsureSeedProbeInputFrame(
                    AxisId(frame.main_x, frame.main_y),
                    AxisId(frame.c_x, frame.c_y),
                    AxisId(frame.trig_l, frame.trig_r),
                    &input_frame_id,
                    error_out)) {
                return false;
            }
            jobs.push_back(
                {
                    .version = kJobSpecVersion,
                    .stage = SeedProbeJobStage::Survey,
                    .input_frame_id = input_frame_id,
                    .sample_ordinal = ordinal++,
                });
        }

        const auto published = PublishJobSet(
            context,
            root_key,
            context.step.workflow_step_id,
            kRootPurpose,
            "stage=SURVEY",
            probe_run_id,
            run->entry_savestate_id,
            context.step.step_priority + spec->priority,
            jobs,
            error_out);
        if (!published.has_value()) {
            return false;
        }
        job_set_id = published->job_set_id;
        result_out->job_set_id = job_set_id;
        result_out->persistence = {
            .program_ref_kind = std::string(kRunRefKind),
            .program_ref_id = probe_run_id,
            .fingerprint = root_key,
            .program_version = kProgramVersion,
        };
        result_out->event_lines.push_back(
            "[seedprobe-materialized] run="
            + std::to_string(probe_run_id)
            + " survey_jobs=" + std::to_string(jobs.size())
            + " job_set="
            + std::to_string(job_set_id));
        if (error_out != nullptr) {
            error_out->clear();
        }
        return true;
    }

    bool Continue(
        const ProgramJobContinuationContext& context,
        ProgramJobContinuationResult* result_out,
        std::string* error_out) const override {
        if (result_out == nullptr) {
            return Fail(
                "SeedProbe continuation output is required",
                error_out);
        }
        *result_out = {};
        if (!DependenciesReady(error_out)) {
            return false;
        }
        const auto probe_run_id =
            context.materialization.step.domain_ref_id;
        const auto run =
            analysis_db_->GetSeedProbeRun(probe_run_id);
        if (!run.has_value()
            || context.job_set_id <= 0) {
            return Fail(
                "SeedProbe continuation cannot resolve its run",
                error_out);
        }
        if (run->status == SeedProbeRunStatus::Invalidated) {
            result_out->disposition = ProgramJobContinuationDisposition::Failed;
            result_out->failure_code = "SEEDPROBE_RUN_ENDPOINT_INVALIDATED";
            result_out->failure_text = run->invalidation_diagnostic.value_or(
                "SeedProbe run was invalidated by conflicting endpoint evidence");
            return true;
        }
        if (run->status == SeedProbeRunStatus::Failed) {
            result_out->disposition = ProgramJobContinuationDisposition::Failed;
            result_out->failure_code = "SEEDPROBE_RUN_FAILED";
            result_out->failure_text = run->invalidation_diagnostic.value_or(
                "SeedProbe run is durably failed");
            return true;
        }

        if (context.materialization.step.step_kind == "seedprobe.survey") {
            return ContinueSurvey(
                context,
                *run,
                result_out,
                error_out);
        }
        if (context.materialization.step.step_kind == "seedprobe.search") {
            return ContinueSearch(
                context,
                *run,
                result_out,
                error_out);
        }
        if (context.materialization.step.step_kind == "seedprobe.confirm") {
            return ContinueConfirm(
                context,
                *run,
                result_out,
                error_out);
        }
        return Fail("unsupported SeedProbe continuation step kind", error_out);
    }

    WorkflowTransitionDecision EvaluateTransition(
        const WorkflowTransitionContext& context) const override {
        WorkflowTransitionDecision decision{};
        decision.should_advance = true;
        const auto jobs = execution_db_->ListJobsInJobSet(context.job_set_id);
        const auto source_job = jobs.empty()
            ? std::optional<ExecutionJobRecord>{}
            : execution_db_->GetExecutionJob(jobs.front().job_id);
        if (!source_job.has_value()
            || source_job->program_ref_kind != kRunRefKind) {
            decision.workflow_failure = true;
            decision.blocked_reason = "SeedProbe transition cannot resolve its run";
            return decision;
        }
        const auto run = analysis_db_->GetSeedProbeRun(source_job->program_ref_id);
        if (!run.has_value()) {
            decision.workflow_failure = true;
            decision.blocked_reason = "SeedProbe transition run is missing";
            return decision;
        }
        if (run->status == SeedProbeRunStatus::Failed
            || run->status == SeedProbeRunStatus::Invalidated) {
            decision.workflow_failure = true;
            decision.blocked_reason = run->invalidation_diagnostic.value_or(
                "SeedProbe run is durably failed");
            return decision;
        }

        std::vector<SeedProbeJobSpec> specs;
        std::string stage;
        std::string step_kind;
        std::string error;
        if (context.step_kind == "seedprobe.survey") {
            if (run->status == SeedProbeRunStatus::Search) {
                auto planned = BuildSearchSpecs(*run, &error);
                if (!planned.has_value()) {
                    decision.workflow_failure = true;
                    decision.blocked_reason = error;
                    return decision;
                }
                specs = std::move(*planned);
                stage = "Search";
                step_kind = "seedprobe.search";
            } else if (run->status == SeedProbeRunStatus::Confirm) {
                specs = BuildConfirmSpecs(run->probe_run_id);
                stage = "Confirm";
                step_kind = "seedprobe.confirm";
            }
        } else if (context.step_kind == "seedprobe.search") {
            specs = BuildConfirmSpecs(run->probe_run_id);
            stage = "Confirm";
            step_kind = "seedprobe.confirm";
        } else if (context.step_kind == "seedprobe.confirm"
                   && run->status == SeedProbeRunStatus::Confirm) {
            specs = BuildConfirmSpecs(run->probe_run_id);
            if (!specs.empty()) {
                stage = "Confirm";
                step_kind = "seedprobe.confirm";
            }
        }
        if (stage.empty()) {
            return decision;
        }
        if (specs.empty()) {
            decision.workflow_failure = true;
            decision.blocked_reason = "SeedProbe next stage has no jobs";
            return decision;
        }
        decision.spawn_steps.push_back({
            .step_key = DynamicStepKey(context.graph_node_key, stage, specs),
            .step_kind = std::move(step_kind),
            .input_ref_kind = std::string(kRunRefKind),
            .input_ref_id = run->probe_run_id,
            .priority = 0,
            .max_attempts = 1,
        });
        return decision;
    }

private:
    bool MaterializeContinuationStage(
        const ProgramJobMaterializationContext& context,
        WorkflowStepScheduleResult* result_out,
        std::string* error_out) const {
        const auto run = analysis_db_->GetSeedProbeRun(context.step.domain_ref_id);
        if (!run.has_value()) {
            return Fail("SeedProbe continuation stage run is missing", error_out);
        }
        std::vector<SeedProbeJobSpec> specs;
        std::string_view purpose;
        std::string_view stage;
        if (context.step.step_kind == "seedprobe.search") {
            auto planned = BuildSearchSpecs(*run, error_out);
            if (!planned.has_value()) return false;
            specs = std::move(*planned);
            purpose = kSearchPurpose;
            stage = "Search";
        } else {
            specs = BuildConfirmSpecs(run->probe_run_id);
            purpose = kConfirmPurpose;
            stage = "Confirm";
        }
        if (specs.empty()) {
            return Fail("SeedProbe continuation stage has no jobs", error_out);
        }
        const auto graph_node_key = context.graph.has_value()
            ? context.graph->activation_graph_node_key
            : context.step.step_key.substr(0, context.step.step_key.find('/'));
        const auto expected_key = DynamicStepKey(graph_node_key, stage, specs);
        if (context.step.step_key != expected_key) {
            return Fail("SeedProbe stage plan hash does not match its workflow step key", error_out);
        }
        const auto materialization_key =
            "seedprobe.step."
            + std::to_string(context.step.workflow_step_id)
            + "." + context.step.step_key;
        const auto published = PublishJobSet(
            context,
            materialization_key,
            context.step.workflow_step_id,
            purpose,
            "stage=" + std::string(stage) + ";plan_sha256=" + PlanSha256(specs),
            run->probe_run_id,
            run->entry_savestate_id,
            context.step.step_priority,
            specs,
            error_out);
        if (!published.has_value()) return false;
        result_out->job_set_id = published->job_set_id;
        result_out->persistence = {
            .program_ref_kind = std::string(kRunRefKind),
            .program_ref_id = run->probe_run_id,
            .fingerprint = materialization_key,
            .program_version = kProgramVersion,
        };
        result_out->event_lines.push_back(
            "[seedprobe-materialized] run=" + std::to_string(run->probe_run_id)
            + " stage=" + std::string(stage)
            + " jobs=" + std::to_string(specs.size())
            + " job_set=" + std::to_string(published->job_set_id));
        return true;
    }

    bool Fail(
        std::string message,
        std::string* error_out) const {
        if (error_out != nullptr) {
            *error_out = std::move(message);
        }
        return false;
    }

    bool DependenciesReady(std::string* error_out) const {
        if (execution_db_ == nullptr || state_db_ == nullptr
            || analysis_db_ == nullptr
            || authoring_db_ == nullptr) {
            return Fail(
                "SeedProbe descriptor dependencies are unavailable",
                error_out);
        }
        if (!phase_ || !phase_->identity()) {
            return Fail(
                module_error_.empty()
                    ? "SeedProbe module identity is unavailable"
                    : module_error_,
                error_out);
        }
        return true;
    }

    std::optional<PublishedJobSet> PublishJobSet(
        const ProgramJobMaterializationContext& materialization,
        std::string materialization_key,
        std::int64_t workflow_step_id,
        std::string_view purpose,
        std::string meta_note,
        std::int64_t probe_run_id,
        std::int64_t savestate_id,
        int priority,
        const std::vector<SeedProbeJobSpec>& specs,
        std::string* error_out) const {
        ResolvedWorksetObservationBindingV1 observation;
        if (!ResolveWorksetObservationBindingV1(
                materialization,
                ObservationDefaults(),
                WorkingRoot(config_.working_dir_root) / "captures",
                &observation,
                error_out))
        {
            return std::nullopt;
        }
        const auto program_package =
            savor::runtime::fullphase::BuildFullPhaseProgramPackage(*phase_);
        ResolvedWorksetDerivedStateBindingV1 derived_state;
        if (!ResolveWorksetDerivedStateBindingV1(
                std::span<const std::string>{},
                program_package,
                &derived_state,
                error_out)) {
            return std::nullopt;
        }
        EnsureMaterializingJobSetReceipt ensured{};
        if (!execution_db_->EnsureMaterializingJobSet(
                {
                    .materialization_key =
                        materialization_key,
                    .program_kind =
                        static_cast<std::int32_t>(
                            savor::PK_SeedProbe),
                    .purpose = std::string(purpose),
                    .created_by =
                        std::string(kCreatedBy),
                    .created_at_utc = NowMs(),
                    .priority_boost = priority,
                    .expected_total =
                        static_cast<int>(specs.size()),
                    .domain_ref_kind =
                        std::string(kRunRefKind),
                    .domain_ref_id = probe_run_id,
                    .meta_note = std::move(meta_note),
                },
                &ensured,
                error_out)
            || (ensured.disposition
                    != ExecutionDbOperationDisposition::Applied
                && ensured.disposition
                    != ExecutionDbOperationDisposition::AlreadyApplied)) {
            return std::nullopt;
        }

        if (ensured.materialization_state == "MATERIALIZING") {
            for (const auto& spec : specs) {
                CreatePendingJobReceipt receipt{};
                if (!execution_db_->CreatePendingJob(
                        {
                            .job_set_id =
                                ensured.job_set_id,
                            .program_kind =
                                static_cast<std::int32_t>(
                                    savor::PK_SeedProbe),
                            .program_version =
                                kProgramVersion,
                            .program_ref_kind =
                                std::string(kRunRefKind),
                            .program_ref_id = probe_run_id,
                            .savestate_id = savestate_id,
                            .fingerprint = Fingerprint(
                                materialization_key,
                                spec),
                            .priority = priority,
                            .max_attempts = 3,
                            .input_ini =
                                EncodeSeedProbeJobSpec(spec),
                            .cancellation_group_key =
                                SeedProbeCancellationGroupKey(
                                    probe_run_id,
                                    spec),
                        },
                        &receipt,
                        error_out)
                    || (receipt.disposition
                            != ExecutionDbOperationDisposition::Applied
                        && receipt.disposition
                            != ExecutionDbOperationDisposition::AlreadyApplied)) {
                    return std::nullopt;
                }
            }
        }

        const auto durable_jobs =
            execution_db_->ListJobsInJobSet(
                ensured.job_set_id);
        if (durable_jobs.size() != specs.size()) {
            if (error_out != nullptr) {
                *error_out =
                    "SeedProbe job-set population does not match "
                    "the deterministic plan";
            }
            return std::nullopt;
        }
        for (std::size_t i = 0; i < specs.size(); ++i) {
            if (durable_jobs[i].input_ini
                    != EncodeSeedProbeJobSpec(specs[i])
                || durable_jobs[i].cancellation_group_key
                    != SeedProbeCancellationGroupKey(
                        probe_run_id,
                        specs[i])) {
                if (error_out != nullptr) {
                    *error_out =
                        "SeedProbe durable job order differs from "
                        "the deterministic plan";
                }
                return std::nullopt;
            }
        }

        SealJobPopulationReceipt sealed{};
        if (!execution_db_->SealJobPopulation(
                {
                    .job_set_id = ensured.job_set_id,
                    .expected_job_count =
                        static_cast<int>(specs.size()),
                    .requested_by =
                        std::string(kCreatedBy),
                },
                &sealed,
                error_out)
            || (sealed.disposition
                    != ExecutionDbOperationDisposition::Applied
                && sealed.disposition
                    != ExecutionDbOperationDisposition::AlreadyApplied)) {
            return std::nullopt;
        }

        const auto chunk_size = std::max<std::size_t>(
            1,
            config_.maximum_items_per_workset);
        const auto workset_count =
            (durable_jobs.size() + chunk_size - 1)
            / chunk_size;
        std::vector<PublishWorksetCommand> worksets;
        worksets.reserve(workset_count);
        const auto& runtime_contract = phase_->runtime_contract();
        const auto contract_key =
            "seedprobe:v2:savestate:"
            + std::to_string(savestate_id)
            + ":phase:"
            + phase_->identity().canonical_sha256;
        for (std::size_t chunk = 0;
             chunk < workset_count;
             ++chunk) {
            const auto begin = chunk * chunk_size;
            const auto end = std::min(
                durable_jobs.size(),
                begin + chunk_size);
            std::vector<std::int64_t> job_ids;
            job_ids.reserve(end - begin);
            for (auto i = begin; i < end; ++i) {
                job_ids.push_back(durable_jobs[i].job_id);
            }
            worksets.push_back(
                {
                        .job_set_id =
                            ensured.job_set_id,
                        .workflow_step_id = workflow_step_id,
                        .workset_key =
                            materialization_key
                            + ".workset."
                            + std::to_string(chunk),
                        .program_kind =
                            static_cast<std::int32_t>(
                                savor::PK_SeedProbe),
                        .program_version =
                            kProgramVersion,
                        .contract =
                            {
                                .contract_key = contract_key,
                                .module_canonical_id =
                                    runtime_contract.module
                                        .canonical_id,
                                .module_version =
                                    static_cast<std::int32_t>(
                                        runtime_contract.module
                                            .revision),
                                .module_sha256 =
                                    runtime_contract.module
                                        .canonical_hash,
                                .entrypoint =
                                    runtime_contract.entrypoint,
                                .verified_dependency_sha256 =
                                    runtime_contract
                                        .verified_dependency_sha256,
                                .runtime_profile_sha256 =
                                    runtime_contract
                                        .runtime_profile_sha256,
                                .program_package_sha256 =
                                    program_package.canonical_sha256,
                                .estimated_payload_bytes =
                                    static_cast<std::uint64_t>(
                                        (end - begin) * 256 * 1024),
                            },
                        .derived_state = {
                            .binding_payload =
                                derived_state.encoded_binding,
                            .binding_sha256 =
                                derived_state.binding_sha256,
                        },
                        .observation = {
                            .capture_binding_payload =
                                observation.encoded_capture_binding,
                            .capture_binding_sha256 =
                                observation.capture_binding_sha256,
                            .progress_plan_payload =
                                observation.encoded_progress_plan,
                            .progress_plan_sha256 =
                                observation.progress_plan_sha256,
                        },
                        .priority = priority,
                        .ordered_job_ids =
                            std::move(job_ids),
                        .requested_by =
                            std::string(kCreatedBy),
                    });
        }

        PublishWorksetWaveReceipt published{};
        if (!execution_db_->PublishWorksetWave(
                {
                    .job_set_id = ensured.job_set_id,
                    .expected_job_count =
                        static_cast<int>(specs.size()),
                    .worksets = std::move(worksets),
                    .requested_by =
                        std::string(kCreatedBy),
                },
                &published,
                error_out)
            || (published.disposition
                    != ExecutionDbOperationDisposition::Applied
                && published.disposition
                    != ExecutionDbOperationDisposition::AlreadyApplied)) {
            return std::nullopt;
        }
        return PublishedJobSet{
            .job_set_id = ensured.job_set_id,
            .created =
                ensured.disposition
                == ExecutionDbOperationDisposition::Applied,
        };
    }

    bool SetRunStatus(
        std::int64_t probe_run_id,
        SeedProbeRunStatus expected,
        SeedProbeRunStatus desired,
        std::string* error_out) const {
        bool changed = false;
        const bool terminal =
            desired == SeedProbeRunStatus::Completed
            || desired
                == SeedProbeRunStatus::CompletedPartial
            || desired == SeedProbeRunStatus::Failed;
        return analysis_db_->UpdateSeedProbeRunStatus(
            {
                .probe_run_id = probe_run_id,
                .expected_status = expected,
                .new_status = desired,
                .completed_at_utc = terminal
                    ? std::optional<types::UtcTimePoint>(
                          types::UtcNow())
                    : std::nullopt,
                .changed_at_utc = types::UtcNow(),
                .correlation_id =
                    "seedprobe-run-"
                    + std::to_string(probe_run_id),
                .causation_id = "seedprobe-continuation",
            },
            &changed,
            error_out);
    }

    bool FailRun(
        const SeedProbeRunSnapshot& run,
        std::string code,
        std::string text,
        ProgramJobContinuationResult* result_out,
        std::string* error_out) const {
        if (!SetRunStatus(
                run.probe_run_id,
                run.status,
                SeedProbeRunStatus::Failed,
                error_out)) {
            return false;
        }
        result_out->disposition =
            ProgramJobContinuationDisposition::Failed;
        result_out->failure_code = std::move(code);
        result_out->failure_text = std::move(text);
        return true;
    }

    std::optional<std::vector<SurveyObservation>>
    LoadSurveyObservations(
        const SeedProbeRunSnapshot& run,
        std::string* error_out) const {
        const auto results =
            analysis_db_->ListSeedProbeResults(
                run.probe_run_id);
        std::vector<SurveyObservation> observations;
        observations.reserve(results.size());
        for (const auto& result : results) {
            const auto job = execution_db_->GetExecutionJob(result.source_job_id);
            if (!job.has_value()) {
                return Fail("SeedProbe Survey source job is missing", error_out), std::nullopt;
            }
            const auto spec = DecodeSeedProbeJobSpec(job->input_ini);
            if (!spec.has_value() || spec->stage != SeedProbeJobStage::Survey) {
                continue;
            }
            if (!IsBusinessFinal(job->state)) {
                if (error_out != nullptr) {
                    *error_out =
                        "SeedProbe Survey continuation ran before "
                        "all Survey jobs became business-final";
                }
                return std::nullopt;
            }
            const auto frame =
                analysis_db_->GetAnalysisInputFrame(
                    spec->input_frame_id);
            if (!frame.has_value()) {
                if (error_out != nullptr) {
                    *error_out =
                        "SeedProbe Survey input frame is missing";
                }
                return std::nullopt;
            }
            const auto family =
                ClassifySurveyFrame(*frame);
            if (!family.has_value()) {
                if (error_out != nullptr) {
                    *error_out =
                        "SeedProbe Survey frame changes more than "
                        "one inferred input family";
                }
                return std::nullopt;
            }
            observations.push_back(
                {
                    .job = *spec,
                    .result = result,
                    .frame = *frame,
                    .family = *family,
                });
        }
        std::sort(
            observations.begin(), observations.end(),
            [](const auto& lhs, const auto& rhs) {
                return lhs.job.sample_ordinal < rhs.job.sample_ordinal;
            });
        if (observations.empty()) {
            return Fail("SeedProbe Survey produced no observations", error_out), std::nullopt;
        }
        return observations;
    }

    std::optional<std::vector<SeedProbeJobSpec>> BuildSearchSpecs(
        const SeedProbeRunSnapshot& run,
        std::string* error_out) const {
        const auto observations = LoadSurveyObservations(run, error_out);
        if (!observations.has_value()) {
            return std::nullopt;
        }
        const SurveyObservation* neutral = nullptr;
        RandSeedProbeResult grid{};
        for (const auto& observation : *observations) {
            if (observation.family == SeedFamily::Neutral) {
                if (neutral != nullptr) {
                    return Fail("SeedProbe Survey has multiple neutral frames", error_out), std::nullopt;
                }
                neutral = &observation;
            }
        }
        if (neutral == nullptr) {
            return Fail("SeedProbe Survey has no neutral frame", error_out), std::nullopt;
        }
        grid.base_seed = neutral->result.seed_value;
        for (const auto& observation : *observations) {
            if (observation.family == SeedFamily::Neutral) continue;
            RandSeedProbeEntry entry{};
            entry.grid_n = observation.job.sample_ordinal;
            entry.family = observation.family;
            entry.seed = observation.result.seed_value;
            entry.delta = SignedDelta(observation.result.seed_value, neutral->result.seed_value);
            entry.ok = true;
            if (observation.family == SeedFamily::Main) {
                entry.x = static_cast<std::uint8_t>(observation.frame.main_x);
                entry.y = static_cast<std::uint8_t>(observation.frame.main_y);
            } else if (observation.family == SeedFamily::CStick) {
                entry.x = static_cast<std::uint8_t>(observation.frame.cstick_x);
                entry.y = static_cast<std::uint8_t>(observation.frame.cstick_y);
            } else {
                entry.x = static_cast<std::uint8_t>(observation.frame.trigger_x);
                entry.y = static_cast<std::uint8_t>(observation.frame.trigger_y);
            }
            grid.entries.push_back(std::move(entry));
        }
        const auto authored = authoring_db_->GetSeedProbeSpec(run.seed_probe_spec_id);
        if (!authored.has_value()) {
            return Fail("SeedProbe authored specification is missing", error_out), std::nullopt;
        }
        const auto planned = phase_->PlanSearch(
            grid,
            static_cast<std::uint32_t>(std::max(0, authored->combo_attempts_per_target)),
            static_cast<std::uint32_t>(std::max(0, authored->combo_sampler_tries)));
        std::vector<SeedProbeJobSpec> specs;
        std::int32_t ordinal = 0;
        for (const auto& target : planned.samples) {
            for (const auto& frame : target.frames) {
                std::int64_t input_frame_id = 0;
                if (!analysis_db_->EnsureSeedProbeInputFrame(
                        AxisId(frame.main_x, frame.main_y),
                        AxisId(frame.c_x, frame.c_y),
                        AxisId(frame.trig_l, frame.trig_r),
                        &input_frame_id, error_out)) {
                    return std::nullopt;
                }
                specs.push_back({
                    .version = kJobSpecVersion,
                    .stage = SeedProbeJobStage::Search,
                    .input_frame_id = input_frame_id,
                    .sample_ordinal = ordinal++,
                    .desired_delta = target.target_delta,
                });
            }
        }
        return specs;
    }

    std::optional<SeedProbeResultRow> NeutralResult(
        std::int64_t probe_run_id) const {
        for (const auto& result :
             analysis_db_->ListSeedProbeResults(
                 probe_run_id)) {
            const auto spec =
                JobSpecFor(execution_db_, result.source_job_id);
            const auto frame =
                analysis_db_->GetAnalysisInputFrame(
                    result.input_frame_id);
            if (spec.has_value()
                && spec->stage
                    == SeedProbeJobStage::Survey
                && frame.has_value()
                && ClassifySurveyFrame(*frame)
                    == std::optional<SeedFamily>(
                        SeedFamily::Neutral)) {
                return result;
            }
        }
        return std::nullopt;
    }

    std::vector<SeedProbeJobSpec> BuildConfirmSpecs(
        std::int64_t probe_run_id) const {
        const auto results =
            analysis_db_->ListSeedProbeResults(
                probe_run_id);
        std::unordered_set<std::int64_t> confirmed_candidates;
        for (const auto& result : results) {
            if (result.confirmation_of_probe_result_id.has_value()) {
                confirmed_candidates.insert(
                    *result.confirmation_of_probe_result_id);
            }
        }
        std::vector<SeedProbeResultRow> candidates;
        for (const auto& result : results) {
            if (result.evidence_state
                    == SeedProbeEvidenceState::Provisional
                && !confirmed_candidates.contains(
                    result.probe_result_id)) {
                candidates.push_back(result);
            }
        }
        std::sort(
            candidates.begin(),
            candidates.end(),
            [](const auto& lhs, const auto& rhs) {
                return lhs.probe_result_id
                    < rhs.probe_result_id;
            });
        std::vector<SeedProbeJobSpec> specs;
        specs.reserve(candidates.size());
        std::int32_t ordinal = 0;
        for (const auto& candidate : candidates) {
            specs.push_back(
                {
                    .version = kJobSpecVersion,
                    .stage = SeedProbeJobStage::Confirm,
                    .input_frame_id =
                        candidate.input_frame_id,
                    .sample_ordinal = ordinal++,
                    .confirmation_of_probe_result_id =
                        candidate.probe_result_id,
                });
        }
        return specs;
    }

    bool ContinueSurvey(
        const ProgramJobContinuationContext& context,
        const SeedProbeRunSnapshot& run,
        ProgramJobContinuationResult* result_out,
        std::string* error_out) const {
        auto observations = LoadSurveyObservations(
            run,
            error_out);
        if (!observations.has_value()) {
            const auto detail =
                error_out != nullptr && !error_out->empty()
                ? *error_out
                : "Survey did not produce every planned observation";
            if (error_out != nullptr) {
                error_out->clear();
            }
            return FailRun(
                run,
                "SEEDPROBE_SURVEY_EXHAUSTED",
                detail,
                result_out,
                error_out);
        }

        const SurveyObservation* neutral = nullptr;
        for (const auto& observation : *observations) {
            if (observation.family == SeedFamily::Neutral) {
                if (neutral != nullptr) {
                    return FailRun(
                        run,
                        "SEEDPROBE_SURVEY_INVALID",
                        "Survey contains more than one neutral frame",
                        result_out,
                        error_out);
                }
                neutral = &observation;
            }
        }
        if (neutral == nullptr) {
            return FailRun(
                run,
                "SEEDPROBE_SURVEY_INVALID",
                "Survey does not contain its neutral frame",
                result_out,
                error_out);
        }

        std::map<std::uint32_t, const SurveyObservation*>
            representative_by_seed;
        for (const auto& observation : *observations) {
            const auto [it, inserted] =
                representative_by_seed.emplace(
                    observation.result.seed_value,
                    &observation);
            if (!inserted
                && observation.job.sample_ordinal
                    < it->second->job.sample_ordinal) {
                it->second = &observation;
            }
        }
        for (const auto& [seed, observation] :
             representative_by_seed) {
            (void)seed;
            if (observation->result.evidence_state
                == SeedProbeEvidenceState::Observed) {
                bool changed = false;
                if (!analysis_db_->TransitionSeedProbeEvidence(
                        {
                            .probe_result_id =
                                observation->result
                                    .probe_result_id,
                            .expected_state =
                                SeedProbeEvidenceState::Observed,
                            .new_state =
                                SeedProbeEvidenceState::
                                    Provisional,
                            .changed_at_utc = types::UtcNow(),
                            .correlation_id =
                                "seedprobe-run-"
                                + std::to_string(
                                    run.probe_run_id),
                            .causation_id =
                                "seedprobe-survey-selection",
                        },
                        &changed,
                        error_out)) {
                    return false;
                }
            } else if (
                observation->result.evidence_state
                != SeedProbeEvidenceState::Provisional
                && observation->result.evidence_state
                != SeedProbeEvidenceState::Confirmed) {
                return FailRun(
                    run,
                    "SEEDPROBE_SURVEY_INVALID",
                    "Selected Survey evidence has an invalid state",
                    result_out,
                    error_out);
            }
        }

        RandSeedProbeResult grid{};
        grid.base_seed = neutral->result.seed_value;
        for (const auto& observation : *observations) {
            if (observation.family == SeedFamily::Neutral) {
                continue;
            }
            const auto delta = SignedDelta(
                observation.result.seed_value,
                neutral->result.seed_value);
            RandSeedProbeEntry entry{};
            entry.grid_n = observation.job.sample_ordinal;
            entry.family = observation.family;
            entry.seed = observation.result.seed_value;
            entry.delta = delta;
            entry.ok = true;
            if (observation.family == SeedFamily::Main) {
                entry.x = static_cast<std::uint8_t>(
                    observation.frame.main_x);
                entry.y = static_cast<std::uint8_t>(
                    observation.frame.main_y);
            } else if (
                observation.family == SeedFamily::CStick) {
                entry.x = static_cast<std::uint8_t>(
                    observation.frame.cstick_x);
                entry.y = static_cast<std::uint8_t>(
                    observation.frame.cstick_y);
            } else {
                entry.x = static_cast<std::uint8_t>(
                    observation.frame.trigger_x);
                entry.y = static_cast<std::uint8_t>(
                    observation.frame.trigger_y);
            }
            grid.entries.push_back(std::move(entry));
        }

        const auto authored =
            authoring_db_->GetSeedProbeSpec(
                run.seed_probe_spec_id);
        if (!authored.has_value()) {
            return FailRun(
                run,
                "SEEDPROBE_SPEC_MISSING",
                "The authored SeedProbe specification is missing",
                result_out,
                error_out);
        }
        const auto planned = phase_->PlanSearch(
            grid,
            static_cast<std::uint32_t>(
                std::max(
                    0,
                    authored->combo_attempts_per_target)),
            static_cast<std::uint32_t>(
                std::max(0, authored->combo_sampler_tries)));

        std::vector<SeedProbeJobSpec> search_specs;
        std::int32_t ordinal = 0;
        for (const auto& target : planned.samples) {
            for (const auto& frame : target.frames) {
                std::int64_t input_frame_id = 0;
                if (!analysis_db_->EnsureSeedProbeInputFrame(
                        AxisId(frame.main_x, frame.main_y),
                        AxisId(frame.c_x, frame.c_y),
                        AxisId(frame.trig_l, frame.trig_r),
                        &input_frame_id,
                        error_out)) {
                    return false;
                }
                search_specs.push_back(
                    {
                        .version = kJobSpecVersion,
                        .stage = SeedProbeJobStage::Search,
                        .input_frame_id = input_frame_id,
                        .sample_ordinal = ordinal++,
                        .desired_delta =
                            target.target_delta,
                    });
            }
        }

        if (!search_specs.empty()) {
            if (!SetRunStatus(
                    run.probe_run_id,
                    SeedProbeRunStatus::Survey,
                    SeedProbeRunStatus::Search,
                    error_out)) {
                return false;
            }
            result_out->disposition =
                ProgramJobContinuationDisposition::Complete;
            result_out->event_lines.push_back(
                "[seedprobe-stage] run="
                + std::to_string(run.probe_run_id)
                + " stage=SEARCH jobs="
                + std::to_string(search_specs.size()));
            return true;
        }

        const auto confirm_specs =
            BuildConfirmSpecs(run.probe_run_id);
        if (confirm_specs.empty()
            || !SetRunStatus(
                run.probe_run_id,
                SeedProbeRunStatus::Survey,
                SeedProbeRunStatus::Confirm,
                error_out)) {
            return confirm_specs.empty()
                ? Fail(
                      "SeedProbe Survey selected no evidence to confirm",
                      error_out)
                : false;
        }
        result_out->disposition =
            ProgramJobContinuationDisposition::Complete;
        result_out->event_lines.push_back(
            "[seedprobe-stage] run="
            + std::to_string(run.probe_run_id)
            + " stage=CONFIRM jobs="
            + std::to_string(confirm_specs.size()));
        return true;
    }

    bool ContinueSearch(
        const ProgramJobContinuationContext& context,
        const SeedProbeRunSnapshot& run,
        ProgramJobContinuationResult* result_out,
        std::string* error_out) const {
        const auto confirm_specs =
            BuildConfirmSpecs(run.probe_run_id);
        if (confirm_specs.empty()) {
            return FailRun(
                run,
                "SEEDPROBE_CONFIRM_PLAN_EMPTY",
                "SeedProbe Search selected no evidence to confirm",
                result_out,
                error_out);
        }
        if (!SetRunStatus(
                run.probe_run_id,
                SeedProbeRunStatus::Search,
                SeedProbeRunStatus::Confirm,
                error_out)) {
            return false;
        }
        result_out->disposition =
            ProgramJobContinuationDisposition::Complete;
        result_out->event_lines.push_back(
            "[seedprobe-stage] run="
            + std::to_string(run.probe_run_id)
            + " stage=CONFIRM jobs="
            + std::to_string(confirm_specs.size()));
        return true;
    }

    bool RejectUnobservedConfirmations(
        const SeedProbeRunSnapshot& run,
        std::int64_t job_set_id,
        std::string* error_out) const {
        for (const auto& job : execution_db_->ListJobsInJobSet(job_set_id)) {
                if (!IsBusinessFinal(job.state)) {
                    return Fail(
                        "SeedProbe Confirm continuation ran before "
                        "every Confirm job became business-final",
                        error_out);
                }
                const auto spec =
                    DecodeSeedProbeJobSpec(job.input_ini);
                if (!spec.has_value()
                    || spec->stage
                        != SeedProbeJobStage::Confirm
                    || !spec->confirmation_of_probe_result_id
                            .has_value()) {
                    return Fail(
                        "SeedProbe Confirm job metadata is invalid",
                        error_out);
                }
                if (analysis_db_
                        ->GetSeedProbeResultForSourceJob(
                            job.job_id)
                        .has_value()) {
                    continue;
                }
                const auto candidate =
                    analysis_db_->GetSeedProbeResult(
                        *spec->
                            confirmation_of_probe_result_id);
                if (!candidate.has_value()
                    || candidate->probe_run_id
                        != run.probe_run_id) {
                    return Fail(
                        "SeedProbe Confirm candidate is missing",
                        error_out);
                }
                if (candidate->evidence_state
                    != SeedProbeEvidenceState::Provisional) {
                    continue;
                }
                bool changed = false;
                if (!analysis_db_->TransitionSeedProbeEvidence(
                        {
                            .probe_result_id =
                                candidate->probe_result_id,
                            .expected_state =
                                SeedProbeEvidenceState::
                                    Provisional,
                            .new_state =
                                SeedProbeEvidenceState::
                                    Rejected,
                            .changed_at_utc = types::UtcNow(),
                            .correlation_id =
                                "seedprobe-run-"
                                + std::to_string(
                                    run.probe_run_id),
                            .causation_id =
                                "seedprobe-confirm-exhausted",
                        },
                        &changed,
                        error_out)) {
                    return false;
                }
        }
        return true;
    }

    struct DeltaCandidate {
        SeedProbeResultRow result;
        ExecutionJobRecord source_job;
        SeedProbeJobSpec spec;
        std::int32_t delta = 0;
        bool neutral = false;
    };

    std::optional<std::vector<DeltaCandidate>>
    LoadDeltaCandidates(
        const SeedProbeRunSnapshot& run,
        std::uint32_t neutral_seed,
        std::string* error_out) const {
        std::vector<DeltaCandidate> candidates;
        for (const auto& result :
             analysis_db_->ListSeedProbeResults(
                 run.probe_run_id)) {
            if (result.confirmation_of_probe_result_id
                    .has_value()) {
                continue;
            }
            const auto source_job =
                execution_db_->GetExecutionJob(result.source_job_id);
            const auto spec = source_job.has_value()
                ? DecodeSeedProbeJobSpec(
                      source_job->input_ini)
                : std::nullopt;
            if (!source_job.has_value()
                || !spec.has_value()
                || (spec->stage
                        != SeedProbeJobStage::Survey
                    && spec->stage
                        != SeedProbeJobStage::Search)) {
                Fail(
                    "SeedProbe candidate has invalid source job metadata",
                    error_out);
                return std::nullopt;
            }
            bool neutral = false;
            if (spec->stage == SeedProbeJobStage::Survey) {
                const auto frame =
                    analysis_db_->GetAnalysisInputFrame(
                        result.input_frame_id);
                const auto family = frame.has_value()
                    ? ClassifySurveyFrame(*frame)
                    : std::nullopt;
                if (!family.has_value()) {
                    Fail(
                        "SeedProbe Survey candidate frame is invalid",
                        error_out);
                    return std::nullopt;
                }
                neutral = *family == SeedFamily::Neutral;
            }
            candidates.push_back(
                {
                    .result = result,
                    .source_job = *source_job,
                    .spec = *spec,
                    .delta = SignedDelta(
                        result.seed_value,
                        neutral_seed),
                    .neutral = neutral,
                });
        }
        return candidates;
    }

    static bool CandidateLess(
        const DeltaCandidate& lhs,
        const DeltaCandidate& rhs) {
        const int lhs_rank =
            lhs.spec.stage == SeedProbeJobStage::Survey
            ? 0
            : 1;
        const int rhs_rank =
            rhs.spec.stage == SeedProbeJobStage::Survey
            ? 0
            : 1;
        if (lhs_rank != rhs_rank) {
            return lhs_rank < rhs_rank;
        }
        if (lhs.spec.stage == SeedProbeJobStage::Survey) {
            if (lhs.spec.sample_ordinal
                != rhs.spec.sample_ordinal) {
                return lhs.spec.sample_ordinal
                    < rhs.spec.sample_ordinal;
            }
        } else {
            if (lhs.source_job.job_set_id
                != rhs.source_job.job_set_id) {
                return lhs.source_job.job_set_id
                    < rhs.source_job.job_set_id;
            }
            if (lhs.spec.sample_ordinal
                != rhs.spec.sample_ordinal) {
                return lhs.spec.sample_ordinal
                    < rhs.spec.sample_ordinal;
            }
        }
        if (lhs.source_job.job_id
            != rhs.source_job.job_id) {
            return lhs.source_job.job_id
                < rhs.source_job.job_id;
        }
        return lhs.result.probe_result_id
            < rhs.result.probe_result_id;
    }

    bool ContinueConfirm(
        const ProgramJobContinuationContext& context,
        const SeedProbeRunSnapshot& run,
        ProgramJobContinuationResult* result_out,
        std::string* error_out) const {
        if (!RejectUnobservedConfirmations(
                run,
                context.job_set_id,
                error_out)) {
            return false;
        }

        const auto neutral =
            NeutralResult(run.probe_run_id);
        if (!neutral.has_value()
            || neutral->evidence_state
                != SeedProbeEvidenceState::Confirmed) {
            return FailRun(
                run,
                "SEEDPROBE_NEUTRAL_CONFIRMATION_MISSING",
                "The neutral Survey observation was not confirmed",
                result_out,
                error_out);
        }

        auto candidates = LoadDeltaCandidates(
            run,
            neutral->seed_value,
            error_out);
        if (!candidates.has_value()) {
            return false;
        }
        std::sort(
            candidates->begin(),
            candidates->end(),
            CandidateLess);

        std::set<std::int32_t> confirmed_deltas;
        std::map<std::int32_t, std::vector<SeedProbeResultRow>>
            rejected_by_delta;
        std::map<std::int32_t, std::unordered_set<std::int64_t>>
            rejected_frames_by_delta;
        for (const auto& candidate : *candidates) {
            if (candidate.result.evidence_state
                == SeedProbeEvidenceState::Confirmed) {
                confirmed_deltas.insert(candidate.delta);
            } else if (
                candidate.result.evidence_state
                == SeedProbeEvidenceState::Rejected) {
                rejected_by_delta[candidate.delta].push_back(
                    candidate.result);
                rejected_frames_by_delta[candidate.delta].insert(
                    candidate.result.input_frame_id);
            }
        }

        // A rejected delta first searches the complete factual ledger.
        // CandidateLess keeps one-axis Grid frames ahead of combination
        // Search frames without treating either origin as a different
        // accepted-evidence kind.
        for (const auto& [delta, rejected] :
             rejected_by_delta) {
            (void)rejected;
            if (delta == 0
                || confirmed_deltas.contains(delta)) {
                continue;
            }
            const DeltaCandidate* best = nullptr;
            for (const auto& candidate : *candidates) {
                if (candidate.delta != delta
                    || candidate.result.evidence_state
                        != SeedProbeEvidenceState::Observed
                    || rejected_frames_by_delta[delta].contains(
                        candidate.result.input_frame_id)) {
                    continue;
                }
                if (best == nullptr) {
                    best = &candidate;
                }
            }
            if (best != nullptr) {
                bool changed = false;
                if (!analysis_db_->TransitionSeedProbeEvidence(
                        {
                            .probe_result_id =
                                best->result.probe_result_id,
                            .expected_state =
                                SeedProbeEvidenceState::
                                    Observed,
                            .new_state =
                                SeedProbeEvidenceState::
                                    Provisional,
                            .changed_at_utc = types::UtcNow(),
                            .correlation_id =
                                "seedprobe-run-"
                                + std::to_string(
                                    run.probe_run_id),
                            .causation_id =
                                "seedprobe-delta-ledger-recovery",
                        },
                        &changed,
                        error_out)) {
                    return false;
                }
            }
        }

        auto confirm_specs =
            BuildConfirmSpecs(run.probe_run_id);
        if (!confirm_specs.empty()) {
            result_out->disposition =
                ProgramJobContinuationDisposition::Complete;
            result_out->event_lines.push_back(
                "[seedprobe-recovery] run="
                + std::to_string(run.probe_run_id)
                + " action=confirm_recorded_delta_candidate jobs="
                + std::to_string(confirm_specs.size()));
            return true;
        }

        candidates = LoadDeltaCandidates(
            run,
            neutral->seed_value,
            error_out);
        if (!candidates.has_value()) {
            return false;
        }
        confirmed_deltas.clear();
        rejected_by_delta.clear();
        rejected_frames_by_delta.clear();
        for (const auto& candidate : *candidates) {
            if (candidate.result.evidence_state
                == SeedProbeEvidenceState::Confirmed) {
                confirmed_deltas.insert(candidate.delta);
            } else if (
                candidate.result.evidence_state
                == SeedProbeEvidenceState::Rejected) {
                rejected_by_delta[candidate.delta].push_back(
                    candidate.result);
                rejected_frames_by_delta[candidate.delta].insert(
                    candidate.result.input_frame_id);
            }
        }
        return CompleteRun(
            context,
            run,
            result_out,
            error_out);
    }

    bool CompleteRun(
        const ProgramJobContinuationContext& context,
        const SeedProbeRunSnapshot& run,
        ProgramJobContinuationResult* result_out,
        std::string* error_out) const {
        (void)context;
        const auto neutral =
            NeutralResult(run.probe_run_id);
        if (!neutral.has_value()
            || neutral->evidence_state
                != SeedProbeEvidenceState::Confirmed) {
            return FailRun(
                run,
                "SEEDPROBE_NEUTRAL_CONFIRMATION_MISSING",
                "SeedProbe cannot complete without confirmed "
                "neutral-frame evidence",
                result_out,
                error_out);
        }

        const auto candidates = LoadDeltaCandidates(
            run,
            neutral->seed_value,
            error_out);
        if (!candidates.has_value()) {
            return false;
        }

        std::set<std::int32_t> discovered_deltas;
        std::set<std::int32_t> rejected_deltas;
        std::map<std::int32_t, DeltaCandidate>
            confirmed_by_delta;
        for (const auto& candidate : *candidates) {
            discovered_deltas.insert(candidate.delta);
            if (candidate.result.evidence_state
                == SeedProbeEvidenceState::Rejected) {
                rejected_deltas.insert(candidate.delta);
            }
            if (candidate.result.evidence_state
                != SeedProbeEvidenceState::Confirmed) {
                continue;
            }
            const auto [it, inserted] =
                confirmed_by_delta.emplace(
                    candidate.delta,
                    candidate);
            if (!inserted
                && it->second.result.probe_result_id
                    != candidate.result.probe_result_id) {
                return Fail(
                    "SeedProbe has multiple confirmed representatives "
                    "for one actual delta",
                    error_out);
            }
        }
        const auto confirmed_neutral =
            confirmed_by_delta.find(0);
        if (confirmed_neutral == confirmed_by_delta.end()
            || !confirmed_neutral->second.neutral
            || confirmed_neutral->second.result.probe_result_id
                != neutral->probe_result_id) {
            return FailRun(
                run,
                "SEEDPROBE_NEUTRAL_CONFIRMATION_MISSING",
                "SeedProbe delta zero is not represented by its "
                "confirmed Neutral frame",
                result_out,
                error_out);
        }

        std::set<std::int32_t> unresolved_deltas;
        for (const auto delta : discovered_deltas) {
            if (confirmed_by_delta.contains(delta)) {
                continue;
            }
            if (delta == 0) {
                return FailRun(
                    run,
                    "SEEDPROBE_NEUTRAL_CONFIRMATION_MISSING",
                    "SeedProbe discovered delta zero without a "
                    "confirmed Neutral representative",
                    result_out,
                    error_out);
            }
            if (!rejected_deltas.contains(delta)) {
                return Fail(
                    "SeedProbe discovered delta has neither a "
                    "confirmed nor exhausted representative",
                    error_out);
            }
            unresolved_deltas.insert(delta);
        }

        std::vector<std::int64_t> frame_ids;
        std::unordered_set<std::int64_t> seen;
        frame_ids.push_back(
            confirmed_neutral->second.result.input_frame_id);
        seen.insert(
            confirmed_neutral->second.result.input_frame_id);
        for (const auto& [delta, candidate] :
             confirmed_by_delta) {
            if (delta == 0) {
                continue;
            }
            if (!seen.insert(
                    candidate.result.input_frame_id)
                    .second) {
                return Fail(
                    "SeedProbe accepted frame represents more than "
                    "one actual delta",
                    error_out);
            }
            frame_ids.push_back(
                candidate.result.input_frame_id);
        }
        if (!analysis_db_->ReplaceSeedProbeAcceptedInputFrames(
                {
                    .probe_run_id = run.probe_run_id,
                    .input_frame_ids = frame_ids,
                    .replaced_at_utc = types::UtcNow(),
                    .correlation_id =
                        "seedprobe-run-"
                        + std::to_string(run.probe_run_id),
                    .causation_id =
                        "seedprobe-completion",
                },
                error_out)) {
            return false;
        }
        const bool partial = !unresolved_deltas.empty();
        const auto terminal_status = partial
            ? SeedProbeRunStatus::CompletedPartial
            : SeedProbeRunStatus::Completed;
        if (!SetRunStatus(
                run.probe_run_id,
                SeedProbeRunStatus::Confirm,
                terminal_status,
                error_out)) {
            return false;
        }
        result_out->disposition =
            ProgramJobContinuationDisposition::Complete;
        result_out->output = ProgramJobContinuationOutput{
            .output_key = "seed_probe_run",
            .data_kind = "analysis.seed_probe_run",
            .ref_kind = "sp_probe_run",
            .ref_id = run.probe_run_id,
        };
        result_out->event_lines.push_back(
            "[seedprobe-completed] run="
            + std::to_string(run.probe_run_id)
            + " status="
            + std::string(
                partial ? "COMPLETED_PARTIAL" : "COMPLETED")
            + " accepted="
            + std::to_string(frame_ids.size())
            + " discovered="
            + std::to_string(discovered_deltas.size())
            + " unresolved="
            + std::to_string(unresolved_deltas.size()));
        return true;
    }

    IExecutionDb* execution_db_ = nullptr;
    IStateDb* state_db_ = nullptr;
    IAnalysisDb* analysis_db_ = nullptr;
    IAuthoringDb* authoring_db_ = nullptr;
    SeedProbeProgramConfig config_;
    std::shared_ptr<const runtime::seedprobe::
        ISeedProbeFullPhaseDefinitionV2> phase_;
    std::string module_error_;
};

} // namespace

ProgramKindDescriptor BuildSeedProbeProgramDescriptor(
    IExecutionDb* execution_db,
    IStateDb* state_db,
    IAnalysisDb* analysis_db,
    IAuthoringDb* authoring_db,
    SeedProbeProgramConfig config) {
    auto materializer =
        std::make_shared<SeedProbeMaterializer>(
            execution_db,
            state_db,
            analysis_db,
            authoring_db,
            config);
    auto execution = BuildSeedProbeExecutionAdapters(
        execution_db,
        state_db,
        analysis_db,
        config.working_dir_root);

    ProgramKindDescriptor descriptor{};
    descriptor.program_kind =
        static_cast<std::int32_t>(savor::PK_SeedProbe);
    descriptor.program_name = savor::ProgramKindDisplayName(descriptor.program_kind);
    descriptor.result_staging_root = config.working_dir_root;
    descriptor.full_phase_identity =
        savor::runtime::seedprobe::
            SeedProbeFullPhaseDefinitionV2()->identity();
    descriptor.default_progress_library_ids =
        ObservationDefaults().progress_library_ids;
    descriptor.default_derived_state_block_ids =
        std::vector<std::string>{};
    descriptor.default_progress_runtime_trigger_pcs =
        ObservationDefaults().runtime_sample_trigger_pcs;
    descriptor.job_materializer = materializer;
    descriptor.workflow_transition = materializer;
    descriptor.workset_reconstruction =
        std::move(execution.reconstruction);
    descriptor.result_handler =
        std::move(execution.result_handler);
    descriptor.supports_workflow_orchestration = true;
    return descriptor;
}

} // namespace savor::db::execution::programdb::seedprobe
