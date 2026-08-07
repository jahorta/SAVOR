#include "TasMovieRealWorkerScenario.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

#include "Analysis/IAnalysisDb.h"
#include "Execution/ProgramDB/ProductionProgramKindRegistry.h"
#include "Execution/ProgramDB/ProgramKindRegistry.h"
#include "Phases/Programs/TasMovieValidation/TasMovieValidationModule.h"
#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "State/IStateDb.h"
#include "Tas/DtmFile.h"
#include "Utils/Hash.h"

#include "DbSetup.h"
#include "DurableLogFile.h"
#include "SplitCoordinatorRuntime.h"
#include "WorkerStartupBarrier.h"

namespace savor::e2e {
namespace {

constexpr std::uint32_t kRootTerminalPc =
    savor::runtime::tasmovie::BeforeRandSeedSetPc;
constexpr std::uint32_t kWorkerStartupOperationTimeoutMs = 60'000;

struct VerifiedRootCursorAttempt {
    std::int64_t validation_attempt_id = 0;
    std::int64_t itinerary_artifact_id = 0;
    std::string itinerary_sha256;
    std::uint32_t pc = 0;
    std::uint64_t input_count = 0;
    std::string worker_id;
    std::uint64_t worker_process_generation = 0;
    std::uint64_t workset_epoch = 0;
};

const char* ToString(
    savor::db::execution::workflow::WorkflowInstanceState state) {
    using savor::db::execution::workflow::WorkflowInstanceState;
    switch (state) {
    case WorkflowInstanceState::Pending: return "PENDING";
    case WorkflowInstanceState::Running: return "RUNNING";
    case WorkflowInstanceState::Completed: return "COMPLETED";
    case WorkflowInstanceState::Failed: return "FAILED";
    case WorkflowInstanceState::Canceled: return "CANCELED";
    }
    return "UNKNOWN";
}

const char* ToString(savor::db::TasMovieValidationFailureReason reason) {
    using savor::db::TasMovieValidationFailureReason;
    switch (reason) {
    case TasMovieValidationFailureReason::None: return "None";
    case TasMovieValidationFailureReason::MovieDesynchronized:
        return "MovieDesynchronized";
    case TasMovieValidationFailureReason::ExpectedTerminalNotReached:
        return "ExpectedTerminalNotReached";
    case TasMovieValidationFailureReason::Unknown: return "Unknown";
    }
    return "Unknown";
}

std::string HexPc(std::uint32_t pc) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << std::setw(8)
        << std::setfill('0') << pc;
    return out.str();
}

std::string FormatWorkflowStateLine(
    const savor::db::execution::workflow::WorkflowGraphSnapshot& graph) {
    std::size_t completed = 0;
    for (const auto& step : graph.steps) {
        if (step.state
            == savor::db::execution::workflow::WorkflowStepState::Completed) {
            ++completed;
        }
    }
    std::ostringstream out;
    out << "workflow=" << ToString(graph.instance.state)
        << " steps=" << completed << "/" << graph.steps.size();
    return out.str();
}

bool FindTerminalInfrastructureJobFailure(
    savor::db::IExecutionDb* execution_db,
    const savor::db::execution::workflow::WorkflowGraphSnapshot& graph,
    std::string* diagnostic_out) {
    if (execution_db == nullptr) return false;
    for (const auto& step : graph.steps) {
        if (!step.job_set_id) continue;
        for (const auto& member : execution_db->ListJobsInJobSet(
                 *step.job_set_id)) {
            if (member.state != "FAILED"
                && member.state != "CANCELED") {
                continue;
            }
            const auto job = execution_db->GetJob(member.job_id);
            std::ostringstream diagnostic;
            diagnostic << "workflow job " << member.job_id
                << " reached infrastructure state " << member.state;
            if (job && job->worker_terminal_status) {
                diagnostic << " after worker terminal "
                    << *job->worker_terminal_status;
            }
            if (diagnostic_out) *diagnostic_out = diagnostic.str();
            return true;
        }
    }
    return false;
}

bool VerifySingletonEstablishmentGraph(
    const std::optional<
        savor::db::execution::workflow::WorkflowGraphSnapshot>& graph,
    std::string* error_out) {
    if (!graph || graph->unit_activations.size() != 1
        || graph->steps.size() != 1 || !graph->edges.empty()
        || !graph->unit_activation_edges.empty()
        || graph->unit_activations.front().unit_kind
            != "tas_movie_establish_root_cursor"
        || graph->steps.front().step_kind
            != "tasmovie.establish_root_cursor"
        || graph->steps.front().max_attempts != 1
        || !graph->arguments.empty()) {
        if (error_out) {
            *error_out = "TAS Movie E2E workflow is not the closed singleton root-cursor establishment shape";
        }
        return false;
    }
    return true;
}

bool VerifySingletonRootValidationGraph(
    const std::optional<
        savor::db::execution::workflow::WorkflowGraphSnapshot>& graph,
    std::int64_t establishment_attempt_id,
    std::int64_t rtc_value,
    std::string* error_out) {
    if (!graph || graph->unit_activations.size() != 1
        || graph->steps.size() != 1 || !graph->edges.empty()
        || !graph->unit_activation_edges.empty()
        || graph->instance.root_scope_kind != "manual"
        || graph->instance.root_scope_id != establishment_attempt_id
        || graph->unit_activations.front().unit_kind
            != "tas_movie_validate_root"
        || graph->steps.front().step_kind != "tasmovie.validate_root"
        || graph->steps.front().max_attempts != 1
        || graph->input_bindings.size() != 1
        || graph->arguments.size() != 1) {
        if (error_out) {
            *error_out = "TAS Movie root validation workflow is not a closed singleton shape";
        }
        return false;
    }
    const auto& input = graph->input_bindings.front();
    const auto& argument = graph->arguments.front();
    if (input.node_key != "tas_validate_1"
        || input.input_key != "root_establishment"
        || input.data_kind != "analysis.tas_movie_validation_attempt_id"
        || input.ref_kind != "tmv_validation_attempt"
        || input.ref_id != establishment_attempt_id
        || input.source_kind != "external"
        || argument.node_key != "tas_validate_1"
        || argument.argument_key != "rtc"
        || argument.value_type != "integer"
        || argument.integer_value != rtc_value
        || argument.text_value.has_value()
        || argument.source_kind != "scenario") {
        if (error_out) {
            *error_out = "TAS Movie root validation workflow input or exact RTC binding drifted";
        }
        return false;
    }
    return true;
}

std::optional<std::vector<std::uint8_t>> ReadBytes(
    const std::filesystem::path& path,
    std::string* error_out) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        if (error_out) *error_out = "could not open " + path.string();
        return std::nullopt;
    }
    const auto end = input.tellg();
    if (end < 0) {
        if (error_out) *error_out = "could not determine size of " + path.string();
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty()
        && !input.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()))) {
        if (error_out) *error_out = "could not read " + path.string();
        return std::nullopt;
    }
    return bytes;
}

