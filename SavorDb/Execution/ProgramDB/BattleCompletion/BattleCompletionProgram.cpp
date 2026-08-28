#include "BattleCompletionProgram.h"
#include "../../../../SavorCore/Phases/Programs/TasMovieCheckpoint/TasMovieCheckpointModule.h"

#include "../WorksetDerivedStateBinding.h"
#include "../WorksetObservationBinding.h"
#include "../../../Analysis/IAnalysisDb.h"
#include "../../../Common/Types/UtcTimestamp.h"
#include "../../../Execution/IExecutionDb.h"
#include "../../../State/IStateDb.h"
#include "../../../../SavorCore/Phases/Programs/BattleCompletion/BattleCompletionModule.h"
#include "../../../../SavorCore/Runner/IPC/DurableWorkerTerminalEnvelope.h"
#include "../../../../SavorCore/Runner/Runtime/ProgramKind.h"
#include "../../../../SavorCore/Runner/Runtime/Worksets/WorksetWireCodec.h"
#include "../../../../SavorCore/Utils/Hash.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>

namespace savor::db::execution::programdb::battlecompletion {
namespace {

namespace phase = savor::runtime::battlecompletion;

constexpr std::string_view kStepKind = "battle.completion";
constexpr std::string_view kProgramRefKind = "analysis_battle.battle_completion";
constexpr std::string_view kTurnJobRefKind = "analysis_battle.turn_job";
constexpr std::string_view kInputKey = "victory_turn_job";
constexpr std::string_view kInputDataKind = "analysis_battle.battle_turn_job";
constexpr std::string_view kOutputKey = "completion";
constexpr std::string_view kOutputDataKind = "analysis_battle.battle_completion";
constexpr std::string_view kPurpose = "BATTLE_COMPLETION";
constexpr std::string_view kCreatedBy = "battle_completion_program_kind";
constexpr std::size_t kDeclaredTerminalBytes = 2ull * 1024ull * 1024ull;

const WorksetObservationDefaultsV1& ObservationDefaults() {
    static const WorksetObservationDefaultsV1 defaults{};
    return defaults;
}

std::int64_t NowMs() { return types::UtcNow().time_since_epoch().count(); }

bool Fail(std::string message, std::string* error_out) {
    if (error_out) *error_out = std::move(message);
    return false;
}

std::filesystem::path Root(const std::filesystem::path& configured) {
    return configured.empty()
        ? std::filesystem::temp_directory_path() / "savor-battle-completion"
        : configured;
}

std::optional<std::string> HashFile(const std::filesystem::path& path) {
    try { return hash::sha256_of_file(path.string()); }
    catch (...) { return std::nullopt; }
}

bool ExactFile(const ArtifactRecord& artifact,
               const std::filesystem::path& path) {
    std::error_code error;
    return artifact.size_bytes > 0 && artifact.sha256.size() == 64 &&
        std::filesystem::is_regular_file(path, error) && !error &&
        static_cast<std::int64_t>(std::filesystem::file_size(path, error)) ==
            artifact.size_bytes && !error &&
        HashFile(path) == std::optional<std::string>(artifact.sha256);
}

std::optional<std::filesystem::path> ResolveState(
    IStateDb* state_db, const SavestateRecord& state,
    const std::filesystem::path& root, std::string* error_out) {
    const std::filesystem::path recorded(state.artifact_filename);
    const ArtifactRecord artifact{.sha256 = state.artifact_sha256,
        .size_bytes = state.artifact_size_bytes};
    if (ExactFile(artifact, recorded)) return recorded;
    const auto destination = Root(root) / "baselines" /
        (std::to_string(state.savestate_id) + "-" + state.artifact_sha256 + ".sav");
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) return Fail("could not create Battle Completion baseline directory", error_out), std::nullopt;
    if (!ExactFile(artifact, destination)) {
        if (!state_db->MaterializeSavestateToPath(
                state.savestate_id, destination.string(), error_out) ||
            !ExactFile(artifact, destination)) {
            if (error_out && error_out->empty())
                *error_out = "State DB did not materialize the exact Victory state";
            return std::nullopt;
        }
    }
    return destination;
}

std::optional<std::int64_t> SelectedTurnJob(
    const ProgramJobMaterializationContext& context) {
    if (context.graph) {
        for (const auto& binding : context.graph->input_bindings)
            if (binding.input_key == kInputKey &&
                binding.data_kind == kInputDataKind &&
                binding.ref_kind == kTurnJobRefKind && binding.ref_id > 0)
                return binding.ref_id;
    }
    if (context.step.domain_ref_kind == kTurnJobRefKind &&
        context.step.domain_ref_id > 0)
        return context.step.domain_ref_id;
    return std::nullopt;
}

std::string JobInput(std::int64_t id) {
    return "BCMP1:" + std::to_string(id);
}

std::string Fingerprint(const BattleCompletionRecord& row,
                        const phase::IBattleCompletionFullPhaseDefinitionV1& definition) {
    const std::string canonical = std::to_string(row.selected_turn_job_id) + "\n" +
        std::to_string(row.selected_execution_job_id) + "\n" +
        std::to_string(row.entry_savestate_id) + "\n" +
        definition.identity().canonical_sha256;
    return "PK=9;PV=1;completion=" +
        hash::sha256(canonical.data(), canonical.size());
}

ProgramResultDecision Decision(std::string state,
    std::optional<std::string> code = std::nullopt,
    std::optional<std::string> text = std::nullopt) {
    ProgramResultDecision result{};
    result.final_job_state = std::move(state);
    result.error_code = std::move(code);
    result.error_text = std::move(text);
    return result;
}

ProgramResultOutput Output(std::int64_t id) {
    return {.output_key = std::string(kOutputKey),
        .data_kind = std::string(kOutputDataKind),
        .ref_kind = std::string(kProgramRefKind), .ref_id = id};
}

ProgramJobContinuationOutput ContinuationOutput(std::int64_t id) {
    return {.output_key = std::string(kOutputKey),
        .data_kind = std::string(kOutputDataKind),
        .ref_kind = std::string(kProgramRefKind), .ref_id = id};
}

bool WriteAtomically(const std::filesystem::path& destination,
                     std::span<const std::uint8_t> bytes,
                     std::string* error_out) {
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) return Fail("could not create Battle Completion artifact directory", error_out);
    const auto temporary = std::filesystem::path(destination.string() + ".publishing");
    std::filesystem::remove(temporary, error);
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) return Fail("could not open Battle Completion artifact", error_out);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream) return Fail("could not write Battle Completion artifact", error_out);
    std::filesystem::rename(temporary, destination, error);
    if (error) return Fail("could not publish Battle Completion artifact: " + error.message(), error_out);
    return true;
}

