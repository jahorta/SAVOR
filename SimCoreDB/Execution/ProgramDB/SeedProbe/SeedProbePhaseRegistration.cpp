#include "SeedProbePhaseRegistration.h"

namespace simcore::db::execution::programdb::seedprobe {

namespace {

const WorkflowGraphInputBinding* FindBinding(
    const WorkflowGraphStepScheduleContext& context,
    std::string_view input_key,
    std::string_view data_kind) {
    for (const auto& binding : context.input_bindings) {
        if (binding.input_key == input_key && binding.data_kind == data_kind && binding.ref_id > 0) {
            return &binding;
        }
    }
    return nullptr;
}

std::string ProbeFlavorForContext(const WorkflowGraphStepScheduleContext& context) {
    if (context.unit_kind == "dungeon_seed_probe" || context.unit_variant == "dungeon") {
        return "DUNGEON_PRE";
    }
    if (context.unit_kind == "overworld_seed_probe" || context.unit_variant == "overworld") {
        return "OVERWORLD_PRE";
    }
    return "BATTLE_PRE";
}

std::string BreakpointPolicyForContext(const WorkflowGraphStepScheduleContext& context) {
    if (!context.breakpoint_profile_key.empty()) {
        return context.breakpoint_profile_key;
    }
    return "default";
}

class SeedProbeChainGraphJobPersistenceAdapter final : public IWorkflowGraphJobPersistenceAdapter {
public:
    SeedProbeChainGraphJobPersistenceAdapter(
        simcore::db::IAnalysisDb* analysis_db,
        simcore::db::IAuthoringDb* authoring_db,
        std::shared_ptr<IJobPersistenceAdapter> neutral_job_persistence)
        : analysis_db_(analysis_db)
        , authoring_db_(authoring_db)
        , neutral_job_persistence_(std::move(neutral_job_persistence)) {
    }