bool VerifyRootCursorAttempt(
    savor::db::core::DBService* db_service,
    std::int64_t workflow_instance_id,
    std::int64_t source_dtm_artifact_id,
    const std::filesystem::path& source_dtm_path,
    const std::filesystem::path& scenario_workspace_root,
    VerifiedRootCursorAttempt* verified_out,
    std::string* error_out) {
    using savor::db::TasMovieValidationFailureReason;
    using savor::db::TasMovieValidationOperation;
    using savor::db::TasMovieValidationOutcome;
    using savor::db::TasMovieValidationSourceKind;

    const auto outputs = db_service->ExecutionDb()
        ->WorkflowQueryService()->ListStepOutputs(workflow_instance_id);
    std::vector<
        const savor::db::execution::workflow::WorkflowStepOutputRecord*>
        attempt_outputs;
    for (const auto& output : outputs) {
        if (output.output_key == "tas_movie_validation_attempt"
            && output.data_kind
                == "analysis.tas_movie_validation_attempt_id"
            && output.ref_kind == "tmv_validation_attempt"
            && output.ref_id > 0) {
            attempt_outputs.push_back(&output);
        }
    }
    if (attempt_outputs.size() != 1) {
        if (error_out) {
            *error_out = "completed establishment workflow does not expose exactly one typed validation-attempt output";
        }
        return false;
    }

    const auto attempt = db_service->AnalysisDb()
        ->GetTasMovieValidationAttempt(attempt_outputs.front()->ref_id);
    if (!attempt) {
        if (error_out) *error_out = "persisted TAS Movie validation attempt is missing";
        return false;
    }
    const auto request = db_service->AnalysisDb()
        ->GetTasMovieValidationRequest(attempt->validation_request_id);
    const auto source_artifact = db_service->StateDb()->GetArtifact(
        source_dtm_artifact_id);
    const auto phase = savor::runtime::tasmovie::
        TasMovieValidationFullPhaseDefinitionV1();
    if (!request
        || !source_artifact
        || source_artifact->artifact_kind != "DTM"
        || request->workflow_instance_id != workflow_instance_id
        || request->operation != TasMovieValidationOperation::EstablishRootCursor
        || request->source_kind != TasMovieValidationSourceKind::DtmArtifact
        || request->source_ref_id != source_dtm_artifact_id
        || request->source_dtm_artifact_id != source_dtm_artifact_id
        || request->source_dtm_sha256 != source_artifact->sha256
        || request->effective_dtm_sha256 != source_artifact->sha256
        || request->rtc_value.has_value()
        || request->itinerary_artifact_id.has_value()
        || request->itinerary_sha256.has_value()
        || request->capture_root_checkpoint
        || request->required_final_breakpoint_pc != kRootTerminalPc
        || !phase
        || request->full_phase_program_kind
            != phase->identity().program_kind
        || request->full_phase_program_version
            != phase->identity().program_version
        || request->full_phase_canonical_id
            != phase->identity().canonical_id
        || request->full_phase_contract_revision
            != phase->identity().contract_revision
        || request->full_phase_sha256
            != phase->identity().canonical_sha256
        || request->module_canonical_id
            != phase->runtime_contract().module.canonical_id
        || request->module_revision
            != phase->runtime_contract().module.revision
        || request->module_sha256
            != phase->runtime_contract().module.canonical_hash) {
        if (error_out) {
            *error_out = "persisted TAS Movie request is not the exact establishment-only request";
        }
        return false;
    }

    if (attempt->outcome == TasMovieValidationOutcome::Invalid) {
        std::ostringstream diagnostic;
        diagnostic << "root cursor establishment returned durable Invalid: reason="
            << ToString(attempt->failure_reason)
            << " expected_pc="
            << (attempt->expected_pc
                    ? HexPc(*attempt->expected_pc)
                    : std::string("none"))
            << " expected_count="
            << (attempt->expected_input_count
                    ? std::to_string(*attempt->expected_input_count)
                    : std::string("none"))
            << " actual_pc=" << HexPc(attempt->actual_pc)
            << " actual_count=" << attempt->actual_input_count
            << " attempt=" << attempt->validation_attempt_id;
        std::cout << "[tasmovie-domain-invalid] " << diagnostic.str() << '\n';
        if (error_out) *error_out = diagnostic.str();
        return false;
    }

    if (attempt->outcome != TasMovieValidationOutcome::RootCursorEstablished
        || attempt->failure_reason != TasMovieValidationFailureReason::None
        || attempt->actual_pc != kRootTerminalPc
        || attempt->expected_pc.has_value()
        || attempt->expected_input_count.has_value()
        || attempt->last_verified_itinerary_index.has_value()
        || attempt->last_known_good_savestate_id.has_value()
        || !attempt->candidate_itinerary_artifact_id
        || !attempt->candidate_itinerary_sha256
        || attempt->produced_tas_movie_root_id.has_value()) {
        if (error_out) {
            *error_out = "persisted TAS Movie root-cursor attempt has an illegal result shape";
        }
        return false;
    }

    savor::tas::DtmFile source_dtm;
    if (!source_dtm.load(source_dtm_path.string()) || !source_dtm.valid()) {
        if (error_out) *error_out = "source DTM could not be reloaded for cursor verification";
        return false;
    }
    try {
        if (hash::sha256_of_file(source_dtm_path.string())
            != source_artifact->sha256) {
            if (error_out) {
                *error_out = "source DTM bytes no longer match the immutable establishment request";
            }
            return false;
        }
    } catch (const std::exception& ex) {
        if (error_out) {
            *error_out = "failed hashing source DTM during cursor verification: "
                + std::string(ex.what());
        }
        return false;
    }
    if (attempt->actual_input_count >= source_dtm.info().input_count) {
        if (error_out) {
            *error_out = "established root cursor is not before the source DTM input-list boundary";
        }
        return false;
    }

    const auto artifact = db_service->StateDb()->GetArtifact(
        *attempt->candidate_itinerary_artifact_id);
    if (!artifact || artifact->artifact_kind != "TAS_MOVIE_ITINERARY"
        || artifact->file_ext != ".tmi"
        || artifact->sha256 != *attempt->candidate_itinerary_sha256) {
        if (error_out) *error_out = "root-cursor TMI artifact identity is missing or inconsistent";
        return false;
    }

    const auto verification_path = scenario_workspace_root / "verification"
        / (artifact->sha256 + ".tmi");
    std::string materialize_error;
    const auto materialized = db_service->StateDb()->MaterializeArtifactToPath(
        artifact->artifact_id,
        verification_path.string(),
        &materialize_error);
    if (!materialized) {
        if (error_out) {
            *error_out = "failed materializing root-cursor TMI: "
                + materialize_error;
        }
        return false;
    }
    try {
        if (hash::sha256_of_file(*materialized) != artifact->sha256) {
            if (error_out) *error_out = "materialized root-cursor TMI hash drifted";
            return false;
        }
    } catch (const std::exception& ex) {
        if (error_out) {
            *error_out = "failed hashing materialized root-cursor TMI: "
                + std::string(ex.what());
        }
        return false;
    }

    const auto bytes = ReadBytes(*materialized, error_out);
    savor::runtime::tasmovie::TasMovieItineraryV1 itinerary;
    std::string itinerary_error;
    if (!bytes
        || !savor::runtime::tasmovie::DecodeTasMovieItineraryArtifactV1(
            *bytes, itinerary, &itinerary_error)
        || !savor::runtime::tasmovie::ValidateTasMovieItineraryArtifactV1(
            itinerary,
            source_dtm.info().input_count,
            kRootTerminalPc,
            &itinerary_error)
        || itinerary.checkpoints.size() != 1
        || itinerary.checkpoints.front().pc != attempt->actual_pc
        || itinerary.checkpoints.front().input_count.value
            != attempt->actual_input_count) {
        if (error_out) {
            *error_out = "root-cursor TMI does not match its persisted attempt: "
                + itinerary_error;
        }
        return false;
    }

    std::cout << "[tasmovie-root-cursor-established] attempt="
        << attempt->validation_attempt_id
        << " pc=" << HexPc(attempt->actual_pc)
        << " input_count=" << attempt->actual_input_count
        << " dtm_input_count=" << source_dtm.info().input_count
        << " itinerary_artifact=" << artifact->artifact_id
        << " itinerary_sha256=" << artifact->sha256
        << " materialized=\"" << *materialized << "\"\n";
    if (verified_out) {
        *verified_out = {
            .validation_attempt_id = attempt->validation_attempt_id,
            .itinerary_artifact_id = artifact->artifact_id,
            .itinerary_sha256 = artifact->sha256,
            .pc = attempt->actual_pc,
            .input_count = attempt->actual_input_count,
            .worker_id = attempt->worker_id,
            .worker_process_generation = attempt->worker_process_generation,
            .workset_epoch = attempt->workset_epoch,
        };
    }
    return true;
}

