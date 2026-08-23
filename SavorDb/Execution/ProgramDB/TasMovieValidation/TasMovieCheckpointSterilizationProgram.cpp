#include "TasMovieCheckpointSterilizationProgram.h"
#include "PreparedSterilizedCheckpointEvidence.h"
#include "../WorksetDerivedStateBinding.h"
#include "../WorksetObservationBinding.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../IExecutionDb.h"
#include "../../../Analysis/IAnalysisDb.h"
#include "../../../Common/Types/UtcTimestamp.h"
#include "../../../State/IStateDb.h"
#include "../../../../SavorCore/Phases/Programs/TasMovieValidation/TasMovieValidationModule.h"
#include "../../../../SavorCore/Runner/IPC/DurableWorkerTerminalEnvelope.h"
#include "../../../../SavorCore/Runner/Runtime/ProgramKind.h"
#include "../../../../SavorCore/Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.h"
#include "../../../../SavorCore/Runner/Runtime/Worksets/ProgramBaseline.h"
#include "../../../../SavorCore/Runner/Runtime/Worksets/WorksetWireCodec.h"
#include "../../../../SavorCore/Utils/Hash.h"

namespace savor::db::execution::programdb::tasmoviecheckpointsterilization {
namespace {

constexpr std::string_view kProgramRefKind = "tmv_checkpoint_sterilization_request";
constexpr std::string_view kCreatedBy = "tas_movie_checkpoint_sterilization_program_kind";
constexpr std::string_view kPurpose = "TAS_MOVIE_CHECKPOINT_STERILIZATION";
constexpr std::string_view kStepKind = "tasmovie.checkpoint_sterilize";
constexpr std::string_view kInputKey = "paired_checkpoint_savestate";
constexpr std::string_view kInputDataKind = "state.movie_paired_savestate_id";
constexpr std::string_view kOutputKey = "sterilized_checkpoint_savestate";
constexpr std::string_view kOutputDataKind = "state.movie_inactive_savestate_id";
constexpr std::string_view kStateRefKind = "state.savestate";
constexpr std::size_t kDeclaredTerminalBytes = 128ull * 1024ull;

const WorksetObservationDefaultsV1& ObservationDefaults()
{
    static const WorksetObservationDefaultsV1 defaults{};
    return defaults;
}

std::int64_t NowMs() { return types::UtcNow().time_since_epoch().count(); }

bool IsLowerHexSha256(std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

std::filesystem::path WorkingRoot(const std::filesystem::path& configured) {
    return configured.empty()
        ? std::filesystem::temp_directory_path() / "savor-tas-movie-checkpoint-sterilization"
        : configured;
}

bool EnsureParent(const std::filesystem::path& path, std::string* error_out) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (!error) return true;
    if (error_out) *error_out = "failed creating TAS Movie sterilization directory: " + error.message();
    return false;
}

std::optional<std::string> HashFile(const std::filesystem::path& path) {
    try { return hash::sha256_of_file(path.string()); }
    catch (...) { return std::nullopt; }
}

std::optional<std::int64_t> Binding(
    const ProgramJobMaterializationContext& context) {
    if (context.graph) {
        for (const auto& binding : context.graph->input_bindings) {
            if (binding.input_key == kInputKey && binding.data_kind == kInputDataKind
                && binding.ref_kind == kStateRefKind && binding.ref_id > 0) {
                return binding.ref_id;
            }
        }
    }
    if (context.step.domain_ref_id > 0 &&
        context.step.domain_ref_kind == kStateRefKind)
        return context.step.domain_ref_id;
    return std::nullopt;
}

std::string JobInput(std::int64_t request_id) {
    return "TCSR1:" + std::to_string(request_id);
}

std::string Fingerprint(
    const TasMovieCheckpointSterilizationRequestRecord& request) {
    const std::string value = request.materialization_key + "\n"
        + request.source_savestate_sha256 + "\n" + request.source_dtm_sha256
        + "\n" + request.full_phase_sha256;
    return "PK=11;PV=1;tasmovie_checkpoint_sterilize="
        + hash::sha256(value.data(), value.size());
}

ProgramResultDecision FinalDecision(
    std::string state,
    std::optional<std::string> code = std::nullopt,
    std::optional<std::string> text = std::nullopt) {
    ProgramResultDecision result{};
    result.final_job_state = std::move(state);
    result.error_code = std::move(code);
    result.error_text = std::move(text);
    return result;
}

ProgramResultOutput Output(std::int64_t savestate_id) {
    return {
        .output_key = std::string(kOutputKey),
        .data_kind = std::string(kOutputDataKind),
        .ref_kind = std::string(kStateRefKind),
        .ref_id = savestate_id,
    };
}

bool VerifySourceSnapshot(
    IStateDb* state_db,
    const TasMovieCheckpointSterilizationRequestRecord& request,
    SavestateRecord* source_out,
    std::string* error_out) {
    return tasmovieevidence::VerifyMoviePairedCheckpointSnapshot(
        state_db, request, source_out, error_out);
}

bool VerifyCanonicalResult(
    IStateDb* state_db,
    const TasMovieCheckpointSterilizationRequestRecord& request,
    std::int64_t result_savestate_id,
    std::optional<std::string_view> expected_sha,
    std::string* error_out) {
    return tasmovieevidence::VerifyCanonicalSterilizedCheckpoint(
        state_db, request, result_savestate_id, expected_sha, error_out);
}

class Materializer final : public IProgramJobMaterializer {
public:
    Materializer(
        IExecutionDb* execution_db,
        IStateDb* state_db,
        IAnalysisDb* analysis_db,
        TasMovieCheckpointSterilizationProgramConfig config)
        : execution_db_(execution_db), state_db_(state_db), analysis_db_(analysis_db),
          config_(std::move(config)),
          phase_(savor::runtime::tasmovie::TasMovieCheckpointSterilizationFullPhaseDefinitionV1()) {}