    WorkflowStepScheduleResult EncodeForGraphQueueing(
        const WorkflowGraphStepScheduleContext& context) const override {
        if (analysis_db_ == nullptr
            || authoring_db_ == nullptr
            || neutral_job_persistence_ == nullptr
            || !context.workflow_graph_revision_id.has_value()
            || context.workflow_instance_id <= 0
            || context.workflow_step_id <= 0) {
            return {};
        }

        const auto graph = authoring_db_->GetWorkflowGraphRevision(*context.workflow_graph_revision_id);
        if (!graph.has_value()) {
            return {};
        }

        const simcore::db::WorkflowGraphNodeSnapshot* node = nullptr;
        const auto node_key = context.activation_graph_node_key.empty()
            ? context.step_key
            : context.activation_graph_node_key;
        for (const auto& candidate : graph->nodes) {
            if (candidate.node_key == node_key) {
                node = &candidate;
                break;
            }
        }
        const auto unit_kind = !context.unit_kind.empty() ? context.unit_kind : (node != nullptr ? node->unit_kind : "");
        if (node == nullptr
            || (unit_kind != "seed_probe_chain"
                && unit_kind != "battle_seed_probe"
                && unit_kind != "dungeon_seed_probe"
                && unit_kind != "overworld_seed_probe")
            || node->authored_ref_kind.value_or("") != "seed_probe_spec"
            || !node->authored_ref_id.has_value()
            || *node->authored_ref_id <= 0) {
            return {};
        }

        const auto* savestate = FindBinding(context, "entry_savestate", "state.savestate_id");
        if (savestate == nullptr) {
            return {};
        }

        const auto now = simcore::db::types::UtcNow();
        const auto aggregate = std::to_string(context.workflow_instance_id)
            + "-" + std::to_string(context.workflow_step_id);

        std::string error;
        std::int64_t probe_set_id = 0;
        if (!analysis_db_->CreateSeedProbeSet(
                {
                    .name = "workflow-seed-probe-" + aggregate,
                    .probe_flavor = ProbeFlavorForContext(context),
                    .breakpoint_policy_name = BreakpointPolicyForContext(context),
                    .segment_source_kind = "workflow_graph",
                    .created_at_utc = now,
                    .event_id = "workflow-seedprobe-set-" + aggregate,
                    .correlation_id = "workflow-instance-" + std::to_string(context.workflow_instance_id),
                    .causation_id = "workflow-step-" + std::to_string(context.workflow_step_id),
                },
                &probe_set_id,
                &error)) {
            return {};
        }

        std::int64_t probe_run_id = 0;
        if (!analysis_db_->RequestSeedProbeRun(
                {
                    .probe_set_id = probe_set_id,
                    .entry_savestate_id = savestate->ref_id,
                    .seed_probe_spec_id = *node->authored_ref_id,
                    .codec_version = 1,
                    .status = "queued",
                    .requested_at_utc = now,
                    .event_id = "workflow-seedprobe-run-" + aggregate,
                    .correlation_id = "workflow-instance-" + std::to_string(context.workflow_instance_id),
                    .causation_id = "workflow-step-" + std::to_string(context.workflow_step_id),
                },
                &probe_run_id,
                &error)) {
            return {};
        }

        auto result = neutral_job_persistence_->EncodeForQueueing(probe_run_id);
        result.event_lines.push_back(
            "[workflow-graph-seedprobe-bootstrap] workflow_instance_id="
            + std::to_string(context.workflow_instance_id)
            + " workflow_step_id=" + std::to_string(context.workflow_step_id)
            + " probe_set_id=" + std::to_string(probe_set_id)
            + " probe_run_id=" + std::to_string(probe_run_id)
            + " entry_savestate_id=" + std::to_string(savestate->ref_id));
        return result;
    }

private:
    simcore::db::IAnalysisDb* analysis_db_ = nullptr;
    simcore::db::IAuthoringDb* authoring_db_ = nullptr;
    std::shared_ptr<IJobPersistenceAdapter> neutral_job_persistence_;
};

class SeedProbeChainTransitionHandler final : public IWorkflowTransitionHandler {
public:
    WorkflowTransitionDecision EvaluateTransition(const WorkflowTransitionContext& context) const override {
        WorkflowTransitionDecision decision{};
        if (!context.input_ref_id.has_value() || *context.input_ref_id <= 0) {
            decision.should_advance = false;
            decision.blocked_reason = "seed_probe_chain missing descriptor-created probe run id";
            return decision;
        }

        if (context.failed_total > 0) {
            decision.should_advance = false;
            decision.blocked_reason = "seed_probe_chain previous_step_failed";
            return decision;
        }

        decision.should_advance = true;
        decision.spawn_steps.push_back(
            WorkflowTransitionDecision::DynamicStep{
                .step_key = context.step_key + "/Grid",
                .step_kind = "seedprobe.grid",
                .input_ref_kind = std::string("sp_probe_run"),
                .input_ref_id = *context.input_ref_id,
                .priority = 0,
                .max_attempts = 1,
            });
        return decision;
    }
};

ProgramKindDescriptor BuildSeedProbeChainDescriptor(
    simcore::db::IAnalysisDb* analysis_db,
    simcore::db::IAuthoringDb* authoring_db,
    const ProgramKindDescriptor& neutral_descriptor) {
    ProgramKindDescriptor descriptor{};
    descriptor.program_kind = simcore::PK_SeedProbe;
    descriptor.program_name = "SeedProbeChain";
    descriptor.graph_job_persistence = std::make_shared<SeedProbeChainGraphJobPersistenceAdapter>(
        analysis_db,
        authoring_db,
        neutral_descriptor.job_persistence);
    descriptor.runtime_init = neutral_descriptor.runtime_init;
    descriptor.result_mapper = neutral_descriptor.result_mapper;
    descriptor.result_payload_writer = neutral_descriptor.result_payload_writer;
    descriptor.workflow_transition = std::make_shared<SeedProbeChainTransitionHandler>();
    descriptor.supports_workflow_orchestration = true;
    return descriptor;
}

} // namespace

void RegisterSeedProbePhaseDescriptors(
    ProgramKindRegistry* registry,
    simcore::db::IExecutionDb* execution_db,
    simcore::db::IAnalysisDb* analysis_db,
    SeedProbePhaseRegistrationConfig config) {
    if (registry == nullptr) {
        return;
    }

    auto neutral = BuildSeedProbeNeutralDescriptor(execution_db, analysis_db, config.authoring_db);
    auto grid = BuildSeedProbeGridDescriptor(
        execution_db,
        analysis_db,
        config.blueprint,
        config.grid,
        config.authoring_db);
    auto unique = BuildSeedProbeUniqueDescriptor(
        execution_db,
        analysis_db,
        config.blueprint,
        config.unique,
        std::move(config.unique_completion_gate),
        config.authoring_db);
    auto chain = BuildSeedProbeChainDescriptor(
        analysis_db,
        config.authoring_db,
        neutral);

    (void)registry->Register(neutral);
    (void)registry->RegisterForStepKind("seed_probe_chain", chain);
    (void)registry->RegisterForStepKind("seedprobe.neutral", neutral);
    (void)registry->RegisterForStepKind("seedprobe.grid", grid);
    (void)registry->RegisterForStepKind("seedprobe.unique", unique);
}

} // namespace simcore::db::execution::programdb::seedprobe