std::string RouteName(savor::runtime::tasmovie::TasMovieNextPhaseV1 route) {
    return std::string(savor::runtime::tasmovie::TasMovieNextPhaseNameV1(route));
}

std::optional<std::int64_t> PublishSavestate(
    IStateDb* state_db, const BattleCompletionRecord& completion,
    const savor::runtime::program::ArtifactReferenceValue& artifact,
    std::string* error_out) {
    const std::filesystem::path path(artifact.storage_reference);
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    const auto sha = HashFile(path);
    if (!artifact.complete || error || size == 0 || !sha ||
        *sha != artifact.content_hash.ToHex()) {
        Fail("Battle Completion successor state does not match worker evidence", error_out);
        return std::nullopt;
    }
    std::int64_t artifact_id = 0;
    if (!state_db->StoreArtifact({.sha256 = *sha,
            .size_bytes = static_cast<std::int64_t>(size),
            .filename = path.string(), .file_ext = ".sav",
            .artifact_kind = "SAV", .created_at_utc = types::UtcNow(),
            .correlation_id = "battle-completion-" +
                std::to_string(completion.battle_completion_id),
            .causation_id = "execution-job-" +
                std::to_string(completion.exec_job_id.value_or(0))},
            &artifact_id, error_out)) return std::nullopt;
    std::int64_t savestate_id = 0;
    if (!state_db->CreateSavestate({.artifact_id = artifact_id,
            .playback_state = SavestatePlaybackState::MovieInactive,
            .savestate_type = "BATTLE_COMPLETION_PRESEED",
            .note = "Accepted field preseed after Battle completion",
            .is_complete = true, .created_at_utc = types::UtcNow(),
            .correlation_id = "battle-completion-" +
                std::to_string(completion.battle_completion_id),
            .causation_id = "execution-job-" +
                std::to_string(completion.exec_job_id.value_or(0))},
            &savestate_id, error_out)) return std::nullopt;
    if (!state_db->DeriveSavestate({
            .from_savestate_id = completion.entry_savestate_id,
            .to_savestate_id = savestate_id,
            .method_kind = "battle.completion.v1",
            .source_context_kind = std::string(kProgramRefKind),
            .source_context_id = completion.battle_completion_id,
            .created_at_utc = types::UtcNow(),
            .correlation_id = "battle-completion-" +
                std::to_string(completion.battle_completion_id),
            .causation_id = "execution-job-" +
                std::to_string(completion.exec_job_id.value_or(0))},
            nullptr, error_out)) return std::nullopt;
    return savestate_id;
}

