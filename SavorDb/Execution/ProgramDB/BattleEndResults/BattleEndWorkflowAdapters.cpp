#include "BattleEndResultsAdapters.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../SeedProbe/SeedProbeContracts.h"
#include "../SeedProbe/SeedProbeGridAdapters.h"
#include "../SeedProbe/SeedProbeNeutralAdapters.h"
#include "../SeedProbe/SeedProbeUniqueAdapters.h"
#include "../../IExecutionDb.h"
#include "../../Jobs/JobEventOrchestration.h"
#include "../../Workflow/WorkflowOrchestration.h"
#include "../../../Analysis/IAnalysisDb.h"
#include "../../../Authoring/IAuthoringDb.h"
#include "../../../Common/Types/UtcTimestamp.h"
#include "../../../State/IStateDb.h"
#include "../../../../SavorCore/Phases/Programs/BattleCompletion/BattleCompletionManifest.h"
#include "../../../../SavorCore/Phases/Programs/BattleCompletion/BattleCompletionPayload.h"
#include "../../../../SavorCore/Phases/Programs/BattleEndResults/BattleEndResultsPayload.h"
#include "../../../../SavorCore/Phases/Programs/BattleEndResults/BattleEndResultsReport.h"
#include "../../../../SavorCore/Phases/Programs/SeedProbe/SeedProbePayload.h"
#include "../../../../SavorCore/Runner/IPC/Wire.h"
#include "../../../../SavorCore/Runner/Parallel/PRTypes.h"
#include "../../../../SavorCore/Runner/Script/CtxRegistry.h"
#include "../../../../SavorCore/Utils/Base64.h"
#include "../../../../SavorCore/Utils/Hash.h"
#include "../../../../SavorCore/Utils/IniDoc.h"

namespace savor::db::execution::programdb::battleend {
namespace {

constexpr const char* kCompletionUnit = "battle_completion";
constexpr const char* kCompletionStep = "battle.completion";
constexpr const char* kCompletionRef = "analysis_battle.battle_completion";
constexpr const char* kCompletionData = "analysis_battle.battle_completion_id";
constexpr const char* kFieldUnit = "field_return_seed_probe";
constexpr const char* kFieldStep = "battle.field_return_seed_probe";
constexpr const char* kFieldGridStep = "battle.field_return_seed_probe.grid";
constexpr const char* kFieldUniqueStep = "battle.field_return_seed_probe.unique";
constexpr const char* kFieldMaterializeStep = "battle.field_return_seed_probe.materialize";
constexpr const char* kResultsUnit = "battle_results_screen";
constexpr const char* kResultsStep = "battle.results_screen";
constexpr const char* kResultsRef = "analysis_battle.battle_results";
constexpr const char* kTurnRef = "analysis_battle.turn_job";
constexpr const char* kNeutralSeedRef = "analysisseedprobe.neutral_seed";
constexpr const char* kUniqueSeedRef = "analysisseedprobe.unique_seed";

std::filesystem::path WorkRoot(
    const std::filesystem::path& configured,
    std::string_view leaf) {
    const auto root = configured.empty()
        ? std::filesystem::temp_directory_path() / "savor-battle-end-workflow"
        : configured;
    return root / leaf;
}

std::int64_t FileSize(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    return error ? 0 : static_cast<std::int64_t>(size);
}

std::uint32_t ClampU32(std::int64_t value) {
    if (value <= 0) return 0;
    return static_cast<std::uint32_t>(std::min<std::int64_t>(
        value,
        std::numeric_limits<std::uint32_t>::max()));
}

int CountBits(std::uint32_t value) {
    int count = 0;
    while (value != 0) {
        count += static_cast<int>(value & 1u);
        value >>= 1u;
    }
    return count;
}

const WorkflowGraphInputBinding* FindBinding(
    const WorkflowGraphStepScheduleContext& context,
    std::string_view key,
    std::string_view data_kind) {
    const WorkflowGraphInputBinding* found = nullptr;
    for (const auto& binding : context.input_bindings) {
        if (binding.input_key != key || binding.data_kind != data_kind || binding.ref_id <= 0) continue;
        if (found != nullptr) return nullptr;
        found = &binding;
    }
    return found;
}

std::vector<const WorkflowGraphArgument*> FindArguments(
    const WorkflowGraphStepScheduleContext& context,
    std::string_view key) {
    std::vector<const WorkflowGraphArgument*> result;
    for (const auto& argument : context.arguments) {
        if (argument.argument_key == key) result.push_back(&argument);
    }
    return result;
}

WorkflowStepScheduleResult ScheduleFailure(
    const WorkflowGraphStepScheduleContext& context,
    std::string reason) {
    WorkflowStepScheduleResult result{};
    result.event_lines.push_back(
        "[battle-end-workflow-enqueue] ok=false step=" + context.step_kind
        + " workflow_instance_id=" + std::to_string(context.workflow_instance_id)
        + " workflow_step_id=" + std::to_string(context.workflow_step_id)
        + " error=" + std::move(reason));
    return result;
}

bool AppendTerminal(
    savor::db::IExecutionDb* execution_db,
    std::int64_t job_id,
    bool succeeded,
    std::string_view requested_by,
    std::vector<std::string>* lines) {
    const std::string target_state = succeeded ? "SUCCEEDED" : "FAILED";
    std::string error;
    bool ok = false;
    bool idempotent = false;
    if (execution_db == nullptr || execution_db->JobCommandService() == nullptr) {
        error = "execution job command service unavailable";
    } else {
        const auto before = execution_db->GetJob(job_id);
        if (!before.has_value()) {
            error = "execution job not found";
        } else if (before->state == target_state) {
            ok = true;
            idempotent = true;
        } else if (before->state != "QUEUED"
            && before->state != "CLAIMED"
            && before->state != "RUNNING") {
            error = "execution job state " + before->state
                + " cannot transition to " + target_state;
        } else {
            ok = execution_db->JobCommandService()->AppendLifecycleEvent(
            {
                .kind = savor::db::execution::jobs::JobLifecycleEventKind::JobCompleted,
                .job_id = job_id,
                .terminal_state = target_state,
                .requested_by = std::string(requested_by),
            },
            &error);
            const auto after = ok ? execution_db->GetJob(job_id) : std::nullopt;
            if (ok && (!after.has_value() || after->state != target_state)) {
                ok = false;
                error = "execution job terminal transition was not durable";
            }
        }
    }
    if (lines != nullptr) {
        lines->push_back(
            "[battle-end-workflow-job-terminal] job=" + std::to_string(job_id)
            + " state=" + target_state
            + " ok=" + (ok ? "true" : "false")
            + (idempotent ? " idempotent=true" : "")
            + (error.empty() ? "" : " error=" + error));
    }
    return ok;
}

void FailCompletionAggregate(
    savor::db::IAnalysisDb* analysis_db,
    std::int64_t completion_id,
    std::int64_t job_id,
    std::optional<std::string> manifest_blob,
    int mismatch_count,
    int invariant_failure_count,
    std::vector<std::string>* lines) {
    if (analysis_db == nullptr || completion_id <= 0) return;
    std::string error;
    const auto ok = analysis_db->FailBattleCompletion(
        {
            .battle_completion_id = completion_id,
            .manifest_version = manifest_blob.has_value()
                ? std::optional<int>(phase::battle::completion::ManifestVersion)
                : std::nullopt,
            .manifest_blob = std::move(manifest_blob),
            .mismatch_count = std::max(0, mismatch_count),
            .invariant_failure_count = std::max(0, invariant_failure_count),
            .completed_at_utc = savor::db::types::UtcNow(),
            .correlation_id = "battle-completion-" + std::to_string(completion_id),
            .causation_id = "job-" + std::to_string(job_id),
        },
        &error);
    if (lines != nullptr) {
        lines->push_back("[battle-completion-aggregate-failed] id=" + std::to_string(completion_id)
            + " ok=" + (ok ? "true" : "false") + (error.empty() ? "" : " error=" + error));
    }
}

void FailResultsAggregate(
    savor::db::IAnalysisDb* analysis_db,
    std::int64_t results_id,
    std::int64_t job_id,
    std::optional<std::int64_t> entry_seed,
    std::optional<std::int64_t> final_seed,
    std::optional<std::int64_t> result_artifact_id,
    int mismatch_count,
    int invariant_failure_count,
    std::vector<std::string>* lines) {
    if (analysis_db == nullptr || results_id <= 0) return;
    std::string error;
    const auto ok = analysis_db->FailBattleResults(
        {
            .battle_results_id = results_id,
            .entry_rng_seed = entry_seed,
            .final_rng_seed = final_seed,
            .result_artifact_id = result_artifact_id,
            .mismatch_count = std::max(0, mismatch_count),
            .invariant_failure_count = std::max(0, invariant_failure_count),
            .completed_at_utc = savor::db::types::UtcNow(),
            .correlation_id = "battle-results-" + std::to_string(results_id),
            .causation_id = "job-" + std::to_string(job_id),
        },
        &error);
    if (lines != nullptr) {
        lines->push_back("[battle-results-aggregate-failed] id=" + std::to_string(results_id)
            + " ok=" + (ok ? "true" : "false") + (error.empty() ? "" : " error=" + error));
    }
}

std::optional<std::int64_t> StoreBytesArtifact(
    savor::db::IStateDb* state_db,
    const std::filesystem::path& path,
    const std::string& bytes,
    std::string extension,
    std::string correlation,
    std::vector<std::string>* lines) {
    if (state_db == nullptr || bytes.empty()) return std::nullopt;
    std::filesystem::create_directories(path.parent_path());
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!output.good()) return std::nullopt;
    }
    std::string error;
    std::int64_t artifact_id = 0;
    if (!state_db->StoreArtifact(
            {
                .sha256 = hash::sha256_of_file(path.string()),
                .size_bytes = FileSize(path),
                .compression_kind = 0,
                .filename = std::filesystem::absolute(path).string(),
                .file_ext = std::move(extension),
                .artifact_kind = "OTHER",
                .created_at_utc = savor::db::types::UtcNow(),
                .correlation_id = correlation,
                .causation_id = correlation,
            },
            &artifact_id,
            &error)
        || artifact_id <= 0) {
        if (lines != nullptr) lines->push_back("[battle-end-workflow-artifact] ok=false error=" + error);
        return std::nullopt;
    }
    return artifact_id;
}

std::optional<std::int64_t> StoreSavestate(
    savor::db::IStateDb* state_db,
    const std::filesystem::path& path,
    std::string type,
    std::string note,
    std::string correlation,
    std::vector<std::string>* lines) {
    if (state_db == nullptr || !std::filesystem::is_regular_file(path)) return std::nullopt;
    std::string error;
    std::int64_t artifact_id = 0;
    if (!state_db->StoreArtifact(
            {
                .sha256 = hash::sha256_of_file(path.string()),
                .size_bytes = FileSize(path),
                .compression_kind = 0,
                .filename = std::filesystem::absolute(path).string(),
                .file_ext = path.extension().string(),
                .artifact_kind = "SAV",
                .created_at_utc = savor::db::types::UtcNow(),
                .correlation_id = correlation,
                .causation_id = correlation,
            },
            &artifact_id,
            &error)
        || artifact_id <= 0) {
        if (lines != nullptr) lines->push_back("[battle-end-workflow-savestate] ok=false stage=artifact error=" + error);
        return std::nullopt;
    }
    std::int64_t savestate_id = 0;
    if (!state_db->CreateSavestate(
            {
                .artifact_id = artifact_id,
                .savestate_type = std::move(type),
                .note = std::move(note),
                .is_complete = true,
                .created_at_utc = savor::db::types::UtcNow(),
                .correlation_id = correlation,
                .causation_id = "artifact-" + std::to_string(artifact_id),
            },
            &savestate_id,
            &error)
        || savestate_id <= 0) {
        if (lines != nullptr) lines->push_back("[battle-end-workflow-savestate] ok=false stage=row error=" + error);
        return std::nullopt;
    }
    return savestate_id;
}

bool ValidStateArtifact(
    const savor::db::SavestateRecord& state,
    bool verify_hash = true) {
    const std::filesystem::path path(state.artifact_filename);
    return state.is_complete
        && state.artifact_id > 0
        && state.artifact_kind == "SAV"
        && !state.artifact_sha256.empty()
        && std::filesystem::is_regular_file(path)
        && (!verify_hash || hash::sha256_of_file(path.string()) == state.artifact_sha256);
}

std::optional<std::uint32_t> ResolveRunTimeout(
    savor::db::IAuthoringDb* authoring_db,
    const savor::db::BattleSetSnapshot& battle_set,
    const savor::db::ExecutionJobRecord& source_exec,
    const WorkflowGraphStepScheduleContext& context) {
    const auto args = FindArguments(context, "run_ms");
    if (args.size() > 1) return std::nullopt;
    if (args.size() == 1) {
        if (args.front()->value_type != "integer"
            || !args.front()->integer_value.has_value()
            || *args.front()->integer_value <= 0) return std::nullopt;
        return ClampU32(*args.front()->integer_value);
    }
    const auto source_ini = IniDoc::parse(source_exec.input_ini);
    auto run_ms = source_ini.get_u32("BattleSingleTurn.Job", "run_ms_override", 0);
    if (run_ms == 0 && authoring_db != nullptr) {
        const auto spec = authoring_db->GetBattleRunSpec(battle_set.battle_run_spec_id);
        if (spec.has_value()) run_ms = ClampU32(spec->run_ms);
    }
    if (run_ms == 0) return std::nullopt;
    return run_ms;
}

class AdvanceTransition final : public IWorkflowTransitionHandler {
public:
    WorkflowTransitionDecision EvaluateTransition(const WorkflowTransitionContext& context) const override {
        WorkflowTransitionDecision decision{};
        if (context.failed_total > 0) {
            decision.terminal_failure = true;
            decision.blocked_reason = "battle-end workflow step contains failed jobs";
            return decision;
        }
        decision.should_advance = true;
        return decision;
    }
};

struct CompletionJobIni {
    static constexpr const char* Section = "BattleCompletion.Job";
    std::int64_t completion_id = 0;
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::int64_t source_turn_job_id = 0;
    std::int64_t entry_savestate_id = 0;
    std::int64_t entry_artifact_id = 0;
    std::string entry_artifact_sha256;
    std::uint32_t run_timeout_ms = 0;
    bool required_keys_present = false;

    std::string encode() const {
        IniDoc ini;
        ini.set(Section, "completion_id", std::to_string(completion_id));
        ini.set(Section, "workflow_instance_id", std::to_string(workflow_instance_id));
        ini.set(Section, "workflow_step_id", std::to_string(workflow_step_id));
        ini.set(Section, "source_turn_job_id", std::to_string(source_turn_job_id));
        ini.set(Section, "entry_savestate_id", std::to_string(entry_savestate_id));
        ini.set(Section, "entry_artifact_id", std::to_string(entry_artifact_id));
        ini.set(Section, "entry_artifact_sha256", entry_artifact_sha256);
        ini.set(Section, "run_timeout_ms", std::to_string(run_timeout_ms));
        return ini.to_string_sorted();
    }
    static CompletionJobIni decode(const std::string& text) {
        const auto ini = IniDoc::parse(text);
        CompletionJobIni out{};
        out.required_keys_present = ini.has(Section, "completion_id")
            && ini.has(Section, "workflow_instance_id")
            && ini.has(Section, "workflow_step_id")
            && ini.has(Section, "source_turn_job_id")
            && ini.has(Section, "entry_savestate_id")
            && ini.has(Section, "entry_artifact_id")
            && ini.has(Section, "entry_artifact_sha256")
            && ini.has(Section, "run_timeout_ms");
        out.completion_id = ini.get_i64(Section, "completion_id", 0);
        out.workflow_instance_id = ini.get_i64(Section, "workflow_instance_id", 0);
        out.workflow_step_id = ini.get_i64(Section, "workflow_step_id", 0);
        out.source_turn_job_id = ini.get_i64(Section, "source_turn_job_id", 0);
        out.entry_savestate_id = ini.get_i64(Section, "entry_savestate_id", 0);
        out.entry_artifact_id = ini.get_i64(Section, "entry_artifact_id", 0);
        out.entry_artifact_sha256 = ini.get(Section, "entry_artifact_sha256", "");
        out.run_timeout_ms = ini.get_u32(Section, "run_timeout_ms", 0);
        return out;
    }
};

