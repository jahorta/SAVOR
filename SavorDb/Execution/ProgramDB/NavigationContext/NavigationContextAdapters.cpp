#include "NavigationContextAdapters.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../IExecutionDb.h"
#include "../../Jobs/JobEventOrchestration.h"
#include "../../Workflow/WorkflowOrchestration.h"
#include "../../../Common/Types/UtcTimestamp.h"
#include "../../../State/IStateDb.h"
#include "../../../../SavorCore/Core/Memory/Soa/Navigation/NavigationContext.h"
#include "../../../../SavorCore/Core/Memory/Soa/Navigation/NavigationContextCodec.h"
#include "../../../../SavorCore/Phases/Programs/NavigationContext/NavigationContextPayload.h"
#include "../../../../SavorCore/Phases/Programs/NavigationContext/NavigationContextResult.h"
#include "../../../../SavorCore/Runner/Breakpoints/BpRegistry.h"
#include "../../../../SavorCore/Runner/IPC/Wire.h"
#include "../../../../SavorCore/Runner/Parallel/PRTypes.h"
#include "../../../../SavorCore/Runner/Script/CtxRegistry.h"
#include "../../../../SavorCore/Utils/Base64.h"
#include "../../../../SavorCore/Utils/Hash.h"
#include "../../../../SavorCore/Utils/IniDoc.h"

namespace savor::db::execution::programdb::navigationcontext {
namespace {

constexpr const char* kJobSection = "NavigationContext.Job";
constexpr const char* kResultsSection = "NavigationContext.Results";
constexpr const char* kFailedResultKind = "state.navigation_context.failed";
constexpr const char* kOutputSavestateType = "NAVIGATION_CONTEXT";
constexpr const char* kOutputSavestateNote =
    "Navigation context captured at initial player input";

std::string String(std::string_view value) {
    return std::string(value);
}

struct NavigationContextJobIni {
    std::uint32_t version = 0;
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::int64_t workflow_unit_activation_id = 0;
    std::int64_t workflow_graph_revision_id = 0;
    std::string step_key;
    std::string graph_node_key;
    std::int64_t source_savestate_id = 0;
    std::int64_t source_artifact_id = 0;
    std::int64_t source_artifact_size_bytes = 0;
    std::string source_artifact_sha256;
    std::uint32_t run_timeout_ms = 0;
    bool required_keys_present = false;

    std::string Encode() const {
        IniDoc ini;
        ini.set(kJobSection, "version", std::to_string(version));
        ini.set(
            kJobSection,
            "workflow_instance_id",
            std::to_string(workflow_instance_id));
        ini.set(kJobSection, "workflow_step_id", std::to_string(workflow_step_id));
        ini.set(
            kJobSection,
            "workflow_unit_activation_id",
            std::to_string(workflow_unit_activation_id));
        ini.set(
            kJobSection,
            "workflow_graph_revision_id",
            std::to_string(workflow_graph_revision_id));
        ini.set(kJobSection, "step_key", step_key);
        ini.set(kJobSection, "graph_node_key", graph_node_key);
        ini.set(
            kJobSection,
            "source_savestate_id",
            std::to_string(source_savestate_id));
        ini.set(
            kJobSection,
            "source_artifact_id",
            std::to_string(source_artifact_id));
        ini.set(
            kJobSection,
            "source_artifact_size_bytes",
            std::to_string(source_artifact_size_bytes));
        ini.set(kJobSection, "source_artifact_sha256", source_artifact_sha256);
        ini.set(kJobSection, "run_timeout_ms", std::to_string(run_timeout_ms));
        return ini.to_string_sorted();
    }

    static NavigationContextJobIni Decode(const std::string& text) {
        const auto ini = IniDoc::parse(text);
        NavigationContextJobIni out{};
        out.required_keys_present =
            ini.has(kJobSection, "version")
            && ini.has(kJobSection, "workflow_instance_id")
            && ini.has(kJobSection, "workflow_step_id")
            && ini.has(kJobSection, "workflow_unit_activation_id")
            && ini.has(kJobSection, "workflow_graph_revision_id")
            && ini.has(kJobSection, "step_key")
            && ini.has(kJobSection, "graph_node_key")
            && ini.has(kJobSection, "source_savestate_id")
            && ini.has(kJobSection, "source_artifact_id")
            && ini.has(kJobSection, "source_artifact_size_bytes")
            && ini.has(kJobSection, "source_artifact_sha256")
            && ini.has(kJobSection, "run_timeout_ms");
        out.version = ini.get_u32(kJobSection, "version", 0);
        out.workflow_instance_id =
            ini.get_i64(kJobSection, "workflow_instance_id", 0);
        out.workflow_step_id =
            ini.get_i64(kJobSection, "workflow_step_id", 0);
        out.workflow_unit_activation_id =
            ini.get_i64(kJobSection, "workflow_unit_activation_id", 0);
        out.workflow_graph_revision_id =
            ini.get_i64(kJobSection, "workflow_graph_revision_id", 0);
        out.step_key = ini.get(kJobSection, "step_key", "");
        out.graph_node_key = ini.get(kJobSection, "graph_node_key", "");
        out.source_savestate_id =
            ini.get_i64(kJobSection, "source_savestate_id", 0);
        out.source_artifact_id =
            ini.get_i64(kJobSection, "source_artifact_id", 0);
        out.source_artifact_size_bytes =
            ini.get_i64(kJobSection, "source_artifact_size_bytes", 0);
        out.source_artifact_sha256 =
            ini.get(kJobSection, "source_artifact_sha256", "");
        out.run_timeout_ms = ini.get_u32(kJobSection, "run_timeout_ms", 0);
        return out;
    }