    bool Materialize(
        const ProgramJobMaterializationContext& context,
        WorkflowStepScheduleResult* result_out,
        std::string* error_out) const override {
        if (!result_out || !execution_db_ || !state_db_ || !analysis_db_
            || !phase_ || !context.graph || context.step.step_kind != kStepKind) {
            return Fail("TAS Movie checkpoint sterilization materialization is incomplete", error_out);
        }
        *result_out = {};
        const auto source_id = Binding(context);
        const auto source = source_id ? state_db_->GetSavestate(*source_id) : std::nullopt;
        if (!source || !source->is_complete
            || source->playback_state != SavestatePlaybackState::MoviePaired
            || !source->dtm_artifact_id || !source->dtm_sha256
            || !source->dtm_filename || source->artifact_kind != "SAV") {
            return Fail("sterilization requires one complete movie-paired checkpoint", error_out);
        }
        const auto derivation = state_db_->FindSavestateDerivationBySourceAndMethod(
            *source_id, savor::runtime::tasmovie::SterilizationDerivationMethod);
        std::optional<std::int64_t> reused;
        if (derivation) {
            const auto existing = state_db_->GetSavestate(derivation->to_savestate_id);
            if (!existing || !existing->is_complete
                || existing->playback_state != SavestatePlaybackState::MovieInactive
                || existing->dtm_artifact_id) {
                return Fail("canonical sterilization derivation is inconsistent", error_out);
            }
            reused = existing->savestate_id;
        }

        ResolvedWorksetObservationBindingV1 observation;
        ResolvedWorksetDerivedStateBindingV1 derived_state;
        const auto program_package =
            savor::runtime::fullphase::BuildFullPhaseProgramPackage(*phase_);
        if (!reused && !ResolveWorksetObservationBindingV1(
                context,
                ObservationDefaults(),
                WorkingRoot(config_.working_dir_root) / "captures",
                &observation,
                error_out))
        {
            return false;
        }
        if (!reused && !ResolveWorksetDerivedStateBindingV1(
                std::span<const std::string>{},
                program_package,
                &derived_state,
                error_out)) {
            return false;
        }

        const auto& identity = phase_->identity();
        const auto& runtime = phase_->runtime_contract();
        CreateTasMovieCheckpointSterilizationRequestCommand command{};
        command.materialization_key = "tasmovie.checkpoint-sterilize.step."
            + std::to_string(context.step.workflow_step_id);
        command.workflow_instance_id = context.step.workflow_instance_id;
        command.workflow_step_id = context.step.workflow_step_id;
        command.source_savestate_id = source->savestate_id;
        command.source_savestate_artifact_id = source->artifact_id;
        command.source_savestate_sha256 = source->artifact_sha256;
        command.source_dtm_artifact_id = *source->dtm_artifact_id;
        command.source_dtm_sha256 = *source->dtm_sha256;
        command.reused_savestate_id = reused;
        command.full_phase_program_kind = identity.program_kind;
        command.full_phase_program_version = identity.program_version;
        command.full_phase_canonical_id = identity.canonical_id;
        command.full_phase_contract_revision = identity.contract_revision;
        command.full_phase_sha256 = identity.canonical_sha256;
        command.module_canonical_id = runtime.module.canonical_id;
        command.module_revision = runtime.module.revision;
        command.module_sha256 = runtime.module.canonical_hash;
        command.created_at_utc = types::UtcNow();
        std::int64_t request_id = 0;
        if (!analysis_db_->CreateTasMovieCheckpointSterilizationRequest(
                command, &request_id, error_out)) {
            return false;
        }
        const auto request =
            analysis_db_->GetTasMovieCheckpointSterilizationRequest(request_id);
        if (!request) return Fail("sterilization request cannot be reloaded", error_out);

        const int expected = reused ? 0 : 1;
        EnsureMaterializingJobSetReceipt ensured{};
        if (!execution_db_->EnsureMaterializingJobSet({
                .materialization_key = command.materialization_key,
                .program_kind = static_cast<std::int32_t>(savor::PK_TasMovieCheckpointSterilize),
                .purpose = std::string(kPurpose),
                .created_by = std::string(kCreatedBy),
                .created_at_utc = NowMs(),
                .priority_boost = context.step.step_priority,
                .expected_total = expected,
                .domain_ref_kind = std::string(kProgramRefKind),
                .domain_ref_id = request_id,
                .meta_note = std::string(kStepKind),
            }, &ensured, error_out)) {
            return false;
        }
        if (!reused && ensured.materialization_state == "MATERIALIZING") {
            CreatePendingJobReceipt created{};
            if (!execution_db_->CreatePendingJob({
                    .job_set_id = ensured.job_set_id,
                    .program_kind = static_cast<std::int32_t>(savor::PK_TasMovieCheckpointSterilize),
                    .program_version = 1,
                    .program_ref_kind = std::string(kProgramRefKind),
                    .program_ref_id = request_id,
                    .savestate_id = source->savestate_id,
                    .fingerprint = Fingerprint(*request),
                    .priority = context.step.step_priority,
                    .max_attempts = 1,
                    .input_ini = JobInput(request_id),
                }, &created, error_out)) {
                return false;
            }
        }
        const auto jobs = execution_db_->ListJobsInJobSet(ensured.job_set_id);
        if (static_cast<int>(jobs.size()) != expected
            || (!jobs.empty() && jobs.front().input_ini != JobInput(request_id))) {
            return Fail("sterilization job set shape drifted", error_out);
        }
        SealJobPopulationReceipt sealed{};
        if (!execution_db_->SealJobPopulation({
                .job_set_id = ensured.job_set_id,
                .expected_job_count = expected,
                .requested_by = std::string(kCreatedBy)}, &sealed, error_out)) {
            return false;
        }
        std::vector<PublishWorksetCommand> worksets;
        if (!reused) {
            worksets.push_back({
                .job_set_id = ensured.job_set_id,
                .workflow_step_id = context.step.workflow_step_id,
                .root_job_set_id = ensured.job_set_id,
                .workset_key = command.materialization_key + ".workset.0",
                .program_kind = static_cast<std::int32_t>(savor::PK_TasMovieCheckpointSterilize),
                .program_version = 1,
                .contract = {
                    .contract_key = "tasmovie-checkpoint-sterilize:v1:request:"
                        + std::to_string(request_id) + ":phase:" + request->full_phase_sha256,
                    .module_canonical_id = runtime.module.canonical_id,
                    .module_version = static_cast<std::int32_t>(runtime.module.revision),
                    .module_sha256 = runtime.module.canonical_hash,
                    .entrypoint = runtime.entrypoint,
                    .verified_dependency_sha256 = runtime.verified_dependency_sha256,
                    .runtime_profile_sha256 = runtime.runtime_profile_sha256,
                    .program_package_sha256 =
                        program_package.canonical_sha256,
                    .estimated_payload_bytes = 128ull * 1024ull,
                },
                .derived_state = {
                    .binding_payload = derived_state.encoded_binding,
                    .binding_sha256 = derived_state.binding_sha256,
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
                .priority = context.step.step_priority,
                .ordered_job_ids = {jobs.front().job_id},
                .requested_by = std::string(kCreatedBy),
            });
        }
        PublishWorksetWaveReceipt published{};
        if (!execution_db_->PublishWorksetWave({
                .job_set_id = ensured.job_set_id,
                .expected_job_count = expected,
                .worksets = std::move(worksets),
                .requested_by = std::string(kCreatedBy)}, &published, error_out)) {
            return false;
        }
        result_out->root_job_set_id = ensured.job_set_id;
        result_out->persistence = {
            .program_ref_kind = std::string(kProgramRefKind),
            .program_ref_id = request_id,
            .fingerprint = Fingerprint(*request),
            .program_version = 1,
        };
        result_out->event_lines.push_back(
            reused ? "[tasmovie-checkpoint-sterilization-reused] savestate="
                         + std::to_string(*reused)
                   : "[tasmovie-checkpoint-sterilization-materialized] request="
                         + std::to_string(request_id));
        return true;
    }

    bool Continue(
        const ProgramJobContinuationContext& context,
        ProgramJobContinuationResult* result_out,
        std::string* error_out) const override {
        if (!result_out || !analysis_db_ || !execution_db_) {
            return Fail("sterilization continuation is incomplete", error_out);
        }
        const auto request = analysis_db_->GetTasMovieCheckpointSterilizationRequestForWorkflowStep(
            context.materialization.step.workflow_step_id);
        if (!request) return Fail("sterilization continuation request is missing", error_out);
        std::int64_t output_id = 0;
        if (request->reused_savestate_id) {
            output_id = *request->reused_savestate_id;
        } else {
            const auto jobs = execution_db_->ListJobsInJobSet(context.root_job_set_id);
            if (jobs.size() != 1) return Fail("sterilization continuation lost singleton shape", error_out);
            const auto job = execution_db_->GetExecutionJob(jobs.front().job_id);
            if (!job || !job->worker_terminal_fingerprint)
                return Fail("sterilization worker terminal identity is missing", error_out);
            const auto attempt = analysis_db_->FindTasMovieCheckpointSterilizationAttempt(
                job->job_id, *job->worker_terminal_fingerprint);
            if (!attempt || attempt->sterilization_request_id != request->sterilization_request_id)
                return Fail("sterilization attempt is not durably persisted", error_out);
            output_id = attempt->produced_savestate_id;
        }
        std::string verification_error;
        if (!VerifyCanonicalResult(state_db_, *request, output_id, std::nullopt, &verification_error))
            return Fail(std::move(verification_error), error_out);
        result_out->disposition = ProgramJobContinuationDisposition::Complete;
        result_out->output = ProgramJobContinuationOutput{
            .output_key = std::string(kOutputKey),
            .data_kind = std::string(kOutputDataKind),
            .ref_kind = std::string(kStateRefKind),
            .ref_id = output_id,
        };
        return true;
    }

private:
    static bool Fail(std::string message, std::string* error_out) {
        if (error_out) *error_out = std::move(message);
        return false;
    }

    IExecutionDb* execution_db_{};
    IStateDb* state_db_{};
    IAnalysisDb* analysis_db_{};
    TasMovieCheckpointSterilizationProgramConfig config_;
    std::shared_ptr<const savor::runtime::tasmovie::
        ITasMovieCheckpointSterilizationFullPhaseDefinitionV1> phase_;
};

class Reconstruction final : public IWorksetReconstructionAdapter {
public:
    Reconstruction(IStateDb* state_db, IAnalysisDb* analysis_db, std::filesystem::path root)
        : state_db_(state_db), analysis_db_(analysis_db), root_(WorkingRoot(root)),
          phase_(savor::runtime::tasmovie::TasMovieCheckpointSterilizationFullPhaseDefinitionV1()) {}