bool MatchesCompletionJob(
    const savor::db::ExecutionJobRecord& job,
    const CompletionJobIni& input) {
    return input.required_keys_present
        && job.program_kind == static_cast<std::int32_t>(savor::PK_BattleCompletionRunner)
        && job.program_version == static_cast<std::int32_t>(phase::battle::completion::PayloadVersion)
        && job.program_ref_kind == kCompletionRef
        && job.program_ref_id == input.completion_id
        && job.savestate_id == std::optional<std::int64_t>(input.entry_savestate_id)
        && input.completion_id > 0
        && input.workflow_instance_id > 0
        && input.workflow_step_id > 0
        && input.source_turn_job_id > 0
        && input.entry_savestate_id > 0
        && input.entry_artifact_id > 0
        && !input.entry_artifact_sha256.empty()
        && input.run_timeout_ms > 0;
}

bool MatchesCompletionAggregate(
    const savor::db::BattleCompletionRecord& aggregate,
    const CompletionJobIni& input,
    std::int64_t job_id) {
    return aggregate.battle_completion_id == input.completion_id
        && aggregate.workflow_instance_id == input.workflow_instance_id
        && aggregate.workflow_step_id == input.workflow_step_id
        && aggregate.exec_job_id == std::optional<std::int64_t>(job_id)
        && aggregate.entry_savestate_id == input.entry_savestate_id
        && aggregate.status == "QUEUED"
        && !aggregate.completion_savestate_id.has_value()
        && !aggregate.completed_at_utc.has_value();
}

class CompletionGraphAdapter final : public IWorkflowGraphJobPersistenceAdapter {
public:
    CompletionGraphAdapter(
        savor::db::IExecutionDb* execution_db,
        savor::db::IStateDb* state_db,
        savor::db::IAnalysisDb* analysis_db,
        savor::db::IAuthoringDb* authoring_db)
        : execution_db_(execution_db), state_db_(state_db), analysis_db_(analysis_db), authoring_db_(authoring_db) {}

    WorkflowStepScheduleResult EncodeForGraphQueueing(
        const WorkflowGraphStepScheduleContext& context) const override {
        if (execution_db_ == nullptr || state_db_ == nullptr || analysis_db_ == nullptr || authoring_db_ == nullptr
            || context.unit_kind != kCompletionUnit || context.step_kind != kCompletionStep) {
            return ScheduleFailure(context, "invalid_context_or_db");
        }
        const auto* binding = FindBinding(context, "entry_savestate", "state.savestate_id");
        if (binding == nullptr || context.input_bindings.size() != 1 || binding->source_kind != "external"
            || binding->ref_kind != "state.savestate") {
            return ScheduleFailure(context, "invalid_victory_binding");
        }
        const auto source_jobs = analysis_db_->ListBattleTurnJobsByOutputSavestateId(binding->ref_id);
        if (source_jobs.size() != 1) return ScheduleFailure(context, "victory_turn_job_missing_or_ambiguous");
        const auto& turn = source_jobs.front();
        if (turn.job_state != savor::db::BattleTurnJobState::Succeeded
            || !turn.has_results
            || turn.battle_outcome != savor::battle::Outcome::Victory
            || !turn.exec_job_id.has_value()) {
            return ScheduleFailure(context, "source_is_not_successful_victory");
        }
        const auto source_exec = execution_db_->GetJob(*turn.exec_job_id);
        const auto wave = analysis_db_->GetBattleTurnWave(turn.wave_id);
        const auto battle_set = wave.has_value() ? analysis_db_->GetBattleSet(wave->battle_set_id) : std::nullopt;
        const auto state = state_db_->GetSavestate(binding->ref_id);
        if (!source_exec.has_value() || source_exec->state != "SUCCEEDED"
            || source_exec->program_kind != static_cast<std::int32_t>(savor::PK_BattleSingleTurnRunner)
            || source_exec->program_ref_kind != kTurnRef
            || source_exec->program_ref_id != turn.turn_job_id
            || turn.source_savestate_id != source_exec->savestate_id
            || turn.output_savestate_id != std::optional<std::int64_t>(binding->ref_id)
            || !wave.has_value() || !battle_set.has_value()
            || !state.has_value() || !ValidStateArtifact(*state)) {
            return ScheduleFailure(context, "source_provenance_or_artifact_invalid");
        }
        const auto timeout = ResolveRunTimeout(authoring_db_, *battle_set, *source_exec, context);
        if (!timeout.has_value() || *timeout == 0) return ScheduleFailure(context, "run_timeout_unavailable");

        const auto now = savor::db::types::UtcNow();
        std::string error;
        std::int64_t completion_id = 0;
        if (!analysis_db_->CreateBattleCompletion(
                {
                    .workflow_instance_id = context.workflow_instance_id,
                    .workflow_step_id = context.workflow_step_id,
                    .entry_savestate_id = binding->ref_id,
                    .status = "QUEUED",
                    .created_at_utc = now,
                    .correlation_id = "workflow-" + std::to_string(context.workflow_instance_id),
                    .causation_id = "turn-job-" + std::to_string(turn.turn_job_id),
                },
                &completion_id,
                &error)
            || completion_id <= 0) {
            return ScheduleFailure(context, "create_completion_failed:" + error);
        }

        CompletionJobIni input{};
        input.completion_id = completion_id;
        input.workflow_instance_id = context.workflow_instance_id;
        input.workflow_step_id = context.workflow_step_id;
        input.source_turn_job_id = turn.turn_job_id;
        input.entry_savestate_id = binding->ref_id;
        input.entry_artifact_id = state->artifact_id;
        input.entry_artifact_sha256 = state->artifact_sha256;
        input.run_timeout_ms = *timeout;
        const auto input_ini = input.encode();
        const auto fingerprint = "PK=" + std::to_string(savor::PK_BattleCompletionRunner)
            + ";PV=" + std::to_string(phase::battle::completion::PayloadVersion)
            + ";completion=" + std::to_string(completion_id)
            + ";workflow=" + std::to_string(context.workflow_instance_id)
            + ";step=" + std::to_string(context.workflow_step_id)
            + ";sha=" + hash::sha256(input_ini.data(), input_ini.size());

        std::int64_t job_set_id = 0;
        if (!execution_db_->CreateJobSet(
                {
                    .program_kind = static_cast<std::int32_t>(savor::PK_BattleCompletionRunner),
                    .purpose = "Battle Completion",
                    .created_by = "battle.completion.graph_adapter",
                    .created_at_utc = now.time_since_epoch().count(),
                    .expected_total = 1,
                    .domain_ref_kind = std::string(kCompletionRef),
                    .domain_ref_id = completion_id,
                },
                &job_set_id,
                &error)
            || job_set_id <= 0) {
            FailCompletionAggregate(analysis_db_, completion_id, 0, std::nullopt, 0, 0, nullptr);
            return ScheduleFailure(context, "create_job_set_failed:" + error);
        }
        std::int64_t job_id = 0;
        if (!execution_db_->EnqueueJob(
                {
                    .job_set_id = job_set_id,
                    .program_kind = static_cast<std::int32_t>(savor::PK_BattleCompletionRunner),
                    .program_version = static_cast<std::int32_t>(phase::battle::completion::PayloadVersion),
                    .program_ref_kind = kCompletionRef,
                    .program_ref_id = completion_id,
                    .savestate_id = binding->ref_id,
                    .fingerprint = fingerprint,
                    .priority = context.step_priority,
                    .max_attempts = 1,
                    .input_ini = input_ini,
                    .pending_until_workflow_materialized = true,
                },
                &job_id,
                &error)
            || job_id <= 0) {
            FailCompletionAggregate(analysis_db_, completion_id, 0, std::nullopt, 0, 0, nullptr);
            return ScheduleFailure(context, "enqueue_failed:" + error);
        }
        if (!analysis_db_->BindBattleCompletionExecutionJob(
                {
                    .battle_completion_id = completion_id,
                    .workflow_instance_id = context.workflow_instance_id,
                    .workflow_step_id = context.workflow_step_id,
                    .exec_job_id = job_id,
                },
                &error)) {
            FailCompletionAggregate(analysis_db_, completion_id, job_id, std::nullopt, 0, 0, nullptr);
            return ScheduleFailure(context, "bind_completion_job_failed:" + error);
        }

        WorkflowStepScheduleResult result{};
        result.root_job_set_id = job_set_id;
        result.persistence = {
            .program_ref_kind = kCompletionRef,
            .program_ref_id = completion_id,
            .fingerprint = fingerprint,
            .program_version = static_cast<std::int32_t>(phase::battle::completion::PayloadVersion),
        };
        result.event_lines.push_back("[battle-completion-enqueue] ok=true completion_id=" + std::to_string(completion_id)
            + " job=" + std::to_string(job_id));
        return result;
    }
private:
    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IStateDb* state_db_ = nullptr;
    savor::db::IAnalysisDb* analysis_db_ = nullptr;
    savor::db::IAuthoringDb* authoring_db_ = nullptr;
};

class CompletionRuntimeAdapter final : public IRuntimeInitAdapter {
public:
    CompletionRuntimeAdapter(
        savor::db::IExecutionDb* execution_db,
        savor::db::IStateDb* state_db,
        savor::db::IAnalysisDb* analysis_db,
        std::filesystem::path root)
        : execution_db_(execution_db), state_db_(state_db), analysis_db_(analysis_db), root_(std::move(root)) {}

    RuntimeInitRequest BuildRuntimeInit(std::int64_t job_id) const override {
        RuntimeInitRequest request{};
        request.bootstrap_profile = kCompletionStep;
        if (execution_db_ == nullptr || state_db_ == nullptr || analysis_db_ == nullptr) return request;
        const auto job = execution_db_->GetJob(job_id);
        if (!job.has_value()) return request;
        const auto input = CompletionJobIni::decode(job->input_ini);
        const auto aggregate = analysis_db_->GetBattleCompletion(job->program_ref_id);
        const auto state = state_db_->GetSavestate(input.entry_savestate_id);
        if (!MatchesCompletionJob(*job, input)
            || !aggregate.has_value() || !MatchesCompletionAggregate(*aggregate, input, job_id)
            || !state.has_value() || !ValidStateArtifact(*state)
            || state->artifact_id != input.entry_artifact_id
            || state->artifact_sha256 != input.entry_artifact_sha256) return request;
        request.savestate_ref_kind = "state_savestate";
        request.savestate_ref_id = input.entry_savestate_id;
        request.default_timeout_ms = input.run_timeout_ms;
        return request;
    }

    std::optional<savor::PSJob> MaterializePsJob(
        std::int64_t job_id,
        const RuntimeInitRequest& request) const override {
        if (execution_db_ == nullptr || state_db_ == nullptr || analysis_db_ == nullptr) return std::nullopt;
        const auto job = execution_db_->GetJob(job_id);
        if (!job.has_value()) return std::nullopt;
        const auto input = CompletionJobIni::decode(job->input_ini);
        const auto aggregate = analysis_db_->GetBattleCompletion(job->program_ref_id);
        const auto state = state_db_->GetSavestate(input.entry_savestate_id);
        if (!MatchesCompletionJob(*job, input)
            || input.entry_savestate_id != request.savestate_ref_id
            || !aggregate.has_value() || !MatchesCompletionAggregate(*aggregate, input, job_id)
            || !state.has_value() || !ValidStateArtifact(*state)
            || state->artifact_id != input.entry_artifact_id
            || state->artifact_sha256 != input.entry_artifact_sha256) return std::nullopt;
        const auto output = WorkRoot(root_, "completion") / ("job-" + std::to_string(job_id))
            / "output" / "battle_completion.sav";
        std::filesystem::create_directories(output.parent_path());
        phase::battle::completion::EncodeSpec spec{};
        spec.run_timeout_ms = input.run_timeout_ms;
        spec.output_savestate_path = output.string();
        savor::PSJob result{};
        if (!phase::battle::completion::encode_payload(spec, result.payload)) return std::nullopt;
        return result;
    }
private:
    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IStateDb* state_db_ = nullptr;
    savor::db::IAnalysisDb* analysis_db_ = nullptr;
    std::filesystem::path root_;
};

struct CompletionResultIni {
    static constexpr const char* Section = "BattleCompletion.Results";
    std::uint32_t w_err = 0;
    std::uint32_t dw_err = static_cast<std::uint32_t>(savor::RunToBpOutcome::Unknown);
    std::uint32_t outcome = 0;
    std::uint32_t provider_failure = 0;
    std::uint32_t runtime_failure = 0;
    std::uint32_t macro_result = 0;
    std::uint32_t invariant_flags = 0;
    std::string savestate_path;
    std::string manifest_base64;
    std::string diagnostic_base64;
    bool required_keys_present = false;

    std::string encode() const {
        IniDoc ini;
        ini.set(Section, "w_err", std::to_string(w_err));
        ini.set(Section, "dw_err", std::to_string(dw_err));
        if (required_keys_present) ini.set(Section, "outcome", std::to_string(outcome));
        ini.set(Section, "provider_failure", std::to_string(provider_failure));
        ini.set(Section, "runtime_failure", std::to_string(runtime_failure));
        ini.set(Section, "macro_result", std::to_string(macro_result));
        ini.set(Section, "invariant_flags", std::to_string(invariant_flags));
        ini.set(Section, "savestate_path", savestate_path);
        ini.set(Section, "manifest_base64", manifest_base64);
        ini.set(Section, "diagnostic_base64", diagnostic_base64);
        return ini.to_string_sorted();
    }
    static CompletionResultIni decode(const std::string& text) {
        const auto ini = IniDoc::parse(text);
        CompletionResultIni out{};
        out.required_keys_present = ini.has(Section, "w_err")
            && ini.has(Section, "dw_err")
            && ini.has(Section, "outcome")
            && ini.has(Section, "provider_failure")
            && ini.has(Section, "runtime_failure")
            && ini.has(Section, "macro_result")
            && ini.has(Section, "invariant_flags")
            && ini.has(Section, "savestate_path")
            && ini.has(Section, "manifest_base64");
        out.w_err = ini.get_u32(Section, "w_err", 0);
        out.dw_err = ini.get_u32(Section, "dw_err", out.dw_err);
        out.outcome = ini.get_u32(Section, "outcome", 0);
        out.provider_failure = ini.get_u32(Section, "provider_failure", 0);
        out.runtime_failure = ini.get_u32(Section, "runtime_failure", 0);
        out.macro_result = ini.get_u32(Section, "macro_result", 0);
        out.invariant_flags = ini.get_u32(Section, "invariant_flags", 0);
        out.savestate_path = ini.get(Section, "savestate_path", "");
        out.manifest_base64 = ini.get(Section, "manifest_base64", "");
        out.diagnostic_base64 = ini.get(Section, "diagnostic_base64", "");
        return out;
    }
};