    bool IsValid() const {
        return required_keys_present
            && version == phase::navigation::ctx::PayloadVersion
            && workflow_instance_id > 0
            && workflow_step_id > 0
            && workflow_unit_activation_id > 0
            && workflow_graph_revision_id > 0
            && !step_key.empty()
            && !graph_node_key.empty()
            && source_savestate_id > 0
            && source_artifact_id > 0
            && source_artifact_size_bytes > 0
            && !source_artifact_sha256.empty()
            && run_timeout_ms > 0;
    }
};

struct NavigationContextResultIni {
    bool ps_ok = false;
    std::uint32_t w_err = savor::WERR_UnknownError;
    std::uint32_t dw_err =
        static_cast<std::uint32_t>(savor::RunToBpOutcome::Unknown);
    std::uint32_t entry_pc = 0;
    std::uint32_t hit_pc = 0;
    std::uint32_t hit_bp_key = 0;
    std::uint32_t outcome =
        static_cast<std::uint32_t>(phase::navigation::ctx::Outcome::Failed);
    std::uint32_t failure =
        static_cast<std::uint32_t>(
            phase::navigation::ctx::FailureCode::UnexpectedStop);
    std::string savestate_path;
    std::string context_base64;
    std::string diagnostic_base64;
    bool context_complete = false;
    bool required_keys_present = false;

    std::string Encode() const {
        IniDoc ini;
        ini.set(kResultsSection, "ps_ok", ps_ok ? "1" : "0");
        ini.set(kResultsSection, "w_err", std::to_string(w_err));
        ini.set(kResultsSection, "dw_err", std::to_string(dw_err));
        ini.set(kResultsSection, "entry_pc", std::to_string(entry_pc));
        ini.set(kResultsSection, "hit_pc", std::to_string(hit_pc));
        ini.set(kResultsSection, "hit_bp_key", std::to_string(hit_bp_key));
        ini.set(kResultsSection, "outcome", std::to_string(outcome));
        ini.set(kResultsSection, "failure", std::to_string(failure));
        ini.set(kResultsSection, "savestate_path", savestate_path);
        ini.set(kResultsSection, "context_base64", context_base64);
        ini.set(kResultsSection, "diagnostic_base64", diagnostic_base64);
        ini.set(
            kResultsSection,
            "context_complete",
            context_complete ? "1" : "0");
        return ini.to_string_sorted();
    }