    std::optional<WorksetReconstructionResult> Reconstruct(
        const WorksetReconstructionContext& context,
        std::string* error_out) const override {
        const auto fail = [&](std::string message) -> std::optional<WorksetReconstructionResult> {
            if (error_out) *error_out = std::move(message);
            return std::nullopt;
        };
        if (!state_db_ || !analysis_db_ || !phase_ || context.items.size() != 1
            || context.workset_id <= 0 || context.dispatch_attempt_id <= 0
            || context.workflow_step_id <= 0 || context.root_job_set_id <= 0
            || context.dispatch_token.empty() || !context.state_compatibility.Complete()) {
            return fail("sterilization reconstruction requires one exact item");
        }
        const auto& item = context.items.front();
        if (item.program_kind != static_cast<std::int32_t>(savor::PK_TasMovieCheckpointSterilize)
            || item.program_version != 1 || item.program_ref_kind != kProgramRefKind
            || item.program_ref_id <= 0 || item.savestate_id.value_or(0) <= 0
            || item.input_ini != JobInput(item.program_ref_id)
            || item.reserved_attempt_id == 0 || item.claim_token.empty()) {
            return fail("sterilization item identity drifted");
        }
        const auto request = analysis_db_->GetTasMovieCheckpointSterilizationRequest(item.program_ref_id);
        if (!request || request->workflow_step_id != context.workflow_step_id
            || request->source_savestate_id != *item.savestate_id
            || request->reused_savestate_id
            || request->full_phase_sha256 != phase_->identity().canonical_sha256
            || request->module_sha256 != phase_->runtime_contract().module.canonical_hash) {
            return fail("sterilization immutable identity drifted");
        }
        SavestateRecord source{};
        if (!VerifySourceSnapshot(state_db_, *request, &source, error_out)) return std::nullopt;

        const auto request_root = root_ / ("request-" + std::to_string(request->sterilization_request_id));
        const auto source_path = request_root / "source" / "checkpoint.sav";
        const auto dtm_path = std::filesystem::path(source_path.string() + ".dtm");
        const auto output_path = request_root / "capture" / "sterilized.sav";
        if (!EnsureParent(source_path, error_out) || !EnsureParent(output_path, error_out)
            || !state_db_->MaterializeArtifactToPath(
                source.artifact_id, source_path.string(), error_out)
            || !state_db_->MaterializeArtifactToPath(
                *source.dtm_artifact_id, dtm_path.string(), error_out)) {
            return std::nullopt;
        }
        const auto state_hash = HashFile(source_path);
        const auto dtm_hash = HashFile(dtm_path);
        if (!state_hash || *state_hash != request->source_savestate_sha256
            || !dtm_hash || *dtm_hash != request->source_dtm_sha256) {
            return fail("sterilization materialized baseline hash drifted");
        }
        savor::runtime::tasmovie::TasMovieCheckpointSterilizationRequestV1 native{
            .source_savestate_path = source_path.string(),
            .source_dtm_path = dtm_path.string(),
            .output_savestate_path = output_path.string(),
        };
        std::string diagnostic;
        const auto input = savor::runtime::tasmovie::
            EncodeTasMovieCheckpointSterilizationExecutionInputV1(native, &diagnostic);
        if (input.empty()) return fail("sterilization invocation binding failed: " + diagnostic);

        const auto& runtime = phase_->runtime_contract();
        savor::runtime::WorkerWorksetDefinition workset{};
        workset.workset_id = savor::runtime::WorkerWorksetId(
            static_cast<std::uint64_t>(context.dispatch_attempt_id));
        workset.phase_invocation = {
            .invocation_id = {
                .workflow_step_id = static_cast<std::uint64_t>(context.workflow_step_id),
                .root_job_set_id = static_cast<std::uint64_t>(context.root_job_set_id),
            },
            .program_package =
                savor::runtime::fullphase::
                    BuildFullPhaseProgramPackage(*phase_),
            .common_input =
                savor::runtime::fullphase::MakeFullPhaseCommonInput(
                    "soa.tas_movie_checkpoint_sterilize.CommonInput", 1),
        };
        workset.baseline = {
            .artifact = {
                .kind = savor::runtime::ProgramBaselineArtifactKind::Savestate,
                .state_path = source_path,
                .state_sha256 = *state_hash,
                .movie_path = dtm_path,
                .movie_sha256 = *dtm_hash,
                .compatibility = context.state_compatibility,
                .lineage = {
                    .edge = runtime.baseline_lineage,
                    .producer = "SavorDb.PK_TasMovieCheckpointSterilize",
                },
            },
            .lineage = runtime.baseline_lineage,
            .components = {
                savor::runtime::MakeTasMovieCheckpointSterilizationBaselineComponent(),
            },
        };
        workset.derived_state = context.derived_state;
        workset.capture = context.capture;
        workset.progress_plan = context.progress_plan;
        workset.execution_key = {
            .module = runtime.module,
            .entrypoint = runtime.entrypoint,
            .verified_dependency_sha256 = runtime.verified_dependency_sha256,
            .runtime_profile_sha256 = runtime.runtime_profile_sha256,
            .baseline = savor::runtime::ComputeProgramBaselineKey(workset.baseline),
            .movie_policy_sha256 = runtime.movie_policy_sha256,
            .service_policy_sha256 = runtime.service_policy_sha256,
            .program_package_sha256 =
                workset.phase_invocation.program_package
                    .canonical_sha256,
            .common_input_sha256 =
                workset.phase_invocation.common_input
                    .content_sha256,
            .derived_state_binding_sha256 =
                workset.derived_state.content_sha256,
            .capture_binding_sha256 = workset.capture
                ? workset.capture->content_sha256
                : savor::runtime::EmptyWorksetCaptureBindingHashV1(),
            .progress_plan_sha256 =
                workset.progress_plan.content_sha256,
        };
        workset.execution_key.canonical_sha256 =
            savor::runtime::ComputeWorkerWorksetExecutionKeyHash(workset.execution_key);
        workset.items.push_back({
            .item_id = savor::runtime::WorkerWorksetItemId(
                static_cast<std::uint64_t>(item.job_id)),
            .ordinal = 0,
            .execution = {
                .execution_id = savor::runtime::ProgramExecutionId(
                    static_cast<std::uint64_t>(item.job_id)),
                .attempt_id = savor::runtime::AttemptId(item.reserved_attempt_id),
                .input_payload = input,
            },
            .declared_terminal_bytes = kDeclaredTerminalBytes,
            .correlation = {
                .durable_job_id = std::to_string(item.job_id),
                .claim_token = item.claim_token,
                .parent_correlation = context.contract_key,
            },
        });
        std::vector<std::uint8_t> encoded;
        const auto status = savor::runtime::EncodeWorkerWorksetV4(workset, encoded);
        if (!status) return fail("sterilization workset encoding failed: " + status.message);
        workset.encoded_size_bytes = encoded.size();
        return WorksetReconstructionResult{
            .workset = std::move(workset),
            .ordered_job_ids = {item.job_id},
        };
    }

private:
    IStateDb* state_db_{};
    IAnalysisDb* analysis_db_{};
    std::filesystem::path root_;
    std::shared_ptr<const savor::runtime::tasmovie::
        ITasMovieCheckpointSterilizationFullPhaseDefinitionV1> phase_;
};

class ResultHandler final : public IProgramResultHandler {
public:
    ResultHandler(IStateDb* state_db, IAnalysisDb* analysis_db)
        : state_db_(state_db), analysis_db_(analysis_db),
          phase_(savor::runtime::tasmovie::TasMovieCheckpointSterilizationFullPhaseDefinitionV1()) {}