class CompletionResultMapper final : public IResultMapper {
public:
    CompletionResultMapper(
        savor::db::IExecutionDb* execution_db,
        savor::db::IStateDb* state_db,
        savor::db::IAnalysisDb* analysis_db,
        std::filesystem::path root)
        : execution_db_(execution_db), state_db_(state_db), analysis_db_(analysis_db), root_(std::move(root)) {}

    std::string BuildResultIniFromPrResult(std::int64_t, const savor::PRResult& result) const override {
        CompletionResultIni out{};
        out.w_err = result.ps.w_err;
        bool context_complete = result.ps.ctx.get(savor::context::key::core::DW_RUN_OUTCOME_CODE, out.dw_err);
        context_complete = result.ps.ctx.get(savor::context::key::battlecompletion::OUTCOME, out.outcome) && context_complete;
        context_complete = result.ps.ctx.get(savor::context::key::battlecompletion::PROVIDER_FAILURE, out.provider_failure) && context_complete;
        context_complete = result.ps.ctx.get(savor::context::key::battlecompletion::RUNTIME_FAILURE, out.runtime_failure) && context_complete;
        context_complete = result.ps.ctx.get(savor::context::key::battlecompletion::MACRO_RESULT, out.macro_result) && context_complete;
        context_complete = result.ps.ctx.get(savor::context::key::battlecompletion::INVARIANT_FLAGS, out.invariant_flags) && context_complete;
        bool path_present = result.ps.ctx.get(savor::context::key::core::LAST_SAVESTATE_PATH, out.savestate_path);
        if (out.savestate_path.empty()) {
            path_present = result.ps.ctx.get(
                savor::context::key::battlecompletion::OUTPUT_SAVESTATE_PATH, out.savestate_path);
        }
        std::string bytes;
        const bool manifest_present = result.ps.ctx.get(
            savor::context::key::battlecompletion::MANIFEST_BLOB, bytes) && !bytes.empty();
        if (manifest_present) {
            out.manifest_base64 = savor::utils::Base64Encode(bytes);
        }
        if (result.ps.ctx.get(savor::context::key::battlecompletion::DIAGNOSTIC, bytes) && !bytes.empty()) {
            out.diagnostic_base64 = savor::utils::Base64Encode(bytes);
        }
        out.required_keys_present = context_complete && path_present
            && !out.savestate_path.empty() && manifest_present;
        return out.encode();
    }

    ResultMapPayload MapPrimaryResult(std::int64_t job_id, const std::string& text) const override {
        ResultMapPayload payload{};
        payload.result_kind = "analysis_battle.battle_completion.failed";
        if (execution_db_ == nullptr || state_db_ == nullptr || analysis_db_ == nullptr) {
            AppendTerminal(execution_db_, job_id, false, "battle_completion_result_mapper", &payload.event_lines);
            return payload;
        }
        const auto job = execution_db_->GetJob(job_id);
        const auto aggregate_by_job = job.has_value()
            && job->program_kind == static_cast<std::int32_t>(savor::PK_BattleCompletionRunner)
            && job->program_ref_kind == kCompletionRef && job->program_ref_id > 0
            ? analysis_db_->GetBattleCompletion(job->program_ref_id)
            : std::nullopt;
        const auto authoritative_completion_id = aggregate_by_job.has_value()
            && aggregate_by_job->exec_job_id == std::optional<std::int64_t>(job_id)
            ? aggregate_by_job->battle_completion_id : 0;
        auto fail = [&](std::optional<std::string> manifest, int invariant_failures, std::string_view reason) {
            FailCompletionAggregate(
                analysis_db_, authoritative_completion_id, job_id, std::move(manifest), 0,
                invariant_failures, &payload.event_lines);
            AppendTerminal(execution_db_, job_id, false, "battle_completion_result_mapper", &payload.event_lines);
            payload.event_lines.push_back("[battle-completion-result] ok=false reason=" + std::string(reason));
        };
        if (!job.has_value()) {
            AppendTerminal(execution_db_, job_id, false, "battle_completion_result_mapper", &payload.event_lines);
            return payload;
        }
        const auto input = CompletionJobIni::decode(job->input_ini);
        const auto result = CompletionResultIni::decode(text);
        const auto manifest_bytes = savor::utils::Base64Decode(result.manifest_base64);
        phase::battle::completion::Manifest manifest{};
        const bool manifest_decoded = manifest_bytes.has_value()
            && phase::battle::completion::DecodeManifest(*manifest_bytes, manifest);
        const bool manifest_valid = manifest_decoded
            && (manifest.invariant_flags & phase::battle::completion::RequiredManifestInvariants)
                == phase::battle::completion::RequiredManifestInvariants;
        const auto aggregate = aggregate_by_job;
        const auto source_state = state_db_->GetSavestate(input.entry_savestate_id);
        const auto source_jobs = analysis_db_->ListBattleTurnJobsByOutputSavestateId(input.entry_savestate_id);
        const auto source_exec = source_jobs.size() == 1 && source_jobs.front().exec_job_id.has_value()
            ? execution_db_->GetJob(*source_jobs.front().exec_job_id) : std::nullopt;
        const bool source_job_valid = source_jobs.size() == 1
            && source_jobs.front().turn_job_id == input.source_turn_job_id
            && source_jobs.front().job_state == savor::db::BattleTurnJobState::Succeeded
            && source_jobs.front().has_results
            && source_jobs.front().battle_outcome == savor::battle::Outcome::Victory
            && source_jobs.front().output_savestate_id == input.entry_savestate_id
            && source_exec.has_value() && source_exec->state == "SUCCEEDED"
            && source_exec->program_kind == static_cast<std::int32_t>(savor::PK_BattleSingleTurnRunner)
            && source_exec->program_ref_kind == kTurnRef
            && source_exec->program_ref_id == input.source_turn_job_id
            && source_jobs.front().source_savestate_id == source_exec->savestate_id;
        const bool identity_valid = MatchesCompletionJob(*job, input)
            && aggregate.has_value() && MatchesCompletionAggregate(*aggregate, input, job_id)
            && source_state.has_value() && ValidStateArtifact(*source_state)
            && source_state->artifact_id == input.entry_artifact_id
            && source_state->artifact_sha256 == input.entry_artifact_sha256
            && source_job_valid;
        const bool vm_ok = result.required_keys_present
            && result.w_err == 0
            && result.dw_err == static_cast<std::uint32_t>(savor::RunToBpOutcome::Hit)
            && result.outcome == static_cast<std::uint32_t>(phase::battle::endresults::Outcome::Completed)
            && result.provider_failure == 0 && result.runtime_failure == 0 && result.macro_result == 0
            && manifest_decoded && result.invariant_flags == manifest.invariant_flags;
        const auto expected = WorkRoot(root_, "completion") / ("job-" + std::to_string(job_id))
            / "output" / "battle_completion.sav";
        if (!identity_valid || !vm_ok || !manifest_valid || result.savestate_path.empty()
            || !std::filesystem::is_regular_file(result.savestate_path)
            || std::filesystem::weakly_canonical(result.savestate_path)
                != std::filesystem::weakly_canonical(expected)) {
            const auto invariant_failures = CountBits(
                phase::battle::completion::RequiredManifestInvariants & ~result.invariant_flags)
                + ((manifest_decoded && result.invariant_flags != manifest.invariant_flags) ? 1 : 0);
            fail(manifest_decoded ? manifest_bytes : std::nullopt, invariant_failures, "validation_failed");
            return payload;
        }
        const auto report_dir = WorkRoot(root_, "completion") / ("job-" + std::to_string(job_id)) / "report";
        const auto manifest_artifact = StoreBytesArtifact(
            state_db_, report_dir / "battle_completion.bcmb", *manifest_bytes, ".bcmb",
            "battle-completion-" + std::to_string(input.completion_id), &payload.event_lines);
        const auto output_state = StoreSavestate(
            state_db_, result.savestate_path, "BATTLE_COMPLETION",
            "Battle completion rewards committed", "battle-completion-" + std::to_string(input.completion_id),
            &payload.event_lines);
        if (!manifest_artifact.has_value() || !output_state.has_value()) {
            fail(manifest_bytes, 0, "artifact_or_state_store_failed");
            return payload;
        }
        std::string error;
        std::int64_t derivation_id = 0;
        if (!state_db_->DeriveSavestate(
                {
                    .from_savestate_id = input.entry_savestate_id,
                    .to_savestate_id = *output_state,
                    .method_kind = "battle_completion",
                    .source_context_kind = kCompletionRef,
                    .source_context_id = input.completion_id,
                    .created_at_utc = savor::db::types::UtcNow(),
                    .correlation_id = "battle-completion-" + std::to_string(input.completion_id),
                    .causation_id = "savestate-" + std::to_string(*output_state),
                },
                &derivation_id,
                &error)
            || derivation_id <= 0
            || !analysis_db_->CompleteBattleCompletion(
                {
                    .battle_completion_id = input.completion_id,
                    .completion_savestate_id = *output_state,
                    .manifest_version = phase::battle::completion::ManifestVersion,
                    .manifest_blob = *manifest_bytes,
                    .manifest_artifact_id = *manifest_artifact,
                    .mismatch_count = 0,
                    .invariant_failure_count = 0,
                    .status = "COMPLETED",
                    .completed_at_utc = savor::db::types::UtcNow(),
                    .correlation_id = "battle-completion-" + std::to_string(input.completion_id),
                    .causation_id = "job-" + std::to_string(job_id),
                },
                &error)) {
            payload.event_lines.push_back("[battle-completion-result] ok=false error=" + error);
            fail(manifest_bytes, 0, "derivation_or_aggregate_completion_failed");
            return payload;
        }
        if (!AppendTerminal(
                execution_db_, job_id, true, "battle_completion_result_mapper", &payload.event_lines)) {
            payload.event_lines.push_back(
                "[battle-completion-result] ok=false reason=terminal_transition_failed"
                " completion_id=" + std::to_string(input.completion_id));
            return payload;
        }
        payload.result_kind = kCompletionRef;
        payload.result_ref_id = input.completion_id;
        payload.output_key = "completion";
        payload.output_data_kind = kCompletionData;
        payload.output_ref_kind = kCompletionRef;
        payload.output_ref_id = input.completion_id;
        payload.event_lines.push_back("[battle-completion-result] ok=true completion_id=" + std::to_string(input.completion_id)
            + " completion_savestate_id=" + std::to_string(*output_state));
        return payload;
    }

    std::optional<ResultArtifactRef> MapPrimaryArtifact(std::int64_t) const override { return std::nullopt; }
private:
    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IStateDb* state_db_ = nullptr;
    savor::db::IAnalysisDb* analysis_db_ = nullptr;
    std::filesystem::path root_;
};

class FieldReturnChainTransition final : public IWorkflowTransitionHandler {
public:
    WorkflowTransitionDecision EvaluateTransition(const WorkflowTransitionContext& context) const override {
        WorkflowTransitionDecision decision{};
        if (context.failed_total > 0 || !context.input_ref_id.has_value() || *context.input_ref_id <= 0) {
            decision.terminal_failure = true;
            decision.blocked_reason = "field-return neutral probe failed or lost its probe-run id";
            return decision;
        }
        decision.should_advance = true;
        decision.spawn_steps.push_back({
            .step_key = context.step_key + "/Grid",
            .step_kind = kFieldGridStep,
            .input_ref_kind = std::string("sp_probe_run"),
            .input_ref_id = context.input_ref_id,
            .max_attempts = 1,
        });
        return decision;
    }
};

class FieldGridTransition final : public IWorkflowTransitionHandler {
public:
    WorkflowTransitionDecision EvaluateTransition(const WorkflowTransitionContext& context) const override {
        WorkflowTransitionDecision decision{};
        if (context.failed_total > 0 || !context.input_ref_id.has_value()) {
            decision.terminal_failure = true;
            decision.blocked_reason = "field-return grid probe failed";
            return decision;
        }
        const std::string suffix = "/Grid";
        if (context.step_key.size() <= suffix.size()
            || context.step_key.compare(context.step_key.size() - suffix.size(), suffix.size(), suffix) != 0) {
            decision.terminal_failure = true;
            decision.blocked_reason = "field-return grid step key is malformed";
            return decision;
        }
        decision.should_advance = true;
        decision.spawn_steps.push_back({
            .step_key = context.step_key.substr(0, context.step_key.size() - suffix.size()) + "/Unique",
            .step_kind = kFieldUniqueStep,
            .input_ref_kind = std::string("sp_probe_run"),
            .input_ref_id = context.input_ref_id,
            .max_attempts = 1,
        });
        return decision;
    }
};

class FieldUniqueTransition final : public IWorkflowTransitionHandler {
public:
    WorkflowTransitionDecision EvaluateTransition(const WorkflowTransitionContext& context) const override {
        WorkflowTransitionDecision decision{};
        if (context.failed_total > 0 || !context.input_ref_id.has_value()) {
            decision.terminal_failure = true;
            decision.blocked_reason = "field-return unique probe failed";
            return decision;
        }
        const std::string suffix = "/Unique";
        if (context.step_key.size() <= suffix.size()
            || context.step_key.compare(context.step_key.size() - suffix.size(), suffix.size(), suffix) != 0) {
            decision.terminal_failure = true;
            decision.blocked_reason = "field-return unique step key is malformed";
            return decision;
        }
        decision.should_advance = true;
        decision.spawn_steps.push_back({
            .step_key = context.step_key.substr(0, context.step_key.size() - suffix.size()) + "/Materialize",
            .step_kind = kFieldMaterializeStep,
            .input_ref_kind = std::string("sp_probe_run"),
            .input_ref_id = context.input_ref_id,
            .max_attempts = 1,
        });
        return decision;
    }
};

class FieldReturnChainGraphAdapter final : public IWorkflowGraphJobPersistenceAdapter {
public:
    FieldReturnChainGraphAdapter(
        savor::db::IAnalysisDb* analysis_db,
        savor::db::IAuthoringDb* authoring_db,
        std::shared_ptr<IJobPersistenceAdapter> neutral)
        : analysis_db_(analysis_db), authoring_db_(authoring_db), neutral_(std::move(neutral)) {}

