#include "SeedProbeExecutionAdapters.h"

#include "SeedProbeJobSpec.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unordered_map>

#include "../../IExecutionDb.h"
#include "../../../Analysis/IAnalysisDb.h"
#include "../../../Common/Types/UtcTimestamp.h"
#include "../../../State/IStateDb.h"
#include "../../../../SavorCore/Phases/Programs/SeedProbe/SeedProbeModule.h"
#include "../../../../SavorCore/Phases/RNGSeedDeltaMap.h"
#include "../../../../SavorCore/Runner/IPC/DurableWorkerTerminalEnvelope.h"
#include "../../../../SavorCore/Runner/IPC/Wire.h"
#include "../../../../SavorCore/Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.h"
#include "../../../../SavorCore/Runner/Runtime/Worksets/WorksetWireCodec.h"
#include "../../../../SavorCore/Utils/Hash.h"

namespace savor::db::execution::programdb::seedprobe {
namespace {

constexpr std::string_view kProgramRefKind = "sp_probe_run";
constexpr std::string_view kArtifactLineage =
    savor::runtime::seedprobe::BaselineLineage;
constexpr std::size_t kDeclaredTerminalBytes = 64ull * 1024ull;

std::string Sha256(std::string_view value) {
    return hash::sha256(value.data(), value.size());
}

std::optional<std::string> HashFile(
    const std::filesystem::path& path) {
    try {
        return hash::sha256_of_file(path.string());
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool IsRegularFile(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

std::int64_t FileSize(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    return error ? -1 : static_cast<std::int64_t>(size);
}

bool IsLowerHexSha256(std::string_view value) {
    return value.size() == 64
        && std::all_of(
            value.begin(),
            value.end(),
            [](char ch) {
                return (ch >= '0' && ch <= '9')
                    || (ch >= 'a' && ch <= 'f');
            });
}

std::filesystem::path WorkingRoot(
    const std::filesystem::path& configured) {
    if (!configured.empty()) {
        return configured;
    }
    return std::filesystem::temp_directory_path()
        / "savor-seedprobe";
}

bool ExactSavestateFile(
    const savor::db::SavestateRecord& state,
    const std::filesystem::path& path) {
    if (!state.is_complete
        || state.artifact_size_bytes <= 0
        || !IsLowerHexSha256(state.artifact_sha256)
        || !IsRegularFile(path)
        || FileSize(path) != state.artifact_size_bytes) {
        return false;
    }
    const auto sha256 = HashFile(path);
    return sha256.has_value() && *sha256 == state.artifact_sha256;
}

std::optional<std::filesystem::path> ResolveSavestatePath(
    const savor::db::SavestateRecord& state,
    const std::filesystem::path& working_dir_root,
    savor::db::IStateDb* state_db,
    std::string* error_out) {
    const std::filesystem::path recorded(state.artifact_filename);
    if (ExactSavestateFile(state, recorded)) {
        return recorded;
    }

    std::string extension = state.artifact_file_ext;
    if (extension.empty() || extension.front() != '.'
        || extension.find_first_of("/\\") != std::string::npos) {
        extension = ".sav";
    }
    const auto destination =
        WorkingRoot(working_dir_root) / "baselines"
        / (std::to_string(state.savestate_id) + "-"
            + state.artifact_sha256 + extension);
    std::error_code directory_error;
    std::filesystem::create_directories(
        destination.parent_path(),
        directory_error);
    if (directory_error) {
        if (error_out != nullptr) {
            *error_out =
                "failed creating SeedProbe baseline directory: "
                + directory_error.message();
        }
        return std::nullopt;
    }

    if (!ExactSavestateFile(state, destination)) {
        std::string materialize_error;
        const auto materialized = state_db->MaterializeSavestateToPath(
            state.savestate_id,
            destination.string(),
            &materialize_error);
        if (!materialized.has_value()
            || !ExactSavestateFile(state, destination)) {
            if (error_out != nullptr) {
                *error_out = materialize_error.empty()
                    ? "State DB did not materialize the exact SeedProbe baseline"
                    : std::move(materialize_error);
            }
            return std::nullopt;
        }
    }
    return destination;
}

std::optional<savor::GCInputFrame> ResolveFrame(
    savor::db::IAnalysisDb* analysis_db,
    std::int64_t input_frame_id,
    std::string* error_out) {
    const auto row =
        analysis_db->GetAnalysisInputFrame(input_frame_id);
    if (!row.has_value()) {
        if (error_out != nullptr) {
            *error_out =
                "SeedProbe input frame does not exist: "
                + std::to_string(input_frame_id);
        }
        return std::nullopt;
    }
    const auto byte_value = [](std::int32_t value) {
        return value >= 0
            && value
                <= (std::numeric_limits<std::uint8_t>::max)();
    };
    if (!byte_value(row->main_x)
        || !byte_value(row->main_y)
        || !byte_value(row->cstick_x)
        || !byte_value(row->cstick_y)
        || !byte_value(row->trigger_x)
        || !byte_value(row->trigger_y)) {
        if (error_out != nullptr) {
            *error_out =
                "SeedProbe input frame contains an axis outside uint8";
        }
        return std::nullopt;
    }
    return savor::GCInputFrame{
        .buttons = 0,
        .main_x = static_cast<std::uint8_t>(row->main_x),
        .main_y = static_cast<std::uint8_t>(row->main_y),
        .c_x = static_cast<std::uint8_t>(row->cstick_x),
        .c_y = static_cast<std::uint8_t>(row->cstick_y),
        .trig_l = static_cast<std::uint8_t>(row->trigger_x),
        .trig_r = static_cast<std::uint8_t>(row->trigger_y),
    };
}

bool IsNeutralFrame(const savor::db::AnalysisInputSetFrameRow& row) {
    return row.main_x == 128 && row.main_y == 128
        && row.cstick_x == 128 && row.cstick_y == 128
        && row.trigger_x == 0 && row.trigger_y == 0;
}

bool ValidateItemIdentity(
    const WorksetReconstructionItem& item,
    const savor::db::SeedProbeRunSnapshot& run,
    const SeedProbeJobSpec& spec,
    std::string* error_out) {
    const auto fail = [&](std::string message) {
        if (error_out != nullptr) {
            *error_out = std::move(message);
        }
        return false;
    };
    if (item.job_id <= 0 || item.reserved_attempt_id == 0
        || item.claim_token.empty() || item.fingerprint.empty()) {
        return fail(
            "SeedProbe workset item lacks durable execution identity");
    }
    if (item.program_kind
            != static_cast<std::int32_t>(savor::PK_SeedProbe)
        || item.program_version
            != savor::runtime::seedprobe::ProgramVersion
        || item.program_ref_kind != kProgramRefKind
        || item.program_ref_id != run.probe_run_id
        || item.savestate_id
            != std::optional<std::int64_t>(
                run.entry_savestate_id)
        || spec.version != 1) {
        return fail(
            "SeedProbe workset item disagrees with its durable run identity");
    }
    return true;
}

savor::runtime::ProgramBaselineDefinition BuildBaseline(
    const savor::db::SavestateRecord& state,
    const std::filesystem::path& path,
    const savor::runtime::StateCompatibilityToken& compatibility) {
    return {
        .state_kind =
            savor::runtime::ProgramBaselineStateKind::Artifact,
        .artifact =
            savor::runtime::ProgramBaselineArtifact{
                .state_path = path,
                .state_sha256 = state.artifact_sha256,
                .movie_path = std::nullopt,
                .movie_sha256 = {},
                .movie_mode =
                    savor::runtime::ExternalMovieImportMode::NoMovie,
                .compatibility = compatibility,
                .lineage = {
                    .edge = std::string(kArtifactLineage),
                    .producer = "SavorDb.PK_SeedProbe",
                },
            },
        .lineage = std::string(kArtifactLineage),
    };
}

bool IsRetryableTerminal(
    savor::wrms::InvocationTerminalStatus status) {
    using Status =
        savor::wrms::InvocationTerminalStatus;
    return status == Status::Failed
        || status == Status::InfrastructureFailure
        || status == Status::CleanupFailure
        || status == Status::TimedOut;
}

bool IsExecutionTerminalState(std::string_view state) {
    return state == "COMPLETED" || state == "SUCCEEDED"
        || state == "SUCCEEDED_WINNER"
        || state == "SUPERSEDED"
        || state == "SUCCEEDED_DUPLICATE"
        || state == "FAILED" || state == "CANCELED";
}

savor::db::SeedProbeEndpoint AnalysisEndpoint(
    savor::runtime::seedprobe::SeedProbeEndpointV2 endpoint) {
    using RuntimeEndpoint =
        savor::runtime::seedprobe::SeedProbeEndpointV2;
    switch (endpoint) {
    case RuntimeEndpoint::AfterRandSeedSet:
        return savor::db::SeedProbeEndpoint::AfterRandSeedSet;
    case RuntimeEndpoint::RandSeedCommitted:
        return savor::db::SeedProbeEndpoint::RandSeedCommitted;
    }
    return savor::db::SeedProbeEndpoint::Unknown;
}

ProgramResultDecision RetryDecision(
    std::string code,
    std::string text) {
    ProgramResultDecision decision{};
    decision.disposition =
        ProgramResultDisposition::RetryExecution;
    decision.error_code = std::move(code);
    decision.error_text = std::move(text);
    return decision;
}

ProgramResultDecision FinalDecision(
    std::string state,
    std::optional<std::string> code = std::nullopt,
    std::optional<std::string> text = std::nullopt) {
    ProgramResultDecision decision{};
    decision.final_job_state = std::move(state);
    decision.error_code = std::move(code);
    decision.error_text = std::move(text);
    return decision;
}

class SeedProbeWorksetReconstructionAdapter final
    : public IWorksetReconstructionAdapter {
public:
    SeedProbeWorksetReconstructionAdapter(
        savor::db::IStateDb* state_db,
        savor::db::IAnalysisDb* analysis_db,
        std::filesystem::path working_dir_root)
        : state_db_(state_db)
        , analysis_db_(analysis_db)
        , working_dir_root_(std::move(working_dir_root))
        , phase_(savor::runtime::seedprobe::
              SeedProbeFullPhaseDefinitionV2()) {
    }

    std::optional<WorksetReconstructionResult> Reconstruct(
        const WorksetReconstructionContext& context,
        std::string* error_out) const override {
        const auto fail = [&](std::string message)
            -> std::optional<WorksetReconstructionResult> {
            if (error_out != nullptr) {
                *error_out = std::move(message);
            }
            return std::nullopt;
        };
        if (state_db_ == nullptr || analysis_db_ == nullptr) {
            return fail("SeedProbe reconstruction databases are unavailable");
        }
        if (context.workset_id <= 0
            || context.dispatch_attempt_id <= 0
            || context.workflow_step_id <= 0
            || context.root_job_set_id <= 0
            || context.dispatch_token.empty()
            || context.items.empty()
            || !context.state_compatibility.Complete()) {
            return fail(
                "SeedProbe reconstruction context is incomplete");
        }

        if (!phase_ || !phase_->identity())
            return fail("SeedProbe Full Phase definition is unavailable");
        const auto& runtime = phase_->runtime_contract();

        const auto& first = context.items.front();
        const auto run =
            analysis_db_->GetSeedProbeRun(first.program_ref_id);
        if (!run.has_value()) {
            return fail("SeedProbe run does not exist");
        }
        savor::runtime::ProgramBaselineDefinition baseline;
        {
            const auto cache_key = std::to_string(
                context.workflow_step_id) + ":" +
                std::to_string(context.root_job_set_id);
            std::scoped_lock lock(invocation_cache_mutex_);
            const auto cached = invocation_cache_.find(cache_key);
            if (cached != invocation_cache_.end()) {
                if (cached->second.probe_run_id != run->probe_run_id
                    || !cached->second.baseline.artifact.has_value()
                    || cached->second.baseline.artifact->compatibility
                        != context.state_compatibility) {
                    return fail(
                        "SeedProbe Full Phase invocation cache identity changed");
                }
                baseline = cached->second.baseline;
            } else {
                const auto state = state_db_->GetSavestate(
                    run->entry_savestate_id);
                if (!state.has_value()
                    || state->savestate_id != run->entry_savestate_id
                    || !state->is_complete
                    || !IsLowerHexSha256(state->artifact_sha256)
                    || state->artifact_size_bytes <= 0) {
                    return fail(
                        "SeedProbe run does not reference a complete State DB savestate");
                }
                const auto state_path = ResolveSavestatePath(
                    *state,
                    working_dir_root_,
                    state_db_,
                    error_out);
                if (!state_path.has_value()) {
                    return std::nullopt;
                }
                baseline = BuildBaseline(
                    *state,
                    *state_path,
                    context.state_compatibility);
                invocation_cache_.emplace(
                    cache_key,
                    CachedInvocation{
                        .probe_run_id = run->probe_run_id,
                        .baseline = baseline,
                    });
            }
        }

        savor::runtime::WorkerWorksetDefinition workset{};
        workset.workset_id =
            savor::runtime::WorkerWorksetId{
                static_cast<std::uint64_t>(
                    context.dispatch_attempt_id)};
        workset.phase_invocation = {
            .invocation_id = {
                .workflow_step_id = static_cast<std::uint64_t>(
                    context.workflow_step_id),
                .root_job_set_id = static_cast<std::uint64_t>(
                    context.root_job_set_id),
            },
            .program = phase_->identity(),
        };
        workset.baseline = std::move(baseline);
        workset.execution_key = {
            .module = runtime.module,
            .entrypoint = runtime.entrypoint,
            .verified_dependency_sha256 =
                runtime.verified_dependency_sha256,
            .runtime_profile_sha256 =
                runtime.runtime_profile_sha256,
            .baseline =
                savor::runtime::ComputeProgramBaselineKey(
                    workset.baseline),
            .movie_policy_sha256 =
                runtime.movie_policy_sha256,
            .service_policy_sha256 =
                runtime.service_policy_sha256,
        };
        workset.execution_key.canonical_sha256 =
            savor::runtime::
                ComputeWorkerWorksetExecutionKeyHash(
                    workset.execution_key);

        WorksetReconstructionResult result{};
        result.ordered_job_ids.reserve(context.items.size());
        workset.items.reserve(context.items.size());
        for (std::size_t index = 0;
             index < context.items.size();
             ++index) {
            const auto& item = context.items[index];
            std::string spec_error;
            const auto spec =
                DecodeSeedProbeJobSpec(
                    item.input_ini,
                    &spec_error);
            if (!spec.has_value()) {
                return fail(
                    "invalid SeedProbe job "
                    + std::to_string(item.job_id)
                    + ": " + spec_error);
            }
            if (!ValidateItemIdentity(
                    item,
                    *run,
                    *spec,
                    error_out)) {
                return std::nullopt;
            }
            const auto frame = ResolveFrame(
                analysis_db_,
                spec->input_frame_id,
                error_out);
            if (!frame.has_value()) {
                return std::nullopt;
            }
            if (spec->stage == SeedProbeJobStage::Confirm) {
                const auto candidate =
                    analysis_db_->GetSeedProbeResult(
                        *spec->
                            confirmation_of_probe_result_id);
                if (!candidate.has_value()
                    || candidate->probe_run_id
                        != run->probe_run_id
                    || candidate->input_frame_id
                        != spec->input_frame_id
                    || candidate->source_job_id == item.job_id) {
                    return fail(
                        "SeedProbe Confirm job does not repeat its candidate frame");
                }
            }

            const auto input = savor::runtime::seedprobe::
                EncodeSeedProbeExecutionInputV2({.frame = *frame});

            workset.items.push_back(
                {
                    .item_id =
                        savor::runtime::WorkerWorksetItemId{
                            static_cast<std::uint64_t>(
                                item.job_id)},
                    .ordinal =
                        static_cast<std::uint32_t>(index),
                    .execution = {
                        .execution_id =
                            savor::runtime::ProgramExecutionId{
                                static_cast<std::uint64_t>(
                                    item.job_id)},
                        .attempt_id =
                            savor::runtime::AttemptId{
                                item.reserved_attempt_id},
                        .input_payload = input,
                    },
                    .declared_terminal_bytes =
                        kDeclaredTerminalBytes,
                    .correlation = {
                        .durable_job_id =
                            std::to_string(item.job_id),
                        .claim_token = item.claim_token,
                        .parent_correlation =
                            context.compatibility_key,
                    },
                });
            result.ordered_job_ids.push_back(item.job_id);
        }

        std::vector<std::uint8_t> encoded_workset;
        const auto wire =
            savor::runtime::EncodeWorkerWorksetV2(
                workset,
                encoded_workset);
        if (!wire) {
            return fail(
                "SeedProbe workset encoding failed: "
                + wire.message);
        }
        workset.encoded_size_bytes = encoded_workset.size();
        result.workset = std::move(workset);
        if (error_out != nullptr) {
            error_out->clear();
        }
        return result;
    }

private:
    struct CachedInvocation {
        std::int64_t probe_run_id = 0;
        savor::runtime::ProgramBaselineDefinition baseline;
    };

    savor::db::IStateDb* state_db_ = nullptr;
    savor::db::IAnalysisDb* analysis_db_ = nullptr;
    std::filesystem::path working_dir_root_;
    std::shared_ptr<const savor::runtime::seedprobe::
        ISeedProbeFullPhaseDefinitionV2> phase_;
    mutable std::mutex invocation_cache_mutex_;
    mutable std::unordered_map<std::string, CachedInvocation>
        invocation_cache_;
};

class SeedProbeProgramResultHandler final
    : public IProgramResultHandler {
public:
    SeedProbeProgramResultHandler(
        savor::db::IExecutionDb* execution_db,
        savor::db::IAnalysisDb* analysis_db)
        : execution_db_(execution_db)
        , analysis_db_(analysis_db)
        , phase_(savor::runtime::seedprobe::
              SeedProbeFullPhaseDefinitionV2()) {
    }

    ProgramResultDecision Process(
        const ProgramResultProcessingContext& context)
        const override {
        if (execution_db_ == nullptr || analysis_db_ == nullptr) {
            throw std::runtime_error(
                "SeedProbe result databases are unavailable");
        }
        if (context.job_id <= 0
            || context.program_kind
                != static_cast<std::int32_t>(
                    savor::PK_SeedProbe)
            || context.program_version
                != savor::runtime::seedprobe::ProgramVersion
            || context.program_ref_kind != kProgramRefKind
            || context.program_ref_id <= 0) {
            return FinalDecision(
                "FAILED",
                "SEEDPROBE_JOB_IDENTITY_INVALID",
                "execution job is not an exact SeedProbe request");
        }

        std::string spec_error;
        const auto spec = DecodeSeedProbeJobSpec(
            context.input_ini,
            &spec_error);
        if (!spec.has_value()) {
            return FinalDecision(
                "FAILED",
                "SEEDPROBE_REQUEST_INVALID",
                std::move(spec_error));
        }
        const auto run =
            analysis_db_->GetSeedProbeRun(
                context.program_ref_id);
        if (!run.has_value()) {
            throw std::runtime_error(
                "SeedProbe result references a missing run");
        }
        const auto frame = ResolveFrame(
            analysis_db_,
            spec->input_frame_id,
            nullptr);
        if (!frame.has_value()) {
            throw std::runtime_error(
                "SeedProbe result cannot reconstruct its factual request");
        }

        savor::runtime::DurableWorkerTerminalEnvelope terminal{};
        std::string terminal_error;
        if (!savor::runtime::
                DecodeDurableWorkerTerminalEnvelope(
                    context.terminal.envelope,
                    &terminal,
                    &terminal_error)) {
            return FinalDecision(
                "FAILED",
                "SEEDPROBE_TERMINAL_INVALID",
                std::move(terminal_error));
        }
        const auto status = terminal.terminal.status;
        if (status
            == savor::wrms::
                InvocationTerminalStatus::Cancelled) {
            return FinalDecision(
                "CANCELED",
                "SEEDPROBE_EXECUTION_CANCELED",
                terminal.terminal.message);
        }
        if (IsRetryableTerminal(status)) {
            return RetryDecision(
                terminal.terminal.error_code.empty()
                    ? "SEEDPROBE_EXECUTION_FAILED"
                    : terminal.terminal.error_code,
                terminal.terminal.message.empty()
                    ? "SeedProbe worker invocation did not produce an observation"
                    : terminal.terminal.message);
        }
        if (status
                != savor::wrms::
                    InvocationTerminalStatus::Succeeded
            || terminal.terminal.unstarted
            || terminal.terminal.workset_id
                != static_cast<std::uint64_t>(
                    context.terminal.dispatch_attempt_id)
            || terminal.terminal.item_id
                != static_cast<std::uint64_t>(context.job_id)
            || terminal.terminal.invocation_id
                != static_cast<std::uint64_t>(context.job_id)
            || terminal.terminal.attempt_id
                != context.terminal.reserved_attempt_id
            || terminal.worker_id
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::int64_t>::max)())
            || terminal.process_generation == 0
            || terminal.process_generation
                > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::int64_t>::max)())
            || terminal.terminal.state_epoch == 0
            || terminal.terminal.session_disposition
                != savor::wrms::
                    SessionDispositionCode::Clean) {
            return FinalDecision(
                "FAILED",
                "SEEDPROBE_TERMINAL_MISMATCH",
                "SeedProbe terminal does not match its durable invocation");
        }

        const auto decoded_program =
            savor::runtime::program::
                DecodeProgramResultV1(
                    terminal.terminal.result);
        if (!decoded_program
            || decoded_program.value->invocation_id.value()
                != static_cast<std::uint64_t>(
                    context.job_id)
            || decoded_program.value->attempt_id.value()
                != context.terminal.reserved_attempt_id) {
            return FinalDecision(
                "FAILED",
                "SEEDPROBE_PROGRAM_RESULT_MISMATCH",
                !decoded_program
                    ? decoded_program.status.message
                    : "SeedProbe ProgramResult identity changed");
        }

        savor::runtime::seedprobe::SeedProbeResultV2 observation{};
        std::string observation_error;
        const savor::runtime::seedprobe::SeedProbeRequestV2
            request{
                .frame = *frame,
            };
        if (!phase_
            || !phase_->DecodeProgramResult(
                terminal.terminal.result,
                observation,
                &observation_error)
            || !savor::runtime::seedprobe::
                ValidateSeedProbeResultV2(
                    request,
                    observation,
                    savor::runtime::StateEpoch{
                        terminal.terminal.state_epoch},
                    &observation_error)) {
            return FinalDecision(
                "FAILED",
                "SEEDPROBE_OBSERVATION_INVALID",
                std::move(observation_error));
        }

        const auto endpoint = AnalysisEndpoint(observation.endpoint);
        const auto endpoint_text = std::string(
            ToDbString(endpoint));
        std::optional<std::int64_t> confirmation_of;
        if (spec->stage == SeedProbeJobStage::Confirm) {
            confirmation_of =
                spec->confirmation_of_probe_result_id;
        }
        RecordSeedProbeObservationReceipt observation_receipt{};
        std::string observation_persistence_error;
        if (!analysis_db_->RecordSeedProbeObservation(
                {
                    .probe_run_id = run->probe_run_id,
                    .input_frame_id = spec->input_frame_id,
                    .source_job_id = context.job_id,
                    .seed_value = observation.raw_seed,
                    .origin_worker_id = terminal.worker_id,
                    .origin_process_generation =
                        terminal.process_generation,
                    .origin_state_epoch = observation.semantic_stop
                        .state_epoch.value(),
                    .terminal_sha256 = context.terminal.sha256,
                    .confirmation_of_probe_result_id = confirmation_of,
                    .endpoint = endpoint,
                    .endpoint_mismatch_diagnostic =
                        "SeedProbe run endpoint mismatch: established="
                        + std::string(ToDbString(
                            run->established_endpoint))
                        + " observed=" + endpoint_text
                        + " source_job="
                        + std::to_string(context.job_id),
                    .recorded_at_utc = types::UtcNow(),
                    .correlation_id = "seedprobe-run-"
                        + std::to_string(run->probe_run_id),
                    .causation_id =
                        "execution-job-"
                        + std::to_string(context.job_id),
                },
                &observation_receipt,
                &observation_persistence_error)) {
            throw std::runtime_error(
                observation_persistence_error.empty()
                    ? "SeedProbe observation could not be persisted atomically"
                    : std::move(observation_persistence_error));
        }
        const bool conflicting_endpoint =
            observation_receipt.endpoint_disposition ==
                SeedProbeEndpointObservationDisposition::Invalidated
            || observation_receipt.endpoint_disposition ==
                SeedProbeEndpointObservationDisposition::
                    AlreadyInvalidatedConflicting;
        if (conflicting_endpoint) {
            auto decision = FinalDecision("SUCCEEDED");
            for (const auto& sibling :
                 execution_db_->ListJobsByProgramReference(
                     static_cast<std::int32_t>(savor::PK_SeedProbe),
                     kProgramRefKind,
                     run->probe_run_id)) {
                if (sibling.job_id == context.job_id
                    || sibling.state == "EXECUTION_FINISHED"
                    || IsExecutionTerminalState(sibling.state)) {
                    continue;
                }
                decision.cancellations.push_back({
                    .job_id = sibling.job_id,
                    .request_key =
                        "seedprobe-endpoint-invalidated:"
                        + std::to_string(run->probe_run_id)
                        + ":" + std::to_string(sibling.job_id),
                    .reason_code = "SEEDPROBE_ENDPOINT_MISMATCH",
                    .reason_text =
                        "SeedProbe run was invalidated by a conflicting factual endpoint",
                });
            }
            decision.event_lines.push_back(
                "[seedprobe-invalidated] run="
                + std::to_string(run->probe_run_id)
                + " job=" + std::to_string(context.job_id)
                + " established="
                + std::string(ToDbString(
                    observation_receipt.established_endpoint))
                + " conflicting=" + endpoint_text);
            return decision;
        }

        const auto& persisted = observation_receipt.observation;

        if (observation_receipt.endpoint_disposition ==
            SeedProbeEndpointObservationDisposition::
                AlreadyInvalidatedMatching) {
            auto decision = FinalDecision("SUCCEEDED");
            decision.event_lines.push_back(
                "[seedprobe-invalidated-late-fact] run="
                + std::to_string(run->probe_run_id)
                + " job=" + std::to_string(context.job_id)
                + " result="
                + std::to_string(persisted.probe_result_id));
            return decision;
        }

        if (spec->stage == SeedProbeJobStage::Survey) {
            auto decision = FinalDecision("SUCCEEDED");
            decision.event_lines.push_back(
                "[seedprobe-result] stage=SURVEY job="
                + std::to_string(context.job_id)
                + " result="
                + std::to_string(persisted.probe_result_id)
                + " raw_seed="
                + std::to_string(persisted.seed_value));
            return decision;
        }
        if (spec->stage == SeedProbeJobStage::Search) {
            return ProcessSearch(
                context,
                *run,
                *spec,
                persisted);
        }
        return ProcessConfirm(
            context,
            *run,
            *spec,
            persisted);
    }

private:
    std::optional<savor::db::SeedProbeResultRow>
    FindNeutralRepresentative(
        std::int64_t probe_run_id) const {
        const auto result_id =
            FindSurveyNeutralSeedProbeResultId(
                execution_db_,
                analysis_db_,
                probe_run_id);
        return result_id.has_value()
            ? analysis_db_->GetSeedProbeResult(*result_id)
            : std::nullopt;
    }

