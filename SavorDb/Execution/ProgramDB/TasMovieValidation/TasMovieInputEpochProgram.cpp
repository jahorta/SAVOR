#include "TasMovieInputEpochProgram.h"

#include "../WorksetDerivedStateBinding.h"
#include "../WorksetObservationBinding.h"

#include "../../IExecutionDb.h"
#include "../../../Analysis/IAnalysisDb.h"
#include "../../../Common/Types/UtcTimestamp.h"
#include "../../../State/IStateDb.h"
#include "../../../../SavorCore/Phases/Programs/TasMovieInputEpoch/TasMovieInputEpochModule.h"
#include "../../../../SavorCore/Phases/Programs/TasMovieValidation/TasMovieValidationModule.h"
#include "../../../../SavorCore/Runner/IPC/DurableWorkerTerminalEnvelope.h"
#include "../../../../SavorCore/Runner/Runtime/ProgramKind.h"
#include "../../../../SavorCore/Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.h"
#include "../../../../SavorCore/Runner/Runtime/Worksets/WorksetWireCodec.h"
#include "../../../../SavorCore/Tas/DtmFile.h"
#include "../../../../SavorCore/Utils/Hash.h"
#include "../../../../SavorCaptureFormat/CaptureFormat.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace savor::db::execution::programdb::tasmovieinputepoch {
namespace {

namespace inputepoch = savor::runtime::tasmovie::inputepoch;

constexpr std::string_view kAnnotationRefKind =
    "tmv_input_epoch_annotation_request";
constexpr std::string_view kRewriteRefKind = "tmv_input_epoch_rewrite_request";
constexpr std::string_view kCutsceneRefKind = "tmv_cutscene_request";
constexpr std::size_t kDeclaredTerminalBytes = 16ull * 1024ull * 1024ull;

const WorksetObservationDefaultsV1& ObservationDefaults() {
    static const WorksetObservationDefaultsV1 defaults{
        .progress_library_ids = {
            "soa.progress.runtime.vi/1",
            "soa.progress.soa.script_location/1",
        },
        .runtime_sample_trigger_pcs = {inputepoch::PadReadReturnedPc},
    };
    return defaults;
}

const WorksetObservationDefaultsV1& CutsceneObservationDefaults(
    bool enable_seed_call_progress)
{
    static const WorksetObservationDefaultsV1 normal = ObservationDefaults();
    static const WorksetObservationDefaultsV1 with_seed_calls{
        .progress_library_ids = {
            "soa.progress.runtime.vi/1",
            "soa.progress.soa.script_location/1",
            "soa.progress.soa.seed_calls/1",
        },
        .runtime_sample_trigger_pcs = {inputepoch::PadReadReturnedPc},
    };
    return enable_seed_call_progress ? with_seed_calls : normal;
}

std::string AnnotationCaptureProfile(
    const TasMovieInputEpochProgramConfig& config,
    std::optional<std::uint32_t> root_pc = std::nullopt)
{
    std::string profile = std::string(R"json({"schema":"savor.capture.profile/1","name":"tasmovie-padread-input-epochs","revision":2,"expected_module_sha256":")json")
        + config.capture_module_sha256
        + R"json(","limits":{"queue_bytes":16777216,"max_events":4096,"progress_events":16},"probes":[{"id":"tasmovie.padread.returned","group":"tasmovie.input_epochs","kind":"pc","address":)json"
        + std::to_string(0x801D6E7Cu)
        + R"json(,"subscriptions":["capture"],"samples":[{"name":"movie_input_count","type":"routed_sample","width":8,"descriptor_id":1397096450},{"name":"received_pad_status","type":"address_program","width":8,"program":[7,60,118,52,128,2,0]}]})json";
    if (root_pc) {
        profile += R"json(,{"id":"tasmovie.root.boundary","group":"tasmovie.root","kind":"pc","address":)json"
            + std::to_string(*root_pc)
            + R"json(,"subscriptions":["capture"],"samples":[{"name":"movie_input_count","type":"routed_sample","width":8,"descriptor_id":1397096450}]})json";
    }
    profile += "]}";
    return profile;
}

std::int64_t NowMs() { return types::UtcNow().time_since_epoch().count(); }

bool Fail(std::string message, std::string* error_out) {
    if (error_out) *error_out = std::move(message);
    return false;
}

bool IsLowerHexSha256(std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

std::filesystem::path WorkingRoot(const std::filesystem::path& root) {
    return root.empty()
        ? std::filesystem::temp_directory_path() / "savor-tas-movie-input-epochs"
        : root;
}

bool EnsureParent(const std::filesystem::path& path, std::string* error_out) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (!error) return true;
    return Fail("failed creating TAS input-epoch staging directory: "
        + error.message(), error_out);
}

std::optional<std::string> HashFile(const std::filesystem::path& path) {
    try { return hash::sha256_of_file(path.string()); }
    catch (...) { return std::nullopt; }
}

std::optional<std::vector<std::uint8_t>> ReadFile(
    const std::filesystem::path& path, std::string* error_out) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        Fail("failed opening TAS input-epoch artifact: " + path.string(), error_out);
        return std::nullopt;
    }
    stream.seekg(0, std::ios::end);
    const auto end = stream.tellg();
    if (end < 0) return std::nullopt;
    stream.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    if (!bytes.empty() && !stream.read(reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()))) {
        Fail("failed reading TAS input-epoch artifact", error_out);
        return std::nullopt;
    }
    return bytes;
}

bool WriteFile(const std::filesystem::path& path,
    std::span<const std::uint8_t> bytes, std::string* error_out) {
    if (!EnsureParent(path, error_out)) return false;
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream || (!bytes.empty() && !stream.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size())))) {
        return Fail("failed writing TAS input-epoch artifact", error_out);
    }
    return true;
}

bool ValidateCompleteBootDtm(const std::filesystem::path& path,
    savor::tas::DtmFile* dtm_out, std::vector<GCInputFrame>* polls_out,
    std::string* error_out) {
    savor::tas::DtmFile dtm;
    std::string reason;
    if (!dtm.load(path.string()) || !dtm.supports_gc_poll_editing(&reason))
        return Fail("input-epoch phase requires an aligned GC DTM: " + reason,
            error_out);
    const auto info = dtm.info();
    if (info.is_wii || info.starts_from_savestate || info.controllers != 1
        || info.input_count == 0 || info.input_count != dtm.gc_poll_count()) {
        return Fail("input-epoch phase requires a nonempty complete boot GC DTM using only controller port 0",
            error_out);
    }
    std::vector<GCInputFrame> polls;
    polls.reserve(dtm.gc_poll_count());
    for (std::size_t i = 0; i < dtm.gc_poll_count(); ++i) {
        savor::tas::DtmGCPoll poll{};
        if (!dtm.read_gc_poll(i, poll))
            return Fail("DTM poll stream became unreadable", error_out);
        polls.push_back(savor::tas::decode_gc_controller_state(poll));
    }
    if (dtm_out) *dtm_out = std::move(dtm);
    if (polls_out) *polls_out = std::move(polls);
    return true;
}

std::optional<std::filesystem::path> MaterializeArtifact(IStateDb* state_db,
    const ArtifactRecord& artifact, const std::filesystem::path& destination,
    std::string* error_out) {
    if (!state_db || !IsLowerHexSha256(artifact.sha256)
        || artifact.size_bytes <= 0 || !EnsureParent(destination, error_out))
        return std::nullopt;
    if (const auto hash = HashFile(destination); hash && *hash == artifact.sha256)
        return destination;
    if (!state_db->MaterializeArtifactToPath(
            artifact.artifact_id, destination.string(), error_out))
        return std::nullopt;
    const auto hash = HashFile(destination);
    if (!hash || *hash != artifact.sha256) {
        Fail("materialized TAS input-epoch artifact hash drifted", error_out);
        return std::nullopt;
    }
    return destination;
}

std::optional<std::int64_t> Binding(const ProgramJobMaterializationContext& context,
    std::string_view input_key, std::string_view data_kind,
    std::string_view ref_kind) {
    if (context.graph) {
        for (const auto& binding : context.graph->input_bindings) {
            if (binding.input_key == input_key && binding.data_kind == data_kind
                && binding.ref_kind == ref_kind && binding.ref_id > 0)
                return binding.ref_id;
        }
    }
    if (context.step.domain_ref_id > 0
        && context.step.domain_ref_kind == ref_kind)
        return context.step.domain_ref_id;
    return std::nullopt;
}

std::optional<std::int64_t> IntegerArgument(
    const ProgramJobMaterializationContext& context, std::string_view key) {
    if (!context.graph) return std::nullopt;
    for (const auto& argument : context.graph->arguments) {
        if (argument.argument_key == key && argument.value_type == "integer"
            && argument.integer_value) return argument.integer_value;
    }
    return std::nullopt;
}

std::optional<std::string> TextArgument(
    const ProgramJobMaterializationContext& context, std::string_view key) {
    if (!context.graph) return std::nullopt;
    for (const auto& argument : context.graph->arguments) {
        if (argument.argument_key == key &&
            (argument.value_type == "text" || argument.value_type == "choice") &&
            argument.text_value) return argument.text_value;
    }
    return std::nullopt;
}

bool IsExactButtonOnly(const GCInputFrame& input, std::uint16_t button) {
    return input.buttons == button && input.main_x == 128 && input.main_y == 128
        && input.c_x == 128 && input.c_y == 128
        && input.trig_l == 0 && input.trig_r == 0;
}

std::string JobInput(std::string_view prefix, std::int64_t request_id) {
    return std::string(prefix) + std::to_string(request_id);
}

std::string Fingerprint(std::int32_t kind, std::int64_t request_id,
    std::string_view source_sha, std::string_view phase_sha) {
    const std::string value = std::to_string(request_id) + "\n"
        + std::string(source_sha) + "\n" + std::string(phase_sha);
    return "PK=" + std::to_string(kind) + ";PV=1;input_epoch="
        + hash::sha256(value.data(), value.size());
}

ProgramResultDecision FinalDecision(std::string state,
    std::optional<std::string> code = std::nullopt,
    std::optional<std::string> text = std::nullopt) {
    ProgramResultDecision result{};
    result.final_job_state = std::move(state);
    result.error_code = std::move(code);
    result.error_text = std::move(text);
    return result;
}

std::string FailureCode(inputepoch::InputEpochFailureReasonV1 reason) {
    using Reason = inputepoch::InputEpochFailureReasonV1;
    switch (reason) {
    case Reason::CursorZero: return "TAS_INPUT_EPOCH_CURSOR_ZERO";
    case Reason::CursorRegressed: return "TAS_INPUT_EPOCH_CURSOR_REGRESSED";
    case Reason::CursorPastSource: return "TAS_INPUT_EPOCH_CURSOR_PAST_SOURCE";
    case Reason::UnexpectedMovieEnd: return "TAS_INPUT_EPOCH_UNEXPECTED_MOVIE_END";
    case Reason::PrefixDiverged: return "TAS_INPUT_EPOCH_PREFIX_DIVERGED";
    case Reason::None: break;
    }
    return "TAS_INPUT_EPOCH_UNKNOWN_DIVERGENCE";
}