    WorkflowStepScheduleResult EncodeForGraphQueueing(
        const WorkflowGraphStepScheduleContext& context) const override {
        if (analysis_db_ == nullptr || authoring_db_ == nullptr || neutral_ == nullptr
            || !context.workflow_graph_revision_id.has_value()
            || context.unit_kind != kFieldUnit || context.step_kind != kFieldStep) {
            return ScheduleFailure(context, "invalid_field_return_context");
        }
        const auto* completion_binding = FindBinding(context, "completion", kCompletionData);
        if (completion_binding == nullptr || completion_binding->ref_kind != kCompletionRef
            || completion_binding->source_kind != "upstream") {
            return ScheduleFailure(context, "completion_binding_missing");
        }
        const auto completion = analysis_db_->GetBattleCompletion(completion_binding->ref_id);
        if (!completion.has_value() || completion->status != "COMPLETED"
            || completion->workflow_instance_id != context.workflow_instance_id
            || !completion->completion_savestate_id.has_value()
            || !completion->manifest_blob.has_value()) {
            return ScheduleFailure(context, "completion_not_ready");
        }
        const auto graph = authoring_db_->GetWorkflowGraphRevision(*context.workflow_graph_revision_id);
        if (!graph.has_value()) return ScheduleFailure(context, "authored_graph_missing");
        const auto node_key = context.activation_graph_node_key.empty()
            ? context.step_key : context.activation_graph_node_key;
        const auto node = std::find_if(graph->nodes.begin(), graph->nodes.end(), [&](const auto& row) {
            return row.node_key == node_key;
        });
        if (node == graph->nodes.end()
            || node->unit_kind != kFieldUnit
            || node->authored_ref_kind.value_or("") != "seed_probe_spec"
            || !node->authored_ref_id.has_value() || *node->authored_ref_id <= 0
            || !authoring_db_->GetSeedProbeSpec(*node->authored_ref_id).has_value()) {
            return ScheduleFailure(context, "seed_probe_spec_missing");
        }
        std::int64_t samples_per_axis = 0;
        const auto samples = FindArguments(context, "samples_per_axis");
        if (samples.size() > 1) return ScheduleFailure(context, "duplicate_samples_per_axis");
        if (samples.size() == 1) {
            if (samples.front()->value_type != "integer" || !samples.front()->integer_value.has_value()
                || *samples.front()->integer_value <= 0 || *samples.front()->integer_value > 255) {
                return ScheduleFailure(context, "invalid_samples_per_axis");
            }
            samples_per_axis = *samples.front()->integer_value;
        }
        const auto now = savor::db::types::UtcNow();
        const auto correlation = "workflow-" + std::to_string(context.workflow_instance_id);
        std::string error;
        std::int64_t probe_set_id = 0;
        if (!analysis_db_->CreateSeedProbeSet(
                {
                    .name = "field-return-" + std::to_string(context.workflow_instance_id)
                        + "-" + std::to_string(context.workflow_step_id),
                    .probe_flavor = "FIELD_RETURN",
                    .breakpoint_policy_name = "seedprobe.field_return",
                    .segment_source_kind = "workflow_graph",
                    .created_at_utc = now,
                    .correlation_id = correlation,
                    .causation_id = "battle-completion-" + std::to_string(completion->battle_completion_id),
                },
                &probe_set_id,
                &error)
            || probe_set_id <= 0) return ScheduleFailure(context, "create_probe_set_failed:" + error);
        std::int64_t probe_run_id = 0;
        if (!analysis_db_->RequestSeedProbeRun(
                {
                    .probe_set_id = probe_set_id,
                    .entry_savestate_id = *completion->completion_savestate_id,
                    .seed_probe_spec_id = *node->authored_ref_id,
                    .launch_samples_per_axis = static_cast<int>(samples_per_axis),
                    .codec_version = 2,
                    .status = "queued",
                    .requested_at_utc = now,
                    .correlation_id = correlation,
                    .causation_id = "battle-completion-" + std::to_string(completion->battle_completion_id),
                },
                &probe_run_id,
                &error)
            || probe_run_id <= 0) return ScheduleFailure(context, "create_probe_run_failed:" + error);
        auto result = neutral_->EncodeForQueueing({
            .workflow_instance_id = context.workflow_instance_id,
            .workflow_step_id = context.workflow_step_id,
            .step_key = context.step_key,
            .step_kind = context.step_kind,
            .domain_ref_id = probe_run_id,
            .step_priority = context.step_priority,
        });
        result.event_lines.push_back(
            "[field-return-probe-bootstrap] ok=" + std::string(result.root_job_set_id > 0 ? "true" : "false")
            + " battle_completion_id=" + std::to_string(completion->battle_completion_id)
            + " probe_run_id=" + std::to_string(probe_run_id));
        return result;
    }
private:
    savor::db::IAnalysisDb* analysis_db_ = nullptr;
    savor::db::IAuthoringDb* authoring_db_ = nullptr;
    std::shared_ptr<IJobPersistenceAdapter> neutral_;
};

struct FieldMaterializeJobIni {
    static constexpr const char* Section = "FieldReturnSeed.Job";
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::int64_t completion_id = 0;
    std::int64_t probe_run_id = 0;
    std::string selector;
    std::int64_t selector_value = 0;
    std::string seed_ref_kind;
    std::int64_t seed_ref_id = 0;
    std::uint32_t expected_seed = 0;
    std::string frame_hex;
    std::int64_t entry_savestate_id = 0;
    std::int64_t entry_artifact_id = 0;
    std::string entry_artifact_sha256;
    std::uint32_t run_timeout_ms = 0;
    bool required_keys_present = false;

    std::string encode() const {
        IniDoc ini;
        ini.set(Section, "workflow_instance_id", std::to_string(workflow_instance_id));
        ini.set(Section, "workflow_step_id", std::to_string(workflow_step_id));
        ini.set(Section, "completion_id", std::to_string(completion_id));
        ini.set(Section, "probe_run_id", std::to_string(probe_run_id));
        ini.set(Section, "selector", selector);
        ini.set(Section, "selector_value", std::to_string(selector_value));
        ini.set(Section, "seed_ref_kind", seed_ref_kind);
        ini.set(Section, "seed_ref_id", std::to_string(seed_ref_id));
        ini.set(Section, "expected_seed", std::to_string(expected_seed));
        ini.set(Section, "frame_hex", frame_hex);
        ini.set(Section, "entry_savestate_id", std::to_string(entry_savestate_id));
        ini.set(Section, "entry_artifact_id", std::to_string(entry_artifact_id));
        ini.set(Section, "entry_artifact_sha256", entry_artifact_sha256);
        ini.set(Section, "run_timeout_ms", std::to_string(run_timeout_ms));
        return ini.to_string_sorted();
    }
    static FieldMaterializeJobIni decode(const std::string& text) {
        const auto ini = IniDoc::parse(text);
        FieldMaterializeJobIni out{};
        out.required_keys_present = ini.has(Section, "workflow_instance_id")
            && ini.has(Section, "workflow_step_id")
            && ini.has(Section, "completion_id")
            && ini.has(Section, "probe_run_id")
            && ini.has(Section, "selector")
            && ini.has(Section, "selector_value")
            && ini.has(Section, "seed_ref_kind")
            && ini.has(Section, "seed_ref_id")
            && ini.has(Section, "expected_seed")
            && ini.has(Section, "frame_hex")
            && ini.has(Section, "entry_savestate_id")
            && ini.has(Section, "entry_artifact_id")
            && ini.has(Section, "entry_artifact_sha256")
            && ini.has(Section, "run_timeout_ms");
        out.workflow_instance_id = ini.get_i64(Section, "workflow_instance_id", 0);
        out.workflow_step_id = ini.get_i64(Section, "workflow_step_id", 0);
        out.completion_id = ini.get_i64(Section, "completion_id", 0);
        out.probe_run_id = ini.get_i64(Section, "probe_run_id", 0);
        out.selector = ini.get(Section, "selector", "");
        out.selector_value = ini.get_i64(Section, "selector_value", 0);
        out.seed_ref_kind = ini.get(Section, "seed_ref_kind", "");
        out.seed_ref_id = ini.get_i64(Section, "seed_ref_id", 0);
        out.expected_seed = ini.get_u32(Section, "expected_seed", 0);
        out.frame_hex = ini.get(Section, "frame_hex", "");
        out.entry_savestate_id = ini.get_i64(Section, "entry_savestate_id", 0);
        out.entry_artifact_id = ini.get_i64(Section, "entry_artifact_id", 0);
        out.entry_artifact_sha256 = ini.get(Section, "entry_artifact_sha256", "");
        out.run_timeout_ms = ini.get_u32(Section, "run_timeout_ms", 0);
        return out;
    }
};

bool MatchesFieldMaterializeJob(
    const savor::db::ExecutionJobRecord& job,
    const FieldMaterializeJobIni& input) {
    const bool known_seed_ref = input.seed_ref_kind == kNeutralSeedRef
        || input.seed_ref_kind == kUniqueSeedRef;
    return input.required_keys_present
        && job.program_kind == static_cast<std::int32_t>(savor::PK_SeedProbe)
        && job.program_version == 2
        && known_seed_ref
        && job.program_ref_kind == input.seed_ref_kind
        && job.program_ref_id == input.seed_ref_id
        && job.savestate_id == std::optional<std::int64_t>(input.entry_savestate_id)
        && input.workflow_instance_id > 0
        && input.workflow_step_id > 0
        && input.completion_id > 0
        && input.probe_run_id > 0
        && input.seed_ref_id > 0
        && input.entry_savestate_id > 0
        && input.entry_artifact_id > 0
        && !input.entry_artifact_sha256.empty()
        && !input.frame_hex.empty()
        && input.run_timeout_ms > 0;
}

bool MatchesUniqueSeedFrame(
    const savor::db::IAnalysisDb* analysis_db,
    const savor::db::SeedProbeUniqueSeedRow& row,
    const savor::GCInputFrame& frame) {
    if (analysis_db == nullptr || row.input_frame_id <= 0 || frame.buttons != 0) return false;
    const auto input_frame = analysis_db->GetAnalysisInputFrame(row.input_frame_id);
    const auto probe_run = analysis_db->GetSeedProbeRun(row.probe_run_id);
    const auto run_frames = probe_run.has_value()
        ? analysis_db->ListAnalysisInputSetFrames(probe_run->unique_input_set_id)
        : std::vector<savor::db::AnalysisInputSetFrameRow>{};
    const auto frame_belongs_to_run = std::count_if(
        run_frames.begin(), run_frames.end(), [&](const auto& candidate) {
            return candidate.input_frame_id == row.input_frame_id;
        }) == 1;
    return input_frame.has_value()
        && frame_belongs_to_run
        && input_frame->input_frame_id == row.input_frame_id
        && input_frame->main_x == row.main_x && input_frame->main_y == row.main_y
        && input_frame->cstick_x == row.cstick_x && input_frame->cstick_y == row.cstick_y
        && input_frame->trigger_x == row.trigger_x && input_frame->trigger_y == row.trigger_y
        && row.main_x == frame.main_x && row.main_y == frame.main_y
        && row.cstick_x == frame.c_x && row.cstick_y == frame.c_y
        && row.trigger_x == frame.trig_l && row.trigger_y == frame.trig_r;
}

bool MatchesFieldMaterializationSource(
    const savor::db::IAnalysisDb* analysis_db,
    const FieldMaterializeJobIni& input,
    const savor::GCInputFrame& frame) {
    if (analysis_db == nullptr) return false;
    const auto completion = analysis_db->GetBattleCompletion(input.completion_id);
    const auto probe_run = analysis_db->GetSeedProbeRun(input.probe_run_id);
    if (!completion.has_value() || completion->status != "COMPLETED"
        || completion->workflow_instance_id != input.workflow_instance_id
        || !completion->completion_savestate_id.has_value()
        || *completion->completion_savestate_id != input.entry_savestate_id
        || !probe_run.has_value()
        || probe_run->codec_version != 2
        || probe_run->probe_flavor != "FIELD_RETURN"
        || probe_run->entry_savestate_id != input.entry_savestate_id) return false;
    if (input.seed_ref_kind == kNeutralSeedRef) {
        const auto row = analysis_db->GetSeedProbeNeutralSeed(input.seed_ref_id);
        return input.selector == "neutral" && input.selector_value == 0
            && row.has_value() && row->probe_run_id == input.probe_run_id
            && row->neutral_seed_value >= 0 && row->neutral_seed_value <= 0xFFFFFFFFll
            && static_cast<std::uint32_t>(row->neutral_seed_value) == input.expected_seed
            && frame == savor::GCInputFrame{};
    }
    if (input.seed_ref_kind == kUniqueSeedRef) {
        const auto row = analysis_db->GetSeedProbeUniqueSeed(input.seed_ref_id);
        if (!row.has_value() || row->probe_run_id != input.probe_run_id
            || row->seed_value < 0 || row->seed_value > 0xFFFFFFFFll
            || static_cast<std::uint32_t>(row->seed_value) != input.expected_seed
            || !MatchesUniqueSeedFrame(analysis_db, *row, frame)) return false;
        return (input.selector == "seed_value" && row->seed_value == input.selector_value)
            || (input.selector == "seed_delta" && row->seed_delta == input.selector_value);
    }
    return false;
}

std::string NodeKeyFromDynamicStep(std::string step_key, std::string_view suffix) {
    if (step_key.size() <= suffix.size()
        || step_key.compare(step_key.size() - suffix.size(), suffix.size(), suffix) != 0) return {};
    step_key.resize(step_key.size() - suffix.size());
    return step_key;
}

class FieldMaterializePersistenceAdapter final : public IJobPersistenceAdapter {
public:
    FieldMaterializePersistenceAdapter(
        savor::db::IExecutionDb* execution_db,
        savor::db::IStateDb* state_db,
        savor::db::IAnalysisDb* analysis_db,
        savor::db::IAuthoringDb* authoring_db)
        : execution_db_(execution_db), state_db_(state_db), analysis_db_(analysis_db), authoring_db_(authoring_db) {}

