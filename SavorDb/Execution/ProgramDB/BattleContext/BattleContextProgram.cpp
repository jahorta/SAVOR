#include "BattleContextProgram.h"
#include "../WorksetDerivedStateBinding.h"
#include "../WorksetObservationBinding.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../IExecutionDb.h"
#include "../../../Analysis/IAnalysisDb.h"
#include "../../../Common/Types/UtcTimestamp.h"
#include "../../../State/IStateDb.h"
#include "../../../../SavorCore/Core/Memory/Soa/Battle/BattleContextCodec.h"
#include "../../../../SavorCore/Phases/Programs/BattleContext/BattleContextModule.h"
#include "../../../../SavorCore/Runner/IPC/DurableWorkerTerminalEnvelope.h"
#include "../../../../SavorCore/Runner/Runtime/ProgramKind.h"
#include "../../../../SavorCore/Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.h"
#include "../../../../SavorCore/Runner/Runtime/Worksets/WorksetWireCodec.h"
#include "../../../../SavorCore/Utils/Hash.h"

namespace savor::db::execution::programdb::battlecontext {
namespace {

constexpr std::string_view kProgramRefKind = "ab_battle_context";
constexpr std::string_view kCreatedBy = "battle_context_program_kind";
constexpr std::string_view kPurpose = "BATTLE_CONTEXT";
constexpr std::string_view kStepKind = "battle.context";
constexpr std::string_view kInputKey = "entry_savestate";
constexpr std::string_view kInputDataKind = "state.movie_inactive_savestate_id";
constexpr std::string_view kOutputKey = "battle_context";
constexpr std::string_view kOutputDataKind = "analysis_battle.battle_context_id";
constexpr std::string_view kStateRefKind = "state.savestate";
constexpr std::size_t kDeclaredTerminalBytes = 512ull * 1024ull;

const WorksetObservationDefaultsV1& ObservationDefaults()
{
    static const WorksetObservationDefaultsV1 defaults{};
    return defaults;
}

std::int64_t NowMs() { return types::UtcNow().time_since_epoch().count(); }

bool Fail(std::string message, std::string* error_out) {
    if (error_out) *error_out = std::move(message);
    return false;
}

std::filesystem::path WorkingRoot(const std::filesystem::path& configured) {
    return configured.empty()
        ? std::filesystem::temp_directory_path() / "savor-battle-context"
        : configured;
}

bool IsLowerHexSha256(std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

std::optional<std::string> HashFile(const std::filesystem::path& path) {
    try { return hash::sha256_of_file(path.string()); }
    catch (...) { return std::nullopt; }
}

bool ExactSavestate(const SavestateRecord& state, const std::filesystem::path& path) {
    std::error_code error;
    return state.is_complete && state.artifact_size_bytes > 0
        && IsLowerHexSha256(state.artifact_sha256)
        && std::filesystem::is_regular_file(path, error) && !error
        && static_cast<std::int64_t>(std::filesystem::file_size(path, error))
            == state.artifact_size_bytes && !error
        && HashFile(path) == std::optional<std::string>(state.artifact_sha256);
}

std::optional<std::filesystem::path> ResolveSavestate(
    const SavestateRecord& state,
    const std::filesystem::path& root,
    IStateDb* state_db,
    std::string* error_out) {
    const std::filesystem::path recorded(state.artifact_filename);
    if (ExactSavestate(state, recorded)) return recorded;
    const auto destination = WorkingRoot(root) / "baselines"
        / (std::to_string(state.savestate_id) + "-" + state.artifact_sha256 + ".sav");
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) {
        Fail("failed creating Battle Context baseline directory: " + error.message(), error_out);
        return std::nullopt;
    }
    if (!ExactSavestate(state, destination)) {
        const auto materialized = state_db->MaterializeSavestateToPath(
            state.savestate_id, destination.string(), error_out);
        if (!materialized || !ExactSavestate(state, destination)) {
            if (error_out && error_out->empty())
                *error_out = "State DB did not materialize the exact Battle Context baseline";
            return std::nullopt;
        }
    }
    return destination;
}

std::optional<std::int64_t> SourceBinding(
    const ProgramJobMaterializationContext& context) {
    if (!context.graph) return std::nullopt;
    for (const auto& binding : context.graph->input_bindings) {
        if (binding.input_key == kInputKey
            && binding.data_kind == kInputDataKind
            && binding.ref_kind == kStateRefKind
            && binding.ref_id > 0) {
            return binding.ref_id;
        }
    }
    return std::nullopt;
}

std::string JobInput(std::int64_t context_id) {
    return "BCTX1:" + std::to_string(context_id);
}

std::string Fingerprint(const BattleContextProbeSnapshot& request) {
    const std::string canonical = request.materialization_key.value_or("") + "\n"
        + request.source_savestate_sha256.value_or("") + "\n"
        + request.full_phase_sha256.value_or("");
    return "PK=4;PV=1;bctx=" + hash::sha256(canonical.data(), canonical.size());
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

ProgramResultOutput ContextOutput(std::int64_t context_id) {
    return {
        .output_key = std::string(kOutputKey),
        .data_kind = std::string(kOutputDataKind),
        .ref_kind = std::string(kProgramRefKind),
        .ref_id = context_id,
    };
}

bool ValidateSource(
    IStateDb* state_db,
    const BattleContextProbeSnapshot& request,
    SavestateRecord* source_out,
    std::string* error_out) {
    const auto source = state_db ? state_db->GetSavestate(request.source_savestate_id)
                                 : std::nullopt;
    if (!source || !source->is_complete || source->artifact_kind != "SAV"
        || source->playback_state != SavestatePlaybackState::MovieInactive
        || source->dtm_artifact_id
        || request.source_savestate_artifact_id != source->artifact_id
        || request.source_savestate_sha256 != source->artifact_sha256) {
        return Fail("Battle Context source savestate snapshot drifted", error_out);
    }
    if (source_out) *source_out = *source;
    return true;
}

bool WriteContextAtomically(
    const std::filesystem::path& destination,
    std::string_view bytes,
    std::string* error_out) {
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) return Fail("failed creating Battle Context artifact directory: " + error.message(), error_out);
    if (std::filesystem::is_regular_file(destination, error) && !error
        && HashFile(destination) == std::optional<std::string>(
            hash::sha256(bytes.data(), bytes.size()))) {
        return true;
    }
    const auto temporary = std::filesystem::path(destination.string() + ".publishing");
    std::filesystem::remove(temporary, error);
    error.clear();
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return Fail("failed opening Battle Context temporary artifact", error_out);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream) return Fail("failed writing Battle Context temporary artifact", error_out);
    }
    std::filesystem::rename(temporary, destination, error);
    if (!error) return true;
    if (std::filesystem::is_regular_file(destination)
        && HashFile(destination) == std::optional<std::string>(
            hash::sha256(bytes.data(), bytes.size()))) {
        std::filesystem::remove(temporary, error);
        return true;
    }
    return Fail("failed publishing Battle Context artifact: " + error.message(), error_out);
}