    bool TransitionEvidence(
        const savor::db::SeedProbeResultRow& row,
        savor::db::SeedProbeEvidenceState expected,
        savor::db::SeedProbeEvidenceState desired,
        const ProgramResultProcessingContext& context) const {
        bool changed = false;
        std::string error;
        if (analysis_db_->TransitionSeedProbeEvidence(
                {
                    .probe_result_id =
                        row.probe_result_id,
                    .expected_state = expected,
                    .new_state = desired,
                    .changed_at_utc =
                        savor::db::types::UtcNow(),
                    .correlation_id =
                        "seedprobe-run-"
                        + std::to_string(
                            row.probe_run_id),
                    .causation_id =
                        "execution-job-"
                        + std::to_string(context.job_id),
                },
                &changed,
                &error)) {
            return true;
        }
        const auto current =
            analysis_db_->GetSeedProbeResult(
                row.probe_result_id);
        if (current.has_value()
            && current->evidence_state == desired) {
            return true;
        }
        return false;
    }

    ProgramResultDecision ProcessSearch(
        const ProgramResultProcessingContext& context,
        const savor::db::SeedProbeRunSnapshot& run,
        const SeedProbeJobSpec& spec,
        const savor::db::SeedProbeResultRow& persisted)
        const {
        const auto neutral =
            FindNeutralRepresentative(run.probe_run_id);
        if (!neutral.has_value()) {
            throw std::runtime_error(
                "Search result cannot resolve the Survey neutral observation");
        }
        const std::int32_t delta =
            savor::wrapped_seed_delta(
                persisted.seed_value,
                neutral->seed_value);
        bool winner = false;
        winner = TransitionEvidence(
            persisted,
            savor::db::SeedProbeEvidenceState::Observed,
            savor::db::SeedProbeEvidenceState::Provisional,
            context);
        const auto current =
            analysis_db_->GetSeedProbeResult(
                persisted.probe_result_id);
        winner = winner && current.has_value()
            && current->source_job_id == context.job_id
            && (current->evidence_state
                    == savor::db::SeedProbeEvidenceState::
                        Provisional
                || current->evidence_state
                    == savor::db::SeedProbeEvidenceState::
                        Confirmed);
        if (!winner) {
            const auto all =
                analysis_db_->ListSeedProbeResults(
                    run.probe_run_id);
            const bool another_winner =
                std::any_of(
                    all.begin(),
                    all.end(),
                    [&](const auto& row) {
                        return row.probe_result_id
                                != persisted.probe_result_id
                            && row.seed_value
                                == persisted.seed_value
                            && (row.evidence_state
                                    == savor::db::
                                        SeedProbeEvidenceState::
                                            Provisional
                                || row.evidence_state
                                    == savor::db::
                                        SeedProbeEvidenceState::
                                            Confirmed);
                    });
            if (!another_winner) {
                throw std::runtime_error(
                    "failed advancing Search observation for "
                    "its actual delta");
            }
        }

        auto decision =
            FinalDecision(
                winner ? "SUCCEEDED_WINNER"
                       : "SUCCEEDED");
        decision.event_lines.push_back(
            "[seedprobe-result] stage=SEARCH job="
            + std::to_string(context.job_id)
            + " desired_delta="
            + std::to_string(*spec.desired_delta)
            + " observed_delta="
            + std::to_string(delta)
            + " winner=" + (winner ? "true" : "false"));
        if (!winner) {
            return decision;
        }

        const auto winner_job =
            execution_db_->GetJob(context.job_id);
        const auto source_group =
            SeedProbeCancellationGroupKey(
                run.probe_run_id,
                spec);
        const auto observed_group =
            SeedProbeCancellationGroupKey(
                run.probe_run_id,
                delta);
        if (!winner_job.has_value()
            || !source_group.has_value()
            || !observed_group.has_value()
            || winner_job->cancellation_group_key
                != source_group) {
            throw std::runtime_error(
                "Search winner has no matching durable cancellation group");
        }
        for (const auto& sibling :
             execution_db_->ListJobsInJobSet(
                 context.job_set_id)) {
            if (sibling.job_id == context.job_id
                || sibling.state == "EXECUTION_FINISHED"
                || IsExecutionTerminalState(
                    sibling.state)
                || sibling.cancellation_group_key
                    != observed_group) {
                continue;
            }
            decision.cancellations.push_back(
                {
                    .job_id = sibling.job_id,
                    .request_key =
                        "seedprobe-search-"
                        + std::to_string(
                            run.probe_run_id)
                        + "-delta-"
                        + std::to_string(delta)
                        + "-winner-"
                        + std::to_string(context.job_id)
                        + "-cancel-"
                        + std::to_string(
                            sibling.job_id),
                    .reason_code =
                        "SEEDPROBE_DELTA_SATISFIED",
                    .reason_text =
                        "observed delta "
                        + std::to_string(delta)
                        + " was observed by job "
                        + std::to_string(
                            context.job_id),
                });
        }
        return decision;
    }