    ProgramResultDecision Process(
        const ProgramResultProcessingContext& context) const override {
        if (!state_db_ || !analysis_db_ || !phase_
            || context.program_kind != static_cast<std::int32_t>(savor::PK_TasMovieCheckpointSterilize)
            || context.program_version != 1 || context.program_ref_kind != kProgramRefKind
            || context.program_ref_id <= 0 || context.input_ini != JobInput(context.program_ref_id)) {
            return FinalDecision("FAILED", "TAS_MOVIE_STERILIZATION_IDENTITY_INVALID",
                "job is not an exact checkpoint sterilization request");
        }
        const auto request = analysis_db_->GetTasMovieCheckpointSterilizationRequest(
            context.program_ref_id);
        if (!request || request->reused_savestate_id) {
            return FinalDecision("FAILED", "TAS_MOVIE_STERILIZATION_REQUEST_INVALID",
                "worker execution has no exact non-reused request");
        }
        std::string error;
        SavestateRecord source{};
        if (!VerifySourceSnapshot(state_db_, *request, &source, &error))
            return FinalDecision("FAILED", "TAS_MOVIE_STERILIZATION_SOURCE_DRIFT", error);

        savor::runtime::DurableWorkerTerminalEnvelope terminal{};
        if (!savor::runtime::DecodeDurableWorkerTerminalEnvelope(
                context.terminal.envelope, &terminal, &error)) {
            return FinalDecision("FAILED", "TAS_MOVIE_STERILIZATION_TERMINAL_INVALID", error);
        }
        using TerminalStatus = savor::wrms::InvocationTerminalStatus;
        if (terminal.terminal.status == TerminalStatus::Cancelled)
            return FinalDecision("FAILED", "UNCLASSIFIED_CANCELLED_TERMINAL_REACHED_DESCRIPTOR",
                "cancelled worker terminal bypassed ProgramResultProcessor");
        if (terminal.terminal.status != TerminalStatus::Succeeded
            || terminal.terminal.unstarted
            || terminal.terminal.workset_id
                != static_cast<std::uint64_t>(context.terminal.dispatch_attempt_id)
            || terminal.terminal.item_id != static_cast<std::uint64_t>(context.job_id)
            || terminal.terminal.invocation_id != static_cast<std::uint64_t>(context.job_id)
            || terminal.terminal.attempt_id != context.terminal.reserved_attempt_id
            || terminal.process_generation == 0 || terminal.terminal.workset_epoch == 0
            || terminal.terminal.session_disposition
                != savor::wrms::SessionDispositionCode::Clean) {
            return FinalDecision("FAILED",
                terminal.terminal.error_code.empty()
                    ? "TAS_MOVIE_STERILIZATION_EXECUTION_FAILED"
                    : terminal.terminal.error_code,
                terminal.terminal.message.empty()
                    ? "worker invocation did not complete cleanly"
                    : terminal.terminal.message);
        }
        const auto decoded = savor::runtime::program::DecodeProgramResultV1(
            terminal.terminal.result);
        if (!decoded
            || decoded.value->invocation_id.value()
                != static_cast<std::uint64_t>(context.job_id)
            || decoded.value->attempt_id.value() != context.terminal.reserved_attempt_id
            || decoded.value->artifacts.size() != 1) {
            return FinalDecision("FAILED", "TAS_MOVIE_STERILIZATION_PROGRAM_RESULT_MISMATCH",
                decoded ? "ProgramResult identity or artifact shape drifted"
                        : decoded.status.message);
        }
        savor::runtime::tasmovie::TasMovieCheckpointSterilizationResultV1 outcome{};
        if (!phase_->DecodeProgramResult(terminal.terminal.result, outcome, &error)
            || outcome.outcome != savor::runtime::tasmovie::
                TasMovieCheckpointSterilizationOutcomeV1::Sterilized) {
            return FinalDecision("FAILED", "TAS_MOVIE_STERILIZATION_OUTCOME_INVALID", error);
        }
        const auto& capture = decoded.value->artifacts.front().artifact;
        const std::filesystem::path path(capture.storage_reference);
        const auto actual_sha = HashFile(path);
        std::error_code size_error;
        const auto size = std::filesystem::file_size(path, size_error);
        if (!capture.complete || path.extension() != ".sav" || !actual_sha
            || *actual_sha != capture.content_hash.ToHex() || size_error || size == 0) {
            return FinalDecision("FAILED", "TAS_MOVIE_STERILIZATION_ARTIFACT_INVALID",
                "native savestate does not match finalized worker evidence");
        }
        if (std::filesystem::exists(path.string() + ".dtm")) {
            return FinalDecision("FAILED", "TAS_MOVIE_STERILIZATION_UNEXPECTED_SIDECAR",
                "movie-inactive native save produced a DTM sidecar");
        }

        CreateOrGetSterilizedCheckpointReceipt state_receipt{};
        if (!state_db_->CreateOrGetSterilizedCheckpoint({
                .from_savestate_id = request->source_savestate_id,
                .artifact = {
                    .sha256 = *actual_sha,
                    .size_bytes = static_cast<std::int64_t>(size),
                    .compression_kind = 0,
                    .filename = path.string(),
                    .file_ext = ".sav",
                    .artifact_kind = "SAV",
                    .created_at_utc = types::UtcNow(),
                    .correlation_id = "tmv-sterilization-request-"
                        + std::to_string(request->sterilization_request_id),
                    .causation_id = "execution-job-" + std::to_string(context.job_id),
                },
                .savestate_type = "TAS_MOVIE_STERILIZED_CHECKPOINT",
                .note = "Movie-inactive checkpoint derived from canonical TAS Movie checkpoint",
                .method_kind = std::string(savor::runtime::tasmovie::SterilizationDerivationMethod),
                .source_context_kind = std::string(kProgramRefKind),
                .source_context_id = request->sterilization_request_id,
                .created_at_utc = types::UtcNow(),
                .correlation_id = "tmv-sterilization-request-"
                    + std::to_string(request->sterilization_request_id),
                .causation_id = "execution-job-" + std::to_string(context.job_id),
            }, &state_receipt, &error)) {
            throw std::runtime_error(error.empty()
                ? "sterilized checkpoint State publication failed" : error);
        }
        if (!VerifyCanonicalResult(state_db_, *request, state_receipt.savestate_id,
                *actual_sha, &error)) {
            throw std::runtime_error(error);
        }
        RecordTasMovieCheckpointSterilizationAttemptCommand attempt{
            .sterilization_request_id = request->sterilization_request_id,
            .source_job_id = context.job_id,
            .worker_terminal_sha256 = context.terminal.sha256,
            .candidate_savestate_sha256 = *actual_sha,
            .produced_savestate_id = state_receipt.savestate_id,
            .worker_id = std::to_string(terminal.worker_id),
            .worker_process_generation = terminal.process_generation,
            .workset_epoch = terminal.terminal.workset_epoch,
            .recorded_at_utc = types::UtcNow(),
        };
        std::int64_t attempt_id = 0;
        if (!analysis_db_->RecordTasMovieCheckpointSterilizationAttempt(
                attempt, &attempt_id, &error)) {
            throw std::runtime_error(error);
        }
        auto decision = FinalDecision("SUCCEEDED");
        decision.cleanup_worker_staging = true;
        decision.outputs.push_back(Output(state_receipt.savestate_id));
        decision.event_lines.push_back(
            "[tasmovie-checkpoint-sterilized] attempt=" + std::to_string(attempt_id)
            + " savestate=" + std::to_string(state_receipt.savestate_id));
        return decision;
    }