class Materializer final : public IProgramJobMaterializer {
public:
    Materializer(IExecutionDb* execution_db, IStateDb* state_db,
                 IAnalysisDb* analysis_db, BattleContextProgramConfig config)
        : execution_db_(execution_db), state_db_(state_db), analysis_db_(analysis_db),
          config_(std::move(config)),
          phase_(savor::runtime::battlecontext::BattleContextFullPhaseDefinitionV1()) {}

    bool Materialize(const ProgramJobMaterializationContext& context,
                     WorkflowStepScheduleResult* result_out,
                     std::string* error_out) const override {
        if (!result_out || !execution_db_ || !state_db_ || !analysis_db_ || !phase_
            || !context.graph || context.step.step_kind != kStepKind) {
            return Fail("Battle Context materialization is incomplete", error_out);
        }
        *result_out = {};
        const auto source_id = SourceBinding(context);
        const auto source = source_id ? state_db_->GetSavestate(*source_id) : std::nullopt;
        if (!source || !source->is_complete || source->artifact_kind != "SAV"
            || source->playback_state != SavestatePlaybackState::MovieInactive
            || source->dtm_artifact_id) {
            return Fail("battle.context requires one complete movie-inactive entry savestate", error_out);
        }
        const auto& identity = phase_->identity();
        const auto& runtime = phase_->runtime_contract();
        const std::string materialization_key = "battle.context.step."
            + std::to_string(context.step.workflow_step_id);
        ResolvedWorksetObservationBindingV1 observation;
        if (!ResolveWorksetObservationBindingV1(
                context,
                ObservationDefaults(),
                WorkingRoot(config_.working_dir_root) / "captures",
                &observation,
                error_out))
        {
            return false;
        }
        const auto program_package = savor::runtime::fullphase::
            BuildFullPhaseProgramPackage(*phase_);
        ResolvedWorksetDerivedStateBindingV1 derived_state;
        if (!ResolveWorksetDerivedStateBindingV1(
                std::span<const std::string>{},
                program_package,
                &derived_state,
                error_out))
        {
            return false;
        }
        std::int64_t context_id = 0;
        if (!analysis_db_->CreateBattleContextProbe({
                .wave_id = 0,
                .source_savestate_id = source->savestate_id,
                .materialization_key = materialization_key,
                .workflow_instance_id = context.step.workflow_instance_id,
                .workflow_step_id = context.step.workflow_step_id,
                .source_savestate_artifact_id = source->artifact_id,
                .source_savestate_sha256 = source->artifact_sha256,
                .full_phase_program_kind = identity.program_kind,
                .full_phase_program_version = identity.program_version,
                .full_phase_canonical_id = identity.canonical_id,
                .full_phase_contract_revision = identity.contract_revision,
                .full_phase_sha256 = identity.canonical_sha256,
                .module_canonical_id = runtime.module.canonical_id,
                .module_revision = runtime.module.revision,
                .module_sha256 = runtime.module.canonical_hash,
                .probe_status = BattleContextProbeStatus::Queued,
                .created_at_utc = types::UtcNow(),
                .correlation_id = materialization_key,
                .causation_id = "workflow-materialization",
            }, &context_id, error_out)) return false;
        const auto request = analysis_db_->GetBattleContextProbe(context_id);
        if (!request) return Fail("Battle Context request could not be reloaded", error_out);

        EnsureMaterializingJobSetReceipt ensured{};
        if (!execution_db_->EnsureMaterializingJobSet({
                .materialization_key = materialization_key,
                .program_kind = static_cast<std::int32_t>(savor::PK_BattleContext),
                .purpose = std::string(kPurpose),
                .created_by = std::string(kCreatedBy),
                .created_at_utc = NowMs(),
                .priority_boost = context.step.step_priority,
                .expected_total = 1,
                .domain_ref_kind = std::string(kProgramRefKind),
                .domain_ref_id = context_id,
                .meta_note = std::string(kStepKind),
            }, &ensured, error_out)) return false;
        if (ensured.materialization_state == "MATERIALIZING") {
            CreatePendingJobReceipt created{};
            if (!execution_db_->CreatePendingJob({
                    .job_set_id = ensured.job_set_id,
                    .program_kind = static_cast<std::int32_t>(savor::PK_BattleContext),
                    .program_version = savor::runtime::battlecontext::ProgramVersion,
                    .program_ref_kind = std::string(kProgramRefKind),
                    .program_ref_id = context_id,
                    .savestate_id = source->savestate_id,
                    .fingerprint = Fingerprint(*request),
                    .priority = context.step.step_priority,
                    .max_attempts = 1,
                    .input_ini = JobInput(context_id),
                }, &created, error_out)) return false;
        }
        const auto jobs = execution_db_->ListJobsInJobSet(ensured.job_set_id);
        if (jobs.size() != 1 || jobs.front().input_ini != JobInput(context_id)) {
            return Fail("Battle Context job set shape drifted", error_out);
        }
        if (!request->exec_job_id
            && !analysis_db_->SetBattleContextProbeExecJobId(
                context_id, jobs.front().job_id, error_out)) return false;
        SealJobPopulationReceipt sealed{};
        if (!execution_db_->SealJobPopulation({
                .job_set_id = ensured.job_set_id,
                .expected_job_count = 1,
                .requested_by = std::string(kCreatedBy)}, &sealed, error_out)) return false;
        PublishWorksetWaveReceipt published{};
        if (!execution_db_->PublishWorksetWave({
                .job_set_id = ensured.job_set_id,
                .expected_job_count = 1,
                .worksets = {{
                    .job_set_id = ensured.job_set_id,
                    .workflow_step_id = context.step.workflow_step_id,
                    .workset_key = materialization_key + ".workset.0",
                    .program_kind = static_cast<std::int32_t>(savor::PK_BattleContext),
                    .program_version = savor::runtime::battlecontext::ProgramVersion,
                    .contract = {
                        .contract_key = "battle-context:v1:request:"
                            + std::to_string(context_id) + ":phase:" + identity.canonical_sha256,
                        .module_canonical_id = runtime.module.canonical_id,
                        .module_version = static_cast<std::int32_t>(runtime.module.revision),
                        .module_sha256 = runtime.module.canonical_hash,
                        .entrypoint = runtime.entrypoint,
                        .verified_dependency_sha256 = runtime.verified_dependency_sha256,
                        .runtime_profile_sha256 = runtime.runtime_profile_sha256,
                        .program_package_sha256 =
                            program_package.canonical_sha256,
                        .estimated_payload_bytes = kDeclaredTerminalBytes,
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
                }},
                .requested_by = std::string(kCreatedBy),
            }, &published, error_out)) return false;
        result_out->job_set_id = ensured.job_set_id;
        result_out->persistence = {
            .program_ref_kind = std::string(kProgramRefKind),
            .program_ref_id = context_id,
            .fingerprint = Fingerprint(*request),
            .program_version = savor::runtime::battlecontext::ProgramVersion,
        };
        result_out->event_lines.push_back(
            "[battle-context-materialized] context=" + std::to_string(context_id));
        return true;
    }

    bool Continue(const ProgramJobContinuationContext& context,
                  ProgramJobContinuationResult* result_out,
                  std::string* error_out) const override {
        if (!result_out || !analysis_db_ || !execution_db_)
            return Fail("Battle Context continuation is incomplete", error_out);
        const auto jobs = execution_db_->ListJobsInJobSet(context.job_set_id);
        if (jobs.size() != 1) return Fail("Battle Context continuation lost singleton shape", error_out);
        const auto job = execution_db_->GetExecutionJob(jobs.front().job_id);
        if (!job || job->program_ref_kind != kProgramRefKind)
            return Fail("Battle Context continuation job identity is missing", error_out);
        const auto row = analysis_db_->GetBattleContextProbe(job->program_ref_id);
        if (!row || row->probe_status != BattleContextProbeStatus::Succeeded)
            return Fail("Battle Context result is not durably complete", error_out);
        result_out->disposition = ProgramJobContinuationDisposition::Complete;
        result_out->output = ProgramJobContinuationOutput{
            .output_key = std::string(kOutputKey),
            .data_kind = std::string(kOutputDataKind),
            .ref_kind = std::string(kProgramRefKind),
            .ref_id = row->context_probe_id,
        };
        return true;
    }

private:
    IExecutionDb* execution_db_{};
    IStateDb* state_db_{};
    IAnalysisDb* analysis_db_{};
    BattleContextProgramConfig config_;
    std::shared_ptr<const savor::runtime::battlecontext::IBattleContextFullPhaseDefinitionV1> phase_;
};

class Reconstruction final : public IWorksetReconstructionAdapter {
public:
    Reconstruction(IStateDb* state_db, IAnalysisDb* analysis_db, std::filesystem::path root)
        : state_db_(state_db), analysis_db_(analysis_db), root_(WorkingRoot(root)),
          phase_(savor::runtime::battlecontext::BattleContextFullPhaseDefinitionV1()) {}