    WorkflowStepScheduleResult EncodeForQueueing(const WorkflowStepScheduleContext& context) const override {
        WorkflowStepScheduleResult result{};
        if (execution_db_ == nullptr || state_db_ == nullptr || analysis_db_ == nullptr || authoring_db_ == nullptr
            || context.domain_ref_id <= 0) return result;
        const auto graph = execution_db_->WorkflowQueryService() != nullptr
            ? execution_db_->WorkflowQueryService()->GetWorkflowGraph(context.workflow_instance_id)
            : std::nullopt;
        const auto node_key = NodeKeyFromDynamicStep(context.step_key, "/Materialize");
        if (!graph.has_value() || node_key.empty()) return result;
        const auto binding = std::find_if(graph->input_bindings.begin(), graph->input_bindings.end(), [&](const auto& row) {
            return row.node_key == node_key && row.input_key == "completion"
                && row.data_kind == kCompletionData && row.ref_kind == kCompletionRef;
        });
        if (binding == graph->input_bindings.end()) return result;
        const auto completion = analysis_db_->GetBattleCompletion(binding->ref_id);
        const auto probe_run = analysis_db_->GetSeedProbeRun(context.domain_ref_id);
        if (!completion.has_value() || completion->status != "COMPLETED"
            || !completion->completion_savestate_id.has_value()
            || !probe_run.has_value()
            || probe_run->codec_version != 2
            || probe_run->probe_flavor != "FIELD_RETURN"
            || probe_run->entry_savestate_id != *completion->completion_savestate_id) return result;

        std::string selector = "neutral";
        std::optional<std::int64_t> selector_value;
        for (const auto& argument : graph->arguments) {
            if (argument.node_key != node_key) continue;
            if (argument.argument_key == "seed_selector" && argument.value_type == "text"
                && argument.text_value.has_value()) selector = *argument.text_value;
            if (argument.argument_key == "seed_selector_value" && argument.value_type == "integer"
                && argument.integer_value.has_value()) selector_value = *argument.integer_value;
        }

        FieldMaterializeJobIni input{};
        input.workflow_instance_id = context.workflow_instance_id;
        input.workflow_step_id = context.workflow_step_id;
        input.completion_id = completion->battle_completion_id;
        input.probe_run_id = context.domain_ref_id;
        input.selector = selector;
        input.selector_value = selector_value.value_or(0);
        savor::GCInputFrame frame{};
        if (selector == "neutral") {
            if (selector_value.has_value()) return result;
            std::optional<savor::db::SeedProbeNeutralSeedRow> neutral;
            std::string error;
            if (!analysis_db_->TryGetSeedProbeNeutralSeedForRun(context.domain_ref_id, &neutral, &error)
                || !neutral.has_value() || neutral->neutral_seed_id <= 0
                || neutral->probe_run_id != context.domain_ref_id
                || neutral->neutral_seed_value < 0 || neutral->neutral_seed_value > 0xFFFFFFFFll) return result;
            input.seed_ref_kind = kNeutralSeedRef;
            input.seed_ref_id = neutral->neutral_seed_id;
            input.expected_seed = static_cast<std::uint32_t>(neutral->neutral_seed_value);
        } else if (selector == "seed_value" || selector == "seed_delta") {
            if (!selector_value.has_value()) return result;
            const auto rows = analysis_db_->ListSeedProbeUniqueSeeds(context.domain_ref_id);
            std::vector<savor::db::SeedProbeUniqueSeedRow> matches;
            for (const auto& row : rows) {
                if ((selector == "seed_value" && row.seed_value == *selector_value)
                    || (selector == "seed_delta" && row.seed_delta == *selector_value)) matches.push_back(row);
            }
            if (matches.size() != 1 || matches.front().seed_value < 0 || matches.front().seed_value > 0xFFFFFFFFll) return result;
            const auto& row = matches.front();
            const auto input_frame = analysis_db_->GetAnalysisInputFrame(row.input_frame_id);
            const auto run_frames = analysis_db_->ListAnalysisInputSetFrames(probe_run->unique_input_set_id);
            const auto frame_belongs_to_run = std::count_if(
                run_frames.begin(), run_frames.end(), [&](const auto& candidate) {
                    return candidate.input_frame_id == row.input_frame_id;
                }) == 1;
            const auto axis_in_range = [](std::int32_t value) { return value >= 0 && value <= 255; };
            if (row.probe_run_id != context.domain_ref_id || row.input_frame_id <= 0
                || !input_frame.has_value() || !frame_belongs_to_run
                || input_frame->input_frame_id != row.input_frame_id
                || input_frame->main_x != row.main_x || input_frame->main_y != row.main_y
                || input_frame->cstick_x != row.cstick_x || input_frame->cstick_y != row.cstick_y
                || input_frame->trigger_x != row.trigger_x || input_frame->trigger_y != row.trigger_y
                || !axis_in_range(row.main_x) || !axis_in_range(row.main_y)
                || !axis_in_range(row.cstick_x) || !axis_in_range(row.cstick_y)
                || !axis_in_range(row.trigger_x) || !axis_in_range(row.trigger_y)) return result;
            input.seed_ref_kind = kUniqueSeedRef;
            input.seed_ref_id = row.unique_seed_id;
            input.expected_seed = static_cast<std::uint32_t>(row.seed_value);
            frame.main_x = static_cast<std::uint8_t>(row.main_x);
            frame.main_y = static_cast<std::uint8_t>(row.main_y);
            frame.c_x = static_cast<std::uint8_t>(row.cstick_x);
            frame.c_y = static_cast<std::uint8_t>(row.cstick_y);
            frame.trig_l = static_cast<std::uint8_t>(row.trigger_x);
            frame.trig_r = static_cast<std::uint8_t>(row.trigger_y);
        } else {
            return result;
        }
        input.frame_hex = frame.to_frame_hex();
        const auto state = state_db_->GetSavestate(*completion->completion_savestate_id);
        const auto spec = authoring_db_->GetSeedProbeSpec(probe_run->seed_probe_spec_id);
        if (!state.has_value() || !ValidStateArtifact(*state) || !spec.has_value()) return result;
        input.entry_savestate_id = state->savestate_id;
        input.entry_artifact_id = state->artifact_id;
        input.entry_artifact_sha256 = state->artifact_sha256;
        input.run_timeout_ms = ClampU32(spec->run_ms);
        if (input.run_timeout_ms == 0) return result;
        const auto input_ini = input.encode();
        const auto fingerprint = "PK=" + std::to_string(savor::PK_SeedProbe)
            + ";PV=2;target=FIELD_RETURN;mode=MATERIALIZE"
            + ";completion=" + std::to_string(input.completion_id)
            + ";probe_run=" + std::to_string(input.probe_run_id)
            + ";seed_ref=" + input.seed_ref_kind + ":" + std::to_string(input.seed_ref_id)
            + ";expected=" + std::to_string(input.expected_seed)
            + ";frame=" + input.frame_hex
            + ";selector=" + selector + ":" + std::to_string(input.selector_value)
            + ";sha=" + hash::sha256(input_ini.data(), input_ini.size());
        const auto now = savor::db::types::UtcNow();
        std::string error;
        std::int64_t job_set_id = 0;
        if (!execution_db_->CreateJobSet(
                {
                    .program_kind = static_cast<std::int32_t>(savor::PK_SeedProbe),
                    .purpose = "Field Return Seed Materialization",
                    .created_by = "field_return_seed_materializer",
                    .created_at_utc = now.time_since_epoch().count(),
                    .expected_total = 1,
                    .domain_ref_kind = input.seed_ref_kind,
                    .domain_ref_id = input.seed_ref_id,
                },
                &job_set_id,
                &error) || job_set_id <= 0) return result;
        std::int64_t job_id = 0;
        if (!execution_db_->EnqueueJob(
                {
                    .job_set_id = job_set_id,
                    .program_kind = static_cast<std::int32_t>(savor::PK_SeedProbe),
                    .program_version = 2,
                    .program_ref_kind = input.seed_ref_kind,
                    .program_ref_id = input.seed_ref_id,
                    .savestate_id = input.entry_savestate_id,
                    .fingerprint = fingerprint,
                    .priority = context.step_priority,
                    .max_attempts = 1,
                    .input_ini = input_ini,
                    .pending_until_workflow_materialized = true,
                },
                &job_id,
                &error) || job_id <= 0) return result;
        result.root_job_set_id = job_set_id;
        result.persistence = {
            .program_ref_kind = input.seed_ref_kind,
            .program_ref_id = input.seed_ref_id,
            .fingerprint = fingerprint,
            .program_version = 2,
        };
        result.event_lines.push_back("[field-return-materialize-enqueue] ok=true job=" + std::to_string(job_id)
            + " seed_ref=" + input.seed_ref_kind + ":" + std::to_string(input.seed_ref_id));
        return result;
    }
    std::int64_t DecodeDomainRefId(const JobPersistenceRecord& persisted) const override {
        return persisted.program_ref_id;
    }
private:
    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IStateDb* state_db_ = nullptr;
    savor::db::IAnalysisDb* analysis_db_ = nullptr;
    savor::db::IAuthoringDb* authoring_db_ = nullptr;
};

class FieldMaterializeRuntimeAdapter final : public IRuntimeInitAdapter {
public:
    FieldMaterializeRuntimeAdapter(
        savor::db::IExecutionDb* execution_db,
        savor::db::IStateDb* state_db,
        savor::db::IAnalysisDb* analysis_db,
        std::filesystem::path root)
        : execution_db_(execution_db), state_db_(state_db), analysis_db_(analysis_db), root_(std::move(root)) {}

    RuntimeInitRequest BuildRuntimeInit(std::int64_t job_id) const override {
        RuntimeInitRequest request{};
        request.bootstrap_profile = kFieldMaterializeStep;
        if (execution_db_ == nullptr || state_db_ == nullptr || analysis_db_ == nullptr) return request;
        const auto job = execution_db_->GetJob(job_id);
        if (!job.has_value()) return request;
        const auto input = FieldMaterializeJobIni::decode(job->input_ini);
        const auto frame = seedprobe::parse_frame_hex_or_null(input.frame_hex);
        const auto state = state_db_->GetSavestate(input.entry_savestate_id);
        if (!MatchesFieldMaterializeJob(*job, input)
            || !frame.has_value() || !MatchesFieldMaterializationSource(analysis_db_, input, *frame)
            || !state.has_value() || !ValidStateArtifact(*state)
            || state->artifact_id != input.entry_artifact_id
            || state->artifact_sha256 != input.entry_artifact_sha256) return request;
        request.savestate_ref_kind = "state_savestate";
        request.savestate_ref_id = input.entry_savestate_id;
        request.default_timeout_ms = input.run_timeout_ms;
        return request;
    }

    std::optional<savor::PSJob> MaterializePsJob(
        std::int64_t job_id,
        const RuntimeInitRequest& request) const override {
        if (execution_db_ == nullptr || state_db_ == nullptr || analysis_db_ == nullptr) return std::nullopt;
        const auto job = execution_db_->GetJob(job_id);
        if (!job.has_value()) return std::nullopt;
        const auto input = FieldMaterializeJobIni::decode(job->input_ini);
        if (!MatchesFieldMaterializeJob(*job, input)
            || input.entry_savestate_id != request.savestate_ref_id) return std::nullopt;
        const auto state = state_db_->GetSavestate(input.entry_savestate_id);
        if (!state.has_value() || !ValidStateArtifact(*state)
            || state->artifact_id != input.entry_artifact_id
            || state->artifact_sha256 != input.entry_artifact_sha256) return std::nullopt;
        const auto frame = seedprobe::parse_frame_hex_or_null(input.frame_hex);
        if (!frame.has_value() || !MatchesFieldMaterializationSource(analysis_db_, input, *frame)) {
            return std::nullopt;
        }
        const auto output = WorkRoot(root_, "field-return") / ("job-" + std::to_string(job_id))
            / "output" / "field_return_seeded.sav";
        std::filesystem::create_directories(output.parent_path());
        savor::seedprobe::EncodeSpec spec{};
        spec.frame = *frame;
        spec.run_ms = input.run_timeout_ms;
        spec.target = savor::seedprobe::SeedProbeTarget::FieldReturn;
        spec.mode = savor::seedprobe::SeedProbeMode::Materialize;
        spec.expected_seed = input.expected_seed;
        spec.output_savestate_path = output.string();
        savor::PSJob result{};
        if (!savor::seedprobe::encode_payload(spec, result.payload)) return std::nullopt;
        return result;
    }
private:
    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IStateDb* state_db_ = nullptr;
    savor::db::IAnalysisDb* analysis_db_ = nullptr;
    std::filesystem::path root_;
};

struct FieldMaterializeResultIni {
    static constexpr const char* Section = "FieldReturnSeed.Results";
    std::uint32_t w_err = 0;
    std::uint32_t dw_err = static_cast<std::uint32_t>(savor::RunToBpOutcome::Unknown);
    std::uint32_t rng_seed = 0;
    std::string savestate_path;
    bool required_keys_present = false;
    std::string encode() const {
        IniDoc ini;
        ini.set(Section, "w_err", std::to_string(w_err));
        ini.set(Section, "dw_err", std::to_string(dw_err));
        if (required_keys_present) ini.set(Section, "rng_seed", std::to_string(rng_seed));
        ini.set(Section, "savestate_path", savestate_path);
        return ini.to_string_sorted();
    }
    static FieldMaterializeResultIni decode(const std::string& text) {
        const auto ini = IniDoc::parse(text);
        FieldMaterializeResultIni out{};
        out.required_keys_present = ini.has(Section, "w_err")
            && ini.has(Section, "dw_err")
            && ini.has(Section, "rng_seed")
            && ini.has(Section, "savestate_path");
        out.w_err = ini.get_u32(Section, "w_err", 0);
        out.dw_err = ini.get_u32(Section, "dw_err", out.dw_err);
        out.rng_seed = ini.get_u32(Section, "rng_seed", 0);
        out.savestate_path = ini.get(Section, "savestate_path", "");
        return out;
    }
};

class FieldMaterializeResultMapper final : public IResultMapper {
public:
    FieldMaterializeResultMapper(
        savor::db::IExecutionDb* execution_db,
        savor::db::IStateDb* state_db,
        savor::db::IAnalysisDb* analysis_db,
        std::filesystem::path root)
        : execution_db_(execution_db), state_db_(state_db), analysis_db_(analysis_db), root_(std::move(root)) {}

    std::string BuildResultIniFromPrResult(std::int64_t, const savor::PRResult& result) const override {
        FieldMaterializeResultIni out{};
        out.w_err = result.ps.w_err;
        const bool dw_present = result.ps.ctx.get(
            savor::context::key::core::DW_RUN_OUTCOME_CODE, out.dw_err);
        const bool seed_present = result.ps.ctx.get(savor::context::key::seed::RNG_SEED, out.rng_seed);
        const bool path_present = result.ps.ctx.get(
            savor::context::key::core::LAST_SAVESTATE_PATH, out.savestate_path);
        out.required_keys_present = dw_present && seed_present && path_present && !out.savestate_path.empty();
        return out.encode();
    }

    ResultMapPayload MapPrimaryResult(std::int64_t job_id, const std::string& text) const override {
        ResultMapPayload payload{};
        payload.result_kind = "state.field_return_seeded.failed";
        if (execution_db_ == nullptr || state_db_ == nullptr || analysis_db_ == nullptr) {
            AppendTerminal(execution_db_, job_id, false, "field_return_seed_result_mapper", &payload.event_lines);
            return payload;
        }
        const auto job = execution_db_->GetJob(job_id);
        if (!job.has_value()) {
            AppendTerminal(execution_db_, job_id, false, "field_return_seed_result_mapper", &payload.event_lines);
            return payload;
        }
        const auto input = FieldMaterializeJobIni::decode(job->input_ini);
        const auto result = FieldMaterializeResultIni::decode(text);
        const auto frame = seedprobe::parse_frame_hex_or_null(input.frame_hex);
        const auto state = state_db_->GetSavestate(input.entry_savestate_id);
        const auto expected_path = WorkRoot(root_, "field-return") / ("job-" + std::to_string(job_id))
            / "output" / "field_return_seeded.sav";
        if (!MatchesFieldMaterializeJob(*job, input)
            || !frame.has_value() || !MatchesFieldMaterializationSource(analysis_db_, input, *frame)
            || !state.has_value() || !ValidStateArtifact(*state)
            || state->artifact_id != input.entry_artifact_id
            || state->artifact_sha256 != input.entry_artifact_sha256
            || !result.required_keys_present
            || result.w_err != 0
            || result.dw_err != static_cast<std::uint32_t>(savor::RunToBpOutcome::Hit)
            || result.rng_seed != input.expected_seed
            || result.savestate_path.empty()
            || !std::filesystem::is_regular_file(result.savestate_path)
            || std::filesystem::weakly_canonical(result.savestate_path)
                != std::filesystem::weakly_canonical(expected_path)) {
            AppendTerminal(execution_db_, job_id, false, "field_return_seed_result_mapper", &payload.event_lines);
            payload.event_lines.push_back("[field-return-materialize-result] ok=false expected_seed="
                + std::to_string(input.expected_seed) + " observed_seed=" + std::to_string(result.rng_seed));
            return payload;
        }
        const auto output_state = StoreSavestate(
            state_db_, result.savestate_path, "FIELD_RETURN_SEEDED",
            "Field-return reseed materialized", "field-return-seed-job-" + std::to_string(job_id),
            &payload.event_lines);
        if (!output_state.has_value()) {
            AppendTerminal(execution_db_, job_id, false, "field_return_seed_result_mapper", &payload.event_lines);
            return payload;
        }
        std::string error;
        std::int64_t derivation_id = 0;
        if (!state_db_->DeriveSavestate(
                {
                    .from_savestate_id = input.entry_savestate_id,
                    .to_savestate_id = *output_state,
                    .method_kind = "field_return_seed_materialization",
                    .source_context_kind = input.seed_ref_kind,
                    .source_context_id = input.seed_ref_id,
                    .created_at_utc = savor::db::types::UtcNow(),
                    .correlation_id = "field-return-seed-job-" + std::to_string(job_id),
                    .causation_id = "savestate-" + std::to_string(*output_state),
                },
                &derivation_id,
                &error)
            || derivation_id <= 0) {
            AppendTerminal(execution_db_, job_id, false, "field_return_seed_result_mapper", &payload.event_lines);
            payload.event_lines.push_back("[field-return-materialize-result] ok=false derivation_error=" + error);
            return payload;
        }
        if (!AppendTerminal(
                execution_db_, job_id, true, "field_return_seed_result_mapper", &payload.event_lines)) {
            payload.event_lines.push_back(
                "[field-return-materialize-result] ok=false reason=terminal_transition_failed"
                " savestate_id=" + std::to_string(*output_state));
            return payload;
        }
        payload.result_kind = "state.field_return_seeded.savestate";
        payload.result_ref_id = *output_state;
        payload.output_key = "seeded_savestate";
        payload.output_data_kind = "state.savestate_id";
        payload.output_ref_kind = "state.savestate";
        payload.output_ref_id = *output_state;
        payload.event_lines.push_back("[field-return-materialize-result] ok=true savestate_id="
            + std::to_string(*output_state) + " seed_ref=" + input.seed_ref_kind + ":"
            + std::to_string(input.seed_ref_id));
        return payload;
    }
    std::optional<ResultArtifactRef> MapPrimaryArtifact(std::int64_t) const override { return std::nullopt; }
private:
    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IStateDb* state_db_ = nullptr;
    savor::db::IAnalysisDb* analysis_db_ = nullptr;
    std::filesystem::path root_;
};