bool PublishSingleton(IExecutionDb* execution_db,
    const ProgramJobMaterializationContext& context,
    const savor::runtime::fullphase::IFullPhaseProgramDefinition& phase,
    const WorksetObservationDefaultsV1& observation_defaults,
    std::int32_t program_kind, std::string_view purpose,
    std::string_view created_by, std::string_view ref_kind,
    std::int64_t ref_id, std::string fingerprint, std::string input,
    const std::filesystem::path& capture_root,
    std::optional<std::string> capture_profile,
    WorkflowStepScheduleResult* result_out, std::string* error_out,
    std::optional<std::int64_t> savestate_id = std::nullopt) {
    const auto package = savor::runtime::fullphase::BuildFullPhaseProgramPackage(phase);
    ResolvedWorksetDerivedStateBindingV1 derived_state;
    if (!ResolveWorksetDerivedStateBindingV1(std::span<const std::string>{},
            package, &derived_state, error_out)) return false;
    ResolvedWorksetObservationBindingV1 observation;
    ProgramJobMaterializationContext observation_context = context;
    if (capture_profile && observation_context.graph) {
        observation_context.graph->arguments.push_back({
            .node_key = observation_context.graph->activation_graph_node_key,
            .argument_key = std::string(kCaptureProfileJsonArgument),
            .value_type = "text",
            .text_value = std::move(*capture_profile),
            .source_kind = "program_default",
        });
    }
    if (!ResolveWorksetObservationBindingV1(observation_context, observation_defaults,
            capture_root, &observation, error_out)) return false;
    EnsureMaterializingJobSetReceipt ensured{};
    const std::string materialization_key = std::string(created_by) + ".step."
        + std::to_string(context.step.workflow_step_id);
    if (!execution_db->EnsureMaterializingJobSet({
            .materialization_key = materialization_key,
            .program_kind = program_kind,
            .purpose = std::string(purpose),
            .created_by = std::string(created_by),
            .created_at_utc = NowMs(),
            .priority_boost = context.step.step_priority,
            .expected_total = 1,
            .domain_ref_kind = std::string(ref_kind),
            .domain_ref_id = ref_id,
            .meta_note = context.step.step_kind,
        }, &ensured, error_out)) return false;
    if (ensured.materialization_state == "MATERIALIZING") {
        CreatePendingJobReceipt created{};
        if (!execution_db->CreatePendingJob({
                .job_set_id = ensured.job_set_id,
                .program_kind = program_kind,
                .program_version = 1,
                .program_ref_kind = std::string(ref_kind),
                .program_ref_id = ref_id,
                .savestate_id = savestate_id,
                .fingerprint = fingerprint,
                .priority = context.step.step_priority,
                .max_attempts = 1,
                .input_ini = input,
            }, &created, error_out)) return false;
    }
    const auto jobs = execution_db->ListJobsInJobSet(ensured.job_set_id);
    if (jobs.size() != 1 || jobs.front().input_ini != input)
        return Fail("input-epoch job set lost its immutable singleton shape", error_out);
    SealJobPopulationReceipt sealed{};
    if (!execution_db->SealJobPopulation({
            .job_set_id = ensured.job_set_id,
            .expected_job_count = 1,
            .requested_by = std::string(created_by),
        }, &sealed, error_out)) return false;
    const auto& runtime = phase.runtime_contract();
    PublishWorksetCommand workset{
        .job_set_id = ensured.job_set_id,
        .workflow_step_id = context.step.workflow_step_id,
        .workset_key = materialization_key + ".workset.0",
        .program_kind = program_kind,
        .program_version = 1,
        .contract = {
            .contract_key = std::string(created_by) + ":v1:request:"
                + std::to_string(ref_id) + ":phase:" + phase.identity().canonical_sha256,
            .module_canonical_id = runtime.module.canonical_id,
            .module_version = static_cast<std::int32_t>(runtime.module.revision),
            .module_sha256 = runtime.module.canonical_hash,
            .entrypoint = runtime.entrypoint,
            .verified_dependency_sha256 = runtime.verified_dependency_sha256,
            .runtime_profile_sha256 = runtime.runtime_profile_sha256,
            .program_package_sha256 = package.canonical_sha256,
            .estimated_payload_bytes = kDeclaredTerminalBytes,
        },
        .derived_state = {
            .binding_payload = derived_state.encoded_binding,
            .binding_sha256 = derived_state.binding_sha256,
        },
        .observation = {
            .capture_binding_payload = observation.encoded_capture_binding,
            .capture_binding_sha256 = observation.capture_binding_sha256,
            .progress_plan_payload = observation.encoded_progress_plan,
            .progress_plan_sha256 = observation.progress_plan_sha256,
        },
        .priority = context.step.step_priority,
        .ordered_job_ids = {jobs.front().job_id},
        .requested_by = std::string(created_by),
    };
    PublishWorksetWaveReceipt published{};
    if (!execution_db->PublishWorksetWave({
            .job_set_id = ensured.job_set_id,
            .expected_job_count = 1,
            .worksets = {std::move(workset)},
            .requested_by = std::string(created_by),
        }, &published, error_out)) return false;
    result_out->job_set_id = ensured.job_set_id;
    result_out->persistence = {
        .program_ref_kind = std::string(ref_kind),
        .program_ref_id = ref_id,
        .fingerprint = std::move(fingerprint),
        .program_version = 1,
    };
    return true;
}

savor::runtime::WorkerWorksetDefinition MakeWorkset(
    const WorksetReconstructionContext& context,
    const savor::runtime::fullphase::IFullPhaseProgramDefinition& phase,
    const std::filesystem::path& dtm_path, std::string_view dtm_sha,
    std::span<const std::uint8_t> input, std::string_view producer) {
    const auto& item = context.items.front();
    savor::runtime::WorkerWorksetDefinition workset{};
    workset.workset_id = savor::runtime::WorkerWorksetId(
        static_cast<std::uint64_t>(context.dispatch_attempt_id));
    workset.phase_invocation = {
        .invocation_id = {
            .workflow_step_id = static_cast<std::uint64_t>(context.workflow_step_id),
            .job_set_id = static_cast<std::uint64_t>(context.job_set_id),
        },
        .program_package = savor::runtime::fullphase::BuildFullPhaseProgramPackage(phase),
        .common_input = savor::runtime::fullphase::MakeFullPhaseCommonInput(
            "soa.tas_movie_input_epochs.CommonInput", 1),
    };
    workset.baseline = {
        .artifact = {
            .kind = savor::runtime::ProgramBaselineArtifactKind::ReadOnlyMovie,
            .movie_path = dtm_path,
            .movie_sha256 = std::string(dtm_sha),
            .compatibility = context.state_compatibility,
            .lineage = {
                .edge = phase.runtime_contract().baseline_lineage,
                .producer = std::string(producer),
            },
        },
        .lineage = phase.runtime_contract().baseline_lineage,
    };
    workset.derived_state = context.derived_state;
    workset.capture = context.capture;
    workset.progress_plan = context.progress_plan;
    const auto& runtime = phase.runtime_contract();
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
        .derived_state_binding_sha256 = workset.derived_state.content_sha256,
        .capture_binding_sha256 = workset.capture
            ? workset.capture->content_sha256
            : savor::runtime::EmptyWorksetCaptureBindingHashV1(),
        .progress_plan_sha256 = workset.progress_plan.content_sha256,
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
            .input_payload = {input.begin(), input.end()},
        },
        .declared_terminal_bytes = kDeclaredTerminalBytes,
        .correlation = {
            .durable_job_id = std::to_string(item.job_id),
            .claim_token = item.claim_token,
            .parent_correlation = context.contract_key,
        },
    });
    return workset;
}

savor::runtime::WorkerWorksetDefinition MakeSavestateWorkset(
    const WorksetReconstructionContext& context,
    const savor::runtime::fullphase::IFullPhaseProgramDefinition& phase,
    const std::filesystem::path& state_path, std::string_view state_sha,
    const std::filesystem::path& dtm_path, std::string_view dtm_sha,
    std::span<const std::uint8_t> input, std::string_view producer) {
    auto workset = MakeWorkset(context, phase, dtm_path, dtm_sha, input, producer);
    workset.baseline.artifact.kind =
        savor::runtime::ProgramBaselineArtifactKind::Savestate;
    workset.baseline.artifact.state_path = state_path;
    workset.baseline.artifact.state_sha256 = std::string(state_sha);
    workset.execution_key.baseline =
        savor::runtime::ComputeProgramBaselineKey(workset.baseline);
    workset.execution_key.canonical_sha256 =
        savor::runtime::ComputeWorkerWorksetExecutionKeyHash(workset.execution_key);
    return workset;
}

bool ValidateTerminal(const ProgramResultProcessingContext& context,
    savor::runtime::DurableWorkerTerminalEnvelope* terminal,
    savor::runtime::program::ProgramResult* decoded,
    ProgramResultDecision* failure) {
    std::string error;
    if (!savor::runtime::DecodeDurableWorkerTerminalEnvelope(
            context.terminal.envelope, terminal, &error)) {
        *failure = FinalDecision("FAILED", "TAS_INPUT_EPOCH_TERMINAL_INVALID", error);
        return false;
    }
    using Status = savor::wrms::InvocationTerminalStatus;
    if (terminal->terminal.status == Status::Cancelled) {
        *failure = FinalDecision("FAILED",
            "UNCLASSIFIED_CANCELLED_TERMINAL_REACHED_DESCRIPTOR",
            "cancelled worker terminal bypassed ProgramResultProcessor");
        return false;
    }
    if (terminal->terminal.status != Status::Succeeded || terminal->terminal.unstarted
        || terminal->terminal.workset_id
            != static_cast<std::uint64_t>(context.terminal.dispatch_attempt_id)
        || terminal->terminal.item_id != static_cast<std::uint64_t>(context.job_id)
        || terminal->terminal.invocation_id != static_cast<std::uint64_t>(context.job_id)
        || terminal->terminal.attempt_id != context.terminal.reserved_attempt_id
        || terminal->process_generation == 0 || terminal->terminal.workset_epoch == 0
        || terminal->terminal.session_disposition
            != savor::wrms::SessionDispositionCode::Clean) {
        *failure = FinalDecision("FAILED",
            terminal->terminal.error_code.empty()
                ? "TAS_INPUT_EPOCH_EXECUTION_FAILED" : terminal->terminal.error_code,
            terminal->terminal.message.empty()
                ? "worker invocation did not complete cleanly" : terminal->terminal.message);
        return false;
    }
    const auto result = savor::runtime::program::DecodeProgramResultV1(
        terminal->terminal.result);
    if (!result || result.value->invocation_id.value()
            != static_cast<std::uint64_t>(context.job_id)
        || result.value->attempt_id.value() != context.terminal.reserved_attempt_id) {
        *failure = FinalDecision("FAILED", "TAS_INPUT_EPOCH_PROGRAM_RESULT_MISMATCH",
            result ? "ProgramResult identity drifted" : result.status.message);
        return false;
    }
    *decoded = std::move(*result.value);
    return true;
}

bool DecodePassiveAnnotationCapture(
    std::span<const savor::wrms::WorksetArtifactPayload> artifacts,
    std::string_view source_dtm_sha256,
    std::uint64_t source_poll_count,
    inputepoch::TasMovieInputEpochScheduleV1* schedule,
    std::string* error_out)
{
    if (!schedule)
        return Fail("capture schedule output is required", error_out);
    const auto found = std::ranges::find_if(artifacts,
        [](const savor::wrms::WorksetArtifactPayload& artifact) {
            return artifact.schema_id ==
                "savor.capture.profile.artifact";
        });
    if (found == artifacts.end() || !found->complete)
        return Fail("worker terminal is missing the complete capture artifact", error_out);
    std::vector<savor::capture_format::Event> events;
    savor::capture_format::VerificationReport report;
    std::string read_error;
    if (!savor::capture_format::Reader::read_all(
            found->storage_reference, events, &report, &read_error))
        return Fail("capture artifact could not be read: " + read_error, error_out);
    if (!report.ok)
        return Fail("capture artifact verification failed", error_out);

    *schedule = {.source_dtm_sha256 = std::string(source_dtm_sha256),
        .source_poll_count = source_poll_count};
    std::uint64_t previous_cursor = 0;
    for (const auto& event : events) {
        if (event.kind == savor::capture_format::EventKind::Gap)
            return Fail("capture artifact contains a loss marker", error_out);
        if (event.kind != savor::capture_format::EventKind::Pc
            || event.probe_id != "tasmovie.padread.returned")
            continue;
        const auto cursor = std::ranges::find(event.fields,
            std::string("movie_input_count"), &savor::capture_format::Field::name);
        const auto pad = std::ranges::find(event.fields,
            std::string("received_pad_status"), &savor::capture_format::Field::name);
        if (cursor == event.fields.end() || pad == event.fields.end()
            || cursor->status != savor::capture_format::FieldStatus::Present
            || pad->status != savor::capture_format::FieldStatus::Present)
            return Fail("capture event is missing a complete movie_input_count or received_pad_status field", error_out);
        if (cursor->value == 0 || cursor->value < previous_cursor
            || cursor->value > source_poll_count)
            return Fail("capture movie_input_count is zero, regressed, or exceeds source_poll_count", error_out);
        schedule->epochs.push_back({cursor->value,
            inputepoch::DecodeGuestPadStatusV1(pad->value)});
        previous_cursor = cursor->value;
    }
    if (schedule->epochs.empty())
        return Fail("capture artifact contains no PadReadReturned observations", error_out);
    return inputepoch::ValidateInputEpochScheduleV1(*schedule, error_out);
}

bool DecodeRootEstablishmentCapture(
    std::span<const savor::wrms::WorksetArtifactPayload> artifacts,
    std::uint32_t expected_pc,
    std::uint64_t child_poll_count,
    std::uint64_t* cursor_out,
    std::uint64_t* vi_count_out,
    std::string* error_out)
{
    if (!cursor_out || !vi_count_out || expected_pc == 0)
        return Fail("root capture output and expected PC are required", error_out);
    const auto found = std::ranges::find_if(artifacts,
        [](const savor::wrms::WorksetArtifactPayload& artifact) {
            return artifact.schema_id == "savor.capture.profile.artifact";
        });
    if (found == artifacts.end() || !found->complete)
        return Fail("worker terminal is missing the complete capture artifact", error_out);
    std::vector<savor::capture_format::Event> events;
    savor::capture_format::VerificationReport report;
    std::string read_error;
    if (!savor::capture_format::Reader::read_all(
            found->storage_reference, events, &report, &read_error)
        || !report.ok)
        return Fail("root capture artifact could not be verified: " + read_error, error_out);
    std::optional<std::uint64_t> cursor;
    std::optional<std::uint64_t> vi_count;
    for (const auto& event : events) {
        if (event.kind == savor::capture_format::EventKind::Gap)
            return Fail("root capture artifact contains a loss marker", error_out);
        if (event.kind != savor::capture_format::EventKind::Pc
            || event.probe_id != "tasmovie.root.boundary")
            continue;
        if (event.pc != expected_pc || cursor)
            return Fail("root capture is missing a unique inherited root observation", error_out);
        const auto field = std::ranges::find(event.fields,
            std::string("movie_input_count"), &savor::capture_format::Field::name);
        if (field == event.fields.end()
            || field->status != savor::capture_format::FieldStatus::Present
            || field->value > child_poll_count)
            return Fail("root capture has an invalid movie_input_count", error_out);
        cursor = field->value;
        vi_count = event.frame_index;
    }
    if (!cursor)
        return Fail("root capture contains no inherited root observation", error_out);
    *cursor_out = *cursor;
    *vi_count_out = *vi_count;
    return true;
}