    std::optional<WorksetReconstructionResult> Reconstruct(
        const WorksetReconstructionContext& context,
        std::string* error_out) const override {
        const auto fail = [&](std::string message) -> std::optional<WorksetReconstructionResult> {
            Fail(std::move(message), error_out); return std::nullopt;
        };
        if (!state_db_ || !analysis_db_ || !phase_ || context.items.size() != 1
            || context.workset_id <= 0 || context.dispatch_attempt_id <= 0
            || context.workflow_step_id <= 0 || context.job_set_id <= 0
            || context.dispatch_token.empty() || !context.state_compatibility.Complete())
            return fail("Battle Context reconstruction requires one exact item");
        const auto& item = context.items.front();
        if (item.program_kind != static_cast<std::int32_t>(savor::PK_BattleContext)
            || item.program_version != savor::runtime::battlecontext::ProgramVersion
            || item.program_ref_kind != kProgramRefKind || item.program_ref_id <= 0
            || item.savestate_id.value_or(0) <= 0 || item.input_ini != JobInput(item.program_ref_id)
            || item.reserved_attempt_id == 0 || item.claim_token.empty())
            return fail("Battle Context item identity drifted");
        const auto request = analysis_db_->GetBattleContextProbe(item.program_ref_id);
        if (!request || request->source_savestate_id != *item.savestate_id
            || request->workflow_step_id != context.workflow_step_id
            || request->full_phase_sha256 != phase_->identity().canonical_sha256
            || request->module_sha256 != phase_->runtime_contract().module.canonical_hash)
            return fail("Battle Context immutable request identity drifted");
        SavestateRecord source{};
        if (!ValidateSource(state_db_, *request, &source, error_out)) return std::nullopt;
        const auto path = ResolveSavestate(source, root_, state_db_, error_out);
        if (!path) return std::nullopt;

        const auto& runtime = phase_->runtime_contract();
        savor::runtime::WorkerWorksetDefinition workset{};
        workset.workset_id = savor::runtime::WorkerWorksetId(
            static_cast<std::uint64_t>(context.dispatch_attempt_id));
        workset.phase_invocation = {
            .invocation_id = {
                .workflow_step_id = static_cast<std::uint64_t>(context.workflow_step_id),
                .job_set_id = static_cast<std::uint64_t>(context.job_set_id),
            },
            .program_package = savor::runtime::fullphase::BuildFullPhaseProgramPackage(*phase_),
            .common_input = savor::runtime::fullphase::MakeFullPhaseCommonInput(
                "soa.battle.context.CommonInput", 1),
        };
        workset.baseline = {
            .artifact = {
                .kind = savor::runtime::ProgramBaselineArtifactKind::Savestate,
                .state_path = *path,
                .state_sha256 = source.artifact_sha256,
                .compatibility = context.state_compatibility,
                .lineage = {
                    .edge = runtime.baseline_lineage,
                    .producer = "SavorDb.PK_BattleContext",
                },
            },
            .lineage = runtime.baseline_lineage,
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
            .program_package_sha256 = workset.phase_invocation.program_package.canonical_sha256,
            .common_input_sha256 = workset.phase_invocation.common_input.content_sha256,
            .derived_state_binding_sha256 =
                workset.derived_state.content_sha256,
            .capture_binding_sha256 = workset.capture
                ? workset.capture->content_sha256
                : savor::runtime::EmptyWorksetCaptureBindingHashV1(),
            .progress_plan_sha256 = workset.progress_plan.content_sha256,
        };
        workset.execution_key.canonical_sha256 =
            savor::runtime::ComputeWorkerWorksetExecutionKeyHash(workset.execution_key);
        workset.items.push_back({
            .item_id = savor::runtime::WorkerWorksetItemId(static_cast<std::uint64_t>(item.job_id)),
            .ordinal = 0,
            .execution = {
                .execution_id = savor::runtime::ProgramExecutionId(static_cast<std::uint64_t>(item.job_id)),
                .attempt_id = savor::runtime::AttemptId(item.reserved_attempt_id),
                .input_payload = savor::runtime::battlecontext::EncodeBattleContextExecutionInputV1(),
            },
            .declared_terminal_bytes = kDeclaredTerminalBytes,
            .correlation = {
                .durable_job_id = std::to_string(item.job_id),
                .claim_token = item.claim_token,
                .parent_correlation = context.contract_key,
            },
        });
        std::vector<std::uint8_t> encoded;
        const auto status = savor::runtime::EncodeWorkerWorksetV5(workset, encoded);
        if (!status) return fail("Battle Context workset encoding failed: " + status.message);
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
    std::shared_ptr<const savor::runtime::battlecontext::IBattleContextFullPhaseDefinitionV1> phase_;
};

class ResultHandler final : public IProgramResultHandler {
public:
    ResultHandler(IStateDb* state_db, IAnalysisDb* analysis_db, std::filesystem::path root)
        : state_db_(state_db), analysis_db_(analysis_db), root_(WorkingRoot(root)),
          phase_(savor::runtime::battlecontext::BattleContextFullPhaseDefinitionV1()) {}