    static NavigationContextResultIni Decode(const std::string& text) {
        const auto ini = IniDoc::parse(text);
        NavigationContextResultIni out{};
        out.required_keys_present =
            ini.has(kResultsSection, "ps_ok")
            && ini.has(kResultsSection, "w_err")
            && ini.has(kResultsSection, "dw_err")
            && ini.has(kResultsSection, "entry_pc")
            && ini.has(kResultsSection, "hit_pc")
            && ini.has(kResultsSection, "hit_bp_key")
            && ini.has(kResultsSection, "outcome")
            && ini.has(kResultsSection, "failure")
            && ini.has(kResultsSection, "savestate_path")
            && ini.has(kResultsSection, "context_base64")
            && ini.has(kResultsSection, "diagnostic_base64")
            && ini.has(kResultsSection, "context_complete");
        out.ps_ok = ini.get_i64(kResultsSection, "ps_ok", 0) != 0;
        out.w_err = ini.get_u32(
            kResultsSection,
            "w_err",
            savor::WERR_UnknownError);
        out.dw_err = ini.get_u32(
            kResultsSection,
            "dw_err",
            static_cast<std::uint32_t>(savor::RunToBpOutcome::Unknown));
        out.entry_pc = ini.get_u32(kResultsSection, "entry_pc", 0);
        out.hit_pc = ini.get_u32(kResultsSection, "hit_pc", 0);
        out.hit_bp_key = ini.get_u32(kResultsSection, "hit_bp_key", 0);
        out.outcome = ini.get_u32(
            kResultsSection,
            "outcome",
            static_cast<std::uint32_t>(
                phase::navigation::ctx::Outcome::Failed));
        out.failure = ini.get_u32(
            kResultsSection,
            "failure",
            static_cast<std::uint32_t>(
                phase::navigation::ctx::FailureCode::UnexpectedStop));
        out.savestate_path =
            ini.get(kResultsSection, "savestate_path", "");
        out.context_base64 =
            ini.get(kResultsSection, "context_base64", "");
        out.diagnostic_base64 =
            ini.get(kResultsSection, "diagnostic_base64", "");
        out.context_complete =
            ini.get_i64(kResultsSection, "context_complete", 0) != 0;
        return out;
    }
};

std::filesystem::path WorkingRoot(const std::filesystem::path& configured) {
    if (!configured.empty()) {
        return configured;
    }
    return std::filesystem::temp_directory_path()
        / "savordb-navigation-context";
}

std::filesystem::path OutputSavestatePath(
    const std::filesystem::path& root,
    std::int64_t job_id) {
    return root / ("job-" + std::to_string(job_id))
        / "output" / "navigation_context.sav";
}

std::filesystem::path ContextArtifactPath(
    const std::filesystem::path& root,
    std::int64_t job_id) {
    return root / ("job-" + std::to_string(job_id))
        / "context" / "navigation_context.nctx";
}

std::optional<std::string> HashFile(const std::filesystem::path& path) {
    try {
        return hash::sha256_of_file(path.string());
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::int64_t FileSize(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    return error ? 0 : static_cast<std::int64_t>(size);
}

bool IsRegularFile(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

bool SamePath(
    const std::filesystem::path& lhs,
    const std::filesystem::path& rhs) {
    std::error_code lhs_error;
    std::error_code rhs_error;
    const auto canonical_lhs = std::filesystem::weakly_canonical(lhs, lhs_error);
    const auto canonical_rhs = std::filesystem::weakly_canonical(rhs, rhs_error);
    return !lhs_error && !rhs_error && canonical_lhs == canonical_rhs;
}

bool ValidFrozenState(
    const savor::db::SavestateRecord& state,
    const NavigationContextJobIni& input) {
    if (!state.is_complete
        || state.savestate_id != input.source_savestate_id
        || state.artifact_id != input.source_artifact_id
        || state.artifact_kind != "SAV"
        || state.artifact_size_bytes != input.source_artifact_size_bytes
        || state.artifact_sha256 != input.source_artifact_sha256
        || !IsRegularFile(state.artifact_filename)
        || FileSize(state.artifact_filename) != state.artifact_size_bytes) {
        return false;
    }
    const auto current_hash = HashFile(state.artifact_filename);
    return current_hash.has_value()
        && *current_hash == state.artifact_sha256;
}

const WorkflowGraphInputBinding* FindInputBinding(
    const WorkflowGraphStepScheduleContext& context) {
    const auto it = std::find_if(
        context.input_bindings.begin(),
        context.input_bindings.end(),
        [](const auto& binding) {
            return binding.input_key == InputKey
                && binding.data_kind == InputDataKind
                && binding.ref_kind == "state.savestate"
                && binding.ref_id > 0;
        });
    return it == context.input_bindings.end() ? nullptr : &*it;
}

WorkflowStepScheduleResult ScheduleFailure(
    const WorkflowGraphStepScheduleContext& context,
    std::string reason) {
    WorkflowStepScheduleResult result{};
    result.event_lines.push_back(
        "[navigation-context-enqueue] ok=false reason=" + std::move(reason)
        + " workflow_instance_id="
        + std::to_string(context.workflow_instance_id)
        + " workflow_step_id=" + std::to_string(context.workflow_step_id)
        + " step=" + context.step_key);
    return result;
}

bool MatchesJob(
    const savor::db::ExecutionJobRecord& job,
    const NavigationContextJobIni& input) {
    return input.IsValid()
        && job.program_kind
            == static_cast<std::int32_t>(savor::PK_NavigationContextRunner)
        && job.program_version
            == static_cast<std::int32_t>(
                phase::navigation::ctx::PayloadVersion)
        && job.program_ref_kind == ProgramRefKind
        && job.program_ref_id == input.source_savestate_id
        && job.savestate_id
            == std::optional<std::int64_t>(input.source_savestate_id);
}

bool MatchesWorkflowIdentity(
    savor::db::IExecutionDb* execution_db,
    const savor::db::ExecutionJobRecord& job,
    const NavigationContextJobIni& input) {
    if (execution_db == nullptr || execution_db->WorkflowQueryService() == nullptr) {
        return false;
    }
    const auto graph =
        execution_db->WorkflowQueryService()->GetWorkflowGraph(
            input.workflow_instance_id);
    if (!graph.has_value()
        || graph->instance.workflow_instance_id != input.workflow_instance_id
        || graph->instance.workflow_graph_revision_id
            != std::optional<std::int64_t>(
                input.workflow_graph_revision_id)) {
        return false;
    }
    const auto step_it = std::find_if(
        graph->steps.begin(),
        graph->steps.end(),
        [&](const auto& step) {
            return step.workflow_step_id == input.workflow_step_id;
        });
    if (step_it == graph->steps.end()
        || step_it->workflow_instance_id != input.workflow_instance_id
        || step_it->step_key != input.step_key
        || step_it->graph_node_key != input.graph_node_key
        || step_it->step_kind != StepKind
        || step_it->job_set_id != std::optional<std::int64_t>(job.job_set_id)
        || step_it->workflow_unit_activation_id
            != std::optional<std::int64_t>(
                input.workflow_unit_activation_id)) {
        return false;
    }
    const auto activation_it = std::find_if(
        graph->unit_activations.begin(),
        graph->unit_activations.end(),
        [&](const auto& activation) {
            return activation.workflow_unit_activation_id
                == input.workflow_unit_activation_id;
        });
    if (activation_it == graph->unit_activations.end()
        || activation_it->workflow_instance_id
            != input.workflow_instance_id
        || activation_it->graph_node_key != input.graph_node_key
        || activation_it->unit_kind != UnitKind) {
        return false;
    }
    return true;
}

bool AppendTerminal(
    savor::db::IExecutionDb* execution_db,
    std::int64_t job_id,
    bool succeeded,
    std::vector<std::string>* event_lines) {
    std::string error;
    const bool ok = execution_db != nullptr
        && execution_db->JobCommandService() != nullptr
        && job_id > 0
        && execution_db->JobCommandService()->AppendLifecycleEvent(
            {
                .kind =
                    savor::db::execution::jobs::JobLifecycleEventKind::
                        JobCompleted,
                .job_id = job_id,
                .terminal_state =
                    std::string(succeeded ? "SUCCEEDED" : "FAILED"),
                .requested_by = "navigation_context_result_mapper",
            },
            &error);
    if (event_lines != nullptr) {
        event_lines->push_back(
            "[navigation-context-job-terminal] job=" + std::to_string(job_id)
            + " state=" + (succeeded ? "SUCCEEDED" : "FAILED")
            + " ok=" + (ok ? "true" : "false")
            + (error.empty() ? "" : " error=" + error));
    }
    return ok;
}

bool WriteBytes(
    const std::filesystem::path& path,
    std::string_view bytes,
    std::string* error_out) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        if (error_out != nullptr) {
            *error_out = "failed to create context directory: "
                + error.message();
        }
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output.good()) {
        if (error_out != nullptr) {
            *error_out = "failed to write navigation context artifact";
        }
        return false;
    }
    return true;
}

std::optional<std::int64_t> StoreContextArtifact(
    savor::db::IStateDb* state_db,
    const std::filesystem::path& path,
    std::string_view bytes,
    std::int64_t job_id,
    std::vector<std::string>* event_lines) {
    std::string error;
    if (state_db == nullptr || bytes.empty()
        || !WriteBytes(path, bytes, &error)) {
        if (event_lines != nullptr) {
            event_lines->push_back(
                "[navigation-context-artifact] ok=false stage=write error="
                + error);
        }
        return std::nullopt;
    }
    const auto sha256 = HashFile(path);
    const auto size = FileSize(path);
    if (!sha256.has_value() || sha256->empty() || size <= 0) {
        if (event_lines != nullptr) {
            event_lines->push_back(
                "[navigation-context-artifact] ok=false stage=identity");
        }
        return std::nullopt;
    }
    std::int64_t artifact_id = 0;
    if (!state_db->StoreArtifact(
            {
                .sha256 = *sha256,
                .size_bytes = size,
                .compression_kind = 0,
                .filename = std::filesystem::absolute(path).string(),
                .file_ext = soa::navigation::ctx::codec::ext,
                .artifact_kind = "OTHER",
                .created_at_utc = savor::db::types::UtcNow(),
                .correlation_id =
                    "navigation-context-job-" + std::to_string(job_id),
                .causation_id = "job-" + std::to_string(job_id),
            },
            &artifact_id,
            &error)
        || artifact_id <= 0) {
        if (event_lines != nullptr) {
            event_lines->push_back(
                "[navigation-context-artifact] ok=false stage=store error="
                + error);
        }
        return std::nullopt;
    }
    return artifact_id;
}

std::optional<std::int64_t> StoreOutputSavestate(
    savor::db::IStateDb* state_db,
    const std::filesystem::path& path,
    std::int64_t job_id,
    std::vector<std::string>* event_lines) {
    if (state_db == nullptr || !IsRegularFile(path)) {
        return std::nullopt;
    }
    const auto sha256 = HashFile(path);
    const auto size = FileSize(path);
    if (!sha256.has_value() || sha256->empty() || size <= 0) {
        return std::nullopt;
    }

    const auto now = savor::db::types::UtcNow();
    const auto correlation =
        "navigation-context-job-" + std::to_string(job_id);
    std::string error;
    std::int64_t artifact_id = 0;
    if (!state_db->StoreArtifact(
            {
                .sha256 = *sha256,
                .size_bytes = size,
                .compression_kind = 0,
                .filename = std::filesystem::absolute(path).string(),
                .file_ext = path.extension().string(),
                .artifact_kind = "SAV",
                .created_at_utc = now,
                .correlation_id = correlation,
                .causation_id = "job-" + std::to_string(job_id),
            },
            &artifact_id,
            &error)
        || artifact_id <= 0) {
        if (event_lines != nullptr) {
            event_lines->push_back(
                "[navigation-context-savestate] ok=false stage=artifact error="
                + error);
        }
        return std::nullopt;
    }

    std::int64_t savestate_id = 0;
    if (!state_db->CreateSavestate(
            {
                .artifact_id = artifact_id,
                .savestate_type = kOutputSavestateType,
                .note = kOutputSavestateNote,
                .is_complete = true,
                .created_at_utc = now,
                .correlation_id = correlation,
                .causation_id = "artifact-" + std::to_string(artifact_id),
            },
            &savestate_id,
            &error)
        || savestate_id <= 0) {
        if (event_lines != nullptr) {
            event_lines->push_back(
                "[navigation-context-savestate] ok=false stage=row error="
                + error);
        }
        return std::nullopt;
    }
    return savestate_id;
}

class NavigationContextGraphAdapter final
    : public IWorkflowGraphJobPersistenceAdapter {
public:
    NavigationContextGraphAdapter(
        savor::db::IExecutionDb* execution_db,
        savor::db::IStateDb* state_db,
        std::uint32_t run_timeout_ms)
        : execution_db_(execution_db)
        , state_db_(state_db)
        , run_timeout_ms_(run_timeout_ms) {
    }

    WorkflowStepScheduleResult EncodeForGraphQueueing(
        const WorkflowGraphStepScheduleContext& context) const override {
        if (execution_db_ == nullptr
            || state_db_ == nullptr
            || run_timeout_ms_ == 0
            || context.workflow_instance_id <= 0
            || context.workflow_step_id <= 0
            || !context.workflow_unit_activation_id.has_value()
            || *context.workflow_unit_activation_id <= 0
            || !context.workflow_graph_revision_id.has_value()
            || *context.workflow_graph_revision_id <= 0
            || context.unit_kind != UnitKind
            || context.step_kind != StepKind
            || context.step_key.empty()
            || context.activation_graph_node_key.empty()) {
            return ScheduleFailure(context, "invalid_context_or_db");
        }
        const auto* binding = FindInputBinding(context);
        if (binding == nullptr || context.input_bindings.size() != 1) {
            return ScheduleFailure(context, "invalid_entry_savestate_binding");
        }
        const auto source = state_db_->GetSavestate(binding->ref_id);
        if (!source.has_value()
            || !source->is_complete
            || source->artifact_id <= 0
            || source->artifact_kind != "SAV"
            || source->artifact_size_bytes <= 0
            || source->artifact_sha256.empty()
            || !IsRegularFile(source->artifact_filename)
            || FileSize(source->artifact_filename)
                != source->artifact_size_bytes) {
            return ScheduleFailure(context, "source_savestate_invalid");
        }
        const auto current_hash = HashFile(source->artifact_filename);
        if (!current_hash.has_value()
            || *current_hash != source->artifact_sha256) {
            return ScheduleFailure(context, "source_savestate_hash_mismatch");
        }

        NavigationContextJobIni input{};
        input.version = phase::navigation::ctx::PayloadVersion;
        input.workflow_instance_id = context.workflow_instance_id;
        input.workflow_step_id = context.workflow_step_id;
        input.workflow_unit_activation_id =
            context.workflow_unit_activation_id.value_or(0);
        input.workflow_graph_revision_id =
            context.workflow_graph_revision_id.value_or(0);
        input.step_key = context.step_key;
        input.graph_node_key = context.activation_graph_node_key;
        input.source_savestate_id = source->savestate_id;
        input.source_artifact_id = source->artifact_id;
        input.source_artifact_size_bytes = source->artifact_size_bytes;
        input.source_artifact_sha256 = source->artifact_sha256;
        input.run_timeout_ms = run_timeout_ms_;
        const auto input_ini = input.Encode();
        const auto fingerprint =
            "PK=" + std::to_string(savor::PK_NavigationContextRunner)
            + ";PV="
            + std::to_string(phase::navigation::ctx::PayloadVersion)
            + ";phase=navigation.context_probe"
            + ";workflow=" + std::to_string(context.workflow_instance_id)
            + ";step=" + std::to_string(context.workflow_step_id)
            + ";source=" + std::to_string(source->savestate_id)
            + ";sha="
            + hash::sha256(input_ini.data(), input_ini.size());

        const auto now = savor::db::types::UtcNow();
        std::string error;
        std::int64_t job_set_id = 0;
        if (!execution_db_->CreateJobSet(
                {
                    .program_kind = static_cast<std::int32_t>(
                        savor::PK_NavigationContextRunner),
                    .purpose = "Navigation Context Probe",
                    .created_by = "navigation.context_probe.graph_adapter",
                    .created_at_utc = now.time_since_epoch().count(),
                    .expected_total = 1,
                    .domain_ref_kind = String(ProgramRefKind),
                    .domain_ref_id = source->savestate_id,
                },
                &job_set_id,
                &error)
            || job_set_id <= 0) {
            return ScheduleFailure(
                context,
                "create_job_set_failed:" + error);
        }

        std::int64_t job_id = 0;
        if (!execution_db_->EnqueueJob(
                {
                    .job_set_id = job_set_id,
                    .program_kind = static_cast<std::int32_t>(
                        savor::PK_NavigationContextRunner),
                    .program_version = static_cast<std::int32_t>(
                        phase::navigation::ctx::PayloadVersion),
                    .program_ref_kind = String(ProgramRefKind),
                    .program_ref_id = source->savestate_id,
                    .savestate_id = source->savestate_id,
                    .fingerprint = fingerprint,
                    .priority = context.step_priority,
                    .max_attempts = 1,
                    .input_ini = input_ini,
                    .pending_until_workflow_materialized = true,
                },
                &job_id,
                &error)
            || job_id <= 0) {
            return ScheduleFailure(context, "enqueue_failed:" + error);
        }

        WorkflowStepScheduleResult result{};
        result.root_job_set_id = job_set_id;
        result.persistence = {
            .program_ref_kind = String(ProgramRefKind),
            .program_ref_id = source->savestate_id,
            .fingerprint = fingerprint,
            .program_version = static_cast<std::int32_t>(
                phase::navigation::ctx::PayloadVersion),
        };
        result.event_lines.push_back(
            "[navigation-context-enqueue] ok=true"
            " workflow_instance_id="
            + std::to_string(context.workflow_instance_id)
            + " workflow_step_id=" + std::to_string(context.workflow_step_id)
            + " source_savestate_id="
            + std::to_string(source->savestate_id)
            + " source_artifact_id=" + std::to_string(source->artifact_id)
            + " run_timeout_ms=" + std::to_string(run_timeout_ms_)
            + " job=" + std::to_string(job_id));
        return result;
    }

private:
    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IStateDb* state_db_ = nullptr;
    std::uint32_t run_timeout_ms_ = 0;
};

class NavigationContextRuntimeAdapter final : public IRuntimeInitAdapter {
public:
    NavigationContextRuntimeAdapter(
        savor::db::IExecutionDb* execution_db,
        savor::db::IStateDb* state_db,
        std::filesystem::path working_dir_root)
        : execution_db_(execution_db)
        , state_db_(state_db)
        , working_dir_root_(std::move(working_dir_root)) {
    }

    RuntimeInitRequest BuildRuntimeInit(std::int64_t job_id) const override {
        RuntimeInitRequest request{};
        request.bootstrap_profile = String(StepKind);
        if (execution_db_ == nullptr || state_db_ == nullptr) {
            return request;
        }
        const auto job = execution_db_->GetJob(job_id);
        if (!job.has_value()) {
            return request;
        }
        const auto input = NavigationContextJobIni::Decode(job->input_ini);
        const auto source =
            state_db_->GetSavestate(input.source_savestate_id);
        if (!MatchesJob(*job, input)
            || !MatchesWorkflowIdentity(execution_db_, *job, input)
            || !source.has_value()
            || !ValidFrozenState(*source, input)) {
            return request;
        }
        request.savestate_ref_kind = String(ProgramRefKind);
        request.savestate_ref_id = input.source_savestate_id;
        request.default_timeout_ms = input.run_timeout_ms;
        return request;
    }

    std::optional<savor::PSJob> MaterializePsJob(
        std::int64_t job_id,
        const RuntimeInitRequest& request) const override {
        if (execution_db_ == nullptr || state_db_ == nullptr) {
            return std::nullopt;
        }
        const auto job = execution_db_->GetJob(job_id);
        if (!job.has_value()) {
            return std::nullopt;
        }
        const auto input = NavigationContextJobIni::Decode(job->input_ini);
        const auto source =
            state_db_->GetSavestate(input.source_savestate_id);
        if (!MatchesJob(*job, input)
            || !MatchesWorkflowIdentity(execution_db_, *job, input)
            || request.savestate_ref_kind != ProgramRefKind
            || request.savestate_ref_id != input.source_savestate_id
            || request.default_timeout_ms != input.run_timeout_ms
            || !source.has_value()
            || !ValidFrozenState(*source, input)) {
            return std::nullopt;
        }

        const auto output = OutputSavestatePath(
            WorkingRoot(working_dir_root_),
            job_id);
        std::error_code error;
        std::filesystem::create_directories(output.parent_path(), error);
        if (error) {
            return std::nullopt;
        }
        phase::navigation::ctx::EncodeSpec spec{};
        spec.run_timeout_ms = input.run_timeout_ms;
        spec.output_savestate_path = output.string();
        savor::PSJob result{};
        if (!phase::navigation::ctx::encode_payload(spec, result.payload)) {
            return std::nullopt;
        }
        return result;
    }

private:
    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IStateDb* state_db_ = nullptr;
    std::filesystem::path working_dir_root_;
};

class NavigationContextResultMapper final : public IResultMapper {
public:
    NavigationContextResultMapper(
        savor::db::IExecutionDb* execution_db,
        savor::db::IStateDb* state_db,
        std::filesystem::path working_dir_root)
        : execution_db_(execution_db)
        , state_db_(state_db)
        , working_dir_root_(std::move(working_dir_root)) {
    }

    std::string BuildResultIniFromPrResult(
        std::int64_t,
        const savor::PRResult& result) const override {
        NavigationContextResultIni out{};
        out.ps_ok = result.ps.ok;
        out.w_err = result.ps.w_err;
        bool complete = result.ps.ctx.get(
            savor::context::key::core::DW_RUN_OUTCOME_CODE,
            out.dw_err);
        complete = result.ps.ctx.get(
            savor::context::key::navigation::ENTRY_PC,
            out.entry_pc) && complete;
        complete = result.ps.ctx.get(
            savor::context::key::core::RUN_HIT_PC,
            out.hit_pc) && complete;
        complete = result.ps.ctx.get(
            savor::context::key::core::RUN_HIT_BP_KEY,
            out.hit_bp_key) && complete;
        complete = result.ps.ctx.get(
            savor::context::key::navigation::OUTCOME,
            out.outcome) && complete;
        complete = result.ps.ctx.get(
            savor::context::key::navigation::FAILURE,
            out.failure) && complete;
        complete = result.ps.ctx.get(
            savor::context::key::core::LAST_SAVESTATE_PATH,
            out.savestate_path) && complete;
        std::string context_blob;
        const bool context_present = result.ps.ctx.get(
            savor::context::key::navigation::CTX_BLOB,
            context_blob);
        std::string diagnostic;
        const bool diagnostic_present = result.ps.ctx.get(
            savor::context::key::navigation::DIAGNOSTIC,
            diagnostic);
        out.context_base64 = savor::utils::Base64Encode(context_blob);
        out.diagnostic_base64 = savor::utils::Base64Encode(diagnostic);
        out.context_complete =
            complete && context_present && diagnostic_present;
        return out.Encode();
    }

    ResultMapPayload MapPrimaryResult(
        std::int64_t job_id,
        const std::string& result_ini) const override {
        ResultMapPayload payload{};
        payload.result_kind = kFailedResultKind;
        const auto fail = [&](std::string reason) {
            payload.event_lines.push_back(
                "[navigation-context-result] ok=false job="
                + std::to_string(job_id) + " reason=" + std::move(reason));
            (void)AppendTerminal(
                execution_db_,
                job_id,
                false,
                &payload.event_lines);
        };

        if (execution_db_ == nullptr || state_db_ == nullptr) {
            fail("db_unavailable");
            return payload;
        }
        const auto job = execution_db_->GetJob(job_id);
        if (!job.has_value()) {
            fail("job_missing");
            return payload;
        }
        const auto input = NavigationContextJobIni::Decode(job->input_ini);
        const auto source =
            state_db_->GetSavestate(input.source_savestate_id);
        if (!MatchesJob(*job, input)
            || !MatchesWorkflowIdentity(execution_db_, *job, input)
            || !source.has_value()
            || !ValidFrozenState(*source, input)) {
            fail("frozen_identity_mismatch");
            return payload;
        }

        const auto result = NavigationContextResultIni::Decode(result_ini);
        const auto context_blob =
            savor::utils::Base64Decode(result.context_base64);
        const auto diagnostic =
            savor::utils::Base64Decode(result.diagnostic_base64);
        soa::navigation::ctx::NavigationContext decoded{};
        const bool context_decoded =
            context_blob.has_value()
            && !context_blob->empty()
            && soa::navigation::ctx::codec::decode(*context_blob, decoded);
        const bool result_codes_ok =
            result.required_keys_present
            && result.context_complete
            && result.ps_ok
            && result.w_err == savor::WERR_None
            && result.dw_err
                == static_cast<std::uint32_t>(
                    savor::RunToBpOutcome::Hit)
            && result.outcome
                == static_cast<std::uint32_t>(
                    phase::navigation::ctx::Outcome::Completed)
            && result.failure
                == static_cast<std::uint32_t>(
                    phase::navigation::ctx::FailureCode::None)
            && result.hit_pc == soa::navigation::ctx::CapturePc
            && result.hit_bp_key
                == static_cast<std::uint32_t>(
                    bp::navigation::
                        NavigationContextInitialPlayerInputReady);
        const auto expected_output = OutputSavestatePath(
            WorkingRoot(working_dir_root_),
            job_id);
        const bool output_ok =
            !result.savestate_path.empty()
            && IsRegularFile(result.savestate_path)
            && SamePath(result.savestate_path, expected_output);
        const bool context_ok =
            context_decoded
            && decoded.capture_pc == soa::navigation::ctx::CapturePc;
        if (!result_codes_ok
            || !diagnostic.has_value()
            || !output_ok
            || !context_ok) {
            std::ostringstream reason;
            reason << "validation_failed"
                   << " required=" << result.required_keys_present
                   << " complete=" << result.context_complete
                   << " ps_ok=" << result.ps_ok
                   << " w_err=" << result.w_err
                   << " dw_err=" << result.dw_err
                   << " entry_pc=" << result.entry_pc
                   << " hit_pc=" << result.hit_pc
                   << " hit_bp_key=" << result.hit_bp_key
                   << " outcome=" << result.outcome
                   << " failure=" << result.failure
                   << " context_decoded=" << context_decoded
                   << " output_ok=" << output_ok;
            fail(reason.str());
            return payload;
        }

        const auto context_artifact_id = StoreContextArtifact(
            state_db_,
            ContextArtifactPath(WorkingRoot(working_dir_root_), job_id),
            *context_blob,
            job_id,
            &payload.event_lines);
        const auto output_savestate_id = StoreOutputSavestate(
            state_db_,
            expected_output,
            job_id,
            &payload.event_lines);
        if (!context_artifact_id.has_value()
            || !output_savestate_id.has_value()) {
            fail("artifact_or_savestate_store_failed");
            return payload;
        }

        std::string error;
        std::int64_t derivation_id = 0;
        if (!state_db_->DeriveSavestate(
                {
                    .from_savestate_id = input.source_savestate_id,
                    .to_savestate_id = *output_savestate_id,
                    .method_kind = String(DerivationMethod),
                    .source_context_kind =
                        String(DerivationSourceContextKind),
                    .source_context_id = *context_artifact_id,
                    .created_at_utc = savor::db::types::UtcNow(),
                    .correlation_id =
                        "navigation-context-job-" + std::to_string(job_id),
                    .causation_id =
                        "state-artifact-"
                        + std::to_string(*context_artifact_id),
                },
                &derivation_id,
                &error)
            || derivation_id <= 0) {
            payload.event_lines.push_back(
                "[navigation-context-derivation] ok=false error=" + error);
            fail("derivation_failed");
            return payload;
        }
        if (!AppendTerminal(
                execution_db_,
                job_id,
                true,
                &payload.event_lines)) {
            payload.event_lines.push_back(
                "[navigation-context-result] ok=false job="
                + std::to_string(job_id)
                + " reason=terminal_transition_failed");
            return payload;
        }

        payload.result_kind = String(ResultKind);
        payload.result_ref_id = *context_artifact_id;
        payload.output_key = String(OutputKey);
        payload.output_data_kind = String(OutputDataKind);
        payload.output_ref_kind = String(OutputRefKind);
        payload.output_ref_id = *context_artifact_id;
        payload.event_lines.push_back(
            "[navigation-context-result] ok=true job="
            + std::to_string(job_id)
            + " entry_pc=" + std::to_string(result.entry_pc)
            + " context_artifact_id="
            + std::to_string(*context_artifact_id)
            + " output_savestate_id="
            + std::to_string(*output_savestate_id)
            + " derivation_id=" + std::to_string(derivation_id));
        return payload;
    }

    std::optional<ResultArtifactRef> MapPrimaryArtifact(
        std::int64_t) const override {
        return std::nullopt;
    }

private:
    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IStateDb* state_db_ = nullptr;
    std::filesystem::path working_dir_root_;
};

class NavigationContextTransitionHandler final
    : public IWorkflowTransitionHandler {
public:
    WorkflowTransitionDecision EvaluateTransition(
        const WorkflowTransitionContext&) const override {
        WorkflowTransitionDecision decision{};
        decision.should_advance = true;
        return decision;
    }
};

} // namespace

ProgramKindDescriptor BuildNavigationContextProbeDescriptor(
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    NavigationContextPhaseRegistrationConfig config) {
    ProgramKindDescriptor descriptor{};
    descriptor.program_kind =
        static_cast<std::int32_t>(savor::PK_NavigationContextRunner);
    descriptor.program_name = "NavigationContextRunner";
    descriptor.graph_job_persistence =
        std::make_shared<NavigationContextGraphAdapter>(
            execution_db,
            state_db,
            config.run_timeout_ms);
    descriptor.runtime_init =
        std::make_shared<NavigationContextRuntimeAdapter>(
            execution_db,
            state_db,
            config.working_dir_root);
    descriptor.result_mapper =
        std::make_shared<NavigationContextResultMapper>(
            execution_db,
            state_db,
            std::move(config.working_dir_root));
    descriptor.workflow_transition =
        std::make_shared<NavigationContextTransitionHandler>();
    descriptor.supports_workflow_orchestration = true;
    return descriptor;
}

} // namespace savor::db::execution::programdb::navigationcontext