class AnnotationMaterializer final : public IProgramJobMaterializer {
public:
    AnnotationMaterializer(IExecutionDb* execution, IStateDb* state,
        IAnalysisDb* analysis, TasMovieInputEpochProgramConfig config,
        bool breakpoint_diagnostic = false)
        : execution_(execution), state_(state), analysis_(analysis),
          config_(std::move(config)),
          phase_(breakpoint_diagnostic
              ? inputepoch::BreakpointDiagnosticFullPhaseDefinitionV1()
              : inputepoch::AnnotationFullPhaseDefinitionV1()) {}

    bool Materialize(const ProgramJobMaterializationContext& context,
        WorkflowStepScheduleResult* result_out, std::string* error_out) const override {
        if (!execution_ || !state_ || !analysis_ || !phase_ || !context.graph
            || !result_out)
            return Fail("input-epoch annotation dependencies are incomplete", error_out);
        *result_out = {};
        const auto root_id = Binding(context, "root_establishment",
            "analysis.tas_movie_root_establishment_attempt_id",
            "tmv_root_establishment_attempt");
        const auto root = root_id
            ? analysis_->GetTasMovieRootEstablishmentAttempt(*root_id)
            : std::nullopt;
        const auto artifact = root
            ? state_->GetArtifact(root->source_dtm_artifact_id)
            : std::nullopt;
        if (!root || !artifact || artifact->artifact_kind != "DTM"
            || artifact->sha256 != root->source_dtm_sha256)
            return Fail("input-epoch annotation requires one internally consistent root-establishment authority", error_out);
        const auto& identity = phase_->identity();
        const auto& runtime = phase_->runtime_contract();
        CreateTasMovieInputEpochAnnotationRequestCommand command{
            .materialization_key = "tasmovie.input_epoch.annotation.step."
                + std::to_string(context.step.workflow_step_id),
            .workflow_instance_id = context.step.workflow_instance_id,
            .workflow_step_id = context.step.workflow_step_id,
            .root_establishment_attempt_id = root->root_establishment_attempt_id,
            .source_dtm_artifact_id = artifact->artifact_id,
            .source_dtm_sha256 = artifact->sha256,
            .full_phase_program_kind = identity.program_kind,
            .full_phase_program_version = identity.program_version,
            .full_phase_canonical_id = identity.canonical_id,
            .full_phase_contract_revision = identity.contract_revision,
            .full_phase_sha256 = identity.canonical_sha256,
            .module_canonical_id = runtime.module.canonical_id,
            .module_revision = runtime.module.revision,
            .module_sha256 = runtime.module.canonical_hash,
            .created_at_utc = types::UtcNow(),
        };
        const bool passive_annotation =
            phase_->identity().program_kind == static_cast<std::int32_t>(
                savor::PK_TasMovieAnnotate);
        if (passive_annotation &&
            !IsLowerHexSha256(config_.capture_module_sha256)) {
            return Fail(
                "input-epoch annotation capture module SHA-256 is unavailable",
                error_out);
        }
        std::int64_t request_id = 0;
        if (!analysis_->CreateTasMovieInputEpochAnnotationRequest(
                command, &request_id, error_out)) return false;
        const auto fingerprint = Fingerprint(identity.program_kind, request_id,
            artifact->sha256, identity.canonical_sha256);
        if (!PublishSingleton(execution_, context, *phase_, ObservationDefaults(),
                identity.program_kind, "TAS_MOVIE_INPUT_EPOCH_ANNOTATION",
                "tas_movie_input_epoch_annotation",
                kAnnotationRefKind, request_id, fingerprint,
                JobInput("TMEA1:", request_id),
                WorkingRoot(config_.working_dir_root) / "captures",
                passive_annotation
                    ? std::optional<std::string>(AnnotationCaptureProfile(config_))
                    : std::nullopt,
                result_out, error_out)) return false;
        result_out->event_lines.push_back("[tasmovie-input-epoch-annotation-materialized] request="
            + std::to_string(request_id));
        return true;
    }

    bool Continue(const ProgramJobContinuationContext& context,
        ProgramJobContinuationResult* result_out, std::string* error_out) const override {
        if (!result_out || context.job_set_id <= 0)
            return Fail("input-epoch annotation continuation is invalid", error_out);
        const auto jobs = execution_->ListJobsInJobSet(context.job_set_id);
        if (jobs.size() != 1)
            return Fail("input-epoch annotation lost singleton shape", error_out);
        const auto job = execution_->GetExecutionJob(jobs.front().job_id);
        const auto attempt = job && job->worker_terminal_fingerprint
            ? analysis_->FindTasMovieInputEpochAnnotationAttempt(
                job->job_id, *job->worker_terminal_fingerprint)
            : std::nullopt;
        if (!attempt || !attempt->succeeded)
            return Fail("successful input-epoch annotation attempt is unavailable", error_out);
        result_out->disposition = ProgramJobContinuationDisposition::Complete;
        result_out->output = ProgramJobContinuationOutput{
            .output_key = "annotation_attempt",
            .data_kind = "analysis.tas_movie_input_epoch_annotation_attempt_id",
            .ref_kind = "tmv_input_epoch_annotation_attempt",
            .ref_id = attempt->annotation_attempt_id,
        };
        return true;
    }

private:
    IExecutionDb* execution_{};
    IStateDb* state_{};
    IAnalysisDb* analysis_{};
    TasMovieInputEpochProgramConfig config_;
    std::shared_ptr<const inputepoch::IAnnotationFullPhaseDefinitionV1> phase_;
};

class RewriteMaterializer final : public IProgramJobMaterializer {
public:
    RewriteMaterializer(IExecutionDb* execution, IStateDb* state,
        IAnalysisDb* analysis, TasMovieInputEpochProgramConfig config)
        : execution_(execution), state_(state), analysis_(analysis),
          config_(std::move(config)), phase_(inputepoch::RewriteFullPhaseDefinitionV1()) {}

    bool Materialize(const ProgramJobMaterializationContext& context,
        WorkflowStepScheduleResult* result_out, std::string* error_out) const override {
        if (!execution_ || !state_ || !analysis_ || !phase_ || !context.graph
            || !result_out)
            return Fail("input-epoch rewrite dependencies are incomplete", error_out);
        *result_out = {};
        if (!IsLowerHexSha256(config_.capture_module_sha256)) {
            return Fail(
                "input-epoch rewrite capture module SHA-256 is unavailable",
                error_out);
        }
        const auto attempt_id = Binding(context, "annotation_attempt",
            "analysis.tas_movie_input_epoch_annotation_attempt_id",
            "tmv_input_epoch_annotation_attempt");
        auto insertion = IntegerArgument(context, "insert_before_epoch");
        const auto neutral_count = IntegerArgument(context, "neutral_epoch_count");
        const auto placement_profile = TextArgument(context, "placement_profile");
        const auto attempt = attempt_id
            ? analysis_->GetTasMovieInputEpochAnnotationAttempt(*attempt_id)
            : std::nullopt;
        const auto root_establishment = attempt && attempt->root_establishment_attempt_id
            ? analysis_->GetTasMovieRootEstablishmentAttempt(
                *attempt->root_establishment_attempt_id)
            : std::nullopt;
        if (!attempt || !attempt->succeeded || !attempt->schedule_artifact_id
            || !attempt->schedule_sha256 || !root_establishment || !neutral_count
            || *neutral_count <= 0
            || attempt->source_dtm_artifact_id != root_establishment->source_dtm_artifact_id
            || attempt->source_dtm_sha256 != root_establishment->source_dtm_sha256) {
            return Fail("input-epoch rewrite requires matching successful annotation and root-establishment authorities",
                error_out);
        }
        if (!insertion) {
            if (!placement_profile || *placement_profile != "first_battle.final_dialog")
                return Fail("input-epoch rewrite requires an explicit epoch or a supported placement profile", error_out);
            const auto artifact = state_->GetArtifact(*attempt->schedule_artifact_id);
            const auto bytes = artifact ? ReadFile(artifact->filename, error_out) : std::nullopt;
            inputepoch::TasMovieInputEpochScheduleV1 schedule;
            std::string diagnostic;
            if (!bytes || !inputepoch::DecodeInputEpochScheduleArtifactV1(
                    *bytes, schedule, &diagnostic))
                return Fail("placement profile could not read the annotation schedule: " + diagnostic, error_out);
            for (std::size_t index = 0; index + 1 < schedule.epochs.size(); ++index) {
                if (IsExactButtonOnly(schedule.epochs[index].input, GC_B) &&
                    IsExactButtonOnly(schedule.epochs[index + 1].input, GC_A))
                    insertion = static_cast<std::int64_t>(index);
            }
            if (!insertion)
                return Fail("first_battle.final_dialog found no exact final B-only to A-only transition", error_out);
        }
        if (*insertion < 0 || static_cast<std::uint64_t>(*insertion) >= attempt->epoch_count)
            return Fail("input-epoch rewrite insertion epoch is outside the annotation schedule", error_out);
        const auto& identity = phase_->identity();
        const auto& runtime = phase_->runtime_contract();
        CreateTasMovieInputEpochRewriteRequestCommand command{
            .materialization_key = "tasmovie.input_epoch.rewrite.step."
                + std::to_string(context.step.workflow_step_id),
            .workflow_instance_id = context.step.workflow_instance_id,
            .workflow_step_id = context.step.workflow_step_id,
            .annotation_attempt_id = attempt->annotation_attempt_id,
            .root_establishment_attempt_id = root_establishment->root_establishment_attempt_id,
            .source_dtm_artifact_id = attempt->source_dtm_artifact_id,
            .source_dtm_sha256 = attempt->source_dtm_sha256,
            .schedule_artifact_id = *attempt->schedule_artifact_id,
            .schedule_sha256 = *attempt->schedule_sha256,
            .insert_before_epoch = static_cast<std::uint64_t>(*insertion),
            .neutral_epoch_count = static_cast<std::uint64_t>(*neutral_count),
            .placement_profile = placement_profile.value_or("explicit"),
            .full_phase_program_kind = identity.program_kind,
            .full_phase_program_version = identity.program_version,
            .full_phase_canonical_id = identity.canonical_id,
            .full_phase_contract_revision = identity.contract_revision,
            .full_phase_sha256 = identity.canonical_sha256,
            .module_canonical_id = runtime.module.canonical_id,
            .module_revision = runtime.module.revision,
            .module_sha256 = runtime.module.canonical_hash,
            .created_at_utc = types::UtcNow(),
        };
        std::int64_t request_id = 0;
        if (!analysis_->CreateTasMovieInputEpochRewriteRequest(
                command, &request_id, error_out)) return false;
        const auto fingerprint = Fingerprint(identity.program_kind, request_id,
            command.source_dtm_sha256, identity.canonical_sha256);
        if (!PublishSingleton(execution_, context, *phase_, ObservationDefaults(),
                identity.program_kind, "TAS_MOVIE_INPUT_EPOCH_REWRITE",
                "tas_movie_input_epoch_rewrite",
                kRewriteRefKind, request_id, fingerprint,
                JobInput("TMER1:", request_id),
                WorkingRoot(config_.working_dir_root) / "captures",
                AnnotationCaptureProfile(config_, root_establishment->root_pc),
                result_out, error_out)) return false;
        result_out->event_lines.push_back("[tasmovie-input-epoch-rewrite-materialized] request="
            + std::to_string(request_id));
        return true;
    }

    bool Continue(const ProgramJobContinuationContext& context,
        ProgramJobContinuationResult* result_out, std::string* error_out) const override {
        if (!result_out || context.job_set_id <= 0)
            return Fail("input-epoch rewrite continuation is invalid", error_out);
        const auto jobs = execution_->ListJobsInJobSet(context.job_set_id);
        if (jobs.size() != 1)
            return Fail("input-epoch rewrite lost singleton shape", error_out);
        const auto job = execution_->GetExecutionJob(jobs.front().job_id);
        const auto attempt = job && job->worker_terminal_fingerprint
            ? analysis_->FindTasMovieInputEpochRewriteAttempt(
                job->job_id, *job->worker_terminal_fingerprint)
            : std::nullopt;
        if (!attempt || !attempt->succeeded)
            return Fail("successful input-epoch rewrite attempt is unavailable", error_out);
        result_out->disposition = ProgramJobContinuationDisposition::Complete;
        result_out->output = ProgramJobContinuationOutput{
            .output_key = "rewrite_attempt",
            .data_kind = "analysis.tas_movie_input_epoch_rewrite_attempt_id",
            .ref_kind = "tmv_input_epoch_rewrite_attempt",
            .ref_id = attempt->rewrite_attempt_id,
        };
        return true;
    }

private:
    IExecutionDb* execution_{};
    IStateDb* state_{};
    IAnalysisDb* analysis_{};
    TasMovieInputEpochProgramConfig config_;
    std::shared_ptr<const inputepoch::IRewriteFullPhaseDefinitionV1> phase_;
};