class Materializer final : public IProgramJobMaterializer {
public:
    Materializer(IExecutionDb* execution, IStateDb* state, IAnalysisDb* analysis,
                 BattleCompletionProgramConfig config)
        : execution_(execution), state_(state), analysis_(analysis),
          config_(std::move(config)),
          phase_(phase::BattleCompletionFullPhaseDefinitionV1()) {}

    bool Materialize(const ProgramJobMaterializationContext& context,
        WorkflowStepScheduleResult* result_out,
        std::string* error_out) const override {
        if (!result_out || !execution_ || !state_ || !analysis_ || !phase_ ||
            !context.graph || context.step.step_kind != kStepKind)
            return Fail("Battle Completion materialization is incomplete", error_out);
        *result_out = {};
        const auto selected_id = SelectedTurnJob(context);
        const auto selected = selected_id
            ? analysis_->GetBattleTurnJob(*selected_id) : std::nullopt;
        const auto wave = selected
            ? analysis_->GetBattleTurnWave(selected->wave_id) : std::nullopt;
        const auto terminal = selected && selected->exec_job_id
            ? analysis_->GetBattleSingleTurnResultForExecJob(*selected->exec_job_id)
            : std::nullopt;
        const auto execution = selected && selected->exec_job_id
            ? execution_->GetExecutionJob(*selected->exec_job_id)
            : std::nullopt;
        const auto source = selected && selected->output_savestate_id
            ? state_->GetSavestate(*selected->output_savestate_id) : std::nullopt;
        if (!selected || !selected->exec_job_id || !wave || !terminal || !execution ||
            execution->state != "SUCCEEDED" ||
            !execution->worker_terminal_fingerprint ||
            *execution->worker_terminal_fingerprint != terminal->worker_terminal_sha256 ||
            terminal->terminal_kind != "SUCCEEDED" ||
            terminal->domain_outcome != std::optional<std::string>("Victory") ||
            !terminal->ending_rng ||
            terminal->successor_savestate_id != selected->output_savestate_id ||
            !source || !source->is_complete ||
            source->playback_state != SavestatePlaybackState::MovieInactive ||
            source->dtm_artifact_id)
            return Fail("battle.completion requires a durable Victory job", error_out);

        std::int64_t completion_id = 0;
        const auto materialization_key = "battle.completion.step." +
            std::to_string(context.step.workflow_step_id);
        if (!analysis_->CreateBattleCompletion({
                .workflow_instance_id = context.step.workflow_instance_id,
                .workflow_step_id = context.step.workflow_step_id,
                .battle_set_id = wave->battle_set_id,
                .wave_id = wave->wave_id,
                .selected_turn_job_id = selected->turn_job_id,
                .selected_execution_job_id = *selected->exec_job_id,
                .entry_savestate_id = source->savestate_id,
                .status = "QUEUED", .created_at_utc = types::UtcNow(),
                .correlation_id = materialization_key,
                .causation_id = "victory-completion-selection"},
                &completion_id, error_out)) return false;
        const auto row = analysis_->GetBattleCompletion(completion_id);
        if (!row) return Fail("Battle Completion request could not be reloaded", error_out);

        const auto package = savor::runtime::fullphase::
            BuildFullPhaseProgramPackage(*phase_);
        ResolvedWorksetObservationBindingV1 observation;
        if (!ResolveWorksetObservationBindingV1(context, ObservationDefaults(),
                Root(config_.working_dir_root) / "captures", &observation,
                error_out)) return false;
        ResolvedWorksetDerivedStateBindingV1 derived;
        if (!ResolveWorksetDerivedStateBindingV1({}, package, &derived, error_out))
            return false;
        EnsureMaterializingJobSetReceipt ensured{};
        if (!execution_->EnsureMaterializingJobSet({
                .materialization_key = materialization_key,
                .program_kind = static_cast<std::int32_t>(savor::PK_BattleCompletion),
                .purpose = std::string(kPurpose), .created_by = std::string(kCreatedBy),
                .created_at_utc = NowMs(), .priority_boost = context.step.step_priority,
                .expected_total = 1, .domain_ref_kind = std::string(kProgramRefKind),
                .domain_ref_id = completion_id, .meta_note = std::string(kStepKind)},
                &ensured, error_out)) return false;
        if (ensured.materialization_state == "MATERIALIZING") {
            CreatePendingJobReceipt created{};
            if (!execution_->CreatePendingJob({.job_set_id = ensured.job_set_id,
                    .program_kind = static_cast<std::int32_t>(savor::PK_BattleCompletion),
                    .program_version = phase::ProgramVersion,
                    .program_ref_kind = std::string(kProgramRefKind),
                    .program_ref_id = completion_id,
                    .savestate_id = source->savestate_id,
                    .fingerprint = Fingerprint(*row, *phase_),
                    .priority = context.step.step_priority, .max_attempts = 1,
                    .input_ini = JobInput(completion_id)}, &created, error_out))
                return false;
        }
        const auto jobs = execution_->ListJobsInJobSet(ensured.job_set_id);
        if (jobs.size() != 1 || jobs.front().input_ini != JobInput(completion_id))
            return Fail("Battle Completion singleton job shape drifted", error_out);
        if (!analysis_->BindBattleCompletionExecutionJob({
                .battle_completion_id = completion_id,
                .workflow_instance_id = context.step.workflow_instance_id,
                .workflow_step_id = context.step.workflow_step_id,
                .exec_job_id = jobs.front().job_id}, error_out)) return false;
        SealJobPopulationReceipt sealed{};
        if (!execution_->SealJobPopulation({.job_set_id = ensured.job_set_id,
                .expected_job_count = 1, .requested_by = std::string(kCreatedBy)},
                &sealed, error_out)) return false;
        const auto& identity = phase_->identity();
        const auto& runtime = phase_->runtime_contract();
        PublishWorksetWaveReceipt published{};
        if (!execution_->PublishWorksetWave({.job_set_id = ensured.job_set_id,
                .expected_job_count = 1, .worksets = {{
                    .job_set_id = ensured.job_set_id,
                    .workflow_step_id = context.step.workflow_step_id,
                    .workset_key = materialization_key + ".workset.0",
                    .program_kind = static_cast<std::int32_t>(savor::PK_BattleCompletion),
                    .program_version = phase::ProgramVersion,
                    .contract = {.contract_key = "battle-completion:v1:" +
                            std::to_string(completion_id) + ":phase:" +
                            identity.canonical_sha256,
                        .module_canonical_id = runtime.module.canonical_id,
                        .module_version = static_cast<std::int32_t>(runtime.module.revision),
                        .module_sha256 = runtime.module.canonical_hash,
                        .entrypoint = runtime.entrypoint,
                        .verified_dependency_sha256 = runtime.verified_dependency_sha256,
                        .runtime_profile_sha256 = runtime.runtime_profile_sha256,
                        .program_package_sha256 = package.canonical_sha256,
                        .estimated_payload_bytes = kDeclaredTerminalBytes},
                    .derived_state = {.binding_payload = derived.encoded_binding,
                        .binding_sha256 = derived.binding_sha256},
                    .observation = {
                        .capture_binding_payload = observation.encoded_capture_binding,
                        .capture_binding_sha256 = observation.capture_binding_sha256,
                        .progress_plan_payload = observation.encoded_progress_plan,
                        .progress_plan_sha256 = observation.progress_plan_sha256},
                    .priority = context.step.step_priority,
                    .ordered_job_ids = {jobs.front().job_id},
                    .requested_by = std::string(kCreatedBy)}},
                .requested_by = std::string(kCreatedBy)}, &published, error_out))
            return false;
        result_out->job_set_id = ensured.job_set_id;
        result_out->persistence = {.program_ref_kind = std::string(kProgramRefKind),
            .program_ref_id = completion_id, .fingerprint = Fingerprint(*row, *phase_),
            .program_version = phase::ProgramVersion};
        return true;
    }