struct SeedAnchor {
    std::string ref_kind;
    std::int64_t ref_id = 0;
    std::int64_t seed_value = 0;
};

std::optional<SeedAnchor> ResolveSeedAnchor(
    const savor::db::IAnalysisDb* analysis_db,
    const savor::db::IStateDb* state_db,
    const savor::db::BattleCompletionRecord& completion,
    std::int64_t seeded_savestate_id) {
    if (analysis_db == nullptr || state_db == nullptr || !completion.completion_savestate_id.has_value()) {
        return std::nullopt;
    }
    const auto derivations = state_db->ListIncomingSavestateDerivations(seeded_savestate_id);
    std::vector<savor::db::SavestateDerivationRecord> matching;
    for (const auto& row : derivations) {
        if (row.from_savestate_id == *completion.completion_savestate_id
            && row.method_kind == "field_return_seed_materialization"
            && (row.source_context_kind == kNeutralSeedRef || row.source_context_kind == kUniqueSeedRef)
            && row.source_context_id > 0) matching.push_back(row);
    }
    if (matching.size() != 1) return std::nullopt;
    const auto& source = matching.front();
    SeedAnchor anchor{.ref_kind = source.source_context_kind, .ref_id = source.source_context_id};
    std::int64_t probe_run_id = 0;
    if (source.source_context_kind == kNeutralSeedRef) {
        const auto row = analysis_db->GetSeedProbeNeutralSeed(source.source_context_id);
        if (!row.has_value()) return std::nullopt;
        probe_run_id = row->probe_run_id;
        anchor.seed_value = row->neutral_seed_value;
    } else {
        const auto row = analysis_db->GetSeedProbeUniqueSeed(source.source_context_id);
        if (!row.has_value()) return std::nullopt;
        probe_run_id = row->probe_run_id;
        anchor.seed_value = row->seed_value;
    }
    const auto probe_run = analysis_db->GetSeedProbeRun(probe_run_id);
    if (!probe_run.has_value()
        || probe_run->codec_version != 2
        || probe_run->probe_flavor != "FIELD_RETURN"
        || probe_run->entry_savestate_id != *completion.completion_savestate_id) return std::nullopt;
    if (anchor.seed_value < 0 || anchor.seed_value > 0xFFFFFFFFll) return std::nullopt;
    return anchor;
}

struct ResultsJobIni {
    static constexpr const char* Section = "BattleResultsScreen.Job";
    std::int64_t workflow_instance_id = 0;
    std::int64_t workflow_step_id = 0;
    std::int64_t battle_results_id = 0;
    std::int64_t completion_id = 0;
    std::int64_t entry_savestate_id = 0;
    std::int64_t entry_artifact_id = 0;
    std::string entry_artifact_sha256;
    std::string seed_ref_kind;
    std::int64_t seed_ref_id = 0;
    std::uint32_t selected_seed = 0;
    std::uint32_t run_timeout_ms = 0;
    std::uint32_t acceleration_policy = 1;
    std::string manifest_base64;
    bool required_keys_present = false;

    std::string encode() const {
        IniDoc ini;
        ini.set(Section, "workflow_instance_id", std::to_string(workflow_instance_id));
        ini.set(Section, "workflow_step_id", std::to_string(workflow_step_id));
        ini.set(Section, "battle_results_id", std::to_string(battle_results_id));
        ini.set(Section, "completion_id", std::to_string(completion_id));
        ini.set(Section, "entry_savestate_id", std::to_string(entry_savestate_id));
        ini.set(Section, "entry_artifact_id", std::to_string(entry_artifact_id));
        ini.set(Section, "entry_artifact_sha256", entry_artifact_sha256);
        ini.set(Section, "seed_ref_kind", seed_ref_kind);
        ini.set(Section, "seed_ref_id", std::to_string(seed_ref_id));
        ini.set(Section, "selected_seed", std::to_string(selected_seed));
        ini.set(Section, "run_timeout_ms", std::to_string(run_timeout_ms));
        ini.set(Section, "acceleration_policy", std::to_string(acceleration_policy));
        ini.set(Section, "manifest_base64", manifest_base64);
        return ini.to_string_sorted();
    }
    static ResultsJobIni decode(const std::string& text) {
        const auto ini = IniDoc::parse(text);
        ResultsJobIni out{};
        out.required_keys_present = ini.has(Section, "workflow_instance_id")
            && ini.has(Section, "workflow_step_id")
            && ini.has(Section, "battle_results_id")
            && ini.has(Section, "completion_id")
            && ini.has(Section, "entry_savestate_id")
            && ini.has(Section, "entry_artifact_id")
            && ini.has(Section, "entry_artifact_sha256")
            && ini.has(Section, "seed_ref_kind")
            && ini.has(Section, "seed_ref_id")
            && ini.has(Section, "selected_seed")
            && ini.has(Section, "run_timeout_ms")
            && ini.has(Section, "acceleration_policy")
            && ini.has(Section, "manifest_base64");
        out.workflow_instance_id = ini.get_i64(Section, "workflow_instance_id", 0);
        out.workflow_step_id = ini.get_i64(Section, "workflow_step_id", 0);
        out.battle_results_id = ini.get_i64(Section, "battle_results_id", 0);
        out.completion_id = ini.get_i64(Section, "completion_id", 0);
        out.entry_savestate_id = ini.get_i64(Section, "entry_savestate_id", 0);
        out.entry_artifact_id = ini.get_i64(Section, "entry_artifact_id", 0);
        out.entry_artifact_sha256 = ini.get(Section, "entry_artifact_sha256", "");
        out.seed_ref_kind = ini.get(Section, "seed_ref_kind", "");
        out.seed_ref_id = ini.get_i64(Section, "seed_ref_id", 0);
        out.selected_seed = ini.get_u32(Section, "selected_seed", 0);
        out.run_timeout_ms = ini.get_u32(Section, "run_timeout_ms", 0);
        out.acceleration_policy = ini.get_u32(Section, "acceleration_policy", 1);
        out.manifest_base64 = ini.get(Section, "manifest_base64", "");
        return out;
    }
};

bool MatchesResultsJob(
    const savor::db::ExecutionJobRecord& job,
    const ResultsJobIni& input) {
    return input.required_keys_present
        && job.program_kind == static_cast<std::int32_t>(savor::PK_BattleResultsScreenRunner)
        && job.program_version == static_cast<std::int32_t>(phase::battle::endresults::PayloadVersion)
        && job.program_ref_kind == kResultsRef
        && job.program_ref_id == input.battle_results_id
        && job.savestate_id == std::optional<std::int64_t>(input.entry_savestate_id)
        && input.workflow_instance_id > 0
        && input.workflow_step_id > 0
        && input.battle_results_id > 0
        && input.completion_id > 0
        && input.entry_savestate_id > 0
        && input.entry_artifact_id > 0
        && !input.entry_artifact_sha256.empty()
        && (input.seed_ref_kind == kNeutralSeedRef || input.seed_ref_kind == kUniqueSeedRef)
        && input.seed_ref_id > 0
        && input.run_timeout_ms > 0
        && (input.acceleration_policy
                == static_cast<std::uint32_t>(phase::battle::endresults::AccelerationPolicy::RequiredOnly)
            || input.acceleration_policy
                == static_cast<std::uint32_t>(phase::battle::endresults::AccelerationPolicy::FullAdaptive))
        && !input.manifest_base64.empty();
}

bool MatchesResultsAggregate(
    const savor::db::BattleResultsRecord& aggregate,
    const ResultsJobIni& input,
    std::int64_t job_id) {
    return aggregate.battle_results_id == input.battle_results_id
        && aggregate.battle_completion_id == input.completion_id
        && aggregate.workflow_instance_id == input.workflow_instance_id
        && aggregate.workflow_step_id == input.workflow_step_id
        && aggregate.exec_job_id == std::optional<std::int64_t>(job_id)
        && aggregate.selected_seed_ref_kind == input.seed_ref_kind
        && aggregate.selected_seed_ref_id == input.seed_ref_id
        && aggregate.entry_savestate_id == input.entry_savestate_id
        && aggregate.selected_seed_value == input.selected_seed
        && aggregate.rng_effect_kind == savor::db::RngEffectKind::Preserve
        && aggregate.fixed_draw_count == std::optional<std::int64_t>(0)
        && aggregate.status == "QUEUED"
        && !aggregate.final_savestate_id.has_value()
        && !aggregate.completed_at_utc.has_value();
}

bool SameExpectedView(
    const phase::battle::endresults::ExpectedView& left,
    const phase::battle::endresults::ExpectedView& right) {
    if (left.expected_stat_waves != right.expected_stat_waves
        || left.expected_learned_waves != right.expected_learned_waves
        || left.expected_item_popup != right.expected_item_popup) return false;
    for (std::size_t character_index = 0;
         character_index < phase::battle::endresults::CharacterCount;
         ++character_index) {
        const auto& a = left.characters[character_index];
        const auto& b = right.characters[character_index];
        if (a.character_id != b.character_id || a.level_before != b.level_before
            || a.level_after != b.level_after || a.experience_before != b.experience_before
            || a.experience_after != b.experience_after
            || a.learned_magic_ids != b.learned_magic_ids) return false;
        for (std::size_t element_index = 0;
             element_index < phase::battle::endresults::ElementCount;
             ++element_index) {
            const auto& ae = a.elements[element_index];
            const auto& be = b.elements[element_index];
            if (ae.xp_before != be.xp_before || ae.xp_after != be.xp_after
                || ae.rank_before != be.rank_before || ae.rank_after != be.rank_after) return false;
        }
    }
    return true;
}

bool ValidateResultsFrozenIdentity(
    const savor::db::ExecutionJobRecord& job,
    std::int64_t job_id,
    const ResultsJobIni& input,
    const savor::db::IAnalysisDb* analysis_db,
    const savor::db::IStateDb* state_db,
    phase::battle::completion::Manifest* manifest_out = nullptr) {
    if (analysis_db == nullptr || state_db == nullptr || !MatchesResultsJob(job, input)) return false;
    const auto aggregate = analysis_db->GetBattleResults(input.battle_results_id);
    const auto completion = analysis_db->GetBattleCompletion(input.completion_id);
    const auto state = state_db->GetSavestate(input.entry_savestate_id);
    const auto manifest_bytes = savor::utils::Base64Decode(input.manifest_base64);
    phase::battle::completion::Manifest manifest{};
    if (!aggregate.has_value() || !MatchesResultsAggregate(*aggregate, input, job_id)
        || !completion.has_value() || completion->status != "COMPLETED"
        || completion->workflow_instance_id != input.workflow_instance_id
        || !completion->completion_savestate_id.has_value()
        || !completion->manifest_blob.has_value()
        || !state.has_value() || !ValidStateArtifact(*state)
        || state->artifact_id != input.entry_artifact_id
        || state->artifact_sha256 != input.entry_artifact_sha256
        || !manifest_bytes.has_value() || *manifest_bytes != *completion->manifest_blob
        || !phase::battle::completion::DecodeManifest(*manifest_bytes, manifest)
        || (manifest.invariant_flags & phase::battle::completion::RequiredManifestInvariants)
            != phase::battle::completion::RequiredManifestInvariants) return false;
    const auto anchor = ResolveSeedAnchor(analysis_db, state_db, *completion, input.entry_savestate_id);
    if (!anchor.has_value() || anchor->ref_kind != input.seed_ref_kind
        || anchor->ref_id != input.seed_ref_id || anchor->seed_value < 0
        || anchor->seed_value > 0xFFFFFFFFll
        || static_cast<std::uint32_t>(anchor->seed_value) != input.selected_seed) return false;
    if (manifest_out != nullptr) *manifest_out = std::move(manifest);
    return true;
}

std::optional<phase::battle::endresults::AccelerationPolicy> ReadAccelerationPolicy(
    const WorkflowGraphStepScheduleContext& context) {
    const auto args = FindArguments(context, "acceleration_policy");
    if (args.empty()) return phase::battle::endresults::AccelerationPolicy::FullAdaptive;
    if (args.size() != 1) return std::nullopt;
    const auto* argument = args.front();
    if (argument->value_type == "integer" && argument->integer_value.has_value()
        && (*argument->integer_value == 0 || *argument->integer_value == 1)) {
        return static_cast<phase::battle::endresults::AccelerationPolicy>(*argument->integer_value);
    }
    if (argument->value_type == "text" && argument->text_value.has_value()) {
        if (*argument->text_value == "required_only") return phase::battle::endresults::AccelerationPolicy::RequiredOnly;
        if (*argument->text_value == "full_adaptive") return phase::battle::endresults::AccelerationPolicy::FullAdaptive;
    }
    return std::nullopt;
}

class ResultsGraphAdapter final : public IWorkflowGraphJobPersistenceAdapter {
public:
    ResultsGraphAdapter(
        savor::db::IExecutionDb* execution_db,
        savor::db::IStateDb* state_db,
        savor::db::IAnalysisDb* analysis_db,
        savor::db::IAuthoringDb* authoring_db)
        : execution_db_(execution_db), state_db_(state_db), analysis_db_(analysis_db), authoring_db_(authoring_db) {}