class AnnotationReconstruction final : public IWorksetReconstructionAdapter {
public:
    AnnotationReconstruction(IStateDb* state, IAnalysisDb* analysis,
        std::filesystem::path root, bool breakpoint_diagnostic = false)
        : state_(state), analysis_(analysis), root_(WorkingRoot(root)),
          phase_(breakpoint_diagnostic
              ? inputepoch::BreakpointDiagnosticFullPhaseDefinitionV1()
              : inputepoch::AnnotationFullPhaseDefinitionV1()) {}

    std::optional<WorksetReconstructionResult> Reconstruct(
        const WorksetReconstructionContext& context,
        std::string* error_out) const override {
        const auto fail = [&](std::string text) -> std::optional<WorksetReconstructionResult> {
            Fail(std::move(text), error_out); return std::nullopt;
        };
        if (!state_ || !analysis_ || !phase_ || context.items.size() != 1
            || context.workset_id <= 0 || context.dispatch_attempt_id <= 0
            || context.workflow_step_id <= 0 || context.job_set_id <= 0
            || context.dispatch_token.empty() || !context.state_compatibility.Complete())
            return fail("input-epoch annotation reconstruction requires one exact item");
        const auto& item = context.items.front();
        if (item.program_kind != phase_->identity().program_kind
            || item.program_version != 1 || item.program_ref_kind != kAnnotationRefKind
            || item.program_ref_id <= 0 || item.savestate_id
            || item.input_ini != JobInput("TMEA1:", item.program_ref_id)
            || item.reserved_attempt_id == 0 || item.claim_token.empty())
            return fail("input-epoch annotation item identity drifted");
        const auto request = analysis_->GetTasMovieInputEpochAnnotationRequest(item.program_ref_id);
        const auto artifact = request ? state_->GetArtifact(request->source_dtm_artifact_id)
            : std::nullopt;
        if (!request || request->workflow_step_id != context.workflow_step_id
            || request->full_phase_sha256 != phase_->identity().canonical_sha256
            || request->module_sha256 != phase_->runtime_contract().module.canonical_hash
            || !artifact || artifact->artifact_kind != "DTM"
            || artifact->sha256 != request->source_dtm_sha256)
            return fail("input-epoch annotation immutable identity drifted");
        const auto path = root_ / ("annotation-request-"
            + std::to_string(request->annotation_request_id)) / "source.dtm";
        if (!MaterializeArtifact(state_, *artifact, path, error_out)) return std::nullopt;
        savor::tas::DtmFile dtm;
        std::vector<GCInputFrame> polls;
        if (!ValidateCompleteBootDtm(path, &dtm, &polls, error_out)) return std::nullopt;
        inputepoch::TasMovieInputEpochAnnotationRequestV1 native{
            .source_dtm_path = path.string(),
            .source_dtm_sha256 = artifact->sha256,
            .source_poll_count = polls.size(),
        };
        std::string diagnostic;
        const auto input = inputepoch::EncodeAnnotationExecutionInputV1(native, &diagnostic);
        if (input.empty()) return fail("annotation invocation binding failed: " + diagnostic);
        auto workset = MakeWorkset(context, *phase_, path, artifact->sha256, input,
            "SavorDb.PK_TasMovieAnnotate");
        std::vector<std::uint8_t> encoded;
        const auto status = savor::runtime::EncodeWorkerWorksetV5(workset, encoded);
        if (!status) return fail("annotation workset encoding failed: " + status.message);
        workset.encoded_size_bytes = encoded.size();
        return WorksetReconstructionResult{
            .workset = std::move(workset), .ordered_job_ids = {item.job_id}};
    }

private:
    IStateDb* state_{};
    IAnalysisDb* analysis_{};
    std::filesystem::path root_;
    std::shared_ptr<const inputepoch::IAnnotationFullPhaseDefinitionV1> phase_;
};

class RewriteReconstruction final : public IWorksetReconstructionAdapter {
public:
    RewriteReconstruction(IStateDb* state, IAnalysisDb* analysis,
        std::filesystem::path root)
        : state_(state), analysis_(analysis), root_(WorkingRoot(root)),
          phase_(inputepoch::RewriteFullPhaseDefinitionV1()) {}

    std::optional<WorksetReconstructionResult> Reconstruct(
        const WorksetReconstructionContext& context,
        std::string* error_out) const override {
        const auto fail = [&](std::string text) -> std::optional<WorksetReconstructionResult> {
            Fail(std::move(text), error_out); return std::nullopt;
        };
        if (!state_ || !analysis_ || !phase_ || context.items.size() != 1
            || context.workset_id <= 0 || context.dispatch_attempt_id <= 0
            || context.workflow_step_id <= 0 || context.job_set_id <= 0
            || context.dispatch_token.empty() || !context.state_compatibility.Complete())
            return fail("input-epoch rewrite reconstruction requires one exact item");
        const auto& item = context.items.front();
        if (item.program_kind != static_cast<std::int32_t>(savor::PK_TasMovieRevise)
            || item.program_version != 1 || item.program_ref_kind != kRewriteRefKind
            || item.program_ref_id <= 0 || item.savestate_id
            || item.input_ini != JobInput("TMER1:", item.program_ref_id)
            || item.reserved_attempt_id == 0 || item.claim_token.empty())
            return fail("input-epoch rewrite item identity drifted");
        const auto request = analysis_->GetTasMovieInputEpochRewriteRequest(item.program_ref_id);
        const auto source = request ? state_->GetArtifact(request->source_dtm_artifact_id)
            : std::nullopt;
        const auto schedule_artifact = request
            ? state_->GetArtifact(request->schedule_artifact_id) : std::nullopt;
        if (!request || request->workflow_step_id != context.workflow_step_id
            || request->full_phase_sha256 != phase_->identity().canonical_sha256
            || request->module_sha256 != phase_->runtime_contract().module.canonical_hash
            || !source || source->artifact_kind != "DTM"
            || source->sha256 != request->source_dtm_sha256
            || !schedule_artifact
            || schedule_artifact->artifact_kind != "TAS_MOVIE_INPUT_EPOCH_SCHEDULE"
            || schedule_artifact->sha256 != request->schedule_sha256)
            return fail("input-epoch rewrite immutable identity drifted");
        const auto request_root = root_ / ("rewrite-request-"
            + std::to_string(request->rewrite_request_id));
        const auto source_path = request_root / "source.dtm";
        const auto schedule_path = request_root / "source.schedule";
        if (!MaterializeArtifact(state_, *source, source_path, error_out)
            || !MaterializeArtifact(state_, *schedule_artifact, schedule_path, error_out))
            return std::nullopt;
        savor::tas::DtmFile dtm;
        if (!ValidateCompleteBootDtm(source_path, &dtm, nullptr, error_out))
            return std::nullopt;
        const auto schedule_bytes = ReadFile(schedule_path, error_out);
        inputepoch::TasMovieInputEpochScheduleV1 schedule;
        std::string diagnostic;
        if (!schedule_bytes || !inputepoch::DecodeInputEpochScheduleArtifactV1(
                *schedule_bytes, schedule, &diagnostic)
            || schedule.source_dtm_sha256 != source->sha256
            || request->insert_before_epoch >= schedule.epochs.size())
            return fail("rewrite schedule binding failed: " + diagnostic);
        const auto output_dtm = request_root / "capture" / "rewritten.dtm";
        const auto output_sav = request_root / "capture" / "endpoint.sav";
        std::error_code output_error;
        std::filesystem::create_directories(output_dtm.parent_path(), output_error);
        if (output_error)
            return fail("rewrite output directory creation failed: "
                + output_error.message());
        inputepoch::TasMovieInputEpochRewriteRequestV1 native{
            .source_dtm_path = source_path.string(),
            .schedule = std::move(schedule),
            .insert_before_epoch = request->insert_before_epoch,
            .neutral_epoch_count = request->neutral_epoch_count,
            .output_dtm_path = output_dtm.string(),
            .output_savestate_path = output_sav.string(),
        };
        const auto input = inputepoch::EncodeRewriteExecutionInputV1(native, &diagnostic);
        if (input.empty()) return fail("rewrite invocation binding failed: " + diagnostic);
        auto workset = MakeWorkset(context, *phase_, source_path, source->sha256, input,
            "SavorDb.PK_TasMovieRevise");
        std::vector<std::uint8_t> encoded;
        const auto status = savor::runtime::EncodeWorkerWorksetV5(workset, encoded);
        if (!status) return fail("rewrite workset encoding failed: " + status.message);
        workset.encoded_size_bytes = encoded.size();
        return WorksetReconstructionResult{
            .workset = std::move(workset), .ordered_job_ids = {item.job_id}};
    }

private:
    IStateDb* state_{};
    IAnalysisDb* analysis_{};
    std::filesystem::path root_;
    std::shared_ptr<const inputepoch::IRewriteFullPhaseDefinitionV1> phase_;
};

class AnnotationResultHandler final : public IProgramResultHandler {
public:
    AnnotationResultHandler(IStateDb* state, IAnalysisDb* analysis,
        std::filesystem::path root, bool breakpoint_diagnostic = false)
        : state_(state), analysis_(analysis), root_(WorkingRoot(root)),
          phase_(breakpoint_diagnostic
              ? inputepoch::BreakpointDiagnosticFullPhaseDefinitionV1()
              : inputepoch::AnnotationFullPhaseDefinitionV1()) {}

    ProgramResultDecision Process(
        const ProgramResultProcessingContext& context) const override {
        if (!state_ || !analysis_ || !phase_
            || context.program_kind != phase_->identity().program_kind
            || context.program_version != 1 || context.program_ref_kind != kAnnotationRefKind
            || context.program_ref_id <= 0
            || context.input_ini != JobInput("TMEA1:", context.program_ref_id))
            return FinalDecision("FAILED", "TAS_INPUT_EPOCH_ANNOTATION_IDENTITY_INVALID",
                "job is not an exact input-epoch annotation request");
        const auto request = analysis_->GetTasMovieInputEpochAnnotationRequest(
            context.program_ref_id);
        if (!request) throw std::runtime_error("input-epoch annotation request is missing");
        savor::runtime::DurableWorkerTerminalEnvelope terminal{};
        savor::runtime::program::ProgramResult decoded{};
        ProgramResultDecision failure;
        if (!ValidateTerminal(context, &terminal, &decoded, &failure)) return failure;
        inputepoch::TasMovieInputEpochAnnotationResultV1 outcome{};
        std::string error;
        if (!phase_->DecodeProgramResult(terminal.terminal.result, outcome, &error))
            return FinalDecision("FAILED", "TAS_INPUT_EPOCH_ANNOTATION_RESULT_INVALID",
                "job_id=" + std::to_string(context.job_id)
                + " terminal_sha256=" + context.terminal.sha256
                + " result_size=" + std::to_string(terminal.terminal.result.size())
                + " module=" + phase_->runtime_contract().module.canonical_id
                + " entrypoint=" + phase_->runtime_contract().entrypoint
                + " path=ProgramResult\n" + error);
        const bool passive = context.program_kind == static_cast<std::int32_t>(
            savor::PK_TasMovieAnnotate);
        if (passive && !DecodePassiveAnnotationCapture(
                terminal.terminal.workset_artifacts, request->source_dtm_sha256,
                outcome.schedule.source_poll_count,
                &outcome.schedule, &error))
            return FinalDecision("FAILED", "TAS_INPUT_EPOCH_ANNOTATION_CAPTURE_INVALID",
                "job_id=" + std::to_string(context.job_id)
                + " terminal_sha256=" + context.terminal.sha256
                + " path=DurableWorkerTerminalEnvelope.terminal.workset_artifacts[savor.capture.profile.artifact]\n"
                + error);
        RecordTasMovieInputEpochAnnotationAttemptCommand attempt{
            .producer = TasMovieInputEpochAnnotationProducer::Annotate,
            .annotation_request_id = request->annotation_request_id,
            .root_establishment_attempt_id = request->root_establishment_attempt_id,
            .source_dtm_artifact_id = request->source_dtm_artifact_id,
            .source_dtm_sha256 = request->source_dtm_sha256,
            .source_job_id = context.job_id,
            .worker_terminal_sha256 = context.terminal.sha256,
            .succeeded = outcome.outcome == inputepoch::InputEpochOutcomeV1::Completed,
            .source_poll_count = outcome.schedule.source_poll_count,
            .epoch_count = outcome.schedule.epochs.size(),
            .final_cursor = outcome.schedule.epochs.empty()
                ? 0 : outcome.schedule.epochs.back().movie_input_cursor,
            .worker_id = std::to_string(terminal.worker_id),
            .worker_process_generation = terminal.process_generation,
            .workset_epoch = terminal.terminal.workset_epoch,
            .recorded_at_utc = types::UtcNow(),
        };
        std::optional<ProgramResultStagingFile> staged_schedule;
        if (attempt.succeeded) {
            if ((!passive && !decoded.artifacts.empty())
                || outcome.schedule.source_dtm_sha256 != request->source_dtm_sha256) {
                return FinalDecision("FAILED", "TAS_INPUT_EPOCH_ANNOTATION_SHAPE_INVALID",
                    "completed annotation did not match its immutable source");
            }
            const auto bytes = inputepoch::EncodeInputEpochScheduleArtifactV1(
                outcome.schedule, &error);
            if (bytes.empty())
                return FinalDecision("FAILED", "TAS_INPUT_EPOCH_SCHEDULE_INVALID", error);
            const auto sha = hash::sha256(bytes.data(), bytes.size());
            const auto path = root_ / "published" / (sha + ".tes");
            if (!WriteFile(path, bytes, &error)) throw std::runtime_error(error);
            std::int64_t artifact_id = 0;
            if (!state_->StoreArtifact({
                    .sha256 = sha,
                    .size_bytes = static_cast<std::int64_t>(bytes.size()),
                    .compression_kind = 0,
                    .filename = path.string(),
                    .display_filename = "tas-movie-input-epoch-schedule.tes",
                    .file_ext = ".tes",
                    .artifact_kind = "TAS_MOVIE_INPUT_EPOCH_SCHEDULE",
                    .created_at_utc = types::UtcNow(),
                    .correlation_id = "tmv-input-epoch-annotation-request-"
                        + std::to_string(request->annotation_request_id),
                    .causation_id = "execution-job-" + std::to_string(context.job_id),
                }, &artifact_id, &error)) throw std::runtime_error(error);
            attempt.schedule_artifact_id = artifact_id;
            attempt.schedule_sha256 = sha;
            staged_schedule = ProgramResultStagingFile{
                .relative_path = path.lexically_relative(root_).generic_string(),
                .sha256 = sha,
                .size_bytes = static_cast<std::uint64_t>(bytes.size()),
            };
        } else {
            attempt.divergence_epoch = outcome.failure_epoch;
            attempt.divergence_cursor = outcome.actual_cursor;
            attempt.failure_code = FailureCode(outcome.failure_reason);
            attempt.failure_text = "input-epoch annotation diverged at epoch "
                + std::to_string(outcome.failure_epoch) + ", expected cursor "
                + std::to_string(outcome.expected_cursor) + ", actual cursor "
                + std::to_string(outcome.actual_cursor);
        }
        std::int64_t attempt_id = 0;
        if (!analysis_->RecordTasMovieInputEpochAnnotationAttempt(
                attempt, &attempt_id, &error)) throw std::runtime_error(error);
        auto decision = attempt.succeeded
            ? FinalDecision("SUCCEEDED")
            : FinalDecision("FAILED", attempt.failure_code, attempt.failure_text);
        decision.cleanup_worker_staging = true;
        if (staged_schedule) decision.staging_files.push_back(*staged_schedule);
        decision.outputs.push_back({
            .output_key = "annotation_attempt",
            .data_kind = "analysis.tas_movie_input_epoch_annotation_attempt_id",
            .ref_kind = "tmv_input_epoch_annotation_attempt",
            .ref_id = attempt_id,
        });
        decision.event_lines.push_back("[tasmovie-input-epoch-annotation-recorded] attempt="
            + std::to_string(attempt_id));
        return decision;
    }

private:
    IStateDb* state_{};
    IAnalysisDb* analysis_{};
    std::filesystem::path root_;
    std::shared_ptr<const inputepoch::IAnnotationFullPhaseDefinitionV1> phase_;
};