    ProgramResultDecision Process(const ProgramResultProcessingContext& context) const override {
        if (!state_db_ || !analysis_db_ || !phase_
            || context.program_kind != static_cast<std::int32_t>(savor::PK_BattleContext)
            || context.program_version != savor::runtime::battlecontext::ProgramVersion
            || context.program_ref_kind != kProgramRefKind || context.program_ref_id <= 0
            || context.input_ini != JobInput(context.program_ref_id))
            return FinalDecision("FAILED", "BATTLE_CONTEXT_IDENTITY_INVALID", "job is not an exact battle.context request");
        const auto request = analysis_db_->GetBattleContextProbe(context.program_ref_id);
        std::string error;
        if (!request || !ValidateSource(state_db_, *request, nullptr, &error))
            return FinalDecision("FAILED", "BATTLE_CONTEXT_SOURCE_DRIFT", error);
        savor::runtime::DurableWorkerTerminalEnvelope terminal{};
        if (!savor::runtime::DecodeDurableWorkerTerminalEnvelope(
                context.terminal.envelope, &terminal, &error))
            return FinalDecision("FAILED", "BATTLE_CONTEXT_TERMINAL_INVALID", error);
        using Status = savor::wrms::InvocationTerminalStatus;
        if (terminal.terminal.status != Status::Succeeded || terminal.terminal.unstarted
            || terminal.terminal.workset_id != static_cast<std::uint64_t>(context.terminal.dispatch_attempt_id)
            || terminal.terminal.item_id != static_cast<std::uint64_t>(context.job_id)
            || terminal.terminal.invocation_id != static_cast<std::uint64_t>(context.job_id)
            || terminal.terminal.attempt_id != context.terminal.reserved_attempt_id
            || terminal.process_generation == 0 || terminal.terminal.workset_epoch == 0
            || terminal.terminal.session_disposition != savor::wrms::SessionDispositionCode::Clean) {
            (void)analysis_db_->CompleteBattleContextProbe({
                .exec_job_id = context.job_id,
                .probe_status = BattleContextProbeStatus::Failed,
                .worker_terminal_sha256 = context.terminal.sha256,
                .recorded_at_utc = types::UtcNow(),
            }, nullptr);
            return FinalDecision(
                "FAILED",
                terminal.terminal.error_code.empty() ? "BATTLE_CONTEXT_EXECUTION_FAILED" : terminal.terminal.error_code,
                terminal.terminal.message.empty() ? "battle.context did not complete cleanly" : terminal.terminal.message);
        }
        savor::runtime::battlecontext::BattleContextCaptureResultV1 result{};
        if (!phase_->DecodeProgramResult(terminal.terminal.result, result, &error)
            || result.entry_pc != savor::runtime::battlecontext::BeforeRandSeedSetPc
            || result.capture_pc != savor::runtime::battlecontext::TurnInputsPc
            || result.entry_epoch.value() != terminal.terminal.workset_epoch
            || result.capture_epoch.value() != terminal.terminal.workset_epoch) {
            return FinalDecision("FAILED", "BATTLE_CONTEXT_RESULT_INVALID",
                error.empty() ? "Battle Context result provenance drifted" : error);
        }
        std::string bytes;
        if (!soa::battle::ctx::codec::encode(result.context, bytes))
            return FinalDecision("FAILED", "BATTLE_CONTEXT_ENCODING_FAILED", "BattleContextCodec rejected the captured context");
        const auto destination = root_ / "artifacts"
            / (std::to_string(request->context_probe_id) + "-" + context.terminal.sha256 + ".bctx");
        if (!WriteContextAtomically(destination, bytes, &error))
            throw std::runtime_error(error);
        const auto sha = hash::sha256(bytes.data(), bytes.size());
        std::int64_t artifact_id = 0;
        if (!state_db_->StoreArtifact({
                .sha256 = sha,
                .size_bytes = static_cast<std::int64_t>(bytes.size()),
                .compression_kind = 0,
                .filename = destination.string(),
                .file_ext = soa::battle::ctx::codec::ext,
                .artifact_kind = "BATTLE_CONTEXT",
                .created_at_utc = types::UtcNow(),
                .correlation_id = "battle-context-" + std::to_string(request->context_probe_id),
                .causation_id = "execution-job-" + std::to_string(context.job_id),
            }, &artifact_id, &error)) throw std::runtime_error(error);
        if (!analysis_db_->CompleteBattleContextProbe({
                .exec_job_id = context.job_id,
                .probe_status = BattleContextProbeStatus::Succeeded,
                .context_blob = std::nullopt,
                .context_version = soa::battle::ctx::codec::ver,
                .context_artifact_id = artifact_id,
                .worker_terminal_sha256 = context.terminal.sha256,
                .entry_pc = result.entry_pc,
                .entry_vi_count = result.entry_vi_count,
                .entry_epoch = result.entry_epoch.value(),
                .capture_pc = result.capture_pc,
                .capture_vi_count = result.capture_vi_count,
                .capture_epoch = result.capture_epoch.value(),
                .recorded_at_utc = types::UtcNow(),
            }, &error)) throw std::runtime_error(error);
        auto decision = FinalDecision("SUCCEEDED");
        decision.cleanup_worker_staging = true;
        decision.staging_files.push_back({
            .relative_path = destination.lexically_relative(root_).generic_string(),
            .sha256 = sha,
            .size_bytes = static_cast<std::uint64_t>(bytes.size()),
        });
        decision.outputs.push_back(ContextOutput(request->context_probe_id));
        decision.event_lines.push_back("[battle-context-captured] context="
            + std::to_string(request->context_probe_id) + " artifact=" + std::to_string(artifact_id));
        return decision;
    }

    private:
    IStateDb* state_db_{};
    IAnalysisDb* analysis_db_{};
    std::filesystem::path root_;
    std::shared_ptr<const savor::runtime::battlecontext::IBattleContextFullPhaseDefinitionV1> phase_;
};

} // namespace

ProgramKindDescriptor BuildBattleContextProgramDescriptor(
    IExecutionDb* execution_db, IStateDb* state_db, IAnalysisDb* analysis_db,
    BattleContextProgramConfig config) {
    ProgramKindDescriptor descriptor{};
    descriptor.program_kind = static_cast<std::int32_t>(savor::PK_BattleContext);
    descriptor.program_name = savor::ProgramKindDisplayName(descriptor.program_kind);
    descriptor.result_staging_root = config.working_dir_root;
    descriptor.full_phase_identity = savor::runtime::battlecontext::
        BattleContextFullPhaseDefinitionV1()->identity();
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
    descriptor.result_handler = std::make_shared<ResultHandler>(
        state_db, analysis_db, config.working_dir_root);
    descriptor.supports_workflow_orchestration = true;
    return descriptor;
}

} // namespace savor::db::execution::programdb::battlecontext