    WorkflowStepScheduleResult EncodeForGraphQueueing(
        const WorkflowGraphStepScheduleContext& context) const override {
        if (execution_db_ == nullptr || state_db_ == nullptr || analysis_db_ == nullptr || authoring_db_ == nullptr
            || context.unit_kind != kResultsUnit || context.step_kind != kResultsStep) {
            return ScheduleFailure(context, "invalid_results_context");
        }
        const auto* completion_binding = FindBinding(context, "completion", kCompletionData);
        const auto* state_binding = FindBinding(context, "seeded_savestate", "state.savestate_id");
        if (completion_binding == nullptr || state_binding == nullptr || context.input_bindings.size() != 2
            || completion_binding->ref_kind != kCompletionRef || completion_binding->source_kind != "upstream"
            || state_binding->ref_kind != "state.savestate" || state_binding->source_kind != "upstream") {
            return ScheduleFailure(context, "results_bindings_invalid");
        }
        const auto completion = analysis_db_->GetBattleCompletion(completion_binding->ref_id);
        const auto state = state_db_->GetSavestate(state_binding->ref_id);
        if (!completion.has_value() || completion->status != "COMPLETED"
            || completion->workflow_instance_id != context.workflow_instance_id
            || !completion->manifest_blob.has_value() || !completion->completion_savestate_id.has_value()
            || !state.has_value() || !ValidStateArtifact(*state)) {
            return ScheduleFailure(context, "completion_or_seeded_state_invalid");
        }
        const auto anchor = ResolveSeedAnchor(analysis_db_, state_db_, *completion, state_binding->ref_id);
        if (!anchor.has_value()) return ScheduleFailure(context, "seed_anchor_missing_or_ambiguous");
        const auto policy = ReadAccelerationPolicy(context);
        if (!policy.has_value()) return ScheduleFailure(context, "acceleration_policy_invalid");
        const auto timeout_args = FindArguments(context, "run_ms");
        std::uint32_t timeout = 0;
        if (timeout_args.size() > 1) return ScheduleFailure(context, "duplicate_run_ms");
        if (timeout_args.size() == 1) {
            if (timeout_args.front()->value_type != "integer" || !timeout_args.front()->integer_value.has_value()
                || *timeout_args.front()->integer_value <= 0) return ScheduleFailure(context, "invalid_run_ms");
            timeout = ClampU32(*timeout_args.front()->integer_value);
        }
        if (timeout == 0) {
            const auto source_jobs = analysis_db_->ListBattleTurnJobsByOutputSavestateId(completion->entry_savestate_id);
            if (source_jobs.size() != 1 || !source_jobs.front().exec_job_id.has_value()) {
                return ScheduleFailure(context, "source_turn_timeout_provenance_missing");
            }
            const auto source_exec = execution_db_->GetJob(*source_jobs.front().exec_job_id);
            const auto wave = analysis_db_->GetBattleTurnWave(source_jobs.front().wave_id);
            const auto battle_set = wave.has_value() ? analysis_db_->GetBattleSet(wave->battle_set_id) : std::nullopt;
            if (!source_exec.has_value() || !battle_set.has_value()) return ScheduleFailure(context, "source_run_spec_missing");
            const auto resolved = ResolveRunTimeout(authoring_db_, *battle_set, *source_exec, context);
            if (!resolved.has_value()) return ScheduleFailure(context, "run_timeout_unavailable");
            timeout = *resolved;
        }

        const auto now = savor::db::types::UtcNow();
        std::string error;
        std::int64_t results_id = 0;
        if (!analysis_db_->CreateBattleResults(
                {
                    .battle_completion_id = completion->battle_completion_id,
                    .workflow_instance_id = context.workflow_instance_id,
                    .workflow_step_id = context.workflow_step_id,
                    .selected_seed_ref_kind = anchor->ref_kind,
                    .selected_seed_ref_id = anchor->ref_id,
                    .entry_savestate_id = state_binding->ref_id,
                    .selected_seed_value = anchor->seed_value,
                    .rng_effect_kind = savor::db::RngEffectKind::Preserve,
                    .fixed_draw_count = 0,
                    .status = "QUEUED",
                    .created_at_utc = now,
                    .correlation_id = "workflow-" + std::to_string(context.workflow_instance_id),
                    .causation_id = "battle-completion-" + std::to_string(completion->battle_completion_id),
                },
                &results_id,
                &error)
            || results_id <= 0) return ScheduleFailure(context, "create_results_failed:" + error);

        ResultsJobIni input{};
        input.workflow_instance_id = context.workflow_instance_id;
        input.workflow_step_id = context.workflow_step_id;
        input.battle_results_id = results_id;
        input.completion_id = completion->battle_completion_id;
        input.entry_savestate_id = state_binding->ref_id;
        input.entry_artifact_id = state->artifact_id;
        input.entry_artifact_sha256 = state->artifact_sha256;
        input.seed_ref_kind = anchor->ref_kind;
        input.seed_ref_id = anchor->ref_id;
        input.selected_seed = static_cast<std::uint32_t>(anchor->seed_value);
        input.run_timeout_ms = timeout;
        input.acceleration_policy = static_cast<std::uint32_t>(*policy);
        input.manifest_base64 = savor::utils::Base64Encode(*completion->manifest_blob);
        const auto input_ini = input.encode();
        const auto fingerprint = "PK=" + std::to_string(savor::PK_BattleResultsScreenRunner)
            + ";PV=" + std::to_string(phase::battle::endresults::PayloadVersion)
            + ";results=" + std::to_string(results_id)
            + ";completion=" + std::to_string(input.completion_id)
            + ";seed_ref=" + input.seed_ref_kind + ":" + std::to_string(input.seed_ref_id)
            + ";entry_state=" + std::to_string(input.entry_savestate_id)
            + ";sha=" + hash::sha256(input_ini.data(), input_ini.size());
        std::int64_t job_set_id = 0;
        if (!execution_db_->CreateJobSet(
                {
                    .program_kind = static_cast<std::int32_t>(savor::PK_BattleResultsScreenRunner),
                    .purpose = "Battle Results Screen",
                    .created_by = "battle.results_screen.graph_adapter",
                    .created_at_utc = now.time_since_epoch().count(),
                    .expected_total = 1,
                    .domain_ref_kind = std::string(kResultsRef),
                    .domain_ref_id = results_id,
                },
                &job_set_id,
                &error) || job_set_id <= 0) {
            FailResultsAggregate(analysis_db_, results_id, 0, std::nullopt, std::nullopt, std::nullopt, 0, 0, nullptr);
            return ScheduleFailure(context, "create_job_set_failed:" + error);
        }
        std::int64_t job_id = 0;
        if (!execution_db_->EnqueueJob(
                {
                    .job_set_id = job_set_id,
                    .program_kind = static_cast<std::int32_t>(savor::PK_BattleResultsScreenRunner),
                    .program_version = static_cast<std::int32_t>(phase::battle::endresults::PayloadVersion),
                    .program_ref_kind = kResultsRef,
                    .program_ref_id = results_id,
                    .savestate_id = input.entry_savestate_id,
                    .fingerprint = fingerprint,
                    .priority = context.step_priority,
                    .max_attempts = 1,
                    .input_ini = input_ini,
                    .pending_until_workflow_materialized = true,
                },
                &job_id,
                &error) || job_id <= 0) {
            FailResultsAggregate(analysis_db_, results_id, 0, std::nullopt, std::nullopt, std::nullopt, 0, 0, nullptr);
            return ScheduleFailure(context, "enqueue_failed:" + error);
        }
        if (!analysis_db_->BindBattleResultsExecutionJob(
                {
                    .battle_results_id = results_id,
                    .workflow_instance_id = context.workflow_instance_id,
                    .workflow_step_id = context.workflow_step_id,
                    .exec_job_id = job_id,
                },
                &error)) {
            FailResultsAggregate(analysis_db_, results_id, job_id, std::nullopt, std::nullopt, std::nullopt, 0, 0, nullptr);
            return ScheduleFailure(context, "bind_results_job_failed:" + error);
        }
        WorkflowStepScheduleResult result{};
        result.root_job_set_id = job_set_id;
        result.persistence = {
            .program_ref_kind = kResultsRef,
            .program_ref_id = results_id,
            .fingerprint = fingerprint,
            .program_version = static_cast<std::int32_t>(phase::battle::endresults::PayloadVersion),
        };
        result.event_lines.push_back("[battle-results-screen-enqueue] ok=true battle_results_id="
            + std::to_string(results_id) + " job=" + std::to_string(job_id));
        return result;
    }
private:
    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IStateDb* state_db_ = nullptr;
    savor::db::IAnalysisDb* analysis_db_ = nullptr;
    savor::db::IAuthoringDb* authoring_db_ = nullptr;
};

class ResultsRuntimeAdapter final : public IRuntimeInitAdapter {
public:
    ResultsRuntimeAdapter(
        savor::db::IExecutionDb* execution_db,
        savor::db::IStateDb* state_db,
        savor::db::IAnalysisDb* analysis_db,
        std::filesystem::path root)
        : execution_db_(execution_db), state_db_(state_db), analysis_db_(analysis_db), root_(std::move(root)) {}
    RuntimeInitRequest BuildRuntimeInit(std::int64_t job_id) const override {
        RuntimeInitRequest request{};
        request.bootstrap_profile = kResultsStep;
        if (execution_db_ == nullptr || state_db_ == nullptr || analysis_db_ == nullptr) return request;
        const auto job = execution_db_->GetJob(job_id);
        if (!job.has_value()) return request;
        const auto input = ResultsJobIni::decode(job->input_ini);
        if (!ValidateResultsFrozenIdentity(*job, job_id, input, analysis_db_, state_db_)) return request;
        request.savestate_ref_kind = "state_savestate";
        request.savestate_ref_id = input.entry_savestate_id;
        request.default_timeout_ms = input.run_timeout_ms;
        return request;
    }
    std::optional<savor::PSJob> MaterializePsJob(std::int64_t job_id, const RuntimeInitRequest& request) const override {
        if (execution_db_ == nullptr || state_db_ == nullptr || analysis_db_ == nullptr) return std::nullopt;
        const auto job = execution_db_->GetJob(job_id);
        if (!job.has_value()) return std::nullopt;
        const auto input = ResultsJobIni::decode(job->input_ini);
        const auto manifest = savor::utils::Base64Decode(input.manifest_base64);
        if (input.entry_savestate_id != request.savestate_ref_id
            || !manifest.has_value()
            || !ValidateResultsFrozenIdentity(*job, job_id, input, analysis_db_, state_db_)) return std::nullopt;
        const auto output = WorkRoot(root_, "results") / ("job-" + std::to_string(job_id)) / "output" / "battle_end.sav";
        std::filesystem::create_directories(output.parent_path());
        phase::battle::endresults::EncodeSpec spec{};
        spec.run_timeout_ms = input.run_timeout_ms;
        spec.acceleration_policy = static_cast<phase::battle::endresults::AccelerationPolicy>(input.acceleration_policy);
        spec.completion_manifest_blob = *manifest;
        spec.output_savestate_path = output.string();
        savor::PSJob result{};
        if (!phase::battle::endresults::encode_payload(spec, result.payload)) return std::nullopt;
        return result;
    }
private:
    savor::db::IExecutionDb* execution_db_ = nullptr;
    savor::db::IStateDb* state_db_ = nullptr;
    savor::db::IAnalysisDb* analysis_db_ = nullptr;
    std::filesystem::path root_;
};

struct ResultsResultIni {
    static constexpr const char* Section = "BattleResultsScreen.Results";
    std::uint32_t w_err = 0, dw_err = static_cast<std::uint32_t>(savor::RunToBpOutcome::Unknown), outcome = 0;
    std::uint32_t provider_failure = 0, runtime_failure = 0, macro_result = 0;
    std::uint32_t mismatch_flags = 0, invariant_flags = 0, entry_seed = 0, final_seed = 0;
    std::uint32_t effect_kind = 0, advance_count = 0;
    std::string savestate_path, report_base64;
    bool required_keys_present = false;
    std::string encode() const {
        IniDoc ini;
        ini.set(Section, "w_err", std::to_string(w_err)); ini.set(Section, "dw_err", std::to_string(dw_err));
        if (required_keys_present) ini.set(Section, "outcome", std::to_string(outcome));
        ini.set(Section, "provider_failure", std::to_string(provider_failure));
        ini.set(Section, "runtime_failure", std::to_string(runtime_failure)); ini.set(Section, "macro_result", std::to_string(macro_result));
        ini.set(Section, "mismatch_flags", std::to_string(mismatch_flags)); ini.set(Section, "invariant_flags", std::to_string(invariant_flags));
        ini.set(Section, "entry_seed", std::to_string(entry_seed)); ini.set(Section, "final_seed", std::to_string(final_seed));
        ini.set(Section, "effect_kind", std::to_string(effect_kind)); ini.set(Section, "advance_count", std::to_string(advance_count));
        ini.set(Section, "savestate_path", savestate_path); ini.set(Section, "report_base64", report_base64);
        return ini.to_string_sorted();
    }
    static ResultsResultIni decode(const std::string& text) {
        const auto ini = IniDoc::parse(text); ResultsResultIni out{};
        out.required_keys_present = ini.has(Section,"w_err") && ini.has(Section,"dw_err")
            && ini.has(Section,"outcome") && ini.has(Section,"provider_failure")
            && ini.has(Section,"runtime_failure") && ini.has(Section,"macro_result")
            && ini.has(Section,"mismatch_flags") && ini.has(Section,"invariant_flags")
            && ini.has(Section,"entry_seed") && ini.has(Section,"final_seed")
            && ini.has(Section,"effect_kind") && ini.has(Section,"advance_count")
            && ini.has(Section,"savestate_path") && ini.has(Section,"report_base64");
        out.w_err=ini.get_u32(Section,"w_err",0); out.dw_err=ini.get_u32(Section,"dw_err",out.dw_err); out.outcome=ini.get_u32(Section,"outcome",0);
        out.provider_failure=ini.get_u32(Section,"provider_failure",0); out.runtime_failure=ini.get_u32(Section,"runtime_failure",0); out.macro_result=ini.get_u32(Section,"macro_result",0);
        out.mismatch_flags=ini.get_u32(Section,"mismatch_flags",0); out.invariant_flags=ini.get_u32(Section,"invariant_flags",0);
        out.entry_seed=ini.get_u32(Section,"entry_seed",0); out.final_seed=ini.get_u32(Section,"final_seed",0);
        out.effect_kind=ini.get_u32(Section,"effect_kind",0); out.advance_count=ini.get_u32(Section,"advance_count",0);
        out.savestate_path=ini.get(Section,"savestate_path",""); out.report_base64=ini.get(Section,"report_base64",""); return out;
    }
};