class RewriteResultHandler final : public IProgramResultHandler {
public:
    RewriteResultHandler(IStateDb* state, IAnalysisDb* analysis,
        std::filesystem::path root)
        : state_(state), analysis_(analysis),
          root_(WorkingRoot(root)),
          phase_(inputepoch::RewriteFullPhaseDefinitionV1()) {}

    ProgramResultDecision Process(
        const ProgramResultProcessingContext& context) const override {
        if (!state_ || !analysis_ || !phase_
            || context.program_kind != static_cast<std::int32_t>(
                savor::PK_TasMovieRevise)
            || context.program_version != 1 || context.program_ref_kind != kRewriteRefKind
            || context.program_ref_id <= 0
            || context.input_ini != JobInput("TMER1:", context.program_ref_id))
            return FinalDecision("FAILED", "TAS_INPUT_EPOCH_REWRITE_IDENTITY_INVALID",
                "job is not an exact input-epoch rewrite request");
        const auto request = analysis_->GetTasMovieInputEpochRewriteRequest(
            context.program_ref_id);
        if (!request) throw std::runtime_error("input-epoch rewrite request is missing");
        savor::runtime::DurableWorkerTerminalEnvelope terminal{};
        savor::runtime::program::ProgramResult decoded{};
        ProgramResultDecision failure;
        if (!ValidateTerminal(context, &terminal, &decoded, &failure)) return failure;
        inputepoch::TasMovieInputEpochRewriteResultV1 outcome{};
        std::string error;
        if (!phase_->DecodeProgramResult(terminal.terminal.result, outcome, &error))
            return FinalDecision("FAILED", "TAS_INPUT_EPOCH_REWRITE_RESULT_INVALID",
                "job_id=" + std::to_string(context.job_id)
                + " terminal_sha256=" + context.terminal.sha256
                + " result_size=" + std::to_string(terminal.terminal.result.size())
                + " module=" + phase_->runtime_contract().module.canonical_id
                + " entrypoint=" + phase_->runtime_contract().entrypoint
                + " path=ProgramResult\n" + error);
        RecordTasMovieInputEpochRewriteAttemptCommand attempt{
            .rewrite_request_id = request->rewrite_request_id,
            .source_job_id = context.job_id,
            .worker_terminal_sha256 = context.terminal.sha256,
            .succeeded = outcome.outcome == inputepoch::InputEpochOutcomeV1::Completed,
            .source_epoch_count = outcome.source_epoch_count,
            .rewritten_epoch_count = outcome.child_epoch_count,
            .final_movie_input_count = outcome.final_cursor,
            .worker_id = std::to_string(terminal.worker_id),
            .worker_process_generation = terminal.process_generation,
            .workset_epoch = terminal.terminal.workset_epoch,
            .recorded_at_utc = types::UtcNow(),
        };
        std::optional<std::int64_t> dtm_artifact_id;
        std::optional<std::int64_t> savestate_id;
        std::optional<RecordTasMovieInputEpochAnnotationAttemptCommand> child_annotation;
        std::optional<RecordTasMovieRootEstablishmentAttemptCommand> child_root;
        std::vector<ProgramResultStagingFile> generated_files;
        if (attempt.succeeded) {
            if (outcome.insert_before_epoch != request->insert_before_epoch
                || outcome.neutral_epoch_count != request->neutral_epoch_count
                || outcome.child_epoch_count != outcome.source_epoch_count +
                    request->neutral_epoch_count
                || outcome.artifacts.size() != 2) {
                return FinalDecision("FAILED", "TAS_INPUT_EPOCH_REWRITE_SHAPE_INVALID",
                    "completed rewrite returned an incompatible result shape");
            }
            const savor::runtime::program::ArtifactReferenceValue* dtm = nullptr;
            const savor::runtime::program::ArtifactReferenceValue* sav = nullptr;
            for (const auto& artifact : outcome.artifacts) {
                const std::filesystem::path path(artifact.artifact.storage_reference);
                if (path.extension() == ".dtm") dtm = &artifact.artifact;
                else if (path.extension() == ".sav") sav = &artifact.artifact;
            }
            if (!dtm || !sav)
                return FinalDecision("FAILED", "TAS_INPUT_EPOCH_REWRITE_ARTIFACTS_INVALID",
                    "rewrite did not return one DTM and one SAV artifact");
            const auto persist = [&](const savor::runtime::program::ArtifactReferenceValue& artifact,
                    std::string_view kind, std::int64_t* id_out) {
                const std::filesystem::path path(artifact.storage_reference);
                const auto sha = HashFile(path);
                std::error_code ec;
                const auto size = std::filesystem::file_size(path, ec);
                if (!artifact.complete || !sha || *sha != artifact.content_hash.ToHex()
                    || ec || size == 0)
                    return Fail("rewrite artifact does not match finalized worker evidence", &error);
                return state_->StoreArtifact({
                    .sha256 = *sha,
                    .size_bytes = static_cast<std::int64_t>(size),
                    .compression_kind = 0,
                    .filename = path.string(),
                    .display_filename = path.filename().string(),
                    .file_ext = path.extension().string(),
                    .artifact_kind = std::string(kind),
                    .created_at_utc = types::UtcNow(),
                    .correlation_id = "tmv-input-epoch-rewrite-request-"
                        + std::to_string(request->rewrite_request_id),
                    .causation_id = "execution-job-" + std::to_string(context.job_id),
                }, id_out, &error);
            };
            std::int64_t dtm_id = 0;
            std::int64_t sav_artifact_id = 0;
            if (!persist(*dtm, "DTM", &dtm_id) || !persist(*sav, "SAV", &sav_artifact_id))
                throw std::runtime_error(error);
            std::int64_t state_id = 0;
            if (!state_->CreateSavestate({
                    .artifact_id = sav_artifact_id,
                    .playback_state = SavestatePlaybackState::MoviePaired,
                    .dtm_artifact_id = dtm_id,
                    .savestate_type = "TAS_MOVIE_INPUT_EPOCH_REWRITE_ENDPOINT",
                    .note = "Movie-paired endpoint after guest input-epoch rewrite",
                    .is_complete = true,
                    .created_at_utc = types::UtcNow(),
                    .correlation_id = "tmv-input-epoch-rewrite-request-"
                        + std::to_string(request->rewrite_request_id),
                    .causation_id = "execution-job-" + std::to_string(context.job_id),
                }, &state_id, &error)) throw std::runtime_error(error);
            const auto state_record = state_->GetSavestate(state_id);
            const auto dtm_record = state_->GetArtifact(dtm_id);
            if (!state_record || !state_record->is_complete
                || state_record->playback_state != SavestatePlaybackState::MoviePaired
                || state_record->dtm_artifact_id != dtm_id || !dtm_record)
                throw std::runtime_error("rewritten movie-paired endpoint verification failed");
            dtm_artifact_id = dtm_id;
            savestate_id = state_id;
            attempt.rewritten_dtm_artifact_id = dtm_id;
            attempt.rewritten_dtm_sha256 = dtm_record->sha256;
            attempt.endpoint_savestate_id = state_id;

            savor::tas::DtmFile child_dtm;
            if (!ValidateCompleteBootDtm(
                    std::filesystem::path(dtm->storage_reference),
                    &child_dtm, nullptr, &error))
                return FinalDecision("FAILED", "TAS_INPUT_EPOCH_REWRITE_CHILD_DTM_INVALID", error);
            inputepoch::TasMovieInputEpochScheduleV1 child_schedule;
            if (!DecodePassiveAnnotationCapture(
                    terminal.terminal.workset_artifacts, dtm_record->sha256,
                    child_dtm.info().input_count, &child_schedule, &error))
                return FinalDecision("FAILED", "TAS_INPUT_EPOCH_REWRITE_CAPTURE_INVALID", error);
            const auto source_schedule_artifact = state_->GetArtifact(
                request->schedule_artifact_id);
            const auto source_schedule_bytes = source_schedule_artifact
                ? ReadFile(source_schedule_artifact->filename, &error)
                : std::nullopt;
            inputepoch::TasMovieInputEpochScheduleV1 source_schedule;
            if (!source_schedule_bytes
                || !inputepoch::DecodeInputEpochScheduleArtifactV1(
                    *source_schedule_bytes, source_schedule, &error))
                return FinalDecision("FAILED", "TAS_INPUT_EPOCH_REWRITE_SOURCE_SCHEDULE_INVALID", error);
            std::vector<GCInputFrame> expected;
            expected.reserve(source_schedule.epochs.size()
                + static_cast<std::size_t>(request->neutral_epoch_count));
            for (std::size_t i = 0; i < request->insert_before_epoch; ++i)
                expected.push_back(source_schedule.epochs[i].input);
            for (std::uint64_t i = 0; i < request->neutral_epoch_count; ++i)
                expected.push_back(GCInputFrame{});
            for (std::size_t i = request->insert_before_epoch;
                 i < source_schedule.epochs.size(); ++i)
                expected.push_back(source_schedule.epochs[i].input);
            if (child_schedule.epochs.size() != expected.size())
                return FinalDecision("FAILED", "TAS_INPUT_EPOCH_REWRITE_CAPTURE_SHAPE_INVALID",
                    "captured child schedule epoch count does not match the exact rewrite: expected="
                        + std::to_string(expected.size()) + " captured="
                        + std::to_string(child_schedule.epochs.size()));
            for (std::size_t i = 0; i < expected.size(); ++i) {
                if (!(child_schedule.epochs[i].input == expected[i]))
                    return FinalDecision("FAILED", "TAS_INPUT_EPOCH_REWRITE_CAPTURE_DIVERGED",
                        "captured child schedule diverged at epoch " + std::to_string(i));
            }

            const auto schedule_bytes = inputepoch::EncodeInputEpochScheduleArtifactV1(
                child_schedule, &error);
            if (schedule_bytes.empty())
                return FinalDecision("FAILED", "TAS_INPUT_EPOCH_REWRITE_CHILD_SCHEDULE_INVALID", error);
            const auto schedule_sha = hash::sha256(
                schedule_bytes.data(), schedule_bytes.size());
            const auto schedule_path = root_ / "published" / (schedule_sha + ".tes");
            if (!WriteFile(schedule_path, schedule_bytes, &error))
                throw std::runtime_error(error);
            std::int64_t schedule_artifact_id = 0;
            if (!state_->StoreArtifact({
                    .sha256 = schedule_sha,
                    .size_bytes = static_cast<std::int64_t>(schedule_bytes.size()),
                    .compression_kind = 0,
                    .filename = schedule_path.string(),
                    .display_filename = "tas-movie-input-epoch-schedule.tes",
                    .file_ext = ".tes",
                    .artifact_kind = "TAS_MOVIE_INPUT_EPOCH_SCHEDULE",
                    .created_at_utc = types::UtcNow(),
                    .correlation_id = "tmv-input-epoch-rewrite-request-"
                        + std::to_string(request->rewrite_request_id),
                    .causation_id = "execution-job-" + std::to_string(context.job_id),
                }, &schedule_artifact_id, &error)) throw std::runtime_error(error);

            const auto parent_root = analysis_->GetTasMovieRootEstablishmentAttempt(
                request->root_establishment_attempt_id);
            std::uint64_t child_root_cursor = 0;
            std::uint64_t child_root_vi_count = 0;
            if (!parent_root || !DecodeRootEstablishmentCapture(
                    terminal.terminal.workset_artifacts, parent_root->root_pc,
                    child_dtm.info().input_count, &child_root_cursor,
                    &child_root_vi_count, &error))
                return FinalDecision("FAILED", "TAS_INPUT_EPOCH_REWRITE_ROOT_CAPTURE_INVALID", error);
            savor::runtime::tasmovie::TasMovieItineraryV1 itinerary{{
                savor::runtime::tasmovie::MakeTasMovieCheckpointV1(
                    parent_root->root_pc, child_root_cursor,
                    child_root_vi_count)}};
            if (!savor::runtime::tasmovie::ValidateTasMovieItineraryArtifactV1(
                    itinerary, child_dtm.info().input_count,
                    parent_root->root_pc, &error))
                return FinalDecision("FAILED", "TAS_INPUT_EPOCH_REWRITE_ROOT_INVALID", error);
            const auto itinerary_bytes =
                savor::runtime::tasmovie::EncodeTasMovieItineraryArtifactV1(
                    itinerary, &error);
            if (itinerary_bytes.empty())
                return FinalDecision("FAILED", "TAS_INPUT_EPOCH_REWRITE_ITINERARY_INVALID", error);
            const auto itinerary_sha = hash::sha256(
                itinerary_bytes.data(), itinerary_bytes.size());
            const auto itinerary_path = root_ / "published" / (itinerary_sha + ".tmi");
            if (!WriteFile(itinerary_path, itinerary_bytes, &error))
                throw std::runtime_error(error);
            std::int64_t itinerary_artifact_id = 0;
            if (!state_->StoreArtifact({
                    .sha256 = itinerary_sha,
                    .size_bytes = static_cast<std::int64_t>(itinerary_bytes.size()),
                    .compression_kind = 0,
                    .filename = itinerary_path.string(),
                    .display_filename = "tas-movie-root-itinerary.tmi",
                    .file_ext = ".tmi",
                    .artifact_kind = "TAS_MOVIE_ITINERARY",
                    .created_at_utc = types::UtcNow(),
                    .correlation_id = "tmv-input-epoch-rewrite-request-"
                        + std::to_string(request->rewrite_request_id),
                    .causation_id = "execution-job-" + std::to_string(context.job_id),
                }, &itinerary_artifact_id, &error)) throw std::runtime_error(error);

            child_annotation = RecordTasMovieInputEpochAnnotationAttemptCommand{
                .producer = TasMovieInputEpochAnnotationProducer::Revise,
                .rewrite_request_id = request->rewrite_request_id,
                .source_dtm_artifact_id = dtm_id,
                .source_dtm_sha256 = dtm_record->sha256,
                .source_job_id = context.job_id,
                .worker_terminal_sha256 = context.terminal.sha256,
                .succeeded = true,
                .schedule_artifact_id = schedule_artifact_id,
                .schedule_sha256 = schedule_sha,
                .source_poll_count = child_schedule.source_poll_count,
                .epoch_count = child_schedule.epochs.size(),
                .final_cursor = child_schedule.epochs.back().movie_input_cursor,
                .worker_id = std::to_string(terminal.worker_id),
                .worker_process_generation = terminal.process_generation,
                .workset_epoch = terminal.terminal.workset_epoch,
                .recorded_at_utc = types::UtcNow(),
            };
            child_root = RecordTasMovieRootEstablishmentAttemptCommand{
                .producer = TasMovieRootEstablishmentProducer::Revise,
                .rewrite_request_id = request->rewrite_request_id,
                .parent_root_establishment_attempt_id =
                    request->root_establishment_attempt_id,
                .source_dtm_artifact_id = dtm_id,
                .source_dtm_sha256 = dtm_record->sha256,
                .itinerary_artifact_id = itinerary_artifact_id,
                .itinerary_sha256 = itinerary_sha,
                .root_pc = parent_root->root_pc,
                .movie_input_cursor = child_root_cursor,
                .source_job_id = context.job_id,
                .worker_terminal_sha256 = context.terminal.sha256,
                .recorded_at_utc = types::UtcNow(),
            };
            generated_files.push_back({
                .relative_path = schedule_path.lexically_relative(root_).generic_string(),
                .sha256 = schedule_sha,
                .size_bytes = static_cast<std::uint64_t>(schedule_bytes.size()),
            });
            generated_files.push_back({
                .relative_path = itinerary_path.lexically_relative(root_).generic_string(),
                .sha256 = itinerary_sha,
                .size_bytes = static_cast<std::uint64_t>(itinerary_bytes.size()),
            });
        } else {
            attempt.divergence_epoch = outcome.failure_epoch;
            attempt.divergence_cursor = outcome.actual_cursor;
            attempt.failure_code = FailureCode(outcome.failure_reason);
            attempt.failure_text = "input-epoch rewrite diverged at epoch "
                + std::to_string(outcome.failure_epoch) + ", expected cursor "
                + std::to_string(outcome.expected_cursor) + ", actual cursor "
                + std::to_string(outcome.actual_cursor);
        }
        std::int64_t attempt_id = 0;
        std::optional<std::int64_t> annotation_attempt_id;
        std::optional<std::int64_t> root_establishment_attempt_id;
        if (attempt.succeeded) {
            RecordTasMovieInputEpochRewriteCompletionReceipt receipt{};
            if (!analysis_->RecordTasMovieInputEpochRewriteCompletion({
                    .rewrite = attempt,
                    .annotation = *child_annotation,
                    .root_establishment = *child_root,
                }, &receipt, &error)) throw std::runtime_error(error);
            attempt_id = receipt.rewrite_attempt_id;
            annotation_attempt_id = receipt.annotation_attempt_id;
            root_establishment_attempt_id = receipt.root_establishment_attempt_id;
        } else if (!analysis_->RecordTasMovieInputEpochRewriteAttempt(
                attempt, &attempt_id, &error)) throw std::runtime_error(error);
        auto decision = attempt.succeeded
            ? FinalDecision("SUCCEEDED")
            : FinalDecision("FAILED", attempt.failure_code, attempt.failure_text);
        decision.cleanup_worker_staging = true;
        decision.staging_files.insert(decision.staging_files.end(),
            generated_files.begin(), generated_files.end());
        decision.outputs.push_back({
            .output_key = "rewrite_attempt",
            .data_kind = "analysis.tas_movie_input_epoch_rewrite_attempt_id",
            .ref_kind = "tmv_input_epoch_rewrite_attempt",
            .ref_id = attempt_id,
        });
        if (dtm_artifact_id) decision.outputs.push_back({
            .output_key = "rewritten_dtm",
            .data_kind = "state_artifact.dtm_artifact_id",
            .ref_kind = "state_artifact",
            .ref_id = *dtm_artifact_id,
        });
        if (savestate_id) decision.outputs.push_back({
            .output_key = "rewritten_paired_savestate",
            .data_kind = "state.movie_paired_savestate_id",
            .ref_kind = "state.savestate",
            .ref_id = *savestate_id,
        });
        if (annotation_attempt_id) decision.outputs.push_back({
            .output_key = "annotation_attempt",
            .data_kind = "analysis.tas_movie_input_epoch_annotation_attempt_id",
            .ref_kind = "tmv_input_epoch_annotation_attempt",
            .ref_id = *annotation_attempt_id,
        });
        if (root_establishment_attempt_id) decision.outputs.push_back({
            .output_key = "root_establishment",
            .data_kind = "analysis.tas_movie_root_establishment_attempt_id",
            .ref_kind = "tmv_root_establishment_attempt",
            .ref_id = *root_establishment_attempt_id,
        });
        decision.event_lines.push_back("[tasmovie-input-epoch-rewrite-recorded] attempt="
            + std::to_string(attempt_id));
        return decision;
    }

private:
    IStateDb* state_{};
    IAnalysisDb* analysis_{};
    std::filesystem::path root_;
    std::shared_ptr<const inputepoch::IRewriteFullPhaseDefinitionV1> phase_;
};