    ProgramResultDecision ProcessConfirm(
        const ProgramResultProcessingContext& context,
        const savor::db::SeedProbeRunSnapshot& run,
        const SeedProbeJobSpec& spec,
        const savor::db::SeedProbeResultRow& persisted)
        const {
        const auto candidate =
            analysis_db_->GetSeedProbeResult(
                *spec.confirmation_of_probe_result_id);
        if (!candidate.has_value()
            || candidate->probe_run_id
                != run.probe_run_id
            || candidate->input_frame_id
                != spec.input_frame_id
            || candidate->source_job_id
                == context.job_id
            || (candidate->origin_worker_id
                    == persisted.origin_worker_id
                && candidate->origin_process_generation
                    == persisted.origin_process_generation
                && candidate->origin_state_epoch
                    == persisted.origin_state_epoch)
            || persisted.confirmation_of_probe_result_id
                != spec.confirmation_of_probe_result_id) {
            throw std::runtime_error(
                "Confirm observation does not independently repeat its provisional candidate");
        }

        const bool equal =
            candidate->seed_value
            == persisted.seed_value;
        const auto desired = equal
            ? savor::db::SeedProbeEvidenceState::
                Confirmed
            : savor::db::SeedProbeEvidenceState::
                Rejected;
        if (!TransitionEvidence(
                *candidate,
                savor::db::SeedProbeEvidenceState::
                    Provisional,
                desired,
                context)) {
            throw std::runtime_error(
                "failed finalizing SeedProbe confirmation evidence");
        }

        auto decision = FinalDecision("SUCCEEDED");
        decision.event_lines.push_back(
            "[seedprobe-result] stage=CONFIRM job="
            + std::to_string(context.job_id)
            + " candidate="
            + std::to_string(
                candidate->probe_result_id)
            + " equal=" + (equal ? "true" : "false")
            + " candidate_state="
            + std::string(
                savor::db::ToDbString(desired)));
        return decision;
    }

    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IAnalysisDb* analysis_db_ = nullptr;
    std::shared_ptr<const savor::runtime::seedprobe::
        ISeedProbeFullPhaseDefinitionV2> phase_;
};

} // namespace