std::optional<std::string> HashFile(
    const std::filesystem::path& path,
    std::string* error_out) {
    try {
        return hash::sha256_of_file(path.string());
    } catch (const std::exception& ex) {
        if (error_out) {
            *error_out = "failed hashing '" + path.string()
                + "': " + ex.what();
        }
        return std::nullopt;
    }
}

bool VerifyRootValidationAttempt(
    savor::db::core::DBService* db_service,
    std::int64_t workflow_instance_id,
    std::int64_t source_dtm_artifact_id,
    const std::filesystem::path& source_dtm_path,
    std::int64_t rtc_value,
    const VerifiedRootCursorAttempt& establishment,
    std::string* error_out) {
    using savor::db::TasMovieValidationFailureReason;
    using savor::db::TasMovieValidationOperation;
    using savor::db::TasMovieValidationOutcome;
    using savor::db::TasMovieValidationSourceKind;
    using savor::db::TasMovieValidationStatus;

    const auto outputs = db_service->ExecutionDb()
        ->WorkflowQueryService()->ListStepOutputs(workflow_instance_id);
    std::vector<
        const savor::db::execution::workflow::WorkflowStepOutputRecord*>
        attempt_outputs;
    for (const auto& output : outputs) {
        if (output.output_key == "tas_movie_validation_attempt"
            && output.data_kind
                == "analysis.tas_movie_validation_attempt_id"
            && output.ref_kind == "tmv_validation_attempt"
            && output.ref_id > 0) {
            attempt_outputs.push_back(&output);
        }
    }
    if (attempt_outputs.size() != 1) {
        if (error_out) {
            *error_out = "completed root-validation workflow does not expose exactly one typed validation-attempt output";
        }
        return false;
    }

    const auto attempt = db_service->AnalysisDb()
        ->GetTasMovieValidationAttempt(attempt_outputs.front()->ref_id);
    if (!attempt) {
        if (error_out) *error_out = "persisted root-validation attempt is missing";
        return false;
    }
    const auto request = db_service->AnalysisDb()
        ->GetTasMovieValidationRequest(attempt->validation_request_id);
    const auto source_artifact = db_service->StateDb()->GetArtifact(
        source_dtm_artifact_id);
    savor::tas::DtmFile patched_dtm;
    if (!request || !source_artifact
        || !patched_dtm.load(source_dtm_path.string())
        || !patched_dtm.valid()) {
        if (error_out) {
            *error_out = "root-validation source request or DTM is unavailable";
        }
        return false;
    }
    if (rtc_value < 0
        || static_cast<std::uint64_t>(rtc_value)
            > std::numeric_limits<std::uint32_t>::max()) {
        if (error_out) *error_out = "root-validation RTC is outside the GameCube u32 domain";
        return false;
    }
    patched_dtm.set_gamecube_rtc_seconds(
        static_cast<std::uint32_t>(rtc_value));
    const auto expected_dtm_sha256 = patched_dtm.compute_sha256();
    const auto phase = savor::runtime::tasmovie::
        TasMovieValidationFullPhaseDefinitionV1();
    if (request->workflow_instance_id != workflow_instance_id
        || request->operation != TasMovieValidationOperation::Validate
        || request->source_kind
            != TasMovieValidationSourceKind::RootEstablishment
        || request->source_ref_id != establishment.validation_attempt_id
        || request->source_dtm_artifact_id != source_dtm_artifact_id
        || request->source_dtm_sha256 != source_artifact->sha256
        || request->rtc_value != rtc_value
        || request->effective_dtm_sha256 != expected_dtm_sha256
        || request->itinerary_artifact_id
            != establishment.itinerary_artifact_id
        || request->itinerary_sha256 != establishment.itinerary_sha256
        || request->required_final_breakpoint_pc != kRootTerminalPc
        || !request->capture_root_checkpoint
        || !phase
        || request->full_phase_program_kind
            != phase->identity().program_kind
        || request->full_phase_program_version
            != phase->identity().program_version
        || request->full_phase_canonical_id
            != phase->identity().canonical_id
        || request->full_phase_contract_revision
            != phase->identity().contract_revision
        || request->full_phase_sha256
            != phase->identity().canonical_sha256
        || request->module_canonical_id
            != phase->runtime_contract().module.canonical_id
        || request->module_revision
            != phase->runtime_contract().module.revision
        || request->module_sha256
            != phase->runtime_contract().module.canonical_hash) {
        if (error_out) {
            *error_out = "persisted root-validation request drifted from its establishment, RTC, DTM, itinerary, or Full Phase identity";
        }
        return false;
    }
    if (attempt->worker_id != establishment.worker_id
        || attempt->worker_process_generation
            != establishment.worker_process_generation
        || attempt->workset_epoch == 0
        || establishment.workset_epoch == 0
        || attempt->workset_epoch == establishment.workset_epoch) {
        if (error_out) {
            *error_out = "sequential TAS Movie attempts did not use one worker process with distinct nonzero workset epochs";
        }
        return false;
    }

    const auto status = db_service->AnalysisDb()
        ->GetTasMovieValidationStatus(expected_dtm_sha256);
    if (!status || status->validation_attempt_id
            != attempt->validation_attempt_id) {
        if (error_out) {
            *error_out = "exact RTC-patched DTM lacks the current validation-status projection";
        }
        return false;
    }

    if (attempt->outcome == TasMovieValidationOutcome::Invalid) {
        const auto root = db_service->StateDb()->FindTasMovieRootBySourceRtc(
            source_dtm_artifact_id, rtc_value);
        if (attempt->failure_reason == TasMovieValidationFailureReason::None
            || attempt->candidate_itinerary_artifact_id
            || attempt->candidate_itinerary_sha256
            || attempt->produced_tas_movie_root_id
            || status->status != TasMovieValidationStatus::Quarantined
            || root.has_value()) {
            if (error_out) {
                *error_out = "durable Invalid result did not quarantine exactly the DTM without publishing root state";
            }
            return false;
        }
        std::ostringstream diagnostic;
        diagnostic << "root validation returned durable Invalid: reason="
            << ToString(attempt->failure_reason)
            << " expected_pc="
            << (attempt->expected_pc
                    ? HexPc(*attempt->expected_pc)
                    : std::string("none"))
            << " expected_count="
            << (attempt->expected_input_count
                    ? std::to_string(*attempt->expected_input_count)
                    : std::string("none"))
            << " actual_pc=" << HexPc(attempt->actual_pc)
            << " actual_count=" << attempt->actual_input_count
            << " last_verified_index="
            << (attempt->last_verified_itinerary_index
                    ? std::to_string(*attempt->last_verified_itinerary_index)
                    : std::string("none"))
            << " last_known_good_savestate="
            << (attempt->last_known_good_savestate_id
                    ? std::to_string(*attempt->last_known_good_savestate_id)
                    : std::string("none"))
            << " attempt=" << attempt->validation_attempt_id;
        std::cout << "[tasmovie-validation-invalid] "
                  << diagnostic.str() << '\n';
        if (error_out) *error_out = diagnostic.str();
        return false;
    }

    if (attempt->outcome != TasMovieValidationOutcome::Valid
        || attempt->failure_reason != TasMovieValidationFailureReason::None
        || attempt->expected_pc || attempt->expected_input_count
        || attempt->last_verified_itinerary_index
        || attempt->last_known_good_savestate_id
        || attempt->candidate_itinerary_artifact_id
        || attempt->candidate_itinerary_sha256
        || !attempt->produced_tas_movie_root_id
        || status->status != TasMovieValidationStatus::Valid) {
        if (error_out) *error_out = "persisted Valid root-validation attempt has an illegal result shape";
        return false;
    }

    const auto root = db_service->StateDb()->GetTasMovieRoot(
        *attempt->produced_tas_movie_root_id);
    if (!root
        || root->source_dtm_artifact_id != source_dtm_artifact_id
        || root->rtc_value != rtc_value
        || root->itinerary_artifact_id
            != establishment.itinerary_artifact_id
        || root->required_final_breakpoint_pc != kRootTerminalPc
        || root->source_context_kind != "tmv_validation_request"
        || root->source_context_id != request->validation_request_id) {
        if (error_out) *error_out = "published state_tas_movie_root row drifted from its exact validation request";
        return false;
    }
    const auto exact_root = db_service->StateDb()->FindTasMovieRootBySourceRtc(
        source_dtm_artifact_id, rtc_value);
    const auto dtm_artifact = db_service->StateDb()->GetArtifact(
        root->dtm_artifact_id);
    const auto checkpoint = db_service->StateDb()->GetSavestate(
        root->checkpoint_savestate_id);
    if (!exact_root || exact_root->tas_movie_root_id != root->tas_movie_root_id
        || !dtm_artifact || dtm_artifact->artifact_kind != "DTM"
        || dtm_artifact->sha256 != expected_dtm_sha256
        || !checkpoint || checkpoint->savestate_type
            != "TAS_MOVIE_ROOT_CHECKPOINT"
        || !checkpoint->is_complete || checkpoint->artifact_kind != "SAV") {
        if (error_out) *error_out = "validated root DTM or canonical checkpoint identity is missing";
        return false;
    }
    const auto dtm_file_hash = HashFile(dtm_artifact->filename, error_out);
    const auto checkpoint_hash = HashFile(
        checkpoint->artifact_filename, error_out);
    const auto sidecar_path = std::filesystem::path(
        checkpoint->artifact_filename + ".dtm");
    const auto sidecar_hash = HashFile(sidecar_path, error_out);
    if (!dtm_file_hash || *dtm_file_hash != expected_dtm_sha256
        || !checkpoint_hash
        || *checkpoint_hash != checkpoint->artifact_sha256
        || !sidecar_hash || *sidecar_hash != expected_dtm_sha256) {
        if (error_out && error_out->empty()) {
            *error_out = "root checkpoint or same-name DTM sidecar hash does not match the validated DTM";
        }
        return false;
    }

    std::cout << "[tasmovie-validation-valid] attempt="
        << attempt->validation_attempt_id
        << " rtc=" << rtc_value
        << " dtm_sha256=" << expected_dtm_sha256
        << " root=" << root->tas_movie_root_id
        << " checkpoint=" << root->checkpoint_savestate_id
        << " sidecar=\"" << sidecar_path.string() << "\""
        << " worker_id=" << attempt->worker_id
        << " process_generation=" << attempt->worker_process_generation
        << " establish_epoch=" << establishment.workset_epoch
        << " validate_epoch=" << attempt->workset_epoch << '\n';
    return true;
}