class CutsceneMaterializer final : public IProgramJobMaterializer {
public:
    CutsceneMaterializer(IExecutionDb* execution, IStateDb* state,
        IAnalysisDb* analysis, TasMovieInputEpochProgramConfig config)
        : execution_(execution), state_(state), analysis_(analysis),
          config_(std::move(config)),
          phase_(inputepoch::CutsceneFullPhaseDefinitionV1()) {}

    bool Materialize(const ProgramJobMaterializationContext& context,
        WorkflowStepScheduleResult* result_out,
        std::string* error_out) const override {
        if (!execution_ || !state_ || !analysis_ || !phase_ || !context.graph
            || !result_out)
            return Fail("cutscene dependencies are incomplete", error_out);
        *result_out = {};
        const auto tree_id = Binding(context, "tas_movie_tree",
            "state.tas_movie_tree_id", "state_tas_movie_tree");
        const auto tree = tree_id ? state_->GetTasMovieTree(*tree_id) : std::nullopt;
        const auto source_state = tree
            ? state_->GetSavestate(tree->checkpoint_savestate_id)
            : std::nullopt;
        const auto source_dtm = tree
            ? state_->GetArtifact(tree->dtm_artifact_id)
            : std::nullopt;
        const auto source_itinerary = tree
            ? state_->GetArtifact(tree->itinerary_artifact_id)
            : std::nullopt;
        const auto validation_status = source_dtm
            ? analysis_->GetTasMovieValidationStatus(source_dtm->sha256)
            : std::nullopt;
        const auto validation = validation_status
            ? analysis_->GetTasMovieValidationAttempt(
                validation_status->validation_attempt_id)
            : std::nullopt;
        const auto validation_request = validation
            ? analysis_->GetTasMovieValidationRequest(validation->validation_request_id)
            : std::nullopt;
        if (!validation_status
            || validation_status->status != TasMovieValidationStatus::Valid
            || !validation || validation->outcome != TasMovieValidationOutcome::Valid
            || validation->actual_input_count == 0 || !validation_request
            || validation_request->source_kind != TasMovieValidationSourceKind::Tree
            || validation_request->source_ref_id != tree->tas_movie_tree_id
            || !tree || !source_state || !source_dtm || !source_itinerary
            || !source_state->is_complete
            || source_state->playback_state != SavestatePlaybackState::MoviePaired
            || source_state->dtm_artifact_id != source_dtm->artifact_id
            || source_dtm->artifact_kind != "DTM"
            || source_itinerary->artifact_kind != "TAS_MOVIE_ITINERARY")
            return Fail("cutscene requires a successful validated movie-paired TAS tree",
                error_out);

        const auto& identity = phase_->identity();
        const auto& runtime = phase_->runtime_contract();
        CreateTasMovieCutsceneRequestCommand command{
            .materialization_key = "tasmovie.cutscene.step."
                + std::to_string(context.step.workflow_step_id),
            .workflow_instance_id = context.step.workflow_instance_id,
            .workflow_step_id = context.step.workflow_step_id,
            .source_validation_attempt_id = validation->validation_attempt_id,
            .source_tree_id = tree->tas_movie_tree_id,
            .source_savestate_id = source_state->savestate_id,
            .source_dtm_artifact_id = source_dtm->artifact_id,
            .source_dtm_sha256 = source_dtm->sha256,
            .source_itinerary_artifact_id = source_itinerary->artifact_id,
            .source_itinerary_sha256 = source_itinerary->sha256,
            .source_movie_input_cursor = validation->actual_input_count,
            .full_phase_program_kind = identity.program_kind,
            .full_phase_program_version = identity.program_version,
            .full_phase_canonical_id = identity.canonical_id,
            .full_phase_contract_revision = identity.contract_revision,
            .full_phase_sha256 = identity.canonical_sha256,
            .module_canonical_id = runtime.module.canonical_id,
            .module_revision = runtime.module.revision,
            .module_sha256 = runtime.module.canonical_hash,
            .created_at_utc = types::UtcNow(),
        };
        std::int64_t request_id = 0;
        if (!analysis_->CreateTasMovieCutsceneRequest(
                command, &request_id, error_out)) return false;
        if (!PublishSingleton(execution_, context, *phase_,
                CutsceneObservationDefaults(config_.enable_seed_call_progress),
                identity.program_kind, "TAS_MOVIE_CUTSCENE",
                "tas_movie_cutscene", kCutsceneRefKind,
                request_id, Fingerprint(identity.program_kind, request_id,
                    command.source_dtm_sha256, identity.canonical_sha256),
                JobInput("TMC1:", request_id),
                WorkingRoot(config_.working_dir_root) / "captures", std::nullopt,
                result_out, error_out, source_state->savestate_id))
            return false;
        result_out->event_lines.push_back("[tasmovie-cutscene-materialized] request="
            + std::to_string(request_id));
        return true;
    }