    bool Continue(const ProgramJobContinuationContext& context,
        ProgramJobContinuationResult* result_out,
        std::string* error_out) const override {
        if (!result_out || !analysis_ || !execution_)
            return Fail("Battle Completion continuation is incomplete", error_out);
        const auto jobs = execution_->ListJobsInJobSet(context.job_set_id);
        if (jobs.size() != 1) return Fail("Battle Completion lost singleton shape", error_out);
        const auto job = execution_->GetExecutionJob(jobs.front().job_id);
        const auto row = job ? analysis_->GetBattleCompletion(job->program_ref_id)
                             : std::nullopt;
        if (!job || !row || row->status != "COMPLETED")
            return Fail("Battle Completion result is not durable", error_out);
        result_out->disposition = ProgramJobContinuationDisposition::Complete;
        result_out->output = ContinuationOutput(row->battle_completion_id);
        return true;
    }

private:
    IExecutionDb* execution_{};
    IStateDb* state_{};
    IAnalysisDb* analysis_{};
    BattleCompletionProgramConfig config_;
    std::shared_ptr<const phase::IBattleCompletionFullPhaseDefinitionV1> phase_;
};

class Reconstruction final : public IWorksetReconstructionAdapter {
public:
    Reconstruction(IStateDb* state, IAnalysisDb* analysis,
                   std::filesystem::path root)
        : state_(state), analysis_(analysis), root_(Root(root)),
          phase_(phase::BattleCompletionFullPhaseDefinitionV1()) {}

