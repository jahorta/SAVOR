#include "TasMovieValidationProgram.h"
#include "../WorksetDerivedStateBinding.h"
#include "../WorksetObservationBinding.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
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
#include "../../../../SavorCore/Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "../../../../SavorCore/Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.h"
#include "../../../../SavorCore/Runner/Runtime/Worksets/WorksetWireCodec.h"
#include "../../../../SavorCore/Tas/DtmFile.h"
#include "../../../../SavorCore/Utils/Hash.h"

namespace savor::db::execution::programdb::tasmovievalidation {
namespace {

constexpr std::string_view kProgramRefKind = "tmv_validation_request";
constexpr std::string_view kCreatedBy = "tas_movie_validation_program_kind";
constexpr std::string_view kPurpose = "TAS_MOVIE_COMPLETE_VALIDATION";
constexpr std::uint32_t kRequiredTerminalPc = 0x80101E48u;
constexpr std::size_t kDeclaredTerminalBytes = 256ull * 1024ull;

const WorksetObservationDefaultsV1& ObservationDefaults()
{
    static const WorksetObservationDefaultsV1 defaults{
        .progress_library_ids = {
            "soa.progress.runtime.vi/1",
            "soa.progress.soa.script_location/1",
        },
        .runtime_sample_trigger_pcs = {
            savor::runtime::tasmovie::BeforeRandSeedSetPc,
        },
    };
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
        ? std::filesystem::temp_directory_path() / "savor-tas-movie-validation"
        : configured;
}

bool EnsureParent(const std::filesystem::path& path, std::string* error_out) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (!error) return true;
    if (error_out) *error_out = "failed creating TAS Movie working directory: " + error.message();
    return false;
}

std::optional<std::string> HashFile(const std::filesystem::path& path) {
    try { return hash::sha256_of_file(path.string()); }
    catch (...) { return std::nullopt; }
}

std::optional<std::vector<std::uint8_t>> ReadFile(
    const std::filesystem::path& path,
    std::string* error_out) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        if (error_out) *error_out = "failed opening immutable TAS Movie artifact: " + path.string();
        return std::nullopt;
    }
    stream.seekg(0, std::ios::end);
    const auto end = stream.tellg();
    if (end < 0) return std::nullopt;
    stream.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    if (!bytes.empty() && !stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        if (error_out) *error_out = "failed reading immutable TAS Movie artifact";
        return std::nullopt;
    }
    return bytes;
}

bool WriteFile(
    const std::filesystem::path& path,
    std::span<const std::uint8_t> bytes,
    std::string* error_out) {
    if (!EnsureParent(path, error_out)) return false;
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream || (!bytes.empty() && !stream.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size())))) {
        if (error_out) *error_out = "failed writing immutable TAS Movie artifact";
        return false;
    }
    return true;
}

bool ValidateDtm(
    const std::filesystem::path& path,
    savor::tas::DtmFile* dtm_out,
    std::string* error_out) {
    savor::tas::DtmFile dtm;
    std::string reason;
    if (!dtm.load(path.string()) || !dtm.supports_gc_poll_editing(&reason)) {
        if (error_out) *error_out = "TAS Movie requires an aligned GC-only DTM: " + reason;
        return false;
    }
    const auto info = dtm.info();
    const std::string game_id(info.game_id.data(), info.game_id.size());
    if (game_id != savor::runtime::program::capabilities::kSupportedGameId
        || info.is_wii || info.input_count != dtm.gc_poll_count()) {
        if (error_out) *error_out = "TAS Movie DTM is not the exact supported US GameCube format";
        return false;
    }
    if (dtm_out) *dtm_out = std::move(dtm);
    return true;
}

std::optional<std::filesystem::path> MaterializeArtifact(
    IStateDb* state_db,
    const ArtifactRecord& artifact,
    const std::filesystem::path& destination,
    std::string* error_out) {
    if (!IsLowerHexSha256(artifact.sha256) || artifact.size_bytes <= 0
        || !EnsureParent(destination, error_out)) return std::nullopt;
    if (const auto hash = HashFile(destination); hash && *hash == artifact.sha256) return destination;
    const auto materialized = state_db->MaterializeArtifactToPath(
        artifact.artifact_id, destination.string(), error_out);
    if (!materialized) return std::nullopt;
    const auto hash = HashFile(destination);
    if (!hash || *hash != artifact.sha256) {
        if (error_out) *error_out = "materialized TAS Movie artifact hash drifted";
        return std::nullopt;
    }
    return destination;
}

std::optional<std::filesystem::path> ResolveEffectiveDtm(
    IStateDb* state_db,
    const TasMovieValidationRequestRecord& request,
    const std::filesystem::path& root,
    std::string* error_out) {
    const auto source = state_db->GetArtifact(request.source_dtm_artifact_id);
    if (!source || source->artifact_kind != "DTM" || source->sha256 != request.source_dtm_sha256) {
        if (error_out) *error_out = "TAS Movie request source DTM snapshot drifted";
        return std::nullopt;
    }
    const auto source_path = root / "sources" / (source->sha256 + ".dtm");
    if (!MaterializeArtifact(state_db, *source, source_path, error_out)) return std::nullopt;
    savor::tas::DtmFile dtm;
    if (!ValidateDtm(source_path, &dtm, error_out)) return std::nullopt;

    const auto effective_path = root / "effective" / (request.effective_dtm_sha256 + ".dtm");
    if (request.source_kind == TasMovieValidationSourceKind::RootEstablishment) {
        if (!request.rtc_value || *request.rtc_value < 0
            || static_cast<std::uint64_t>(*request.rtc_value)
                > std::numeric_limits<std::uint32_t>::max()) {
            if (error_out) {
                *error_out = "root validation RTC snapshot must be within 0..UINT32_MAX";
            }
            return std::nullopt;
        }
        dtm.set_gamecube_rtc_seconds(
            static_cast<std::uint32_t>(*request.rtc_value));
        if (!EnsureParent(effective_path, error_out) || !dtm.save(effective_path.string())) {
            if (error_out) *error_out = "failed deterministically regenerating RTC-patched DTM";
            return std::nullopt;
        }
    } else {
        if (!MaterializeArtifact(state_db, *source, effective_path, error_out)) return std::nullopt;
    }
    const auto hash = HashFile(effective_path);
    if (!hash || *hash != request.effective_dtm_sha256) {
        if (error_out) *error_out = "effective TAS Movie DTM hash drifted during reconstruction";
        return std::nullopt;
    }
    if (!ValidateDtm(effective_path, nullptr, error_out)) return std::nullopt;
    return effective_path;
}

std::optional<savor::runtime::tasmovie::TasMovieItineraryV1> ResolveItinerary(
    IStateDb* state_db,
    const TasMovieValidationRequestRecord& request,
    const std::filesystem::path& root,
    std::uint64_t total_input_count,
    std::string* error_out) {
    if (!request.itinerary_artifact_id || !request.itinerary_sha256) return std::nullopt;
    const auto artifact = state_db->GetArtifact(*request.itinerary_artifact_id);
    if (!artifact || artifact->artifact_kind != "TAS_MOVIE_ITINERARY"
        || artifact->sha256 != *request.itinerary_sha256) {
        if (error_out) *error_out = "TAS Movie itinerary snapshot drifted";
        return std::nullopt;
    }
    const auto path = root / "itineraries" / (artifact->sha256 + ".tmi");
    if (!MaterializeArtifact(state_db, *artifact, path, error_out)) return std::nullopt;
    const auto bytes = ReadFile(path, error_out);
    savor::runtime::tasmovie::TasMovieItineraryV1 itinerary;
    if (!bytes || !savor::runtime::tasmovie::DecodeTasMovieItineraryArtifactV1(*bytes, itinerary, error_out)
        || !savor::runtime::tasmovie::ValidateTasMovieItineraryArtifactV1(
            itinerary, total_input_count, request.required_final_breakpoint_pc, error_out)) return std::nullopt;
    return itinerary;
}