std::optional<std::int64_t>
FindSurveyNeutralSeedProbeResultId(
    savor::db::IExecutionDb* execution_db,
    savor::db::IAnalysisDb* analysis_db,
    std::int64_t probe_run_id) {
    if (execution_db == nullptr
        || analysis_db == nullptr
        || probe_run_id <= 0) {
        return std::nullopt;
    }

    std::vector<savor::db::SeedProbeResultRow>
        candidates;
    for (const auto& row :
         analysis_db->ListSeedProbeResults(probe_run_id)) {
        if (row.confirmation_of_probe_result_id.has_value()
            || (row.evidence_state
                    != savor::db::SeedProbeEvidenceState::
                        Provisional
                && row.evidence_state
                    != savor::db::SeedProbeEvidenceState::
                        Confirmed)) {
            continue;
        }
        const auto source_job =
            execution_db->GetJob(row.source_job_id);
        if (!source_job.has_value()
            || source_job->program_kind
                != static_cast<std::int32_t>(
                    savor::PK_SeedProbe)
            || source_job->program_version
                != savor::runtime::seedprobe::ProgramVersion
            || source_job->program_ref_kind
                != kProgramRefKind
            || source_job->program_ref_id
                != probe_run_id) {
            continue;
        }
        const auto source_spec =
            DecodeSeedProbeJobSpec(
                source_job->input_ini);
        if (!source_spec.has_value()
            || source_spec->stage
                != SeedProbeJobStage::Survey
            || source_spec->input_frame_id
                != row.input_frame_id) {
            continue;
        }
        const auto frame =
            analysis_db->GetAnalysisInputFrame(
                row.input_frame_id);
        if (frame.has_value()
            && IsNeutralFrame(*frame)) {
            candidates.push_back(row);
        }
    }

    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const auto& lhs, const auto& rhs) {
            const auto rank = [](auto state) {
                return state
                        == savor::db::
                            SeedProbeEvidenceState::
                                Confirmed
                    ? 0
                    : state
                            == savor::db::
                                SeedProbeEvidenceState::
                                    Provisional
                        ? 1
                        : 2;
            };
            const int lhs_rank =
                rank(lhs.evidence_state);
            const int rhs_rank =
                rank(rhs.evidence_state);
            return lhs_rank != rhs_rank
                ? lhs_rank < rhs_rank
                : lhs.probe_result_id
                    < rhs.probe_result_id;
        });
    return candidates.empty()
        ? std::nullopt
        : std::optional<std::int64_t>{
            candidates.front().probe_result_id};
}

SeedProbeExecutionAdapters BuildSeedProbeExecutionAdapters(
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    savor::db::IAnalysisDb* analysis_db,
    std::filesystem::path working_dir_root) {
    return {
        .reconstruction =
            std::make_shared<
                SeedProbeWorksetReconstructionAdapter>(
                state_db,
                analysis_db,
                std::move(working_dir_root)),
        .result_handler =
            std::make_shared<
                SeedProbeProgramResultHandler>(
                execution_db,
                analysis_db),
    };
}

} // namespace savor::db::execution::programdb::seedprobe