class ResultsResultMapper final : public IResultMapper {
public:
    ResultsResultMapper(savor::db::IExecutionDb* e, savor::db::IStateDb* s, savor::db::IAnalysisDb* a, std::filesystem::path root)
        : execution_db_(e), state_db_(s), analysis_db_(a), root_(std::move(root)) {}
    std::string BuildResultIniFromPrResult(std::int64_t, const savor::PRResult& result) const override {
        ResultsResultIni out{};
        out.w_err = result.ps.w_err;
        bool context_complete = result.ps.ctx.get(savor::context::key::core::DW_RUN_OUTCOME_CODE, out.dw_err);
        context_complete = result.ps.ctx.get(savor::context::key::battleend::OUTCOME, out.outcome) && context_complete;
        context_complete = result.ps.ctx.get(savor::context::key::battleend::PROVIDER_FAILURE, out.provider_failure) && context_complete;
        context_complete = result.ps.ctx.get(savor::context::key::battleend::RUNTIME_FAILURE, out.runtime_failure) && context_complete;
        context_complete = result.ps.ctx.get(savor::context::key::battleend::MACRO_RESULT, out.macro_result) && context_complete;
        context_complete = result.ps.ctx.get(savor::context::key::battleend::MISMATCH_FLAGS, out.mismatch_flags) && context_complete;
        context_complete = result.ps.ctx.get(savor::context::key::battleend::INVARIANT_FLAGS, out.invariant_flags) && context_complete;
        context_complete = result.ps.ctx.get(savor::context::key::battleend::ENTRY_RNG_SEED, out.entry_seed) && context_complete;
        context_complete = result.ps.ctx.get(savor::context::key::battleend::FINAL_RNG_SEED, out.final_seed) && context_complete;
        context_complete = result.ps.ctx.get(savor::context::key::battleend::RNG_EFFECT_KIND, out.effect_kind) && context_complete;
        context_complete = result.ps.ctx.get(savor::context::key::battleend::RNG_ADVANCE_COUNT, out.advance_count) && context_complete;
        const bool path_present = result.ps.ctx.get(
            savor::context::key::core::LAST_SAVESTATE_PATH, out.savestate_path);
        std::string report;
        const bool report_present = result.ps.ctx.get(
            savor::context::key::battleend::REPORT_BLOB, report) && !report.empty();
        if (report_present) out.report_base64 = savor::utils::Base64Encode(report);
        out.required_keys_present = context_complete && path_present
            && !out.savestate_path.empty() && report_present;
        return out.encode();
    }
    ResultMapPayload MapPrimaryResult(std::int64_t job_id,const std::string& text) const override {
        ResultMapPayload payload{};
        payload.result_kind = "state.battle_end.failed";
        const auto job = execution_db_ != nullptr ? execution_db_->GetJob(job_id) : std::nullopt;
        if (!job.has_value() || state_db_ == nullptr || analysis_db_ == nullptr) {
            AppendTerminal(execution_db_, job_id, false, "battle_results_mapper", &payload.event_lines);
            return payload;
        }
        const auto aggregate_by_job = job->program_kind
                == static_cast<std::int32_t>(savor::PK_BattleResultsScreenRunner)
            && job->program_ref_kind == kResultsRef && job->program_ref_id > 0
            ? analysis_db_->GetBattleResults(job->program_ref_id)
            : std::nullopt;
        const auto authoritative_results_id = aggregate_by_job.has_value()
            && aggregate_by_job->exec_job_id == std::optional<std::int64_t>(job_id)
            ? aggregate_by_job->battle_results_id : 0;
        const auto input = ResultsJobIni::decode(job->input_ini);
        const auto result = ResultsResultIni::decode(text);
        const auto report = savor::utils::Base64Decode(result.report_base64);
        constexpr std::uint32_t required_invariants = phase::battle::endresults::InvariantSourcePc
            | phase::battle::endresults::InvariantLifecycleState
            | phase::battle::endresults::InvariantCompletionState
            | phase::battle::endresults::InvariantCompletionPublished
            | phase::battle::endresults::InvariantResultPointerCleared
            | phase::battle::endresults::InvariantFieldMode
            | phase::battle::endresults::InvariantRngPreserved;
        const int mismatch_count = result.required_keys_present ? CountBits(result.mismatch_flags) : 0;
        const int invariant_failure_count = result.required_keys_present
            ? CountBits(required_invariants & ~result.invariant_flags)
            : CountBits(required_invariants);
        auto fail = [&](std::optional<std::int64_t> report_artifact_id, std::string_view reason) {
            FailResultsAggregate(
                analysis_db_, authoritative_results_id, job_id,
                result.required_keys_present ? std::optional<std::int64_t>(result.entry_seed) : std::nullopt,
                result.required_keys_present ? std::optional<std::int64_t>(result.final_seed) : std::nullopt,
                report_artifact_id, mismatch_count, invariant_failure_count, &payload.event_lines);
            AppendTerminal(execution_db_, job_id, false, "battle_results_mapper", &payload.event_lines);
            payload.event_lines.push_back("[battle-results-result] ok=false reason=" + std::string(reason));
        };

        phase::battle::completion::Manifest completion_manifest{};
        const bool identity_ok = ValidateResultsFrozenIdentity(
            *job, job_id, input, analysis_db_, state_db_, &completion_manifest);
        phase::battle::endresults::Report decoded{};
        const bool report_has_blob = report.has_value();
        const bool report_decoded = report_has_blob
            && phase::battle::endresults::DecodeReport(*report, decoded);
        const bool report_outcome_ok = report_decoded
            && decoded.outcome == phase::battle::endresults::Outcome::Completed;
        const bool report_failure_ok = report_decoded
            && decoded.failure == phase::battle::endresults::FailureCode::None;
        const bool report_policy_ok = report_decoded
            && static_cast<std::uint32_t>(decoded.policy) == input.acceleration_policy;
        const bool report_expected_ok = report_decoded
            && SameExpectedView(decoded.expected, completion_manifest.expected);
        const bool report_mismatch_ok = report_decoded
            && decoded.mismatch_flags == result.mismatch_flags;
        const bool report_invariants_ok = report_decoded
            && decoded.invariant_flags == result.invariant_flags;
        const bool report_required_ok = report_decoded
            && (decoded.invariant_flags & required_invariants) == required_invariants;
        const bool report_ok = report_outcome_ok && report_failure_ok && report_policy_ok
            && report_expected_ok && report_mismatch_ok && report_invariants_ok
            && report_required_ok;
        const auto expected = WorkRoot(root_, "results") / ("job-" + std::to_string(job_id))
            / "output" / "battle_end.sav";
        const bool result_codes_ok = result.w_err == 0
            && result.dw_err == static_cast<std::uint32_t>(savor::RunToBpOutcome::Hit)
            && result.outcome == static_cast<std::uint32_t>(phase::battle::endresults::Outcome::Completed)
            && result.provider_failure == 0 && result.runtime_failure == 0 && result.macro_result == 0;
        const bool seed_effect_ok = result.entry_seed == input.selected_seed
            && result.final_seed == input.selected_seed
            && result.effect_kind == 0 && result.advance_count == 0;
        const bool path_present = !result.savestate_path.empty();
        const bool path_regular = path_present && std::filesystem::is_regular_file(result.savestate_path);
        const bool path_matches = path_regular
            && std::filesystem::weakly_canonical(result.savestate_path)
                == std::filesystem::weakly_canonical(expected);
        const bool ok = identity_ok && result.required_keys_present
            && result_codes_ok && seed_effect_ok && report_ok && path_matches;
        if (!ok) {
            payload.event_lines.push_back(
                "[battle-results-validation] identity=" + std::to_string(identity_ok)
                + " required=" + std::to_string(result.required_keys_present)
                + " codes=" + std::to_string(result_codes_ok)
                + " seed_effect=" + std::to_string(seed_effect_ok)
                + " report=" + std::to_string(report_ok)
                + " report_gates=" + std::to_string(report_has_blob)
                    + "/" + std::to_string(report_decoded)
                    + "/" + std::to_string(report_outcome_ok)
                    + "/" + std::to_string(report_failure_ok)
                    + "/" + std::to_string(report_policy_ok)
                    + "/" + std::to_string(report_expected_ok)
                    + "/" + std::to_string(report_mismatch_ok)
                    + "/" + std::to_string(report_invariants_ok)
                    + "/" + std::to_string(report_required_ok)
                + " path_present=" + std::to_string(path_present)
                + " path_regular=" + std::to_string(path_regular)
                + " path_match=" + std::to_string(path_matches)
                + " outcome=" + std::to_string(result.outcome)
                + " seeds=" + std::to_string(result.entry_seed) + "/"
                    + std::to_string(result.final_seed));
            fail(std::nullopt, "validation_failed");
            return payload;
        }
        const auto report_artifact = StoreBytesArtifact(
            state_db_, WorkRoot(root_, "results") / ("job-" + std::to_string(job_id))
                / "report" / "battle_results.berb",
            *report, ".berb", "battle-results-" + std::to_string(input.battle_results_id),
            &payload.event_lines);
        const auto final_state = StoreSavestate(
            state_db_, result.savestate_path, "BATTLE_END", "Battle results screen complete",
            "battle-results-" + std::to_string(input.battle_results_id), &payload.event_lines);
        if (!report_artifact.has_value() || !final_state.has_value()) {
            fail(report_artifact, "artifact_or_state_store_failed");
            return payload;
        }
        std::string error;
        std::int64_t derivation_id = 0;
        if (!state_db_->DeriveSavestate(
                {
                    .from_savestate_id = input.entry_savestate_id,
                    .to_savestate_id = *final_state,
                    .method_kind = "battle_results_screen",
                    .source_context_kind = kResultsRef,
                    .source_context_id = input.battle_results_id,
                    .created_at_utc = savor::db::types::UtcNow(),
                    .correlation_id = "battle-results-" + std::to_string(input.battle_results_id),
                    .causation_id = "savestate-" + std::to_string(*final_state),
                },
                &derivation_id,
                &error)
            || derivation_id <= 0
            || !analysis_db_->CompleteBattleResults(
                {
                    .battle_results_id = input.battle_results_id,
                    .final_savestate_id = *final_state,
                    .entry_rng_seed = result.entry_seed,
                    .final_rng_seed = result.final_seed,
                    .result_artifact_id = *report_artifact,
                    .mismatch_count = mismatch_count,
                    .invariant_failure_count = invariant_failure_count,
                    .status = "COMPLETED",
                    .completed_at_utc = savor::db::types::UtcNow(),
                    .correlation_id = "battle-results-" + std::to_string(input.battle_results_id),
                    .causation_id = "job-" + std::to_string(job_id),
                },
                &error)) {
            payload.event_lines.push_back("[battle-results-result] ok=false error=" + error);
            fail(report_artifact, "derivation_or_aggregate_completion_failed");
            return payload;
        }
        if (!AppendTerminal(execution_db_, job_id, true, "battle_results_mapper", &payload.event_lines)) {
            payload.event_lines.push_back(
                "[battle-results-result] ok=false reason=terminal_transition_failed"
                " battle_results_id=" + std::to_string(input.battle_results_id));
            return payload;
        }
        payload.result_kind = "state.battle_end.savestate";
        payload.result_ref_id = *final_state;
        payload.output_key = "terminal_savestate";
        payload.output_data_kind = "state.savestate_id";
        payload.output_ref_kind = "state.savestate";
        payload.output_ref_id = *final_state;
        payload.event_lines.push_back("[battle-results-result] ok=true battle_results_id="
            + std::to_string(input.battle_results_id) + " final_savestate_id="
            + std::to_string(*final_state));
        return payload;
    }
    std::optional<ResultArtifactRef> MapPrimaryArtifact(std::int64_t) const override{return std::nullopt;}
private:savor::db::IExecutionDb* execution_db_=nullptr;savor::db::IStateDb* state_db_=nullptr;savor::db::IAnalysisDb* analysis_db_=nullptr;std::filesystem::path root_;
};

} // namespace

ProgramKindDescriptor BuildBattleCompletionDescriptor(savor::db::IExecutionDb* e,savor::db::IStateDb* s,savor::db::IAnalysisDb* a,BattleEndWorkflowPhaseRegistrationConfig c){
    ProgramKindDescriptor d{};d.program_kind=savor::PK_BattleCompletionRunner;d.program_name="BattleCompletionRunner";d.graph_job_persistence=std::make_shared<CompletionGraphAdapter>(e,s,a,c.authoring_db);d.runtime_init=std::make_shared<CompletionRuntimeAdapter>(e,s,a,c.working_dir_root);d.result_mapper=std::make_shared<CompletionResultMapper>(e,s,a,c.working_dir_root);d.workflow_transition=std::make_shared<AdvanceTransition>();d.supports_workflow_orchestration=true;return d;}
ProgramKindDescriptor BuildFieldReturnSeedProbeDescriptor(savor::db::IExecutionDb* e,savor::db::IStateDb*,savor::db::IAnalysisDb* a,BattleEndWorkflowPhaseRegistrationConfig c){
    auto neutral=std::make_shared<seedprobe::NeutralProbeJobPersistenceAdapter>(e,a,c.authoring_db,savor::seedprobe::SeedProbeTarget::FieldReturn);auto base=seedprobe::BuildSeedProbeNeutralDescriptor(e,a,c.authoring_db);base.program_name="FieldReturnSeedProbe";base.job_persistence=nullptr;base.graph_job_persistence=std::make_shared<FieldReturnChainGraphAdapter>(a,c.authoring_db,neutral);base.workflow_transition=std::make_shared<FieldReturnChainTransition>();return base;}
ProgramKindDescriptor BuildFieldReturnSeedProbeGridDescriptor(savor::db::IExecutionDb* e,savor::db::IStateDb*,savor::db::IAnalysisDb* a,BattleEndWorkflowPhaseRegistrationConfig c){
    seedprobe::SeedProbeGridBlueprintConfig b{};b.program_version=2;b.target=savor::seedprobe::SeedProbeTarget::FieldReturn;auto d=seedprobe::BuildSeedProbeGridDescriptor(e,a,b,{},c.authoring_db);d.workflow_transition=std::make_shared<FieldGridTransition>();return d;}
ProgramKindDescriptor BuildFieldReturnSeedProbeUniqueDescriptor(savor::db::IExecutionDb* e,savor::db::IStateDb*,savor::db::IAnalysisDb* a,BattleEndWorkflowPhaseRegistrationConfig c){
    seedprobe::SeedProbeGridBlueprintConfig b{};b.program_version=2;b.target=savor::seedprobe::SeedProbeTarget::FieldReturn;auto d=seedprobe::BuildSeedProbeUniqueDescriptor(e,a,b,{}, {},c.authoring_db);d.workflow_transition=std::make_shared<FieldUniqueTransition>();return d;}
ProgramKindDescriptor BuildFieldReturnSeedMaterializeDescriptor(savor::db::IExecutionDb* e,savor::db::IStateDb* s,savor::db::IAnalysisDb* a,BattleEndWorkflowPhaseRegistrationConfig c){
    ProgramKindDescriptor d{};d.program_kind=savor::PK_SeedProbe;d.program_name="FieldReturnSeedMaterialize";d.job_persistence=std::make_shared<FieldMaterializePersistenceAdapter>(e,s,a,c.authoring_db);d.runtime_init=std::make_shared<FieldMaterializeRuntimeAdapter>(e,s,a,c.working_dir_root);d.result_mapper=std::make_shared<FieldMaterializeResultMapper>(e,s,a,c.working_dir_root);d.workflow_transition=std::make_shared<AdvanceTransition>();d.supports_workflow_orchestration=true;return d;}
ProgramKindDescriptor BuildBattleResultsScreenDescriptor(savor::db::IExecutionDb* e,savor::db::IStateDb* s,savor::db::IAnalysisDb* a,BattleEndWorkflowPhaseRegistrationConfig c){
    ProgramKindDescriptor d{};d.program_kind=savor::PK_BattleResultsScreenRunner;d.program_name="BattleResultsScreenRunner";d.graph_job_persistence=std::make_shared<ResultsGraphAdapter>(e,s,a,c.authoring_db);d.runtime_init=std::make_shared<ResultsRuntimeAdapter>(e,s,a,c.working_dir_root);d.result_mapper=std::make_shared<ResultsResultMapper>(e,s,a,c.working_dir_root);d.workflow_transition=std::make_shared<AdvanceTransition>();d.supports_workflow_orchestration=true;return d;}

} // namespace savor::db::execution::programdb::battleend