    std::optional<WorksetReconstructionResult> Reconstruct(
        const WorksetReconstructionContext& context,
        std::string* error_out) const override {
        const auto fail = [&](std::string message) {
            Fail(std::move(message), error_out);
            return std::optional<WorksetReconstructionResult>{};
        };
        if (!state_ || !analysis_ || !phase_ || context.items.size() != 1 ||
            context.workset_id <= 0 || context.dispatch_attempt_id <= 0 ||
            !context.state_compatibility.Complete())
            return fail("Battle Completion reconstruction requires one exact item");
        const auto& item = context.items.front();
        const auto row = analysis_->GetBattleCompletion(item.program_ref_id);
        const auto source = row ? state_->GetSavestate(row->entry_savestate_id)
                                : std::nullopt;
        if (!row || row->status != "QUEUED" || row->exec_job_id != item.job_id ||
            item.program_kind != static_cast<std::int32_t>(savor::PK_BattleCompletion) ||
            item.program_version != phase::ProgramVersion ||
            item.program_ref_kind != kProgramRefKind ||
            item.savestate_id != std::optional<std::int64_t>(row->entry_savestate_id) ||
            item.input_ini != JobInput(row->battle_completion_id) ||
            !source || !source->is_complete ||
            source->playback_state != SavestatePlaybackState::MovieInactive)
            return fail("Battle Completion immutable request drifted");
        const auto path = ResolveState(state_, *source, root_, error_out);
        if (!path) return std::nullopt;
        const auto& runtime = phase_->runtime_contract();
        savor::runtime::WorkerWorksetDefinition workset{};
        workset.workset_id = savor::runtime::WorkerWorksetId(
            static_cast<std::uint64_t>(context.dispatch_attempt_id));
        workset.phase_invocation = {.invocation_id = {
                .workflow_step_id = static_cast<std::uint64_t>(context.workflow_step_id),
                .job_set_id = static_cast<std::uint64_t>(context.job_set_id)},
            .program_package = savor::runtime::fullphase::
                BuildFullPhaseProgramPackage(*phase_),
            .common_input = savor::runtime::fullphase::MakeFullPhaseCommonInput(
                "soa.battle.completion.CommonInput", 1)};
        workset.baseline = {.artifact = {
                .kind = savor::runtime::ProgramBaselineArtifactKind::Savestate,
                .state_path = *path, .state_sha256 = source->artifact_sha256,
                .compatibility = context.state_compatibility,
                .lineage = {.edge = runtime.baseline_lineage,
                    .producer = "SavorDb.PK_BattleCompletion"}},
            .lineage = runtime.baseline_lineage};
        workset.derived_state = context.derived_state;
        workset.capture = context.capture;
        workset.progress_plan = context.progress_plan;
        workset.execution_key = {.module = runtime.module,
            .entrypoint = runtime.entrypoint,
            .verified_dependency_sha256 = runtime.verified_dependency_sha256,
            .runtime_profile_sha256 = runtime.runtime_profile_sha256,
            .baseline = savor::runtime::ComputeProgramBaselineKey(workset.baseline),
            .movie_policy_sha256 = runtime.movie_policy_sha256,
            .service_policy_sha256 = runtime.service_policy_sha256,
            .program_package_sha256 = workset.phase_invocation.program_package.canonical_sha256,
            .common_input_sha256 = workset.phase_invocation.common_input.content_sha256,
            .derived_state_binding_sha256 = workset.derived_state.content_sha256,
            .capture_binding_sha256 = workset.capture
                ? workset.capture->content_sha256
                : savor::runtime::EmptyWorksetCaptureBindingHashV1(),
            .progress_plan_sha256 = workset.progress_plan.content_sha256};
        workset.execution_key.canonical_sha256 =
            savor::runtime::ComputeWorkerWorksetExecutionKeyHash(workset.execution_key);
        phase::BattleCompletionRequestV1 request{
            .lineage = {
                static_cast<std::uint64_t>(row->battle_set_id),
                static_cast<std::uint64_t>(row->wave_id),
                static_cast<std::uint64_t>(row->selected_turn_job_id),
                static_cast<std::uint64_t>(row->selected_execution_job_id)},
            .output_savestate_path = (root_ / "artifacts" /
                ("completion-" + std::to_string(row->battle_completion_id) +
                 "-attempt-" + std::to_string(item.reserved_attempt_id) +
                 ".sav")).string()};
        workset.items.push_back({
            .item_id = savor::runtime::WorkerWorksetItemId(
                static_cast<std::uint64_t>(item.job_id)), .ordinal = 0,
            .execution = {.execution_id = savor::runtime::ProgramExecutionId(
                    static_cast<std::uint64_t>(item.job_id)),
                .attempt_id = savor::runtime::AttemptId(item.reserved_attempt_id),
                .input_payload = phase::EncodeBattleCompletionExecutionInputV1(request)},
            .declared_terminal_bytes = kDeclaredTerminalBytes,
            .correlation = {.durable_job_id = std::to_string(item.job_id),
                .claim_token = item.claim_token,
                .parent_correlation = context.contract_key}});
        std::vector<std::uint8_t> encoded;
        const auto status = savor::runtime::EncodeWorkerWorksetV5(workset, encoded);
        if (!status) return fail("Battle Completion workset encoding failed: " + status.message);
        workset.encoded_size_bytes = encoded.size();
        return WorksetReconstructionResult{.workset = std::move(workset),
            .ordered_job_ids = {item.job_id}};
    }

private:
    IStateDb* state_{};
    IAnalysisDb* analysis_{};
    std::filesystem::path root_;
    std::shared_ptr<const phase::IBattleCompletionFullPhaseDefinitionV1> phase_;
};