    bool Continue(const ProgramJobContinuationContext& context,
        ProgramJobContinuationResult* result_out,
        std::string* error_out) const override {
        if (!result_out || context.job_set_id <= 0)
            return Fail("cutscene continuation is invalid", error_out);
        const auto jobs = execution_->ListJobsInJobSet(context.job_set_id);
        if (jobs.size() != 1)
            return Fail("cutscene lost singleton shape", error_out);
        const auto job = execution_->GetExecutionJob(jobs.front().job_id);
        const auto attempt = job && job->worker_terminal_fingerprint
            ? analysis_->FindTasMovieCutsceneAttempt(
                job->job_id, *job->worker_terminal_fingerprint)
            : std::nullopt;
        if (!attempt || !attempt->succeeded)
            return Fail("successful cutscene attempt is unavailable", error_out);
        result_out->disposition = ProgramJobContinuationDisposition::Complete;
        result_out->output = ProgramJobContinuationOutput{
            .output_key = "cutscene_attempt",
            .data_kind = "analysis.tas_movie_cutscene_attempt_id",
            .ref_kind = "tmv_cutscene_attempt",
            .ref_id = attempt->cutscene_attempt_id,
        };
        return true;
    }

private:
    IExecutionDb* execution_{};
    IStateDb* state_{};
    IAnalysisDb* analysis_{};
    TasMovieInputEpochProgramConfig config_;
    std::shared_ptr<const inputepoch::ICutsceneFullPhaseDefinitionV1> phase_;
};

class CutsceneReconstruction final : public IWorksetReconstructionAdapter {
public:
    CutsceneReconstruction(IStateDb* state, IAnalysisDb* analysis,
        std::filesystem::path root)
        : state_(state), analysis_(analysis), root_(WorkingRoot(root)),
          phase_(inputepoch::CutsceneFullPhaseDefinitionV1()) {}

    std::optional<WorksetReconstructionResult> Reconstruct(
        const WorksetReconstructionContext& context,
        std::string* error_out) const override {
        const auto fail = [&](std::string text)
            -> std::optional<WorksetReconstructionResult> {
            Fail(std::move(text), error_out);
            return std::nullopt;
        };
        if (!state_ || !analysis_ || !phase_ || context.items.size() != 1
            || !context.state_compatibility.Complete())
            return fail("cutscene reconstruction requires one exact item");
        const auto& item = context.items.front();
        if (item.program_kind != static_cast<std::int32_t>(savor::PK_TasMovieCutscene)
            || item.program_version != 1 || item.program_ref_kind != kCutsceneRefKind
            || item.program_ref_id <= 0 || !item.savestate_id
            || item.input_ini != JobInput("TMC1:", item.program_ref_id)
            || item.reserved_attempt_id == 0 || item.claim_token.empty())
            return fail("cutscene item identity drifted");
        const auto request = analysis_->GetTasMovieCutsceneRequest(item.program_ref_id);
        const auto source_state = request
            ? state_->GetSavestate(request->source_savestate_id) : std::nullopt;
        const auto source_dtm = request
            ? state_->GetArtifact(request->source_dtm_artifact_id) : std::nullopt;
        const auto source_sav_artifact = source_state
            ? state_->GetArtifact(source_state->artifact_id) : std::nullopt;
        if (!request || request->workflow_step_id != context.workflow_step_id
            || request->full_phase_sha256 != phase_->identity().canonical_sha256
            || request->module_sha256 != phase_->runtime_contract().module.canonical_hash
            || *item.savestate_id != request->source_savestate_id
            || !source_state || !source_state->is_complete
            || source_state->playback_state != SavestatePlaybackState::MoviePaired
            || !source_dtm || !source_sav_artifact
            || source_dtm->sha256 != request->source_dtm_sha256)
            return fail("cutscene immutable source identity drifted");
        const auto request_root = root_ / ("cutscene-request-"
            + std::to_string(request->cutscene_request_id));
        const auto state_path = request_root / "source.sav";
        const auto dtm_path = std::filesystem::path(state_path.string() + ".dtm");
        const auto output_dtm_path = request_root / "result" / "cutscene.dtm";
        const auto output_savestate_path = request_root / "result" / "cutscene.sav";
        if (!EnsureParent(output_dtm_path, error_out))
            return std::nullopt;
        if (!MaterializeArtifact(state_, *source_sav_artifact, state_path, error_out)
            || !MaterializeArtifact(state_, *source_dtm, dtm_path, error_out))
            return std::nullopt;
        const inputepoch::TasMovieCutsceneRequestV1 native{
            .source_movie_input_cursor = request->source_movie_input_cursor,
            .output_dtm_path = output_dtm_path.string(),
            .output_savestate_path = output_savestate_path.string(),
        };
        std::string diagnostic;
        const auto input = inputepoch::EncodeCutsceneExecutionInputV1(native, &diagnostic);
        if (input.empty())
            return fail("cutscene invocation binding failed: " + diagnostic);
        auto workset = MakeSavestateWorkset(context, *phase_, state_path,
            source_sav_artifact->sha256, dtm_path, source_dtm->sha256, input,
            "SavorDb.PK_TasMovieCutscene");
        std::vector<std::uint8_t> encoded;
        const auto status = savor::runtime::EncodeWorkerWorksetV5(workset, encoded);
        if (!status)
            return fail("cutscene workset encoding failed: " + status.message);
        workset.encoded_size_bytes = encoded.size();
        return WorksetReconstructionResult{
            .workset = std::move(workset), .ordered_job_ids = {item.job_id}};
    }

private:
    IStateDb* state_{};
    IAnalysisDb* analysis_{};
    std::filesystem::path root_;
    std::shared_ptr<const inputepoch::ICutsceneFullPhaseDefinitionV1> phase_;
};

std::string CutsceneEndpointName(inputepoch::TasMovieCutsceneEndpointV1 endpoint) {
    switch (endpoint) {
    case inputepoch::TasMovieCutsceneEndpointV1::PreBattleSeed:
        return "PRE_BATTLE_SEED";
    case inputepoch::TasMovieCutsceneEndpointV1::FieldFastPreseed:
        return "FIELD_FAST_PRESEED";
    case inputepoch::TasMovieCutsceneEndpointV1::FieldDeferredPreseed:
        return "FIELD_DEFERRED_PRESEED";
    }
    return {};
}

bool AppendCutsceneOutputs(
    std::int64_t attempt_id,
    bool succeeded,
    std::optional<std::int64_t> tree_id,
    std::optional<std::int64_t> savestate_id,
    ProgramResultDecision* decision)
{
    if (attempt_id <= 0 || decision == nullptr)
        return false;
    decision->outputs.push_back({
        .output_key = "cutscene_attempt",
        .data_kind = "analysis.tas_movie_cutscene_attempt_id",
        .ref_kind = "tmv_cutscene_attempt",
        .ref_id = attempt_id,
    });
    if (!succeeded)
        return true;
    if (!tree_id || *tree_id <= 0 || !savestate_id || *savestate_id <= 0)
        return false;
    decision->outputs.push_back({
        .output_key = "tas_movie_tree",
        .data_kind = "state.tas_movie_tree_id",
        .ref_kind = "state_tas_movie_tree",
        .ref_id = *tree_id,
    });
    decision->outputs.push_back({
        .output_key = "paired_savestate",
        .data_kind = "state.movie_paired_savestate_id",
        .ref_kind = "state.savestate",
        .ref_id = *savestate_id,
    });
    return true;
}

class CutsceneResultHandler final : public IProgramResultHandler {
public:
    CutsceneResultHandler(IStateDb* state, IAnalysisDb* analysis,
        std::filesystem::path root)
        : state_(state), analysis_(analysis), root_(WorkingRoot(root)),
          phase_(inputepoch::CutsceneFullPhaseDefinitionV1()) {}