std::optional<std::int64_t> Binding(
    const ProgramJobMaterializationContext& context,
    std::string_view input_key,
    std::string_view data_kind,
    std::string_view ref_kind) {
    if (context.step.domain_ref_id > 0 &&
        context.step.domain_ref_kind == ref_kind)
        return context.step.domain_ref_id;
    if (context.graph) {
        for (const auto& binding : context.graph->inputs) {
            if (binding.input_key == input_key && binding.data_kind == data_kind
                && binding.ref_kind == ref_kind && binding.ref_id > 0)
                return binding.ref_id;
        }
    }
    return std::nullopt;
}

std::optional<std::int64_t> IntegerArgument(
    const ProgramJobMaterializationContext& context,
    std::string_view key) {
    if (!context.graph) return std::nullopt;
    for (const auto& argument : context.graph->arguments) {
        if (argument.argument_key == key && argument.value_type == "integer"
            && argument.integer_value) return argument.integer_value;
    }
    return std::nullopt;
}

std::string JobInput(std::int64_t request_id) {
    return "TMVR1:" + std::to_string(request_id);
}

std::string Fingerprint(const TasMovieValidationRequestRecord& request) {
    const std::string value = request.materialization_key + "\n" + request.effective_dtm_sha256
        + "\n" + request.full_phase_sha256 + "\n" + std::to_string(request.capture_root_checkpoint);
    return "PK=2;PV=1;tasmovie_validation=" + hash::sha256(value.data(), value.size());
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

TasMovieValidationFailureReason MapFailure(
    savor::runtime::tasmovie::TasMovieValidationFailureReasonV1 reason) {
    using Source = savor::runtime::tasmovie::TasMovieValidationFailureReasonV1;
    switch (reason) {
    case Source::MovieDesynchronized: return TasMovieValidationFailureReason::MovieDesynchronized;
    case Source::ExpectedTerminalNotReached: return TasMovieValidationFailureReason::ExpectedTerminalNotReached;
    case Source::Unknown: return TasMovieValidationFailureReason::Unknown;
    }
    return TasMovieValidationFailureReason::Unknown;
}

class Materializer final : public IProgramJobMaterializer {
public:
    Materializer(IExecutionDb* execution_db, IStateDb* state_db, IAnalysisDb* analysis_db,
        TasMovieValidationProgramConfig config)
        : execution_db_(execution_db), state_db_(state_db), analysis_db_(analysis_db),
          config_(std::move(config)), phase_(savor::runtime::tasmovie::TasMovieValidationFullPhaseDefinitionV1()) {}

    bool MaterializeJobs(const ProgramJobMaterializationContext& context,
        WorkflowStepScheduleResult* result_out, std::string* error_out) const override {
        if (!result_out || !execution_db_ || !state_db_ || !analysis_db_ || !phase_ || !context.graph) {
            if (error_out) *error_out = "TAS Movie validation materialization dependencies are incomplete";
            return false;
        }
        *result_out = {};
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
        const auto program_package =
            savor::runtime::fullphase::BuildFullPhaseProgramPackage(*phase_);
        ResolvedWorksetDerivedStateBindingV1 derived_state;
        if (!ResolveWorksetDerivedStateBindingV1(
                std::span<const std::string>{},
                program_package,
                &derived_state,
                error_out)) {
            return false;
        }
        const auto& graph = *context.graph;
        CreateTasMovieValidationRequestCommand command{};
        command.materialization_key = "tasmovie.validation.step." + std::to_string(context.step.workflow_step_id);
        command.workflow_instance_id = context.step.workflow_instance_id;
        command.workflow_step_id = context.step.workflow_step_id;
        command.step_kind = context.step.step_kind;
        command.required_final_breakpoint_pc = kRequiredTerminalPc;
        command.created_at_utc = types::UtcNow();
        const auto& identity = phase_->identity();
        const auto& runtime = phase_->runtime_contract();
        command.full_phase_program_kind = identity.program_kind;
        command.full_phase_program_version = identity.program_version;
        command.full_phase_canonical_id = identity.canonical_id;
        command.full_phase_contract_revision = identity.contract_revision;
        command.full_phase_sha256 = identity.canonical_sha256;
        command.module_canonical_id = runtime.module.canonical_id;
        command.module_revision = runtime.module.revision;
        command.module_sha256 = runtime.module.canonical_hash;

        if (context.step.step_kind == "tasmovie.establish_root_cursor") {
            const auto artifact_id = Binding(context, "root_dtm", "state_artifact.dtm_artifact_id", "state_artifact");
            if (!artifact_id) return Fail("root cursor establishment requires one DTM artifact", error_out);
            const auto artifact = state_db_->GetArtifact(*artifact_id);
            if (!artifact || artifact->artifact_kind != "DTM") return Fail("root cursor DTM artifact is unavailable", error_out);
            command.operation = TasMovieValidationOperation::EstablishRootCursor;
            command.source_kind = TasMovieValidationSourceKind::DtmArtifact;
            command.source_ref_id = *artifact_id;
            command.source_dtm_artifact_id = *artifact_id;
            command.source_dtm_sha256 = artifact->sha256;
            command.effective_dtm_sha256 = artifact->sha256;
        } else if (context.step.step_kind == "tasmovie.validate_root") {
            const auto establishment_id = Binding(context, "root_establishment",
                "analysis.tas_movie_root_establishment_attempt_id",
                "tmv_root_establishment_attempt");
            const auto rtc = IntegerArgument(context, "rtc");
            const auto establishment = establishment_id
                ? analysis_db_->GetTasMovieRootEstablishmentAttempt(*establishment_id)
                : std::nullopt;
            if (!establishment
                || !rtc || *rtc < 0
                || static_cast<std::uint64_t>(*rtc)
                    > std::numeric_limits<std::uint32_t>::max())
                return Fail("root validation requires a successful establishment attempt and GameCube RTC in 0..UINT32_MAX", error_out);
            const auto source = state_db_->GetArtifact(establishment->source_dtm_artifact_id);
            const auto itinerary = state_db_->GetArtifact(establishment->itinerary_artifact_id);
            if (!source || source->sha256 != establishment->source_dtm_sha256
                || !itinerary || itinerary->sha256 != establishment->itinerary_sha256)
                return Fail("root validation source snapshot is unavailable", error_out);
            const auto source_path = WorkingRoot(config_.working_dir_root) / "materialization" / (source->sha256 + ".dtm");
            if (!MaterializeArtifact(state_db_, *source, source_path, error_out)) return false;
            savor::tas::DtmFile dtm;
            if (!ValidateDtm(source_path, &dtm, error_out)) return false;
            dtm.set_gamecube_rtc_seconds(static_cast<std::uint32_t>(*rtc));
            command.operation = TasMovieValidationOperation::Validate;
            command.source_kind = TasMovieValidationSourceKind::RootEstablishment;
            command.source_ref_id = *establishment_id;
            command.source_dtm_artifact_id = source->artifact_id;
            command.source_dtm_sha256 = source->sha256;
            command.rtc_value = *rtc;
            command.effective_dtm_sha256 = dtm.compute_sha256();
            command.itinerary_artifact_id = itinerary->artifact_id;
            command.itinerary_sha256 = itinerary->sha256;
            const auto existing_root = state_db_->FindTasMovieRootBySourceRtc(source->artifact_id, *rtc);
            if (existing_root) {
                const auto exact = state_db_->GetArtifact(existing_root->dtm_artifact_id);
                if (!exact || exact->sha256 != command.effective_dtm_sha256
                    || existing_root->itinerary_artifact_id != itinerary->artifact_id) return Fail("existing RTC root conflicts with deterministic validation request", error_out);
                command.capture_root_checkpoint = false;
            } else command.capture_root_checkpoint = true;
        } else if (context.step.step_kind == "tasmovie.validate_tree") {
            const auto tree_id = Binding(context, "tas_movie_tree", "state.tas_movie_tree_id", "state_tas_movie_tree");
            const auto tree = tree_id ? state_db_->GetTasMovieTree(*tree_id) : std::nullopt;
            if (!tree) return Fail("tree validation requires one immutable TAS movie tree", error_out);
            const auto dtm = state_db_->GetArtifact(tree->dtm_artifact_id);
            const auto itinerary = state_db_->GetArtifact(tree->itinerary_artifact_id);
            if (!dtm || !itinerary || dtm->artifact_kind != "DTM" || itinerary->artifact_kind != "TAS_MOVIE_ITINERARY") return Fail("tree validation artifacts are unavailable", error_out);
            command.operation = TasMovieValidationOperation::Validate;
            command.source_kind = TasMovieValidationSourceKind::Tree;
            command.source_ref_id = *tree_id;
            command.source_dtm_artifact_id = dtm->artifact_id;
            command.source_dtm_sha256 = dtm->sha256;
            command.effective_dtm_sha256 = dtm->sha256;
            command.itinerary_artifact_id = itinerary->artifact_id;
            command.itinerary_sha256 = itinerary->sha256;
            command.required_final_breakpoint_pc = tree->required_final_breakpoint_pc;
        } else return Fail("unsupported closed TAS Movie validation step kind", error_out);

        std::int64_t request_id = 0;
        if (!analysis_db_->CreateTasMovieValidationRequest(command, &request_id, error_out)) return false;
        const auto request = analysis_db_->GetTasMovieValidationRequest(request_id);
        if (!request) return Fail("persisted TAS Movie validation request cannot be reloaded", error_out);

        EnsureMaterializingJobSetReceipt ensured{};
        if (!execution_db_->EnsureMaterializingJobSet({
                .materialization_key = command.materialization_key,
                .program_kind = static_cast<std::int32_t>(savor::PK_TasMovie),
                .purpose = std::string(kPurpose), .created_by = std::string(kCreatedBy),
                .created_at_utc = NowMs(), .priority_boost = context.step.step_priority,
                .expected_total = 1, .domain_ref_kind = std::string(kProgramRefKind),
                .domain_ref_id = request_id, .meta_note = command.step_kind,
            }, &ensured, error_out)) return false;
        if (ensured.materialization_state == "MATERIALIZING") {
            CreatePendingJobReceipt created{};
            if (!execution_db_->CreatePendingJob({
                    .job_set_id = ensured.job_set_id,
                    .program_kind = static_cast<std::int32_t>(savor::PK_TasMovie),
                    .program_version = 1, .program_ref_kind = std::string(kProgramRefKind),
                    .program_ref_id = request_id, .fingerprint = Fingerprint(*request),
                    .priority = context.step.step_priority, .max_attempts = 1,
                    .input_ini = JobInput(request_id),
                }, &created, error_out)) return false;
        }
        const auto jobs = execution_db_->ListJobsInJobSet(ensured.job_set_id);
        if (jobs.size() != 1 || jobs.front().input_ini != JobInput(request_id)) return Fail("TAS Movie validation job set is not the immutable singleton", error_out);
        SealJobPopulationReceipt sealed{};
        if (!execution_db_->SealJobPopulation({.job_set_id = ensured.job_set_id, .expected_job_count = 1, .requested_by = std::string(kCreatedBy)}, &sealed, error_out)) return false;
        PublishWorksetWaveReceipt published{};
        PublishWorksetCommand workset{
            .job_set_id = ensured.job_set_id, .workflow_step_id = context.step.workflow_step_id,
            .workset_key = command.materialization_key + ".workset.0",
            .program_kind = static_cast<std::int32_t>(savor::PK_TasMovie), .program_version = 1,
            .contract = {
                .contract_key = "tasmovie-validation:v1:request:" + std::to_string(request_id) + ":phase:" + request->full_phase_sha256,
                .module_canonical_id = runtime.module.canonical_id,
                .module_version = static_cast<std::int32_t>(runtime.module.revision),
                .module_sha256 = runtime.module.canonical_hash, .entrypoint = runtime.entrypoint,
                .verified_dependency_sha256 = runtime.verified_dependency_sha256,
                .runtime_profile_sha256 = runtime.runtime_profile_sha256,
                .program_package_sha256 =
                    program_package.canonical_sha256,
                .estimated_payload_bytes = 256ull * 1024ull,
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
            .priority = context.step.step_priority, .ordered_job_ids = {jobs.front().job_id},
            .requested_by = std::string(kCreatedBy),
        };
        if (!execution_db_->PublishWorksetWave({.job_set_id = ensured.job_set_id, .expected_job_count = 1,
                .worksets = {std::move(workset)}, .requested_by = std::string(kCreatedBy)}, &published, error_out)) return false;
        result_out->job_set_id = ensured.job_set_id;
        result_out->persistence = {.program_ref_kind = std::string(kProgramRefKind), .program_ref_id = request_id,
            .fingerprint = Fingerprint(*request), .program_version = 1};
        result_out->event_lines.push_back("[tasmovie-validation-materialized] request=" + std::to_string(request_id));
        return true;
    }

    bool Continue(const ProgramJobContinuationContext& context,
        ProgramJobContinuationResult* result_out, std::string* error_out) const override {
        if (!result_out || context.job_set_id <= 0) return Fail("TAS Movie validation continuation is invalid", error_out);
        const auto jobs = execution_db_->ListJobsInJobSet(context.job_set_id);
        if (jobs.size() != 1) return Fail("TAS Movie validation workflow lost singleton shape", error_out);
        const auto job = execution_db_->GetExecutionJob(jobs.front().job_id);
        if (!job || !job->worker_terminal_fingerprint) return Fail("TAS Movie validation terminal identity is unavailable", error_out);
        const auto attempt = analysis_db_->FindTasMovieValidationAttempt(job->job_id, *job->worker_terminal_fingerprint);
        if (!attempt) return Fail("TAS Movie validation attempt was not durably persisted", error_out);
        result_out->disposition = ProgramJobContinuationDisposition::Complete;
        result_out->output = ProgramJobContinuationOutput{.output_key = "tas_movie_validation_attempt",
            .data_kind = "analysis.tas_movie_validation_attempt_id", .ref_kind = "tmv_validation_attempt",
            .ref_id = attempt->validation_attempt_id};
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
    TasMovieValidationProgramConfig config_;
    std::shared_ptr<const savor::runtime::tasmovie::ITasMovieValidationFullPhaseDefinitionV1> phase_;
};

class Reconstruction final : public IWorksetReconstructionAdapter {
public:
    Reconstruction(IStateDb* state_db, IAnalysisDb* analysis_db, std::filesystem::path root)
        : state_db_(state_db), analysis_db_(analysis_db), root_(WorkingRoot(root)),
          phase_(savor::runtime::tasmovie::TasMovieValidationFullPhaseDefinitionV1()) {}

    std::optional<WorksetReconstructionResult> Reconstruct(
        const WorksetReconstructionContext& context, std::string* error_out) const override {
        const auto fail = [&](std::string message) -> std::optional<WorksetReconstructionResult> {
            if (error_out) *error_out = std::move(message);
            return std::nullopt;
        };
        if (!state_db_ || !analysis_db_ || !phase_ || context.items.size() != 1
            || context.workset_id <= 0 || context.dispatch_attempt_id <= 0
            || context.workflow_step_id <= 0 || context.job_set_id <= 0
            || context.dispatch_token.empty() || !context.state_compatibility.Complete()) return fail("TAS Movie validation reconstruction requires one exact item");
        const auto& item = context.items.front();
        if (item.program_kind != static_cast<std::int32_t>(savor::PK_TasMovie)
            || item.program_version != 1 || item.program_ref_kind != kProgramRefKind
            || item.program_ref_id <= 0 || item.savestate_id || item.input_ini != JobInput(item.program_ref_id)
            || item.reserved_attempt_id == 0 || item.claim_token.empty()) return fail("TAS Movie validation item identity drifted");
        const auto request = analysis_db_->GetTasMovieValidationRequest(item.program_ref_id);
        if (!request || request->workflow_step_id != context.workflow_step_id
            || request->full_phase_sha256 != phase_->identity().canonical_sha256
            || request->module_sha256 != phase_->runtime_contract().module.canonical_hash) return fail("TAS Movie validation immutable identity drifted");
        const auto dtm_path = ResolveEffectiveDtm(state_db_, *request, root_, error_out);
        if (!dtm_path) return std::nullopt;
        savor::tas::DtmFile dtm;
        if (!ValidateDtm(*dtm_path, &dtm, error_out)) return std::nullopt;
        savor::runtime::tasmovie::TasMovieValidationRequestV1 native{};
        native.operation = request->operation == TasMovieValidationOperation::EstablishRootCursor
            ? savor::runtime::tasmovie::TasMovieValidationOperationV1::EstablishRootCursor
            : savor::runtime::tasmovie::TasMovieValidationOperationV1::Validate;
        native.dtm_path = dtm_path->string();
        if (dtm.info().starts_from_savestate)
            native.startup_savestate_path = dtm_path->string() + ".sav";
        if (request->operation == TasMovieValidationOperation::Validate) {
            const auto itinerary = ResolveItinerary(state_db_, *request, root_, dtm.info().input_count, error_out);
            if (!itinerary) return std::nullopt;
            native.itinerary = *itinerary;
            if (request->capture_root_checkpoint) {
                native.final_checkpoint_path = (root_ / "captures" /
                    ("request-" + std::to_string(request->validation_request_id) + ".sav")).string();
            }
        }
        std::string diagnostic;
        const auto input = savor::runtime::tasmovie::EncodeTasMovieValidationExecutionInputV1(native, &diagnostic);
        if (input.empty()) return fail("TAS Movie validation invocation binding failed: " + diagnostic);

        savor::runtime::WorkerWorksetDefinition workset{};
        workset.workset_id = savor::runtime::WorkerWorksetId(static_cast<std::uint64_t>(context.dispatch_attempt_id));
        workset.phase_invocation = {.invocation_id = {.workflow_step_id = static_cast<std::uint64_t>(context.workflow_step_id),
            .job_set_id = static_cast<std::uint64_t>(context.job_set_id)},
            .program_package = savor::runtime::fullphase::BuildFullPhaseProgramPackage(*phase_),
            .common_input = savor::runtime::fullphase::MakeFullPhaseCommonInput(
                "soa.tas_movie_validation.CommonInput", 1)};
        const std::filesystem::path startup_path(dtm_path->string() + ".sav");
        const bool has_startup = dtm.info().starts_from_savestate;
        if (has_startup && !std::filesystem::is_regular_file(startup_path))
            return fail("TAS Movie DTM requires its exact <dtm>.sav startup artifact");
        workset.baseline = {.artifact = {
                .kind = savor::runtime::ProgramBaselineArtifactKind::ReadOnlyMovie,
                .state_path = has_startup ? startup_path : std::filesystem::path{},
                .state_sha256 = has_startup
                    ? ::hash::sha256_of_file(startup_path.string())
                    : std::string{},
                .movie_path = *dtm_path,
                .movie_sha256 = ::hash::sha256_of_file(dtm_path->string()),
                .compatibility = context.state_compatibility,
                .lineage = {
                    .edge = phase_->runtime_contract().baseline_lineage,
                    .producer = "SavorDb.PK_TasMovie",
                },
            },
            .lineage = phase_->runtime_contract().baseline_lineage};
        workset.derived_state = context.derived_state;
        workset.capture = context.capture;
        workset.progress_plan = context.progress_plan;
        const auto& runtime = phase_->runtime_contract();
        workset.execution_key = {.module = runtime.module, .entrypoint = runtime.entrypoint,
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
            .progress_plan_sha256 = workset.progress_plan.content_sha256};
        workset.execution_key.canonical_sha256 = savor::runtime::ComputeWorkerWorksetExecutionKeyHash(workset.execution_key);
        workset.items.push_back({.item_id = savor::runtime::WorkerWorksetItemId(static_cast<std::uint64_t>(item.job_id)),
            .ordinal = 0, .execution = {.execution_id = savor::runtime::ProgramExecutionId(static_cast<std::uint64_t>(item.job_id)),
                .attempt_id = savor::runtime::AttemptId(item.reserved_attempt_id), .input_payload = input},
            .declared_terminal_bytes = kDeclaredTerminalBytes,
            .correlation = {.durable_job_id = std::to_string(item.job_id), .claim_token = item.claim_token,
                .parent_correlation = context.contract_key}});
        std::vector<std::uint8_t> encoded;
        const auto status = savor::runtime::EncodeWorkerWorksetV5(workset, encoded);
        if (!status) return fail("TAS Movie validation workset encoding failed: " + status.message);
        workset.encoded_size_bytes = encoded.size();
        return WorksetReconstructionResult{.workset = std::move(workset), .ordered_job_ids = {item.job_id}};
    }

private:
    IStateDb* state_db_{};
    IAnalysisDb* analysis_db_{};
    std::filesystem::path root_;
    std::shared_ptr<const savor::runtime::tasmovie::ITasMovieValidationFullPhaseDefinitionV1> phase_;
};

class ResultHandler final : public IProgramResultHandler {
public:
    ResultHandler(IStateDb* state_db, IAnalysisDb* analysis_db, std::filesystem::path root)
        : state_db_(state_db), analysis_db_(analysis_db), root_(WorkingRoot(root)),
          phase_(savor::runtime::tasmovie::TasMovieValidationFullPhaseDefinitionV1()) {}

    ProgramResultDecision Process(const ProgramResultProcessingContext& context) const override {
        if (!state_db_ || !analysis_db_ || !phase_ || context.program_kind != static_cast<std::int32_t>(savor::PK_TasMovie)
            || context.program_version != 1 || context.program_ref_kind != kProgramRefKind
            || context.program_ref_id <= 0 || context.input_ini != JobInput(context.program_ref_id))
            return FinalDecision("FAILED", "TAS_MOVIE_VALIDATION_IDENTITY_INVALID", "job is not an exact TAS Movie validation request");
        const auto request = analysis_db_->GetTasMovieValidationRequest(context.program_ref_id);
        if (!request) throw std::runtime_error("TAS Movie validation request is missing");
        savor::runtime::DurableWorkerTerminalEnvelope terminal{};
        std::string error;
        if (!savor::runtime::DecodeDurableWorkerTerminalEnvelope(context.terminal.envelope, &terminal, &error))
            return FinalDecision("FAILED", "TAS_MOVIE_VALIDATION_TERMINAL_INVALID", std::move(error));
        using TerminalStatus = savor::wrms::InvocationTerminalStatus;
        if (terminal.terminal.status == TerminalStatus::Cancelled)
            return FinalDecision("FAILED", "UNCLASSIFIED_CANCELLED_TERMINAL_REACHED_DESCRIPTOR", "cancelled worker terminal bypassed ProgramResultProcessor");
        if (terminal.terminal.status != TerminalStatus::Succeeded || terminal.terminal.unstarted
            || terminal.terminal.workset_id != static_cast<std::uint64_t>(context.terminal.dispatch_attempt_id)
            || terminal.terminal.item_id != static_cast<std::uint64_t>(context.job_id)
            || terminal.terminal.invocation_id != static_cast<std::uint64_t>(context.job_id)
            || terminal.terminal.attempt_id != context.terminal.reserved_attempt_id
            || terminal.process_generation == 0 || terminal.terminal.workset_epoch == 0
            || terminal.terminal.session_disposition != savor::wrms::SessionDispositionCode::Clean)
            return FinalDecision("FAILED", terminal.terminal.error_code.empty() ? "TAS_MOVIE_VALIDATION_EXECUTION_FAILED" : terminal.terminal.error_code,
                terminal.terminal.message.empty() ? "worker invocation did not complete cleanly" : terminal.terminal.message);
        const auto decoded = savor::runtime::program::DecodeProgramResultV1(terminal.terminal.result);
        if (!decoded || decoded.value->invocation_id.value() != static_cast<std::uint64_t>(context.job_id)
            || decoded.value->attempt_id.value() != context.terminal.reserved_attempt_id)
            return FinalDecision("FAILED", "TAS_MOVIE_VALIDATION_PROGRAM_RESULT_MISMATCH", decoded ? "ProgramResult identity drifted" : decoded.status.message);
        savor::runtime::tasmovie::TasMovieValidationResultV1 outcome{};
        if (!phase_->DecodeProgramResult(terminal.terminal.result, outcome, &error))
            return FinalDecision("FAILED", "TAS_MOVIE_VALIDATION_OUTCOME_INVALID", std::move(error));

        RecordTasMovieValidationAttemptCommand attempt{};
        attempt.validation_request_id = request->validation_request_id;
        attempt.source_job_id = context.job_id;
        attempt.worker_terminal_sha256 = context.terminal.sha256;
        attempt.worker_id = std::to_string(terminal.worker_id);
        attempt.worker_process_generation = terminal.process_generation;
        attempt.workset_epoch = terminal.terminal.workset_epoch;
        attempt.recorded_at_utc = types::UtcNow();
        std::optional<std::int64_t> produced_root;
        std::optional<ProgramResultStagingFile> generated_itinerary;

        using Outcome = savor::runtime::tasmovie::TasMovieValidationOutcomeV1;
        if (outcome.outcome == Outcome::RootCursorEstablished) {
            if (!outcome.candidate_checkpoint || !decoded.value->artifacts.empty()
                || request->operation != TasMovieValidationOperation::EstablishRootCursor)
                return FinalDecision("FAILED", "TAS_MOVIE_ROOT_CURSOR_ARTIFACT_INVALID", "root establishment returned an illegal result shape");
            const auto checkpoint = savor::runtime::tasmovie::MakeTasMovieCheckpointV1(
                outcome.candidate_checkpoint->pc,
                outcome.candidate_checkpoint->input_count.value,
                outcome.candidate_checkpoint->vi_count);
            savor::runtime::tasmovie::TasMovieItineraryV1 itinerary{{checkpoint}};
            savor::tas::DtmFile source_dtm;
            const auto effective = ResolveEffectiveDtm(state_db_, *request, root_, &error);
            if (!effective || !ValidateDtm(*effective, &source_dtm, &error)
                || !savor::runtime::tasmovie::ValidateTasMovieItineraryArtifactV1(
                    itinerary,
                    source_dtm.info().input_count,
                    request->required_final_breakpoint_pc,
                    &error))
                return FinalDecision(
                    "FAILED",
                    "TAS_MOVIE_ROOT_CURSOR_INVALID",
                    error.empty()
                        ? "root establishment checkpoint is not bound to the source DTM"
                        : error);
            const auto bytes = savor::runtime::tasmovie::EncodeTasMovieItineraryArtifactV1(itinerary, &error);
            if (bytes.empty()) return FinalDecision("FAILED", "TAS_MOVIE_ITINERARY_INVALID", error);
            const auto sha = hash::sha256(bytes.data(), bytes.size());
            const auto path = root_ / "published" / (sha + ".tmi");
            if (!WriteFile(path, bytes, &error)) throw std::runtime_error(error);
            std::int64_t artifact_id = 0;
            if (!StoreWorkspaceArtifactFile(state_db_, path, {.sha256 = sha, .size_bytes = static_cast<std::int64_t>(bytes.size()),
                    .compression_kind = 0, .file_ext = ".tmi",
                    .artifact_kind = "TAS_MOVIE_ITINERARY", .created_at_utc = types::UtcNow(),
                    .correlation_id = "tmv-request-" + std::to_string(request->validation_request_id),
                    .causation_id = "execution-job-" + std::to_string(context.job_id)}, &artifact_id, &error))
                throw std::runtime_error(error);
            attempt.outcome = TasMovieValidationOutcome::RootCursorEstablished;
            attempt.actual_pc = checkpoint.pc;
            attempt.actual_input_count = checkpoint.input_count.value;
            attempt.candidate_itinerary_artifact_id = artifact_id;
            attempt.candidate_itinerary_sha256 = sha;
            generated_itinerary = ProgramResultStagingFile{
                .relative_path = path.lexically_relative(root_).generic_string(),
                .sha256 = sha,
                .size_bytes = static_cast<std::uint64_t>(bytes.size()),
            };
        } else if (outcome.outcome == Outcome::Invalid) {
            if (!outcome.failure || !decoded.value->artifacts.empty())
                return FinalDecision("FAILED", "TAS_MOVIE_INVALID_RESULT_SHAPE", "Invalid returned an illegal artifact/result shape");
            attempt.outcome = TasMovieValidationOutcome::Invalid;
            attempt.failure_reason = MapFailure(outcome.failure->reason);
            const auto& diagnostic = outcome.failure->diagnostics;
            attempt.expected_pc = diagnostic.expected_pc;
            if (diagnostic.expected_input_count) attempt.expected_input_count = diagnostic.expected_input_count->value;
            attempt.actual_pc = diagnostic.actual_pc;
            attempt.actual_input_count = diagnostic.actual_input_count.value;
            attempt.last_verified_itinerary_index = diagnostic.last_verified_itinerary_index;
            attempt.last_known_good_savestate_id = ResolveLastKnownGood(*request, diagnostic.last_verified_itinerary_index);
        } else if (outcome.outcome == Outcome::Valid) {
            if (!request->itinerary_artifact_id) return FinalDecision("FAILED", "TAS_MOVIE_VALID_REQUEST_INVALID", "Valid has no immutable itinerary");
            savor::tas::DtmFile dtm;
            const auto effective = ResolveEffectiveDtm(state_db_, *request, root_, &error);
            if (!effective || !ValidateDtm(*effective, &dtm, &error)) return FinalDecision("FAILED", "TAS_MOVIE_DTM_DRIFT", error);
            const auto itinerary = ResolveItinerary(state_db_, *request, root_, dtm.info().input_count, &error);
            if (!itinerary) return FinalDecision("FAILED", "TAS_MOVIE_ITINERARY_DRIFT", error);
            attempt.outcome = TasMovieValidationOutcome::Valid;
            attempt.actual_pc = itinerary->checkpoints.back().pc;
            attempt.actual_input_count = itinerary->checkpoints.back().input_count.value;
            if (request->capture_root_checkpoint) {
                if (request->source_kind != TasMovieValidationSourceKind::RootEstablishment
                    || decoded.value->artifacts.size() != 1)
                    return FinalDecision("FAILED", "TAS_MOVIE_CAPTURE_AUTHORITY_VIOLATION", "authorized root capture did not return exactly one state");
                produced_root = PublishRoot(*request, decoded.value->artifacts.front().artifact, context.job_id, &error);
                if (!produced_root) throw std::runtime_error(error.empty() ? "root publication failed" : error);
                attempt.produced_tas_movie_root_id = produced_root;
            } else if (!decoded.value->artifacts.empty())
                return FinalDecision("FAILED", "TAS_MOVIE_UNEXPECTED_ARTIFACT", "validation without capture authority returned an artifact");
        } else return FinalDecision("FAILED", "TAS_MOVIE_VALIDATION_OUTCOME_UNKNOWN", "worker returned an unknown typed outcome");

        std::optional<std::int64_t> validated_checkpoint_savestate_id;
        if (attempt.outcome == TasMovieValidationOutcome::Valid) {
            validated_checkpoint_savestate_id = ResolveValidatedCheckpoint(
                *request,
                attempt,
                &error);
            if (!validated_checkpoint_savestate_id) {
                throw std::runtime_error(
                    error.empty()
                        ? "validated TAS Movie checkpoint is unavailable"
                        : error);
            }
        }

        std::int64_t attempt_id = 0;
        if (!analysis_db_->RecordTasMovieValidationAttempt(attempt, &attempt_id, &error)) throw std::runtime_error(error);
        std::optional<std::int64_t> root_establishment_id;
        if (attempt.outcome == TasMovieValidationOutcome::RootCursorEstablished) {
            std::int64_t id = 0;
            if (!analysis_db_->RecordTasMovieRootEstablishmentAttempt({
                    .producer = TasMovieRootEstablishmentProducer::Establish,
                    .validation_attempt_id = attempt_id,
                    .source_dtm_artifact_id = request->source_dtm_artifact_id,
                    .source_dtm_sha256 = request->source_dtm_sha256,
                    .itinerary_artifact_id = *attempt.candidate_itinerary_artifact_id,
                    .itinerary_sha256 = *attempt.candidate_itinerary_sha256,
                    .root_pc = attempt.actual_pc,
                    .movie_input_cursor = attempt.actual_input_count,
                    .source_job_id = context.job_id,
                    .worker_terminal_sha256 = context.terminal.sha256,
                    .recorded_at_utc = types::UtcNow(),
                }, &id, &error)) throw std::runtime_error(error);
            root_establishment_id = id;
        }
        ProgramResultDecision decision = FinalDecision("SUCCEEDED");
        decision.cleanup_worker_staging = true;
        if (generated_itinerary)
            decision.staging_files.push_back(*generated_itinerary);
        decision.outputs.push_back({.output_key = "tas_movie_validation_attempt",
            .data_kind = "analysis.tas_movie_validation_attempt_id", .ref_kind = "tmv_validation_attempt", .ref_id = attempt_id});
        if (attempt.outcome == TasMovieValidationOutcome::RootCursorEstablished) {
            decision.outputs.push_back({
                .output_key = "root_establishment",
                .data_kind = "analysis.tas_movie_root_establishment_attempt_id",
                .ref_kind = "tmv_root_establishment_attempt",
                .ref_id = *root_establishment_id,
            });
            decision.outputs.push_back({
                .output_key = "root_dtm",
                .data_kind = "state_artifact.dtm_artifact_id",
                .ref_kind = "state_artifact",
                .ref_id = request->source_dtm_artifact_id,
            });
        }
        if (validated_checkpoint_savestate_id) {
            decision.outputs.push_back({
                .output_key = "validated_checkpoint_savestate",
                .data_kind = "state.movie_paired_savestate_id",
                .ref_kind = "state.savestate",
                .ref_id = *validated_checkpoint_savestate_id,
            });
        }
        if (attempt.outcome == TasMovieValidationOutcome::Invalid) {
            decision.error_code = "TAS_MOVIE_INVALID";
            decision.error_text = "TAS movie validation reported a durable domain invalidity";
            decision.event_lines.push_back("[tasmovie-validation-invalid] attempt=" + std::to_string(attempt_id));
        }
        return decision;
    }

    private:
    std::optional<std::int64_t> ResolveValidatedCheckpoint(
        const TasMovieValidationRequestRecord& request,
        const RecordTasMovieValidationAttemptCommand& attempt,
        std::string* error_out) const {
        const auto fail = [&](std::string message)
            -> std::optional<std::int64_t> {
            if (error_out) *error_out = std::move(message);
            return std::nullopt;
        };
        if (attempt.outcome != TasMovieValidationOutcome::Valid
            || request.operation != TasMovieValidationOperation::Validate) {
            return fail("validated checkpoint requires a typed Valid outcome");
        }

        std::int64_t checkpoint_savestate_id = 0;
        std::int64_t dtm_artifact_id = 0;
        std::int64_t itinerary_artifact_id = 0;
        std::uint32_t final_pc = 0;
        if (request.source_kind == TasMovieValidationSourceKind::RootEstablishment) {
            std::optional<TasMovieRootRecord> root;
            if (attempt.produced_tas_movie_root_id) {
                root = state_db_->GetTasMovieRoot(
                    *attempt.produced_tas_movie_root_id);
            } else if (request.rtc_value) {
                root = state_db_->FindTasMovieRootBySourceRtc(
                    request.source_dtm_artifact_id,
                    *request.rtc_value);
            }
            if (!root || !request.rtc_value
                || root->source_dtm_artifact_id
                    != request.source_dtm_artifact_id
                || root->rtc_value != *request.rtc_value) {
                return fail("validated root checkpoint identity is unavailable");
            }
            checkpoint_savestate_id = root->checkpoint_savestate_id;
            dtm_artifact_id = root->dtm_artifact_id;
            itinerary_artifact_id = root->itinerary_artifact_id;
            final_pc = root->required_final_breakpoint_pc;
        } else if (request.source_kind == TasMovieValidationSourceKind::Tree) {
            const auto tree = state_db_->GetTasMovieTree(
                request.source_ref_id);
            if (!tree) {
                return fail("validated tree checkpoint identity is unavailable");
            }
            checkpoint_savestate_id = tree->checkpoint_savestate_id;
            dtm_artifact_id = tree->dtm_artifact_id;
            itinerary_artifact_id = tree->itinerary_artifact_id;
            final_pc = tree->required_final_breakpoint_pc;
        } else {
            return fail("validated checkpoint source kind is unsupported");
        }

        if (checkpoint_savestate_id <= 0
            || !request.itinerary_artifact_id
            || itinerary_artifact_id != *request.itinerary_artifact_id
            || final_pc != request.required_final_breakpoint_pc) {
            return fail(
                "validated checkpoint row drifted from its immutable request");
        }
        const auto checkpoint = state_db_->GetSavestate(
            checkpoint_savestate_id);
        const auto dtm = state_db_->GetArtifact(dtm_artifact_id);
        if (!dtm || dtm->artifact_kind != "DTM"
            || dtm->sha256 != request.effective_dtm_sha256
            || !checkpoint || !checkpoint->is_complete
            || checkpoint->artifact_kind != "SAV") {
            return fail("validated checkpoint savestate is incomplete");
        }
        return checkpoint_savestate_id;
    }

    bool VerifyRecoverySideEffects(
        const TasMovieValidationRequestRecord& request,
        const TasMovieValidationAttemptRecord& attempt,
        std::string* error_out) const {
        const auto fail = [&](std::string message) {
            if (error_out) *error_out = std::move(message);
            return false;
        };
        if (state_db_ == nullptr) return fail("TAS Movie State recovery dependency is missing");

        if (attempt.outcome == TasMovieValidationOutcome::RootCursorEstablished) {
            if (request.operation != TasMovieValidationOperation::EstablishRootCursor
                || !attempt.candidate_itinerary_artifact_id
                || !attempt.candidate_itinerary_sha256
                || attempt.produced_tas_movie_root_id)
                return fail("persisted root-cursor decision has an illegal shape");
            const auto artifact = state_db_->GetArtifact(
                *attempt.candidate_itinerary_artifact_id);
            if (!artifact
                || artifact->artifact_kind != "TAS_MOVIE_ITINERARY"
                || artifact->sha256 != *attempt.candidate_itinerary_sha256)
                return fail("persisted root-cursor itinerary artifact drifted");
            const auto path = root_ / "recovery" / (artifact->sha256 + ".tmi");
            if (!MaterializeArtifact(state_db_, *artifact, path, error_out))
                return false;
            const auto bytes = ReadFile(path, error_out);
            savor::runtime::tasmovie::TasMovieItineraryV1 itinerary;
            if (!bytes
                || !savor::runtime::tasmovie::DecodeTasMovieItineraryArtifactV1(
                    *bytes, itinerary, error_out)
                || itinerary.checkpoints.size() != 1
                || itinerary.checkpoints.front().pc != attempt.actual_pc
                || itinerary.checkpoints.front().input_count.value
                    != attempt.actual_input_count)
                return fail("persisted root-cursor itinerary does not match the typed attempt");
            savor::tas::DtmFile dtm;
            const auto effective = ResolveEffectiveDtm(
                state_db_, request, root_, error_out);
            if (!effective || !ValidateDtm(*effective, &dtm, error_out)
                || !savor::runtime::tasmovie::ValidateTasMovieItineraryArtifactV1(
                    itinerary,
                    dtm.info().input_count,
                    request.required_final_breakpoint_pc,
                    error_out))
                return false;
            return true;
        }

        if (request.operation != TasMovieValidationOperation::Validate)
            return fail("persisted validation outcome belongs to an establishment request");
        if (attempt.outcome == TasMovieValidationOutcome::Invalid) {
            if (attempt.produced_tas_movie_root_id
                || attempt.candidate_itinerary_artifact_id
                || attempt.candidate_itinerary_sha256)
                return fail("persisted Invalid decision has forbidden State side effects");
            if (attempt.last_known_good_savestate_id
                && !state_db_->GetSavestate(*attempt.last_known_good_savestate_id))
                return fail("persisted Invalid decision lost its last-known-good checkpoint");
            return true;
        }
        if (attempt.outcome != TasMovieValidationOutcome::Valid)
            return fail("persisted TAS Movie attempt has an unknown outcome");
        if (attempt.candidate_itinerary_artifact_id
            || attempt.candidate_itinerary_sha256)
            return fail("persisted Valid decision has a candidate itinerary side effect");
        if (!request.capture_root_checkpoint) {
            if (attempt.produced_tas_movie_root_id)
                return fail("validation without capture authority produced a root");
            return true;
        }
        if (!attempt.produced_tas_movie_root_id || !request.rtc_value
            || !request.itinerary_artifact_id || !request.itinerary_sha256)
            return fail("capturing validation is missing its immutable root side effects");
        const auto root = state_db_->GetTasMovieRoot(
            *attempt.produced_tas_movie_root_id);
        if (!root
            || root->source_dtm_artifact_id != request.source_dtm_artifact_id
            || root->rtc_value != *request.rtc_value
            || root->itinerary_artifact_id != *request.itinerary_artifact_id
            || root->required_final_breakpoint_pc
                != request.required_final_breakpoint_pc
            || root->source_context_kind != "tmv_validation_request"
            || root->source_context_id != request.validation_request_id)
            return fail("persisted root row does not match the immutable validation request");
        const auto dtm = state_db_->GetArtifact(root->dtm_artifact_id);
        const auto itinerary = state_db_->GetArtifact(root->itinerary_artifact_id);
        const auto checkpoint = state_db_->GetSavestate(
            root->checkpoint_savestate_id);
        if (!dtm || dtm->artifact_kind != "DTM"
            || dtm->sha256 != request.effective_dtm_sha256
            || !itinerary
            || itinerary->artifact_kind != "TAS_MOVIE_ITINERARY"
            || itinerary->sha256 != *request.itinerary_sha256
            || !checkpoint || !checkpoint->is_complete
            || checkpoint->artifact_kind != "SAV")
            return fail("persisted root artifact or checkpoint identity drifted");
        const auto dtm_hash = HashFile(dtm->object_path);
        const auto checkpoint_hash = HashFile(checkpoint->artifact_filename);
        const auto sidecar_hash = HashFile(
            std::filesystem::path(checkpoint->artifact_filename + ".dtm"));
        if (!dtm_hash || *dtm_hash != request.effective_dtm_sha256
            || !checkpoint_hash
            || *checkpoint_hash != checkpoint->artifact_sha256
            || !sidecar_hash || *sidecar_hash != request.effective_dtm_sha256)
            return fail("persisted root files or same-name DTM sidecar drifted");
        return true;
    }

    std::optional<std::int64_t> ResolveLastKnownGood(
        const TasMovieValidationRequestRecord& request,
        std::optional<std::uint64_t> last_verified) const {
        if (!last_verified) return std::nullopt;
        if (request.source_kind == TasMovieValidationSourceKind::Tree) {
            const auto tree = state_db_->GetTasMovieTree(request.source_ref_id);
            if (tree) return tree->checkpoint_savestate_id;
        }
        if (request.source_kind == TasMovieValidationSourceKind::RootEstablishment && request.rtc_value) {
            const auto root = state_db_->FindTasMovieRootBySourceRtc(request.source_dtm_artifact_id, *request.rtc_value);
            if (root) return root->checkpoint_savestate_id;
        }
        return std::nullopt;
    }

    std::optional<std::int64_t> PublishRoot(
        const TasMovieValidationRequestRecord& request,
        const savor::runtime::program::ArtifactReferenceValue& capture,
        std::int64_t source_job_id,
        std::string* error_out) const {
        const std::filesystem::path sav_path(capture.storage_reference);
        const auto sav_hash = HashFile(sav_path);
        if (!capture.complete || !sav_hash || *sav_hash != capture.content_hash.ToHex()) {
            if (error_out) *error_out = "root checkpoint artifact hash does not match worker evidence";
            return std::nullopt;
        }
        const std::filesystem::path sidecar(sav_path.string() + ".dtm");
        const auto dtm_hash = HashFile(sidecar);
        if (!dtm_hash || *dtm_hash != request.effective_dtm_sha256) {
            if (error_out) *error_out = "root checkpoint same-name DTM sidecar does not match the validated bytes";
            return std::nullopt;
        }
        std::error_code size_error;
        const auto sav_size = std::filesystem::file_size(sav_path, size_error);
        if (size_error || sav_size == 0) return std::nullopt;
        const auto dtm_size = std::filesystem::file_size(sidecar, size_error);
        if (size_error || dtm_size == 0) return std::nullopt;
        std::int64_t dtm_artifact_id = 0;
        if (!StoreWorkspaceArtifactFile(state_db_, sidecar, {.sha256 = *dtm_hash, .size_bytes = static_cast<std::int64_t>(dtm_size),
                .compression_kind = 0, .file_ext = ".dtm", .artifact_kind = "DTM",
                .created_at_utc = types::UtcNow(), .correlation_id = "tmv-request-" + std::to_string(request.validation_request_id),
                .causation_id = "execution-job-" + std::to_string(source_job_id)}, &dtm_artifact_id, error_out)) return std::nullopt;
        std::int64_t sav_artifact_id = 0;
        if (!StoreWorkspaceArtifactFile(state_db_, sav_path, {.sha256 = *sav_hash, .size_bytes = static_cast<std::int64_t>(sav_size),
                .compression_kind = 0, .file_ext = sav_path.extension().string(), .artifact_kind = "SAV",
                .created_at_utc = types::UtcNow(), .correlation_id = "tmv-request-" + std::to_string(request.validation_request_id),
                .causation_id = "execution-job-" + std::to_string(source_job_id)}, &sav_artifact_id, error_out)) return std::nullopt;
        std::int64_t savestate_id = 0;
        if (!state_db_->CreateSavestate({.artifact_id = sav_artifact_id,
                .playback_state = SavestatePlaybackState::MoviePaired,
                .dtm_artifact_id = dtm_artifact_id,
                .savestate_type = "TAS_MOVIE_ROOT_CHECKPOINT",
                .note = "Canonical RTC-specific TAS movie root checkpoint", .is_complete = true,
                .created_at_utc = types::UtcNow(), .correlation_id = "tmv-request-" + std::to_string(request.validation_request_id),
                .causation_id = "execution-job-" + std::to_string(source_job_id)}, &savestate_id, error_out)) return std::nullopt;
        std::int64_t root_id = 0;
        if (!state_db_->CreateTasMovieRoot({.source_dtm_artifact_id = request.source_dtm_artifact_id,
                .dtm_artifact_id = dtm_artifact_id, .rtc_value = *request.rtc_value,
                .itinerary_artifact_id = *request.itinerary_artifact_id,
                .required_final_breakpoint_pc = request.required_final_breakpoint_pc,
                .checkpoint_savestate_id = savestate_id, .source_context_kind = "tmv_validation_request",
                .source_context_id = request.validation_request_id, .created_at_utc = types::UtcNow(),
                .correlation_id = "tmv-request-" + std::to_string(request.validation_request_id),
                .causation_id = "execution-job-" + std::to_string(source_job_id)}, &root_id, error_out)) return std::nullopt;
        return root_id;
    }

    IStateDb* state_db_{};
    IAnalysisDb* analysis_db_{};
    std::filesystem::path root_;
    std::shared_ptr<const savor::runtime::tasmovie::ITasMovieValidationFullPhaseDefinitionV1> phase_;
};

class BattleRecordingValidationTransition final
    : public IWorkflowTransitionHandler {
public:
    BattleRecordingValidationTransition(
        IStateDb* state_db, IAnalysisDb* analysis_db)
        : state_db_(state_db), analysis_db_(analysis_db) {}

    WorkflowTransitionDecision EvaluateTransition(
        const WorkflowTransitionContext& context) const override {
        WorkflowTransitionDecision decision{};
        decision.should_advance = true;
        if (!state_db_ || !analysis_db_ ||
            context.output_ref_kind !=
                std::optional<std::string>("tmv_validation_attempt") ||
            !context.output_ref_id)
            return decision;

        const auto attempt = analysis_db_->GetTasMovieValidationAttempt(
            *context.output_ref_id);
        const auto request = attempt
            ? analysis_db_->GetTasMovieValidationRequest(
                attempt->validation_request_id)
            : std::nullopt;
        if (!attempt || !request) {
            decision.should_advance = false;
            decision.workflow_failure = true;
            decision.blocked_reason =
                "battle_recording_validation_evidence_missing";
            return decision;
        }
        if (request->source_kind != TasMovieValidationSourceKind::Tree)
            return decision;

        const auto tree = state_db_->GetTasMovieTree(request->source_ref_id);
        if (!tree || tree->source_context_kind !=
                "analysis_battle.battle_recording")
            return decision;
        const auto recording = analysis_db_->GetBattleRecording(
            tree->source_context_id);
        if (!recording || recording->status != "COMPLETED" ||
            recording->outcome != std::optional<std::string>("RECORDED") ||
            recording->tas_movie_tree_id != tree->tas_movie_tree_id ||
            recording->paired_checkpoint_savestate_id !=
                tree->checkpoint_savestate_id) {
            decision.should_advance = false;
            decision.workflow_failure = true;
            decision.blocked_reason = "battle_recording_tree_identity_drifted";
            return decision;
        }
        std::string error;
        if (!analysis_db_->BindBattleRecordingValidation({
                .battle_recording_id = recording->battle_recording_id,
                .tas_movie_tree_id = tree->tas_movie_tree_id,
                .validation_request_id = request->validation_request_id},
                &error)) {
            decision.should_advance = false;
            decision.workflow_failure = true;
            decision.blocked_reason = error.empty()
                ? std::optional<std::string>(
                    "battle_recording_validation_binding_failed")
                : std::optional<std::string>(std::move(error));
            return decision;
        }
        return decision;
    }

private:
    IStateDb* state_db_{};
    IAnalysisDb* analysis_db_{};
};

} // namespace

ProgramKindDescriptor BuildTasMovieValidationProgramDescriptor(
    IExecutionDb* execution_db,
    IStateDb* state_db,
    IAnalysisDb* analysis_db,
    TasMovieValidationProgramConfig config) {
    ProgramKindDescriptor descriptor{};
    descriptor.program_kind = static_cast<std::int32_t>(savor::PK_TasMovie);
    descriptor.program_name = savor::ProgramKindDisplayName(descriptor.program_kind);
    descriptor.result_staging_root = config.working_dir_root;
    descriptor.full_phase_identity = savor::runtime::tasmovie::TasMovieValidationFullPhaseDefinitionV1()->identity();
    descriptor.default_progress_library_ids =
        ObservationDefaults().progress_library_ids;
    descriptor.default_derived_state_block_ids =
        std::vector<std::string>{};
    descriptor.default_progress_runtime_trigger_pcs =
        ObservationDefaults().runtime_sample_trigger_pcs;
    descriptor.job_materializer = std::make_shared<Materializer>(execution_db, state_db, analysis_db, config);
    descriptor.workset_reconstruction = std::make_shared<Reconstruction>(state_db, analysis_db, config.working_dir_root);
    descriptor.result_handler = std::make_shared<ResultHandler>(state_db, analysis_db, config.working_dir_root);
    descriptor.workflow_transition =
        std::make_shared<BattleRecordingValidationTransition>(
            state_db, analysis_db);
    descriptor.supports_workflow_orchestration = true;
    return descriptor;
}

} // namespace savor::db::execution::programdb::tasmovievalidation
