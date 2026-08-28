#pragma once

#include <cstdint>
#include <string>

#include "../../Analysis/IAnalysisDb.h"
#include "../../Authoring/AuthoringRecipeMaterializer.h"
#include "../IExecutionDb.h"
#include "WorkflowComposition.h"
#include "WorkflowGraphLaunchService.h"

namespace savor::db::execution::workflow {

struct RecordBattleVictoryRequest {
    std::int64_t turn_job_id = 0;
    std::string created_by;
};

struct RecordBattleVictoryReceipt {
    std::int64_t workflow_instance_id = 0;
    bool recording_workflow_created = false;
    bool focused_existing_completion = false;
};

class BattleVictoryRecordingService {
public:
    BattleVictoryRecordingService(IAuthoringDb* authoring,
        IExecutionDb* execution, IAnalysisDb* analysis)
        : authoring_(authoring), execution_(execution), analysis_(analysis) {}

    bool Record(const RecordBattleVictoryRequest& request,
        RecordBattleVictoryReceipt* receipt_out,
        std::string* error_out = nullptr) const {
        if (!authoring_ || !execution_ || !analysis_ || !receipt_out
            || request.turn_job_id <= 0 || request.created_by.empty()) {
            return Fail("Battle Victory recording request is incomplete", error_out);
        }
        const auto job = analysis_->GetBattleTurnJob(request.turn_job_id);
        const auto wave = job ? analysis_->GetBattleTurnWave(job->wave_id)
                              : std::nullopt;
        const auto result = job && job->exec_job_id
            ? analysis_->GetBattleSingleTurnResultForExecJob(*job->exec_job_id)
            : std::nullopt;
        const auto execution_job = job && job->exec_job_id
            ? execution_->GetExecutionJob(*job->exec_job_id) : std::nullopt;
        if (!job || !wave || !result || !execution_job || !job->exec_job_id
            || execution_job->state != "SUCCEEDED"
            || !execution_job->worker_terminal_fingerprint
            || *execution_job->worker_terminal_fingerprint
                != result->worker_terminal_sha256
            || result->terminal_kind != "SUCCEEDED"
            || result->domain_outcome != std::optional<std::string>("Victory")
            || !result->ending_rng || !result->successor_savestate_id
            || job->output_savestate_id != result->successor_savestate_id) {
            return Fail("Record Victory requires a current successful durable Victory with ending RNG and successor state", error_out);
        }

        EnsurePendingVictoryRouteBranchReceipt route_branch{};
        if (!analysis_->EnsurePendingVictoryRouteBranch({
                .battle_set_id = wave->battle_set_id,
                .selected_turn_job_id = request.turn_job_id,
                .default_label = "Victory RNG " + std::to_string(*result->ending_rng),
                .created_at_utc = types::UtcNow(),
            }, &route_branch, error_out)) return false;
        const auto bind = [&](const std::int64_t workflow_id,
                              std::string status) {
            return analysis_->BindVictoryRouteBranchWorkflow({
                .selected_turn_job_id = request.turn_job_id,
                .workflow_instance_id = workflow_id,
                .status = std::move(status),
                .updated_at_utc = types::UtcNow(),
            }, error_out);
        };

        const auto completion =
            analysis_->GetBattleCompletionForSelectedTurnJob(request.turn_job_id);
        if (completion && completion->status != "COMPLETED") {
            if (!bind(completion->workflow_instance_id, "WAITING_COMPLETION"))
                return false;
            *receipt_out = {.workflow_instance_id = completion->workflow_instance_id,
                .focused_existing_completion = true};
            return true;
        }
        if (!completion) {
            for (const auto& pool : analysis_->ListBattleAdvancementPoolsForBattleTurn(
                     wave->battle_set_id, wave->turn_index)) {
                if (!pool.pool_name.starts_with("victory-completion-rng-")) continue;
                for (const auto& decision : analysis_->ListBattleAdvancementDecisionsForPool(
                         pool.battle_advancement_pool_id)) {
                    if (decision.turn_job_id == request.turn_job_id
                        && decision.decision_kind ==
                            BattleAdvancementDecisionKind::SelectedForCompletion) {
                        const auto workflow = analysis_->GetBattleWorkflowInstanceId(
                            wave->battle_set_id);
                        if (!workflow) return Fail(
                            "automatic Victory completion lost its Battle workflow identity",
                            error_out);
                        if (!bind(*workflow, "WAITING_COMPLETION")) return false;
                        *receipt_out = {.workflow_instance_id = *workflow,
                            .focused_existing_completion = true};
                        return true;
                    }
                }
            }
        }

        const bool include_completion = !completion.has_value();
        SaveWorkflowGraphResult graph{};
        if (!EnsureGraph(include_completion, &graph, error_out)) return false;
        WorkflowGraphLaunchRequest launch{
            .workflow_graph_revision_id = graph.workflow_graph_revision_id,
            .created_by = request.created_by,
            .launch_key = "battle-victory-recording:"
                + std::to_string(request.turn_job_id),
        };
        if (include_completion) {
            launch.input_bindings.push_back({
                .node_key = "complete", .input_key = "victory_turn_job",
                .data_kind = "analysis_battle.battle_turn_job",
                .ref_kind = "analysis_battle.turn_job",
                .ref_id = request.turn_job_id,
                .source_kind = "record-victory"});
        } else {
            launch.input_bindings.push_back({
                .node_key = "record", .input_key = "completion",
                .data_kind = "analysis_battle.battle_completion",
                .ref_kind = "analysis_battle.battle_completion",
                .ref_id = completion->battle_completion_id,
                .source_kind = "record-victory"});
        }
        WorkflowGraphLaunchService launcher(authoring_, execution_);
        std::int64_t workflow_id = 0;
        if (!launcher.Start(launch, &workflow_id, error_out)) return false;
        if (!bind(workflow_id, "RECORDING")) return false;
        *receipt_out = {.workflow_instance_id = workflow_id,
            .recording_workflow_created = true};
        return true;
    }

private:
    bool EnsureGraph(bool include_completion, SaveWorkflowGraphResult* result,
        std::string* error_out) const {
        const auto registry = BuildDefaultWorkflowUnitRegistry();
        const auto* completion = registry.Find("battle_completion");
        const auto* recording = registry.Find("battle_recording");
        if (!recording || (include_completion && !completion))
            return Fail("hidden Battle recording workflow units are unavailable", error_out);
        authoring::WorkflowGraphDefinition graph{
            .symbol = "battle.victory.recording",
            .name = include_completion
                ? "Internal: Complete and Record Battle Victory"
                : "Internal: Record Completed Battle Victory",
            .description = include_completion
                ? "Completes the selected Victory lineage, records it, and validates the resulting TAS movie tree."
                : "Records a completed Victory and validates the resulting TAS movie tree.",
            .hidden = true,
        };
        if (include_completion) {
            graph.nodes.push_back({.node_key = "complete",
                .unit = {completion->unit_kind},
                .display_name = completion->display_name});
            graph.external_inputs.push_back({"complete", {"victory_turn_job"}});
        }
        graph.nodes.push_back({.node_key = "record",
            .unit = {recording->unit_kind},
            .display_name = recording->display_name});
        if (include_completion) {
            graph.edges.push_back({"complete", {"completion"},
                "record", {"completion"}});
        } else {
            graph.external_inputs.push_back({"record", {"completion"}});
        }
        const authoring::AuthoringRecipeMaterializer materializer(authoring_);
        return materializer.SaveWorkflowGraph(graph, {}, result, error_out);
    }

    static bool Fail(std::string message, std::string* error_out) {
        if (error_out) *error_out = std::move(message);
        return false;
    }

    IAuthoringDb* authoring_ = nullptr;
    IExecutionDb* execution_ = nullptr;
    IAnalysisDb* analysis_ = nullptr;
};

} // namespace savor::db::execution::workflow
