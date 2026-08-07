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
#include <string_view>
#include <thread>
#include <vector>

#include "Analysis/IAnalysisDb.h"
#include "Execution/ProgramDB/ProductionProgramKindRegistry.h"
#include "Execution/ProgramDB/ProgramKindRegistry.h"
#include "Phases/Programs/SeedProbe/SeedProbeModule.h"
#include "Phases/Programs/TasMovieValidation/TasMovieValidationModule.h"
#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "State/IStateDb.h"
#include "Tas/DtmFile.h"
#include "Utils/Hash.h"

#include "DbSetup.h"
#include "DurableLogFile.h"
#include "SeedProbeRealWorkerScenario.h"
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

struct VerifiedRootValidationAttempt {
    std::int64_t validation_attempt_id = 0;
    std::int64_t tas_movie_root_id = 0;
    std::int64_t checkpoint_savestate_id = 0;
    std::string effective_dtm_sha256;
    std::string worker_id;
    std::uint64_t worker_process_generation = 0;
    std::uint64_t workset_epoch = 0;
};

struct VerifiedSterilizationAttempt {
    std::int64_t savestate_id = 0;
    std::int64_t sterilization_attempt_id = 0;
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

bool VerifyComposedTasMovieSeedProbeGraph(
    savor::db::IAuthoringDb* authoring_db,
    const std::optional<
        savor::db::execution::workflow::WorkflowGraphSnapshot>& graph,
    std::int64_t dtm_artifact_id,
    std::int64_t seed_probe_spec_id,
    std::int64_t rtc_value,
    bool require_initial_state,
    std::string* error_out) {
    using savor::db::execution::workflow::WorkflowStepState;
    if (authoring_db == nullptr || !graph
        || !graph->instance.workflow_graph_revision_id
        || graph->instance.root_scope_kind != "manual"
        || graph->instance.root_scope_id != dtm_artifact_id
        || graph->unit_activations.size() != 4
        || graph->steps.size() != 4
        || graph->unit_activation_edges.size() != 3
        || graph->edges.size() != 3
        || graph->arguments.size() != 2) {
        if (error_out) {
            *error_out = "tasmovie_seedprobe runtime graph is not the exact four-unit composition";
        }
        return false;
    }

    const auto find_activation = [&](std::string_view node_key) {
        return std::find_if(
            graph->unit_activations.begin(),
            graph->unit_activations.end(),
            [&](const auto& activation) {
                return activation.graph_node_key == node_key;
            });
    };
    const auto find_step = [&](std::string_view node_key) {
        return std::find_if(
            graph->steps.begin(), graph->steps.end(),
            [&](const auto& step) {
                return step.graph_node_key == node_key;
            });
    };
    const auto establish_activation = find_activation("tas_establish_1");
    const auto validate_activation = find_activation("tas_validate_1");
    const auto sterilize_activation = find_activation("tas_sterilize_1");
    const auto probe_activation = find_activation("probe_1");
    const auto establish_step = find_step("tas_establish_1");
    const auto validate_step = find_step("tas_validate_1");
    const auto sterilize_step = find_step("tas_sterilize_1");
    const auto probe_step = find_step("probe_1");
    if (establish_activation == graph->unit_activations.end()
        || validate_activation == graph->unit_activations.end()
        || sterilize_activation == graph->unit_activations.end()
        || probe_activation == graph->unit_activations.end()
        || establish_step == graph->steps.end()
        || validate_step == graph->steps.end()
        || sterilize_step == graph->steps.end()
        || probe_step == graph->steps.end()
        || establish_activation->unit_kind
            != "tas_movie_establish_root_cursor"
        || validate_activation->unit_kind != "tas_movie_validate_root"
        || sterilize_activation->unit_kind != "tas_movie_checkpoint_sterilize"
        || probe_activation->unit_kind != "battle_seed_probe"
        || probe_activation->authored_ref_kind
            != std::optional<std::string>("seed_probe_spec")
        || probe_activation->authored_ref_id != seed_probe_spec_id
        || establish_step->step_kind
            != "tasmovie.establish_root_cursor"
        || validate_step->step_kind != "tasmovie.validate_root"
        || sterilize_step->step_kind != "tasmovie.checkpoint_sterilize"
        || probe_step->step_kind != "seedprobe.run"
        || establish_step->max_attempts != 1
        || validate_step->max_attempts != 1
        || sterilize_step->max_attempts != 1
        || probe_step->max_attempts != 1) {
        if (error_out) {
            *error_out = "tasmovie_seedprobe unit or singleton-step identities drifted";
        }
        return false;
    }
    const auto has_step_edge = [&](std::int64_t from, std::int64_t to) {
        return std::any_of(
            graph->edges.begin(), graph->edges.end(),
            [&](const auto& edge) {
                return edge.from_step_id == from
                    && edge.to_step_id == to;
            });
    };
    const auto has_activation_edge =
        [&](std::int64_t from, std::int64_t to) {
            return std::any_of(
                graph->unit_activation_edges.begin(),
                graph->unit_activation_edges.end(),
                [&](const auto& edge) {
                    return edge.from_workflow_unit_activation_id == from
                        && edge.to_workflow_unit_activation_id == to;
                });
        };
    if (!has_step_edge(
            establish_step->workflow_step_id,
            validate_step->workflow_step_id)
        || !has_step_edge(
            validate_step->workflow_step_id,
            sterilize_step->workflow_step_id)
        || !has_step_edge(
            sterilize_step->workflow_step_id,
            probe_step->workflow_step_id)
        || !has_activation_edge(
            establish_activation->workflow_unit_activation_id,
            validate_activation->workflow_unit_activation_id)
        || !has_activation_edge(
            validate_activation->workflow_unit_activation_id,
            sterilize_activation->workflow_unit_activation_id)
        || !has_activation_edge(
            sterilize_activation->workflow_unit_activation_id,
            probe_activation->workflow_unit_activation_id)) {
        if (error_out) {
            *error_out = "tasmovie_seedprobe dependency edges drifted";
        }
        return false;
    }
    if (require_initial_state
        && (establish_step->state != WorkflowStepState::Ready
            || validate_step->state != WorkflowStepState::Waiting
            || sterilize_step->state != WorkflowStepState::Waiting
            || probe_step->state != WorkflowStepState::Waiting)) {
        if (error_out) {
            *error_out = "tasmovie_seedprobe did not begin with only establishment READY";
        }
        return false;
    }

    const auto external_input = std::find_if(
        graph->input_bindings.begin(), graph->input_bindings.end(),
        [&](const auto& input) {
            return input.node_key == "tas_establish_1"
                && input.input_key == "root_dtm"
                && input.data_kind == "state_artifact.dtm_artifact_id"
                && input.ref_kind == "state_artifact"
                && input.ref_id == dtm_artifact_id
                && input.source_kind == "external";
        });
    const auto rtc_argument = std::find_if(
        graph->arguments.begin(), graph->arguments.end(),
        [&](const auto& argument) {
            return argument.node_key == "tas_validate_1"
                && argument.argument_key == "rtc"
                && argument.value_type == "integer"
                && argument.integer_value == rtc_value
                && !argument.text_value
                && argument.source_kind == "scenario";
        });
    const auto samples_argument = std::find_if(
        graph->arguments.begin(), graph->arguments.end(),
        [](const auto& argument) {
            return argument.node_key == "probe_1"
                && argument.argument_key == "samples_per_axis"
                && argument.value_type == "integer"
                && argument.integer_value.has_value()
                && *argument.integer_value > 0
                && !argument.text_value
                && argument.source_kind == "scenario";
        });
    if (external_input == graph->input_bindings.end()
        || rtc_argument == graph->arguments.end()
        || samples_argument == graph->arguments.end()) {
        if (error_out) {
            *error_out = "tasmovie_seedprobe external DTM, RTC, or SeedProbe sample binding drifted";
        }
        return false;
    }

    const auto authored = authoring_db->GetWorkflowGraphRevision(
        *graph->instance.workflow_graph_revision_id);
    if (!authored || authored->graph_hash
            != "savor-e2e.workflow_graph.tasmovie_seedprobe.v2"
        || authored->nodes.size() != 4 || authored->edges.size() != 3) {
        if (error_out) {
            *error_out = "tasmovie_seedprobe immutable authored graph is unavailable or drifted";
        }
        return false;
    }
    const auto has_guarded_edge = [&](std::string_view from,
                                      std::string_view output,
                                      std::string_view to,
                                      std::string_view input) {
        return std::any_of(
            authored->edges.begin(), authored->edges.end(),
            [&](const auto& edge) {
                return edge.from_node_key == from
                    && edge.output_key == output
                    && edge.to_node_key == to
                    && edge.input_key == input
                    && edge.guard_kind
                        == std::optional<std::string>(
                            savor::db::kWorkflowOutputPresentGuard)
                    && !edge.guard_value;
            });
    };
    if (!has_guarded_edge(
            "tas_establish_1", "established_root_cursor_attempt",
            "tas_validate_1", "root_establishment")
        || !has_guarded_edge(
            "tas_validate_1", "validated_checkpoint_savestate",
            "tas_sterilize_1", "paired_checkpoint_savestate")
        || !has_guarded_edge(
            "tas_sterilize_1", "sterilized_checkpoint_savestate",
            "probe_1", "entry_savestate")) {
        if (error_out) {
            *error_out = "tasmovie_seedprobe authored output_present edges drifted";
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
    std::string_view graph_node_key,
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
        if (output.graph_node_key == graph_node_key
            && output.output_key == "tas_movie_validation_attempt"
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
    const auto established_output = std::find_if(
        outputs.begin(), outputs.end(), [&](const auto& output) {
            return output.graph_node_key == graph_node_key
                && output.output_key == "established_root_cursor_attempt"
                && output.data_kind
                    == "analysis.tas_movie_validation_attempt_id"
                && output.ref_kind == "tmv_validation_attempt"
                && output.ref_id == attempt_outputs.front()->ref_id;
        });
    if (established_output == outputs.end()) {
        if (error_out) {
            *error_out = "completed establishment workflow lacks its exact success-specific cursor output";
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
    std::string_view graph_node_key,
    bool require_same_worker_process,
    VerifiedRootValidationAttempt* verified_out,
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
        if (output.graph_node_key == graph_node_key
            && output.output_key == "tas_movie_validation_attempt"
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
    if (attempt->workset_epoch == 0
        || establishment.workset_epoch == 0
        || (require_same_worker_process
            && (attempt->worker_id != establishment.worker_id
                || attempt->worker_process_generation
                    != establishment.worker_process_generation
                || attempt->workset_epoch
                    == establishment.workset_epoch))
        || (!require_same_worker_process
            && attempt->worker_id == establishment.worker_id
            && attempt->worker_process_generation
                == establishment.worker_process_generation
            && attempt->workset_epoch == establishment.workset_epoch)) {
        if (error_out) {
            *error_out = require_same_worker_process
                ? "sequential TAS Movie attempts did not use one worker process with distinct nonzero workset epochs"
                : "composed TAS Movie attempts did not record nonzero workset epochs";
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
        || !checkpoint->is_complete || checkpoint->artifact_kind != "SAV"
        || checkpoint->playback_state
            != savor::db::SavestatePlaybackState::MoviePaired
        || checkpoint->dtm_artifact_id != root->dtm_artifact_id
        || checkpoint->dtm_sha256 != dtm_artifact->sha256) {
        if (error_out) *error_out = "validated root DTM or canonical checkpoint identity is missing";
        return false;
    }
    const auto checkpoint_output = std::find_if(
        outputs.begin(), outputs.end(), [&](const auto& output) {
            return output.graph_node_key == graph_node_key
                && output.output_key == "validated_checkpoint_savestate"
                && output.data_kind == "state.movie_paired_savestate_id"
                && output.ref_kind == "state.savestate"
                && output.ref_id == root->checkpoint_savestate_id;
        });
    if (checkpoint_output == outputs.end()) {
        if (error_out) {
            *error_out = "Valid root validation lacks its exact checkpoint-savestate workflow output";
        }
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
    if (verified_out) {
        *verified_out = {
            .validation_attempt_id = attempt->validation_attempt_id,
            .tas_movie_root_id = root->tas_movie_root_id,
            .checkpoint_savestate_id = root->checkpoint_savestate_id,
            .effective_dtm_sha256 = expected_dtm_sha256,
            .worker_id = attempt->worker_id,
            .worker_process_generation = attempt->worker_process_generation,
            .workset_epoch = attempt->workset_epoch,
        };
    }
    return true;
}

bool VerifyCheckpointSterilization(
    savor::db::core::DBService* db_service,
    std::int64_t workflow_instance_id,
    const std::optional<
        savor::db::execution::workflow::WorkflowGraphSnapshot>& graph,
    std::int64_t source_savestate_id,
    VerifiedSterilizationAttempt* verified_out,
    std::string* error_out) {
    if (db_service == nullptr || !graph || source_savestate_id <= 0) {
        if (error_out) *error_out = "sterilization verification inputs are incomplete";
        return false;
    }
    const auto step = std::find_if(
        graph->steps.begin(), graph->steps.end(), [](const auto& candidate) {
            return candidate.graph_node_key == "tas_sterilize_1"
                && candidate.step_kind == "tasmovie.checkpoint_sterilize";
        });
    if (step == graph->steps.end()
        || step->state
            != savor::db::execution::workflow::WorkflowStepState::Completed) {
        if (error_out) *error_out = "checkpoint sterilization step did not complete";
        return false;
    }
    const auto request = db_service->AnalysisDb()
        ->GetTasMovieCheckpointSterilizationRequestForWorkflowStep(
            step->workflow_step_id);
    const auto phase = savor::runtime::tasmovie::
        TasMovieCheckpointSterilizationFullPhaseDefinitionV1();
    if (!request || !phase
        || request->source_savestate_id != source_savestate_id
        || request->full_phase_sha256 != phase->identity().canonical_sha256
        || request->module_sha256 != phase->runtime_contract().module.canonical_hash) {
        if (error_out) *error_out = "checkpoint sterilization request identity drifted";
        return false;
    }
    const auto outputs = db_service->ExecutionDb()
        ->WorkflowQueryService()->ListStepOutputs(workflow_instance_id);
    std::vector<const savor::db::execution::workflow::WorkflowStepOutputRecord*>
        matching;
    for (const auto& output : outputs) {
        if (output.graph_node_key == "tas_sterilize_1"
            && output.output_key == "sterilized_checkpoint_savestate"
            && output.data_kind == "state.movie_inactive_savestate_id"
            && output.ref_kind == "state.savestate" && output.ref_id > 0) {
            matching.push_back(&output);
        }
    }
    if (matching.size() != 1) {
        if (error_out) *error_out = "sterilization did not emit exactly one movie-inactive checkpoint";
        return false;
    }
    const auto result = db_service->StateDb()->GetSavestate(matching.front()->ref_id);
    const auto derivation = db_service->StateDb()
        ->FindSavestateDerivationBySourceAndMethod(
            source_savestate_id,
            savor::runtime::tasmovie::SterilizationDerivationMethod);
    const auto source = db_service->StateDb()->GetSavestate(source_savestate_id);
    if (!source || source->playback_state
            != savor::db::SavestatePlaybackState::MoviePaired
        || !source->dtm_artifact_id || !result || !result->is_complete
        || result->playback_state
            != savor::db::SavestatePlaybackState::MovieInactive
        || result->dtm_artifact_id || result->artifact_kind != "SAV"
        || !derivation || derivation->to_savestate_id != result->savestate_id
        || std::filesystem::exists(result->artifact_filename + ".dtm")) {
        if (error_out) *error_out = "sterilized checkpoint State or lineage evidence drifted";
        return false;
    }
    const auto result_hash = HashFile(result->artifact_filename, error_out);
    const auto source_hash = HashFile(source->artifact_filename, error_out);
    const auto source_sidecar_hash = HashFile(
        source->artifact_filename + ".dtm", error_out);
    if (!result_hash || *result_hash != result->artifact_sha256
        || !source_hash || *source_hash != source->artifact_sha256
        || !source_sidecar_hash || source->dtm_sha256 != *source_sidecar_hash) {
        if (error_out && error_out->empty())
            *error_out = "sterilization artifacts do not match State hashes";
        return false;
    }

    std::int64_t attempt_id = 0;
    std::uint64_t workset_epoch = 0;
    if (request->reused_savestate_id) {
        if (*request->reused_savestate_id != result->savestate_id
            || step->job_set_id == std::nullopt) {
            if (error_out) *error_out = "cached sterilization output drifted";
            return false;
        }
    } else {
        if (!step->job_set_id) {
            if (error_out) *error_out = "executed sterilization lacks a job set";
            return false;
        }
        const auto jobs = db_service->ExecutionDb()->ListJobsInJobSet(*step->job_set_id);
        if (jobs.size() != 1) {
            if (error_out) *error_out = "executed sterilization is not singleton";
            return false;
        }
        const auto job = db_service->ExecutionDb()->GetJob(jobs.front().job_id);
        const auto attempt = job && job->worker_terminal_fingerprint
            ? db_service->AnalysisDb()->FindTasMovieCheckpointSterilizationAttempt(
                  job->job_id, *job->worker_terminal_fingerprint)
            : std::nullopt;
        if (!attempt || attempt->sterilization_request_id
                != request->sterilization_request_id
            || attempt->produced_savestate_id != result->savestate_id
            || attempt->candidate_savestate_sha256 != result->artifact_sha256) {
            if (error_out) *error_out = "sterilization attempt evidence is missing or drifted";
            return false;
        }
        attempt_id = attempt->sterilization_attempt_id;
        workset_epoch = attempt->workset_epoch;
    }
    if (verified_out) {
        *verified_out = {
            .savestate_id = result->savestate_id,
            .sterilization_attempt_id = attempt_id,
            .workset_epoch = workset_epoch,
        };
    }
    std::cout << "[tasmovie-checkpoint-sterilized] source="
        << source_savestate_id << " savestate=" << result->savestate_id
        << " attempt=" << attempt_id << " no_sidecar=true\n";
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
        options.wait_for_workers_ready,
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
        options.wait_for_workers_ready,
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
                "tas_1",
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
        "tas_validate_1",
        true,
        nullptr,
        error_out);
}

bool RunComposedTasMovieSeedProbeScenario(
    const CliOptions& options,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out) {
    if (db_service == nullptr || !db_service->IsRunning()
        || !options.tasmovie_rtc
        || *options.tasmovie_rtc < 0
        || static_cast<std::uint64_t>(*options.tasmovie_rtc)
            > std::numeric_limits<std::uint32_t>::max()) {
        if (error_out) {
            *error_out = "tasmovie_seedprobe requires a running DB service and one exact GameCube RTC";
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
    std::int64_t seed_probe_spec_id = 0;
    std::int64_t workflow_instance_id = 0;
    if (!SeedStateDtmArtifact(
            db_service->StateDb(), options.dtm_file,
            &dtm_artifact_id, &error)
        || !SeedAuthoringSpec(
            db_service->AuthoringDb(), options,
            &seed_probe_spec_id, &error)
        || !SeedTasMovieSeedProbeWorkflow(
            db_service->AuthoringDb(), db_service->ExecutionDb(),
            dtm_artifact_id, seed_probe_spec_id,
            *options.tasmovie_rtc, options,
            &workflow_instance_id, &error)) {
        if (error_out) {
            *error_out = "failed seeding composed TAS Movie/SeedProbe workflow: "
                + error;
        }
        return false;
    }
    const auto initial_graph = db_service->ExecutionDb()
        ->WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
    if (!VerifyComposedTasMovieSeedProbeGraph(
            db_service->AuthoringDb(), initial_graph,
            dtm_artifact_id, seed_probe_spec_id,
            *options.tasmovie_rtc, true, error_out)) {
        return false;
    }

    const auto scenario_workspace_root = options.workspace_root.value_or(
        std::filesystem::temp_directory_path() / "savor-e2e-default");
    auto registry_config =
        savor::db::execution::programdb::
            MakeProductionProgramKindRegistryConfig(
                scenario_workspace_root / "workflow-runtime");
    registry_config.tas_movie_validation.working_dir_root =
        scenario_workspace_root / "tasmovie-validation";
    savor::db::execution::programdb::ProgramKindRegistry registry;
    if (!savor::db::execution::programdb::
            BuildProductionProgramKindRegistry(
                savor::db::execution::programdb::
                    ProductionProgramKindRegistryDependencies{
                        .execution_db = db_service->ExecutionDb(),
                        .state_db = db_service->StateDb(),
                        .analysis_db = db_service->AnalysisDb(),
                        .authoring_db = db_service->AuthoringDb(),
                    },
                std::move(registry_config), &registry, &error)) {
        if (error_out) {
            *error_out = "failed building production program registry: "
                + error;
        }
        return false;
    }
    const auto seed_probe_phase =
        savor::runtime::seedprobe::SeedProbeFullPhaseDefinitionV2();
    if (!seed_probe_phase || !seed_probe_phase->identity()) {
        if (error_out) {
            *error_out = "production SeedProbe Full Phase definition is unavailable";
        }
        return false;
    }
    const auto expected_seed_probe_module =
        seed_probe_phase->runtime_contract().module;

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
        std::deque<std::string> lines;
        std::lock_guard<std::mutex> lock(lines_mtx);
        std::swap(lines, pending_lines);
        return lines;
    };

    std::string iso_sha256;
    try {
        iso_sha256 = hash::sha256_of_file(options.iso_path.string());
    } catch (const std::exception& exception) {
        if (error_out) {
            *error_out = "failed hashing combined E2E ISO: "
                + std::string(exception.what());
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
        .desired_workers = static_cast<std::size_t>(options.worker_count),
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
        options.wait_for_workers_ready,
        [&](bool paused) { coordinators.SetExecutionPaused(paused); },
        [&](const std::string& line) { push_line(line); });
    if (!coordinators.Start(
            db_service->ExecutionDb(), db_service->AuthoringDb(),
            &registry, std::move(worker_config),
            scenario_workspace_root / "object_store",
            std::chrono::milliseconds(
                std::max<std::int64_t>(1, options.poll_ms)),
            std::move(state_compatibility),
            [&](const std::string& line) { push_line(line); },
            &error)) {
        std::string terminal_error;
        (void)TerminalFailInitialWorkerPoolWorkflows(
            db_service->ExecutionDb(), workflow_instance_id,
            coordinators.SnapshotFleetStartup(), &terminal_error);
        if (error_out) {
            *error_out = "split combined coordinator startup failed: "
                + error;
            if (!terminal_error.empty()) {
                *error_out += "; workflow terminal failure: "
                    + terminal_error;
            }
        }
        return false;
    }
    const auto startup_barrier = WaitForInitialWorkerPool(
        options.wait_for_workers_ready,
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
            db_service->ExecutionDb(), workflow_instance_id,
            startup_barrier.snapshot, &terminal_error);
        if (error_out) {
            *error_out = startup_barrier.diagnostic;
            if (!stop_error.empty()) *error_out += "; shutdown: " + stop_error;
            if (!terminal_error.empty()) {
                *error_out += "; workflow terminal failure: " + terminal_error;
            }
        }
        return false;
    }

    bool workflow_completed = false;
    std::string terminal_error;
    std::size_t poll_count = 0;
    while (true) {
        ++poll_count;
        for (const auto& line : drain_lines()) {
            std::cout << line << '\n';
        }
        const auto graph = db_service->ExecutionDb()
            ->WorkflowQueryService()->GetWorkflowGraph(
                workflow_instance_id);
        if (poll_count == 1 || poll_count % 10 == 0) {
            const auto telemetry = coordinators.SnapshotTelemetry();
            std::cout << "[tasmovie-seedprobe] "
                << (graph ? FormatWorkflowStateLine(*graph)
                          : std::string("workflow=unavailable"))
                << " ready_workers="
                << coordinators.SnapshotReadyWorkers().size()
                << " worksets_submitted="
                << telemetry.execution.worksets_submitted
                << " terminals="
                << telemetry.execution.worker_terminals_observed
                << '\n';
        }
        if (graph) {
            if (FindTerminalInfrastructureJobFailure(
                    db_service->ExecutionDb(), *graph,
                    &terminal_error)) {
                break;
            }
            using savor::db::execution::workflow::WorkflowInstanceState;
            if (graph->instance.state == WorkflowInstanceState::Completed) {
                workflow_completed = true;
                break;
            }
            if (graph->instance.state == WorkflowInstanceState::Failed
                || graph->instance.state
                    == WorkflowInstanceState::Canceled) {
                terminal_error = "composed workflow reached "
                    + std::string(ToString(graph->instance.state));
                break;
            }
        }
        const auto worker_start = coordinators.SnapshotWorkerStartResult();
        if (worker_start.status
            == savor::runner::parallel::savordb::
                WorkerCoordinatorStartStatus::StartupExhausted) {
            terminal_error = worker_start.diagnostic.empty()
                ? "all worker startup attempts were exhausted"
                : worker_start.diagnostic;
            break;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(options.poll_ms));
    }

    if (workflow_completed) {
        std::size_t drain_polls = 0;
        while (true) {
            std::string quiescence;
            const bool quiescent = CheckWorkflowQuiescence(
                db_service->ExecutionDb(), &quiescence);
            const auto telemetry = coordinators.SnapshotTelemetry();
            const bool reconciled =
                telemetry.execution.worksets_submitted > 0
                && telemetry.execution.draining_transitions
                    == telemetry.execution.worksets_submitted
                && telemetry.execution.worker_terminal_acks
                    == telemetry.execution.worker_terminals_staged
                && telemetry.execution.worker_terminals_observed
                    == telemetry.execution.worker_terminals_staged;
            if (quiescent && reconciled) break;
            ++drain_polls;
            if (drain_polls % 10 == 0) {
                std::cout << "[tasmovie-seedprobe-draining] "
                    << quiescence
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

    const auto final_graph = db_service->ExecutionDb()
        ->WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
    const auto final_telemetry = coordinators.SnapshotTelemetry();
    const auto final_ready_workers = coordinators.SnapshotReadyWorkers();
    const auto final_warnings = coordinators.SnapshotExecutionWarnings();
    std::string stop_error;
    const bool stopped = coordinators.Stop(&stop_error);
    for (const auto& line : drain_lines()) {
        std::cout << line << '\n';
    }
    if (!stopped) {
        if (error_out) {
            *error_out = "split combined coordinator shutdown failed: "
                + stop_error;
        }
        return false;
    }
    if (!workflow_completed) {
        if (error_out) {
            *error_out = terminal_error.empty()
                ? "composed workflow stopped before completion"
                : terminal_error;
        }
        return false;
    }
    if (!VerifyComposedTasMovieSeedProbeGraph(
            db_service->AuthoringDb(), final_graph,
            dtm_artifact_id, seed_probe_spec_id,
            *options.tasmovie_rtc, false, error_out)) {
        return false;
    }

    VerifiedRootCursorAttempt establishment{};
    VerifiedRootValidationAttempt validation{};
    VerifiedSterilizationAttempt sterilization{};
    if (!VerifyRootCursorAttempt(
            db_service, workflow_instance_id, dtm_artifact_id,
            options.dtm_file, scenario_workspace_root,
            "tas_establish_1", &establishment, error_out)
        || !VerifyRootValidationAttempt(
            db_service, workflow_instance_id, dtm_artifact_id,
            options.dtm_file, *options.tasmovie_rtc,
            establishment, "tas_validate_1", false,
            &validation, error_out)
        || !VerifyCheckpointSterilization(
            db_service, workflow_instance_id, final_graph,
            validation.checkpoint_savestate_id,
            &sterilization, error_out)) {
        return false;
    }

    SeedProbeInfrastructureHealth infrastructure_health =
        SeedProbeInfrastructureHealth::Failed;
    std::vector<std::string> infrastructure_issues;
    std::string seed_probe_error;
    const bool seed_probe_valid = ValidateSeedProbeWorkflowExecution(
        db_service->ExecutionDb(), db_service->AnalysisDb(), final_graph,
        final_telemetry, final_ready_workers,
        expected_seed_probe_module,
        SeedProbeWorkflowValidationOptions{
            .graph_node_key = "probe_1",
            .expected_entry_savestate_id =
                sterilization.savestate_id,
            .require_single_seedprobe_step = false,
        },
        &infrastructure_health, &infrastructure_issues,
        &seed_probe_error);
    if (!seed_probe_valid
        || infrastructure_health != SeedProbeInfrastructureHealth::Clean
        || !infrastructure_issues.empty() || !final_warnings.empty()) {
        if (error_out) {
            std::ostringstream diagnostic;
            diagnostic << "composed TAS Movie/SeedProbe validation failed";
            if (!seed_probe_error.empty()) {
                diagnostic << ": " << seed_probe_error;
            }
            diagnostic << " infrastructure_issues="
                << infrastructure_issues.size()
                << " coordinator_warnings=" << final_warnings.size();
            for (const auto& issue : infrastructure_issues) {
                diagnostic << "\n  - " << issue;
            }
            for (const auto& warning : final_warnings) {
                diagnostic << "\n  - coordinator warning "
                    << warning.sequence
                    << " worker_id=" << warning.worker_id
                    << " job_id=" << warning.job_id
                    << ": " << warning.message;
                if (!warning.detail.empty()) {
                    diagnostic << " (" << warning.detail << ')';
                }
            }
            *error_out = diagnostic.str();
        }
        return false;
    }

    std::cout << "[tasmovie-seedprobe-valid] workflow="
        << workflow_instance_id
        << " checkpoint=" << validation.checkpoint_savestate_id
        << " sterilized_checkpoint=" << sterilization.savestate_id
        << " rtc=" << *options.tasmovie_rtc
        << " worksets=" << final_telemetry.execution.worksets_submitted
        << " ready_workers=" << final_ready_workers.size()
        << " coordinator_warnings=0\n";
    return true;
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

bool RunTasMovieSeedProbeRealWorkerSmoke(
    const CliOptions& options,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out) {
    return RunComposedTasMovieSeedProbeScenario(
        options, argv0, db_service, error_out);
}

} // namespace savor::e2e