    private:
    IStateDb* state_db_{};
    IAnalysisDb* analysis_db_{};
    std::shared_ptr<const savor::runtime::tasmovie::
        ITasMovieCheckpointSterilizationFullPhaseDefinitionV1> phase_;
};

class BattleRecordingSterilizationTransition final
    : public IWorkflowTransitionHandler {
public:
    explicit BattleRecordingSterilizationTransition(
        IAnalysisDb* analysis_db)
        : analysis_db_(analysis_db) {}

    WorkflowTransitionDecision EvaluateTransition(
        const WorkflowTransitionContext& context) const override {
        WorkflowTransitionDecision decision{};
        decision.should_advance = true;
        if (!analysis_db_ || context.output_ref_kind !=
                std::optional<std::string>(kStateRefKind) ||
            !context.output_ref_id)
            return decision;

        const auto request = analysis_db_
            ->GetTasMovieCheckpointSterilizationRequestForWorkflowStep(
                context.workflow_step_id);
        if (!request)
            return decision;

        const auto recording = analysis_db_
            ->GetBattleRecordingForPairedCheckpoint(
                request->source_savestate_id);
        if (!recording)
            return decision;

        if (recording->status != "COMPLETED" ||
            recording->outcome != std::optional<std::string>("RECORDED") ||
            !recording->tas_movie_tree_id ||
            !recording->validation_request_id ||
            recording->paired_checkpoint_savestate_id !=
                request->source_savestate_id) {
            decision.should_advance = false;
            decision.terminal_failure = true;
            decision.blocked_reason =
                "battle_recording_sterilization_source_drifted";
            return decision;
        }

        const auto attempts = analysis_db_
            ->ListTasMovieCheckpointSterilizationAttemptsForRequest(
                request->sterilization_request_id);
        const bool exact_attempt = std::ranges::any_of(
            attempts, [&](const auto& attempt) {
                return attempt.produced_savestate_id ==
                    *context.output_ref_id;
            });
        if (!exact_attempt) {
            decision.should_advance = false;
            decision.terminal_failure = true;
            decision.blocked_reason =
                "battle_recording_sterilization_attempt_missing";
            return decision;
        }

        std::string error;
        if (!analysis_db_->BindBattleRecordingSterilization({
                .battle_recording_id = recording->battle_recording_id,
                .tas_movie_tree_id = *recording->tas_movie_tree_id,
                .sterilization_request_id =
                    request->sterilization_request_id},
                &error)) {
            decision.should_advance = false;
            decision.terminal_failure = true;
            decision.blocked_reason = error.empty()
                ? std::optional<std::string>(
                    "battle_recording_sterilization_binding_failed")
                : std::optional<std::string>(std::move(error));
        }
        return decision;
    }

private:
    IAnalysisDb* analysis_db_{};
};

} // namespace

ProgramKindDescriptor BuildTasMovieCheckpointSterilizationProgramDescriptor(
    IExecutionDb* execution_db,
    IStateDb* state_db,
    IAnalysisDb* analysis_db,
    TasMovieCheckpointSterilizationProgramConfig config) {
    ProgramKindDescriptor descriptor{};
    descriptor.program_kind =
        static_cast<std::int32_t>(savor::PK_TasMovieCheckpointSterilize);
    descriptor.program_name = "TAS Movie Checkpoint Sterilization";
    descriptor.result_staging_root = config.working_dir_root;
    descriptor.full_phase_identity = savor::runtime::tasmovie::
        TasMovieCheckpointSterilizationFullPhaseDefinitionV1()->identity();
    descriptor.default_progress_library_ids =
        ObservationDefaults().progress_library_ids;
    descriptor.default_derived_state_block_ids =
        std::vector<std::string>{};
    descriptor.default_progress_runtime_trigger_pcs =
        ObservationDefaults().runtime_sample_trigger_pcs;
    descriptor.job_materializer = std::make_shared<Materializer>(
        execution_db, state_db, analysis_db, config);
    descriptor.workset_reconstruction = std::make_shared<Reconstruction>(
        state_db, analysis_db, config.working_dir_root);
    descriptor.result_handler = std::make_shared<ResultHandler>(state_db, analysis_db);
    descriptor.workflow_transition =
        std::make_shared<BattleRecordingSterilizationTransition>(analysis_db);
    descriptor.supports_workflow_orchestration = true;
    descriptor.allow_mixed_success_failed_transition = false;
    return descriptor;
}

} // namespace savor::db::execution::programdb::tasmoviecheckpointsterilization