    ProgramResultDecision Process(
        const ProgramResultProcessingContext& context) const override {
        if (!state_ || !analysis_ || !phase_
            || context.program_kind != static_cast<std::int32_t>(savor::PK_TasMovieCutscene)
            || context.program_version != 1 || context.program_ref_kind != kCutsceneRefKind
            || context.program_ref_id <= 0
            || context.input_ini != JobInput("TMC1:", context.program_ref_id))
            return FinalDecision("FAILED", "TAS_CUTSCENE_IDENTITY_INVALID",
                "job is not an exact cutscene request");
        const auto request = analysis_->GetTasMovieCutsceneRequest(context.program_ref_id);
        if (!request) throw std::runtime_error("cutscene request is missing");
        if (const auto existing = analysis_->FindTasMovieCutsceneAttempt(
                context.job_id, context.terminal.sha256)) {
            auto decision = existing->succeeded
                ? FinalDecision("SUCCEEDED")
                : FinalDecision("FAILED", existing->failure_code, existing->failure_text);
            if (!AppendCutsceneOutputs(existing->cutscene_attempt_id,
                    existing->succeeded, existing->output_tree_id,
                    existing->output_savestate_id, &decision))
                throw std::runtime_error(
                    "persisted successful cutscene attempt is missing durable outputs");
            return decision;
        }
        savor::runtime::DurableWorkerTerminalEnvelope terminal{};
        savor::runtime::program::ProgramResult decoded{};
        ProgramResultDecision failure;
        if (!ValidateTerminal(context, &terminal, &decoded, &failure)) return failure;
        inputepoch::TasMovieCutsceneResultV1 outcome{};
        std::string error;
        if (!phase_->DecodeProgramResult(terminal.terminal.result, outcome, &error))
            return FinalDecision("FAILED", "TAS_CUTSCENE_RESULT_INVALID", error);
        const savor::runtime::program::ArtifactReferenceValue* dtm = nullptr;
        const savor::runtime::program::ArtifactReferenceValue* sav = nullptr;
        for (const auto& artifact : outcome.artifacts) {
            const std::filesystem::path path(artifact.artifact.storage_reference);
            if (path.extension() == ".dtm") dtm = &artifact.artifact;
            else if (path.extension() == ".sav") sav = &artifact.artifact;
        }
        if (!dtm || !sav)
            return FinalDecision("FAILED", "TAS_CUTSCENE_ARTIFACTS_INVALID",
                "cutscene did not return one DTM and one SAV artifact");
        const auto persist = [&](const auto& artifact, std::string_view kind,
                                 std::int64_t* id_out) {
            const std::filesystem::path path(artifact.storage_reference);
            const auto sha = HashFile(path);
            std::error_code ec;
            const auto size = std::filesystem::file_size(path, ec);
            if (!artifact.complete || !sha || *sha != artifact.content_hash.ToHex()
                || ec || size == 0)
                return Fail("cutscene artifact does not match worker evidence", &error);
            return state_->StoreArtifact({.sha256 = *sha,
                .size_bytes = static_cast<std::int64_t>(size),
                .filename = path.string(), .display_filename = path.filename().string(),
                .file_ext = path.extension().string(), .artifact_kind = std::string(kind),
                .created_at_utc = types::UtcNow(),
                .correlation_id = "tmv-cutscene-request-" + std::to_string(request->cutscene_request_id),
                .causation_id = "execution-job-" + std::to_string(context.job_id)},
                id_out, &error);
        };
        std::int64_t dtm_id = 0;
        std::int64_t sav_artifact_id = 0;
        if (!persist(*dtm, "DTM", &dtm_id) || !persist(*sav, "SAV", &sav_artifact_id))
            throw std::runtime_error(error);
        const auto dtm_record = state_->GetArtifact(dtm_id);
        const auto source_itinerary = state_->GetArtifact(
            request->source_itinerary_artifact_id);
        const auto itinerary_bytes = source_itinerary
            ? ReadFile(source_itinerary->filename, &error) : std::nullopt;
        savor::runtime::tasmovie::TasMovieItineraryV1 itinerary{};
        savor::tas::DtmFile child_dtm;
        if (!dtm_record || !source_itinerary
            || source_itinerary->sha256 != request->source_itinerary_sha256
            || !itinerary_bytes
            || !savor::runtime::tasmovie::DecodeTasMovieItineraryArtifactV1(
                *itinerary_bytes, itinerary, &error)
            || !child_dtm.load(std::filesystem::path(dtm->storage_reference).string()))
            return FinalDecision("FAILED", "TAS_CUTSCENE_ITINERARY_SOURCE_INVALID", error);
        itinerary.checkpoints.push_back(
            savor::runtime::tasmovie::MakeTasMovieCheckpointV1(
                outcome.endpoint_pc, outcome.checkpoint_input_count,
                outcome.checkpoint_vi_count, std::nullopt,
                outcome.area, outcome.subfield));
        if (!savor::runtime::tasmovie::ValidateTasMovieItineraryArtifactV1(
                itinerary, child_dtm.info().input_count, outcome.endpoint_pc, &error))
            return FinalDecision("FAILED", "TAS_CUTSCENE_ITINERARY_INVALID", error);
        const auto encoded_itinerary =
            savor::runtime::tasmovie::EncodeTasMovieItineraryArtifactV1(itinerary, &error);
        if (encoded_itinerary.empty())
            return FinalDecision("FAILED", "TAS_CUTSCENE_ITINERARY_INVALID", error);
        const auto itinerary_sha = hash::sha256(
            encoded_itinerary.data(), encoded_itinerary.size());
        const auto itinerary_path = root_ / "published" / (itinerary_sha + ".tmi");
        if (!WriteFile(itinerary_path, encoded_itinerary, &error))
            throw std::runtime_error(error);
        std::int64_t itinerary_id = 0;
        if (!state_->StoreArtifact({.sha256 = itinerary_sha,
                .size_bytes = static_cast<std::int64_t>(encoded_itinerary.size()),
                .filename = itinerary_path.string(), .display_filename = "tas-movie-cutscene-itinerary.tmi",
                .file_ext = ".tmi", .artifact_kind = "TAS_MOVIE_ITINERARY",
                .created_at_utc = types::UtcNow(),
                .correlation_id = "tmv-cutscene-request-" + std::to_string(request->cutscene_request_id),
                .causation_id = "execution-job-" + std::to_string(context.job_id)},
                &itinerary_id, &error))
            throw std::runtime_error(error);
        std::int64_t savestate_id = 0;
        if (!state_->CreateSavestate({.artifact_id = sav_artifact_id,
                .playback_state = SavestatePlaybackState::MoviePaired,
                .dtm_artifact_id = dtm_id,
                .savestate_type = "TAS_MOVIE_CUTSCENE_ENDPOINT",
                .note = "Movie-paired endpoint after recorded cutscene dialogue",
                .is_complete = true, .created_at_utc = types::UtcNow(),
                .correlation_id = "tmv-cutscene-request-" + std::to_string(request->cutscene_request_id),
                .causation_id = "execution-job-" + std::to_string(context.job_id)},
                &savestate_id, &error)
            || !state_->DeriveSavestate({.from_savestate_id = request->source_savestate_id,
                .to_savestate_id = savestate_id, .method_kind = "tasmovie.cutscene.v1",
                .source_context_kind = std::string(kCutsceneRefKind),
                .source_context_id = request->cutscene_request_id,
                .created_at_utc = types::UtcNow(),
                .correlation_id = "tmv-cutscene-request-" + std::to_string(request->cutscene_request_id),
                .causation_id = "execution-job-" + std::to_string(context.job_id)},
                nullptr, &error))
            throw std::runtime_error(error);
        const auto source_tree = state_->GetTasMovieTree(request->source_tree_id);
        if (!source_tree)
            throw std::runtime_error("cutscene source TAS tree is missing");
        std::int64_t tree_id = 0;
        if (!state_->CreateTasMovieTree({.tas_movie_root_id = source_tree->tas_movie_root_id,
                .parent_tas_movie_tree_id = source_tree->tas_movie_tree_id,
                .dtm_artifact_id = dtm_id, .itinerary_artifact_id = itinerary_id,
                .required_final_breakpoint_pc = outcome.endpoint_pc,
                .checkpoint_savestate_id = savestate_id,
                .source_context_kind = "CUTSCENE",
                .source_context_id = request->cutscene_request_id,
                .created_at_utc = types::UtcNow(),
                .correlation_id = "tmv-cutscene-request-" + std::to_string(request->cutscene_request_id),
                .causation_id = "execution-job-" + std::to_string(context.job_id)},
                &tree_id, &error))
            throw std::runtime_error(error);
        RecordTasMovieCutsceneAttemptCommand attempt{
            .cutscene_request_id = request->cutscene_request_id,
            .source_job_id = context.job_id,
            .worker_terminal_sha256 = context.terminal.sha256,
            .succeeded = true, .endpoint_kind = CutsceneEndpointName(outcome.endpoint),
            .endpoint_pc = outcome.endpoint_pc,
            .checkpoint_movie_input_cursor = outcome.checkpoint_input_count,
            .final_movie_input_cursor = outcome.final_input_count,
            .output_dtm_artifact_id = dtm_id,
            .output_dtm_sha256 = dtm_record->sha256,
            .output_savestate_id = savestate_id,
            .output_itinerary_artifact_id = itinerary_id,
            .output_tree_id = tree_id,
            .worker_id = std::to_string(terminal.worker_id),
            .worker_process_generation = terminal.process_generation,
            .workset_epoch = terminal.terminal.workset_epoch,
            .recorded_at_utc = types::UtcNow(),
        };
        std::int64_t attempt_id = 0;
        if (!analysis_->RecordTasMovieCutsceneAttempt(attempt, &attempt_id, &error))
            throw std::runtime_error(error);
        auto decision = FinalDecision("SUCCEEDED");
        decision.cleanup_worker_staging = true;
        decision.staging_files.push_back({
            .relative_path = itinerary_path.lexically_relative(root_).generic_string(),
            .sha256 = itinerary_sha,
            .size_bytes = static_cast<std::uint64_t>(encoded_itinerary.size())});
        if (!AppendCutsceneOutputs(attempt_id, true, tree_id, savestate_id,
                &decision))
            throw std::runtime_error(
                "new successful cutscene attempt is missing durable outputs");
        decision.event_lines.push_back("[tasmovie-cutscene-recorded] attempt="
            + std::to_string(attempt_id));
        return decision;
    }

private:
    IStateDb* state_{};
    IAnalysisDb* analysis_{};
    std::filesystem::path root_;
    std::shared_ptr<const inputepoch::ICutsceneFullPhaseDefinitionV1> phase_;
};

class CutsceneTransition final : public IWorkflowTransitionHandler {
public:
    explicit CutsceneTransition(IAnalysisDb* analysis) : analysis_(analysis) {}

    WorkflowTransitionDecision EvaluateTransition(
        const WorkflowTransitionContext& context) const override {
        WorkflowTransitionDecision decision{};
        decision.should_advance = true;
        if (!analysis_ || context.output_ref_kind !=
                std::optional<std::string>("tmv_cutscene_attempt")
            || !context.output_ref_id)
            return decision;
        const auto attempt = analysis_->GetTasMovieCutsceneAttempt(*context.output_ref_id);
        if (!attempt || !attempt->succeeded || !attempt->output_tree_id) {
            decision.should_advance = false;
            decision.workflow_failure = true;
            decision.blocked_reason = "cutscene_tree_missing";
            return decision;
        }
        decision.spawn_steps.push_back({
            .step_key = "Cutscene/" + std::to_string(attempt->cutscene_attempt_id)
                + "/validate",
            .step_kind = "tasmovie.validate_tree",
            .input_ref_kind = "state_tas_movie_tree",
            .input_ref_id = *attempt->output_tree_id,
            .priority = context.priority,
            .max_attempts = 1,
        });
        return decision;
    }

private:
    IAnalysisDb* analysis_{};
};

} // namespace

ProgramKindDescriptor BuildAnnotationProgramDescriptor(IExecutionDb* execution_db,
    IStateDb* state_db, IAnalysisDb* analysis_db,
    TasMovieInputEpochProgramConfig config) {
    ProgramKindDescriptor descriptor{};
    descriptor.program_kind = static_cast<std::int32_t>(
        savor::PK_TasMovieAnnotate);
    descriptor.program_name = savor::ProgramKindDisplayName(descriptor.program_kind);
    descriptor.result_staging_root = config.working_dir_root;
    descriptor.full_phase_identity = inputepoch::AnnotationFullPhaseDefinitionV1()->identity();
    descriptor.default_progress_library_ids = ObservationDefaults().progress_library_ids;
    descriptor.default_derived_state_block_ids = std::vector<std::string>{};
    descriptor.default_progress_runtime_trigger_pcs =
        ObservationDefaults().runtime_sample_trigger_pcs;
    descriptor.job_materializer = std::make_shared<AnnotationMaterializer>(
        execution_db, state_db, analysis_db, config);
    descriptor.workset_reconstruction = std::make_shared<AnnotationReconstruction>(
        state_db, analysis_db, config.working_dir_root);
    descriptor.result_handler = std::make_shared<AnnotationResultHandler>(
        state_db, analysis_db, config.working_dir_root);
    descriptor.supports_workflow_orchestration = true;
    return descriptor;
}

ProgramKindDescriptor BuildRewriteProgramDescriptor(IExecutionDb* execution_db,
    IStateDb* state_db, IAnalysisDb* analysis_db,
    TasMovieInputEpochProgramConfig config) {
    ProgramKindDescriptor descriptor{};
    descriptor.program_kind = static_cast<std::int32_t>(
        savor::PK_TasMovieRevise);
    descriptor.program_name = savor::ProgramKindDisplayName(descriptor.program_kind);
    descriptor.result_staging_root = config.working_dir_root;
    descriptor.full_phase_identity = inputepoch::RewriteFullPhaseDefinitionV1()->identity();
    descriptor.default_progress_library_ids = ObservationDefaults().progress_library_ids;
    descriptor.default_derived_state_block_ids = std::vector<std::string>{};
    descriptor.default_progress_runtime_trigger_pcs =
        ObservationDefaults().runtime_sample_trigger_pcs;
    descriptor.job_materializer = std::make_shared<RewriteMaterializer>(
        execution_db, state_db, analysis_db, config);
    descriptor.workset_reconstruction = std::make_shared<RewriteReconstruction>(
        state_db, analysis_db, config.working_dir_root);
    descriptor.result_handler = std::make_shared<RewriteResultHandler>(
        state_db, analysis_db, config.working_dir_root);
    descriptor.supports_workflow_orchestration = true;
    return descriptor;
}

ProgramKindDescriptor BuildCutsceneProgramDescriptor(IExecutionDb* execution_db,
    IStateDb* state_db, IAnalysisDb* analysis_db,
    TasMovieInputEpochProgramConfig config) {
    ProgramKindDescriptor descriptor{};
    descriptor.program_kind = static_cast<std::int32_t>(savor::PK_TasMovieCutscene);
    descriptor.program_name = savor::ProgramKindDisplayName(descriptor.program_kind);
    descriptor.result_staging_root = config.working_dir_root;
    descriptor.full_phase_identity = inputepoch::CutsceneFullPhaseDefinitionV1()->identity();
    const auto& observation_defaults = CutsceneObservationDefaults(
        config.enable_seed_call_progress);
    descriptor.default_progress_library_ids = observation_defaults.progress_library_ids;
    descriptor.default_derived_state_block_ids = std::vector<std::string>{};
    descriptor.default_progress_runtime_trigger_pcs =
        observation_defaults.runtime_sample_trigger_pcs;
    descriptor.job_materializer = std::make_shared<CutsceneMaterializer>(
        execution_db, state_db, analysis_db, config);
    descriptor.workset_reconstruction = std::make_shared<CutsceneReconstruction>(
        state_db, analysis_db, config.working_dir_root);
    descriptor.result_handler = std::make_shared<CutsceneResultHandler>(
        state_db, analysis_db, config.working_dir_root);
    descriptor.workflow_transition =
        std::make_shared<CutsceneTransition>(analysis_db);
    descriptor.supports_workflow_orchestration = true;
    return descriptor;
}

ProgramKindDescriptor BuildBreakpointDiagnosticProgramDescriptor(
    IExecutionDb* execution_db, IStateDb* state_db, IAnalysisDb* analysis_db,
    TasMovieInputEpochProgramConfig config)
{
    ProgramKindDescriptor descriptor{};
    descriptor.program_kind = static_cast<std::int32_t>(
        savor::PK_TasMovieInputEpochBreakpointDiagnostic);
    descriptor.program_name = savor::ProgramKindDisplayName(descriptor.program_kind);
    descriptor.result_staging_root = config.working_dir_root;
    descriptor.full_phase_identity =
        inputepoch::BreakpointDiagnosticFullPhaseDefinitionV1()->identity();
    descriptor.default_progress_library_ids = ObservationDefaults().progress_library_ids;
    descriptor.default_derived_state_block_ids = std::vector<std::string>{};
    descriptor.default_progress_runtime_trigger_pcs =
        ObservationDefaults().runtime_sample_trigger_pcs;
    descriptor.job_materializer = std::make_shared<AnnotationMaterializer>(
        execution_db, state_db, analysis_db, config, true);
    descriptor.workset_reconstruction = std::make_shared<AnnotationReconstruction>(
        state_db, analysis_db, config.working_dir_root, true);
    descriptor.result_handler = std::make_shared<AnnotationResultHandler>(
        state_db, analysis_db, config.working_dir_root, true);
    descriptor.supports_workflow_orchestration = true;
    return descriptor;
}

} // namespace savor::db::execution::programdb::tasmovieinputepoch