class ResultHandler final : public IProgramResultHandler {
public:
    ResultHandler(IStateDb* state, IAnalysisDb* analysis,
                  std::filesystem::path root)
        : state_(state), analysis_(analysis), root_(Root(root)),
          phase_(phase::BattleCompletionFullPhaseDefinitionV1()) {}

    ProgramResultDecision Process(
        const ProgramResultProcessingContext& context) const override {
        if (!state_ || !analysis_ || !phase_ ||
            context.program_kind != static_cast<std::int32_t>(savor::PK_BattleCompletion) ||
            context.program_version != phase::ProgramVersion ||
            context.program_ref_kind != kProgramRefKind)
            return Decision("FAILED", "BATTLE_COMPLETION_IDENTITY_INVALID",
                "job is not an exact battle.completion request");
        const auto row = analysis_->GetBattleCompletion(context.program_ref_id);
        if (!row || row->exec_job_id != context.job_id)
            return Decision("FAILED", "BATTLE_COMPLETION_REQUEST_DRIFT",
                "durable Battle Completion identity drifted");
        if (row->status == "COMPLETED") {
            if (row->worker_terminal_sha256 != context.terminal.sha256)
                return Decision("FAILED", "BATTLE_COMPLETION_RESULT_DRIFT",
                    "completed Battle Completion belongs to another worker terminal");
            auto persisted = Decision("SUCCEEDED");
            persisted.outputs.push_back(Output(row->battle_completion_id));
            return persisted;
        }
        savor::runtime::DurableWorkerTerminalEnvelope terminal{};
        std::string error;
        if (!savor::runtime::DecodeDurableWorkerTerminalEnvelope(
                context.terminal.envelope, &terminal, &error))
            return PersistFailure(*row, context, "BATTLE_COMPLETION_TERMINAL_INVALID", error);
        using Status = savor::wrms::InvocationTerminalStatus;
        if (terminal.terminal.status != Status::Succeeded ||
            terminal.terminal.unstarted || terminal.process_generation == 0 ||
            terminal.terminal.workset_epoch == 0 ||
            terminal.terminal.session_disposition !=
                savor::wrms::SessionDispositionCode::Clean)
            return PersistFailure(*row, context,
                terminal.terminal.error_code.empty()
                    ? "BATTLE_COMPLETION_EXECUTION_FAILED"
                    : terminal.terminal.error_code,
                terminal.terminal.message.empty()
                    ? "battle.completion did not complete cleanly"
                    : terminal.terminal.message,
                "FAILED");
        phase::BattleCompletionResultV1 result{};
        if (!phase_->DecodeProgramResult(terminal.terminal.result, result, &error) ||
            result.artifacts.size() != 1 ||
            result.manifest.lineage.battle_set_id !=
                static_cast<std::uint64_t>(row->battle_set_id) ||
            result.manifest.lineage.wave_id != static_cast<std::uint64_t>(row->wave_id) ||
            result.manifest.lineage.turn_job_id !=
                static_cast<std::uint64_t>(row->selected_turn_job_id) ||
            result.manifest.lineage.execution_job_id !=
                static_cast<std::uint64_t>(row->selected_execution_job_id) ||
            result.transition != result.manifest.transition)
            return PersistFailure(*row, context, "BATTLE_COMPLETION_RESULT_INVALID",
                error.empty() ? "Battle Completion result evidence drifted" : error);
        const auto route = savor::runtime::tasmovie::ClassifyTasMovieNextPhaseV1(
            result.transition.provenance.pc,
            result.transition.sct_filename,
            result.transition.area);
        if (route == savor::runtime::tasmovie::TasMovieNextPhaseV1::Unknown)
            return PersistFailure(*row, context,
                "BATTLE_COMPLETION_ROUTE_INVALID",
                "Battle Completion transition cannot be classified");
        const auto successor = PublishSavestate(
            state_, *row, result.artifacts.front().artifact, &error);
        if (!successor) throw std::runtime_error(error);
        std::vector<std::uint8_t> manifest;
        if (!phase::EncodeBattleCompletionManifestV1(result.manifest, manifest))
            throw std::runtime_error("Battle Completion manifest encoding failed");
        const auto sha = hash::sha256(manifest.data(), manifest.size());
        const auto path = root_ / "artifacts" /
            ("completion-" + std::to_string(row->battle_completion_id) +
             "-" + context.terminal.sha256 + std::string(phase::ManifestExtension));
        if (!WriteAtomically(path, manifest, &error)) throw std::runtime_error(error);
        std::int64_t manifest_artifact = 0;
        if (!state_->StoreArtifact({.sha256 = sha,
                .size_bytes = static_cast<std::int64_t>(manifest.size()),
                .filename = path.string(),
                .file_ext = std::string(phase::ManifestExtension),
                .artifact_kind = std::string(phase::ManifestArtifactKind),
                .created_at_utc = types::UtcNow(),
                .correlation_id = "battle-completion-" +
                    std::to_string(row->battle_completion_id),
                .causation_id = "execution-job-" + std::to_string(context.job_id)},
                &manifest_artifact, &error)) throw std::runtime_error(error);
        if (!analysis_->CompleteBattleCompletion({
                .battle_completion_id = row->battle_completion_id,
                .completion_savestate_id = *successor,
                .manifest_version = phase::ManifestVersion,
                .manifest_blob = std::string(
                    reinterpret_cast<const char*>(manifest.data()), manifest.size()),
                .manifest_sha256 = sha,
                .manifest_artifact_id = manifest_artifact,
                .route_kind = RouteName(route),
                .transition_filename = result.transition.sct_filename,
                .worker_terminal_sha256 = context.terminal.sha256,
                .status = "COMPLETED", .completed_at_utc = types::UtcNow(),
                .correlation_id = "battle-completion-" +
                    std::to_string(row->battle_completion_id),
                .causation_id = "execution-job-" + std::to_string(context.job_id)},
                &error)) throw std::runtime_error(error);
        auto decision = Decision("SUCCEEDED");
        decision.cleanup_worker_staging = true;
        decision.staging_files.push_back({
            .relative_path = path.lexically_relative(root_).generic_string(),
            .sha256 = sha,
            .size_bytes = static_cast<std::uint64_t>(manifest.size()),
        });
        decision.outputs.push_back(Output(row->battle_completion_id));
        decision.event_lines.push_back("[battle-completion] completion=" +
            std::to_string(row->battle_completion_id) + " route=" + RouteName(route) +
            " file=" + result.transition.sct_filename);
        return decision;
    }