bool RunTasMovieScenario(
    const CliOptions& options,
    const char* argv0,
    savor::db::core::DBService* db_service,
    bool with_validation,
    std::string* error_out) {
    if (db_service == nullptr || !db_service->IsRunning()) {
        if (error_out) *error_out = "db service must be running";
        return false;
    }
    if (with_validation && !options.tasmovie_rtc.has_value()) {
        if (error_out) {
            *error_out = "tasmovie_with_validation requires one exact GameCube RTC";
        }
        return false;
    }

    const auto worker_exe = ResolveWorkerExePath(argv0);
    if (!std::filesystem::exists(worker_exe)) {
        if (error_out) {
            *error_out = "SavorWorker.exe was not found next to SavorE2E: "
                + worker_exe.string();
        }
        return false;
    }

    std::string error;
    std::int64_t dtm_artifact_id = 0;
    if (!SeedStateDtmArtifact(
            db_service->StateDb(), options.dtm_file,
            &dtm_artifact_id, &error)) {
        if (error_out) {
            *error_out = "failed seeding StateDB DTM artifact: " + error;
        }
        return false;
    }

    std::int64_t workflow_instance_id = 0;
    if (!SeedTasMovieWorkflow(
            db_service->AuthoringDb(), db_service->ExecutionDb(),
            dtm_artifact_id, &workflow_instance_id, &error)) {
        if (error_out) {
            *error_out = "failed seeding TAS Movie establishment workflow: "
                + error;
        }
        return false;
    }
    if (!VerifySingletonEstablishmentGraph(
            db_service->ExecutionDb()->WorkflowQueryService()
                ->GetWorkflowGraph(workflow_instance_id),
            error_out)) {
        return false;
    }

    const auto scenario_workspace_root = options.workspace_root.value_or(
        std::filesystem::temp_directory_path() / "savor-e2e-default");
    auto registry_config =
        savor::db::execution::programdb::MakeProductionProgramKindRegistryConfig(
            scenario_workspace_root / "workflow-runtime");
    registry_config.tas_movie_validation.working_dir_root =
        scenario_workspace_root / "tasmovie-validation";
    savor::db::execution::programdb::ProgramKindRegistry registry;
    if (!savor::db::execution::programdb::BuildProductionProgramKindRegistry(
            savor::db::execution::programdb::
                ProductionProgramKindRegistryDependencies{
                    .execution_db = db_service->ExecutionDb(),
                    .state_db = db_service->StateDb(),
                    .analysis_db = db_service->AnalysisDb(),
                    .authoring_db = db_service->AuthoringDb(),
                },
            std::move(registry_config),
            &registry,
            &error)) {
        if (error_out) {
            *error_out = "failed building production program registry: "
                + error;
        }
        return false;
    }

    DurableLogFile durable_log;
    if (!durable_log.Open(options, options.scenario, error_out)) {
        return false;
    }
    std::cout << "[durable-log] path=" << durable_log.path().string()
              << '\n';

    std::mutex lines_mtx;
    std::deque<std::string> pending_lines;
    const auto push_line = [&](std::string line) {
        durable_log.AppendLine(line);
        std::lock_guard<std::mutex> lock(lines_mtx);
        pending_lines.push_back(std::move(line));
    };
    const auto drain_lines = [&]() {
        std::deque<std::string> out;
        std::lock_guard<std::mutex> lock(lines_mtx);
        std::swap(out, pending_lines);
        return out;
    };
    std::string iso_sha256;
    try {
        iso_sha256 = hash::sha256_of_file(options.iso_path.string());
    } catch (const std::exception& exception) {
        if (error_out != nullptr) {
            *error_out = "failed hashing TAS Movie E2E ISO: "
                + std::string(exception.what());
        }
        return false;
    }
    if (iso_sha256.size() != 64) {
        if (error_out != nullptr) {
            *error_out = "TAS Movie E2E ISO hash is not a complete SHA-256";
        }
        return false;
    }
    savor::runtime::ArtifactCompatibilityToken state_compatibility{
        .game_id = std::string(
            savor::runtime::program::capabilities::kSupportedGameId),
        .iso_sha256 = std::move(iso_sha256),
        .emulator_build = "dolphin-2506a",
        .runtime_revision = "worker-runtime-slice4",
    };

    savor::runner::parallel::savordb::WorkerCoordinatorConfig worker_config{
        .desired_workers = 1,
        .controller_sleep_ms = static_cast<std::uint32_t>(
            std::max<std::int64_t>(1, options.poll_ms)),
        .worker_start_timeout_ms = kWorkerStartupOperationTimeoutMs,
        .worker_exe_path = worker_exe.string(),
        .iso_path = options.iso_path.string(),
        .dolphin_base_dir = options.dolphin_base_dir.string(),
        .worker_dir_root = options.worker_dir_root.value_or(
            std::filesystem::temp_directory_path()
                / "savor-e2e-workers").string(),
        .worker_binary_runtime_root =
            (scenario_workspace_root / "worker-runtime").string(),
        .visual_workers = options.visual_worker,
        .auto_resume_visual_workers = false,
        .runtime_artifact_root =
            (scenario_workspace_root / "runtime-artifacts").string(),
        .enabled_program_kinds = registry.RegisteredProgramKinds(),
    };

    SplitCoordinatorRuntime coordinators;
    ArmInitialWorkerPoolBarrier(
        true,
        [&](bool paused) { coordinators.SetExecutionPaused(paused); },
        [&](const std::string& line) { push_line(line); });
    if (!coordinators.Start(
            db_service->ExecutionDb(),
            db_service->AuthoringDb(),
            &registry,
            std::move(worker_config),
            scenario_workspace_root / "object_store",
            std::chrono::milliseconds(
                std::max<std::int64_t>(1, options.poll_ms)),
            std::move(state_compatibility),
            [&](const std::string& line) { push_line(line); },
            &error)) {
        std::string terminal_error;
        (void)TerminalFailInitialWorkerPoolWorkflows(
            db_service->ExecutionDb(),
            workflow_instance_id,
            coordinators.SnapshotFleetStartup(),
            &terminal_error);
        if (error_out != nullptr) {
            *error_out = "split TAS Movie coordinator startup failed: "
                + error;
            if (!terminal_error.empty()) {
                *error_out += "; workflow terminal failure: "
                    + terminal_error;
            }
        }
        return false;
    }

    const auto startup_barrier = WaitForInitialWorkerPool(
        true,
        std::chrono::milliseconds(
            std::max<std::int64_t>(1, options.poll_ms)),
        [&]() { return coordinators.SnapshotFleetStartup(); },
        [&](bool paused) { coordinators.SetExecutionPaused(paused); },
        [&](const std::string& line) { push_line(line); });
    if (!startup_barrier.satisfied) {
        std::string stop_error;
        (void)coordinators.Stop(&stop_error);
        std::string terminal_error;
        (void)TerminalFailInitialWorkerPoolWorkflows(
            db_service->ExecutionDb(),
            workflow_instance_id,
            startup_barrier.snapshot,
            &terminal_error);
        if (error_out != nullptr) {
            *error_out = startup_barrier.diagnostic;
            if (!stop_error.empty()) {
                *error_out += "; shutdown: " + stop_error;
            }
            if (!terminal_error.empty()) {
                *error_out += "; workflow terminal failure: "
                    + terminal_error;
            }
        }
        return false;
    }

    for (const auto& worker : coordinators.SnapshotReadyWorkers()) {
        std::ostringstream ready;
        ready << "[tasmovie-ready-worker] worker_id=" << worker.worker_id
              << " accepting_workset="
              << (worker.accepting_workset ? 1 : 0)
              << " capabilities=" << worker.capabilities
              << " modules=" << worker.runtime_manifest.modules.size();
        push_line(ready.str());
    }

    bool completed = false;
    bool failed = false;
    std::string latest_state = "workflow=unavailable";
    std::string terminal_job_error;
    std::size_t poll_count = 0;
    std::size_t ticks_since_snapshot = 0;
    while (true) {
        ++poll_count;
        ++ticks_since_snapshot;
        const auto event_lines = drain_lines();
        const auto graph = db_service->ExecutionDb()->WorkflowQueryService()
            ->GetWorkflowGraph(workflow_instance_id);
        for (const auto& line : event_lines) {
            std::cout << line << '\n';
        }
        if (ticks_since_snapshot >= 10 || poll_count == 1) {
            ticks_since_snapshot = 0;
            const auto workers = coordinators.SnapshotWorkers();
            const auto telemetry = coordinators.SnapshotTelemetry();
            std::cout << "[tasmovie] " << latest_state
                      << " workers=" << workers.size()
                      << " worksets_submitted="
                      << telemetry.execution.worksets_submitted
                      << " terminals="
                      << telemetry.execution.worker_terminals_observed
                      << '\n';
        }
        if (graph) {
            latest_state = FormatWorkflowStateLine(*graph);
            if (FindTerminalInfrastructureJobFailure(
                    db_service->ExecutionDb(), *graph,
                    &terminal_job_error)) {
                failed = true;
                break;
            }
            using savor::db::execution::workflow::WorkflowInstanceState;
            if (graph->instance.state == WorkflowInstanceState::Completed) {
                completed = true;
                break;
            }
            if (graph->instance.state == WorkflowInstanceState::Failed
                || graph->instance.state == WorkflowInstanceState::Canceled) {
                failed = true;
                break;
            }
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(options.poll_ms));
    }

    VerifiedRootCursorAttempt establishment{};
    std::int64_t validation_workflow_id = 0;
    bool validation_completed = false;
    bool validation_failed = false;
    std::string pre_stop_error;
    if (!completed) {
        pre_stop_error = failed
            ? (terminal_job_error.empty()
                ? "establishment workflow did not complete successfully"
                : terminal_job_error)
            : "establishment workflow stopped before reaching COMPLETED state";
    } else {
        const auto establishment_graph = db_service->ExecutionDb()
            ->WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
        if (!VerifySingletonEstablishmentGraph(
                establishment_graph, &pre_stop_error)
            || !VerifyRootCursorAttempt(
                db_service, workflow_instance_id, dtm_artifact_id,
                options.dtm_file, scenario_workspace_root,
                &establishment, &pre_stop_error)) {
            // The durable typed establishment result is authoritative. A
            // failed check prevents validation from being materialized.
        } else if (with_validation) {
            std::size_t quiescence_polls = 0;
            std::string quiescence_diagnostics;
            while (!CheckWorkflowQuiescence(
                    db_service->ExecutionDb(), &quiescence_diagnostics)) {
                ++quiescence_polls;
                if (quiescence_polls % 10 == 0) {
                    std::cout << "[tasmovie-establishment-draining] "
                              << quiescence_diagnostics << '\n';
                }
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(options.poll_ms));
            }
            std::cout << "[tasmovie-establishment-quiescent] workflow="
                      << workflow_instance_id << '\n';

            if (!SeedTasMovieRootValidationWorkflow(
                    db_service->AuthoringDb(),
                    db_service->ExecutionDb(),
                    establishment.validation_attempt_id,
                    *options.tasmovie_rtc,
                    &validation_workflow_id,
                    &pre_stop_error)
                || !VerifySingletonRootValidationGraph(
                    db_service->ExecutionDb()->WorkflowQueryService()
                        ->GetWorkflowGraph(validation_workflow_id),
                    establishment.validation_attempt_id,
                    *options.tasmovie_rtc,
                    &pre_stop_error)) {
                if (pre_stop_error.empty()) {
                    pre_stop_error = "failed creating exact RTC root-validation workflow";
                }
            } else {
                std::string validation_state = "workflow=unavailable";
                std::string validation_job_error;
                std::size_t validation_poll_count = 0;
                std::size_t validation_ticks_since_snapshot = 0;
                while (true) {
                    ++validation_poll_count;
                    ++validation_ticks_since_snapshot;
                    const auto event_lines = drain_lines();
                    const auto graph = db_service->ExecutionDb()
                        ->WorkflowQueryService()->GetWorkflowGraph(
                            validation_workflow_id);
                    for (const auto& line : event_lines) {
                        std::cout << line << '\n';
                    }
                    if (validation_ticks_since_snapshot >= 10
                        || validation_poll_count == 1) {
                        validation_ticks_since_snapshot = 0;
                        const auto workers = coordinators.SnapshotWorkers();
                        const auto telemetry = coordinators.SnapshotTelemetry();
                        std::cout << "[tasmovie-validation] "
                                  << validation_state
                                  << " workers=" << workers.size()
                                  << " worksets_submitted="
                                  << telemetry.execution.worksets_submitted
                                  << " terminals="
                                  << telemetry.execution.worker_terminals_observed
                                  << '\n';
                    }
                    if (graph) {
                        validation_state = FormatWorkflowStateLine(*graph);
                        if (FindTerminalInfrastructureJobFailure(
                                db_service->ExecutionDb(), *graph,
                                &validation_job_error)) {
                            validation_failed = true;
                            break;
                        }
                        using savor::db::execution::workflow::
                            WorkflowInstanceState;
                        if (graph->instance.state
                            == WorkflowInstanceState::Completed) {
                            validation_completed = true;
                            break;
                        }
                        if (graph->instance.state
                                == WorkflowInstanceState::Failed
                            || graph->instance.state
                                == WorkflowInstanceState::Canceled) {
                            validation_failed = true;
                            break;
                        }
                    }
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(options.poll_ms));
                }
                if (!validation_completed) {
                    pre_stop_error = validation_failed
                        ? (validation_job_error.empty()
                            ? "validation workflow did not complete successfully"
                            : validation_job_error)
                        : "validation workflow stopped before reaching COMPLETED state";
                }
            }
        }
    }

    if (with_validation && validation_completed && pre_stop_error.empty()) {
        std::size_t drain_polls = 0;
        while (true) {
            std::string quiescence_diagnostics;
            const bool workflow_quiescent = CheckWorkflowQuiescence(
                db_service->ExecutionDb(), &quiescence_diagnostics);
            const auto telemetry = coordinators.SnapshotTelemetry();
            if (workflow_quiescent
                && telemetry.execution.worksets_submitted == 2
                && telemetry.execution.draining_transitions == 2
                && telemetry.execution.worker_terminals_staged == 2
                && telemetry.execution.worker_terminal_acks == 2) {
                break;
            }
            ++drain_polls;
            if (drain_polls % 10 == 0) {
                std::cout << "[tasmovie-validation-draining] "
                    << quiescence_diagnostics
                    << " submitted="
                    << telemetry.execution.worksets_submitted
                    << " draining="
                    << telemetry.execution.draining_transitions
                    << " staged="
                    << telemetry.execution.worker_terminals_staged
                    << " acked="
                    << telemetry.execution.worker_terminal_acks << '\n';
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(options.poll_ms));
        }
    }
    const auto final_telemetry = coordinators.SnapshotTelemetry();
    const auto final_warnings = coordinators.SnapshotExecutionWarnings();
    std::string stop_error;
    const bool stopped = coordinators.Stop(&stop_error);

    const auto final_event_lines = drain_lines();
    for (const auto& line : final_event_lines) {
        std::cout << line << '\n';
    }
    const auto final_graph = db_service->ExecutionDb()
        ->WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
    if (final_graph) latest_state = FormatWorkflowStateLine(*final_graph);

    std::cout << "[tasmovie-final] workflow_status="
        << (completed ? "success" : failed ? "failure" : "incomplete")
        << '\n';
    std::cout << "  " << latest_state << '\n';
    if (!stopped) {
        if (error_out != nullptr) {
            *error_out = "split TAS Movie coordinator shutdown failed: "
                + stop_error;
        }
        return false;
    }
    if (!pre_stop_error.empty()) {
        if (error_out) *error_out = pre_stop_error;
        return false;
    }
    if (!VerifySingletonEstablishmentGraph(final_graph, error_out)) {
        return false;
    }
    if (!with_validation) {
        return true;
    }

    const auto validation_graph = db_service->ExecutionDb()
        ->WorkflowQueryService()->GetWorkflowGraph(validation_workflow_id);
    if (!VerifySingletonRootValidationGraph(
            validation_graph,
            establishment.validation_attempt_id,
            *options.tasmovie_rtc,
            error_out)) {
        return false;
    }
    if (final_telemetry.execution.worksets_submitted != 2
        || final_telemetry.execution.draining_transitions != 2
        || final_telemetry.execution.worker_terminals_staged != 2
        || final_telemetry.execution.worker_terminal_acks != 2
        || final_telemetry.execution.worker_terminals_observed != 2
        || !final_warnings.empty()) {
        if (error_out) {
            std::ostringstream diagnostic;
            diagnostic << "sequential TAS Movie coordinator telemetry did not reconcile: submitted="
                << final_telemetry.execution.worksets_submitted
                << " draining="
                << final_telemetry.execution.draining_transitions
                << " observed="
                << final_telemetry.execution.worker_terminals_observed
                << " staged="
                << final_telemetry.execution.worker_terminals_staged
                << " acked="
                << final_telemetry.execution.worker_terminal_acks
                << " warnings=" << final_warnings.size();
            *error_out = diagnostic.str();
        }
        return false;
    }
    std::cout << "[tasmovie-sequential-telemetry] worksets_submitted=2"
              << " draining_transitions=2 terminals=2 acknowledgements=2"
              << " coordinator_warnings=0\n";
    return VerifyRootValidationAttempt(
        db_service,
        validation_workflow_id,
        dtm_artifact_id,
        options.dtm_file,
        *options.tasmovie_rtc,
        establishment,
        error_out);
}

} // namespace

bool RunTasMovieRealWorkerSmoke(
    const CliOptions& options,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out) {
    return RunTasMovieScenario(
        options, argv0, db_service, false, error_out);
}

bool RunTasMovieWithValidationRealWorkerSmoke(
    const CliOptions& options,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out) {
    return RunTasMovieScenario(
        options, argv0, db_service, true, error_out);
}

} // namespace savor::e2e