    private:
    ProgramResultDecision PersistFailure(const BattleCompletionRecord& row,
        const ProgramResultProcessingContext& context,
        std::string code, std::string text,
        std::string job_state = "FAILED") const {
        std::string error;
        if (!analysis_->FailBattleCompletion({
                .battle_completion_id = row.battle_completion_id,
                .error_code = code, .error_text = text,
                .worker_terminal_sha256 = context.terminal.sha256,
                .completed_at_utc = types::UtcNow(),
                .correlation_id = "battle-completion-" +
                    std::to_string(row.battle_completion_id),
                .causation_id = "execution-job-" + std::to_string(context.job_id)},
                &error)) throw std::runtime_error(error);
        auto decision = Decision(
            std::move(job_state), std::move(code), std::move(text));
        decision.cleanup_worker_staging = true;
        return decision;
    }

    IStateDb* state_{};
    IAnalysisDb* analysis_{};
    std::filesystem::path root_;
    std::shared_ptr<const phase::IBattleCompletionFullPhaseDefinitionV1> phase_;
};

} // namespace

ProgramKindDescriptor BuildBattleCompletionProgramDescriptor(
    IExecutionDb* execution_db, IStateDb* state_db, IAnalysisDb* analysis_db,
    BattleCompletionProgramConfig config) {
    ProgramKindDescriptor descriptor{};
    descriptor.program_kind = static_cast<std::int32_t>(savor::PK_BattleCompletion);
    descriptor.program_name = "Battle Completion";
    descriptor.result_staging_root = config.working_dir_root;
    descriptor.full_phase_identity = phase::BattleCompletionFullPhaseDefinitionV1()->identity();
    descriptor.default_progress_library_ids = ObservationDefaults().progress_library_ids;
    descriptor.default_derived_state_block_ids = std::vector<std::string>{};
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

} // namespace savor::db::execution::programdb::battlecompletion
