#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include "Common/Migrations/MigrationRunner.h"
#include "Common/DbService.h"
#include "Common/DbConfigPaths.h"
#include "Common/Events/EventCatalog.h"
#include "Common/Events/EventPayloadDispatch.h"
#include "Common/Events/EventPayloadValidation.h"
#include "Common/Events/EventTypeFormat.h"
#include "Common/Events/OutboxRelay.h"
#include "SavorDb.h"
#include "Analysis/SqliteAnalysisDb.h"
#include "Archive/SqliteArchiveDb.h"
#include "Archive/ArchivePackageService.h"
#include "Archive/RehydrateExecutor.h"
#include "UIRead/SqliteUiReadDb.h"
#include "Execution/ArchiveWorkflowCommands.h"
#include "Execution/Workflow/SqliteExecutionDb.h"
#include "Execution/Jobs/JobEventOrchestration.h"
#include "Execution/Workflow/WorkflowComposition.h"
#include "Execution/Workflow/WorkflowLaunchContract.h"
#include "Execution/Workflow/WorkflowIntegrityChecks.h"
#include "Execution/Workflow/WorkflowProjector.h"
#include "Execution/Workflow/WorkflowRecoveryService.h"
#include "Execution/Workflow/WorkflowStepSettlementGate.h"
#include "UIRead/Projectors/ProjectorContract.h"
#include "Execution/WorkflowCoordinatorBridge.h"
#include "Execution/WorkflowSchedulerAdapter.h"
#include "Execution/StepInputAggregationService.h"
#include "Runner/Runtime/FullPhase/FullPhaseProgram.h"

#include "common/RecordingExecutionDb.h"
#include "common/MapperExecutionDb.h"
#include "common/AlwaysAdvanceTransitionHandler.h"
#include "common/SqliteDbFixture.h"
#include "common/savordb_helpers.h"

namespace savordb {

namespace {

} // namespace

TEST(DbMigrateMigrationsIntegration, DISABLED_FilesystemSourceHasMigrationPerContext) {
    namespace fs = std::filesystem;
    using namespace savor::db::migrations;

    const auto root = fs::weakly_canonical(fs::path("../../SavorDb/migration"));
    const MigrationSourceOptions filesystem_options{
        .source_kind = MigrationSourceKind::Filesystem,
        .filesystem_root = root,
    };

    for (const auto context : ListAllMigrationContexts()) {
        const auto entries = LoadContextMigrations(context, filesystem_options);
        ASSERT_FALSE(entries.empty()) << "Expected at least one migration in context " << ToString(context);
    }
}

TEST(Stage5WorkflowComposition, DefaultUnitsModelCanonicalTypedChains) {
    using namespace savor::db::execution::workflow;

    const auto registry = BuildDefaultWorkflowUnitRegistry();
    const auto* establish = registry.Find("tas_movie_establish_root_cursor");
    const auto* validate_root = registry.Find("tas_movie_validate_root");
    const auto* validate_tree = registry.Find("tas_movie_validate_tree");
    const auto* sterilize = registry.Find("tas_movie_checkpoint_sterilize");
    const auto* seed_probe = registry.Find("seed_probe");
    const auto* battle = registry.Find("battle");
    ASSERT_NE(establish, nullptr);
    ASSERT_NE(validate_root, nullptr);
    ASSERT_NE(validate_tree, nullptr);
    ASSERT_NE(sterilize, nullptr);
    ASSERT_NE(seed_probe, nullptr);
    ASSERT_NE(battle, nullptr);
    EXPECT_EQ(registry.Find("seed_probe_chain"), nullptr);
    EXPECT_EQ(registry.Find("battle_seed_probe"), nullptr);
    EXPECT_EQ(registry.Find("dungeon_seed_probe"), nullptr);
    EXPECT_EQ(registry.Find("overworld_seed_probe"), nullptr);

    ASSERT_EQ(establish->required_inputs.size(), 1u);
    EXPECT_EQ(establish->required_inputs[0].data_kind, "state_artifact.dtm_artifact_id");
    EXPECT_EQ(establish->required_inputs[0].ref_kind, "state_artifact");
    ASSERT_EQ(establish->possible_outputs.size(), 2u);
    EXPECT_TRUE(std::any_of(
        establish->possible_outputs.begin(),
        establish->possible_outputs.end(),
        [](const auto& output) {
            return output.key == "root_establishment"
                && output.data_kind
                    == "analysis.tas_movie_root_establishment_attempt_id"
                && output.ref_kind == "tmv_root_establishment_attempt";
        }));
    ASSERT_EQ(validate_root->required_inputs.size(), 1u);
    EXPECT_EQ(validate_root->required_inputs[0].data_kind, "analysis.tas_movie_root_establishment_attempt_id");
    EXPECT_EQ(validate_root->required_inputs[0].ref_kind, "tmv_root_establishment_attempt");
    EXPECT_TRUE(std::any_of(
        validate_root->possible_outputs.begin(),
        validate_root->possible_outputs.end(),
        [](const auto& output) {
            return output.key == "validated_checkpoint_savestate"
                && output.data_kind == "state.movie_paired_savestate_id";
        }));
    ASSERT_EQ(validate_tree->required_inputs.size(), 1u);
    EXPECT_EQ(validate_tree->required_inputs[0].data_kind, "state.tas_movie_tree_id");
    EXPECT_EQ(validate_tree->required_inputs[0].ref_kind, "state_tas_movie_tree");
    ASSERT_EQ(sterilize->required_inputs.size(), 1u);
    EXPECT_EQ(sterilize->required_inputs[0].data_kind,
        "state.movie_paired_savestate_id");
    ASSERT_EQ(sterilize->possible_outputs.size(), 1u);
    EXPECT_EQ(sterilize->possible_outputs[0].data_kind,
        "state.movie_inactive_savestate_id");
    EXPECT_EQ(registry.Find("tas_movie"), nullptr);
    ASSERT_EQ(seed_probe->required_inputs.size(), 1u);
    EXPECT_EQ(seed_probe->required_inputs[0].data_kind,
        "state.movie_inactive_savestate_id");
    ASSERT_EQ(seed_probe->possible_outputs.size(), 1u);
    EXPECT_EQ(seed_probe->possible_outputs[0].data_kind, "analysis.seed_probe_run");
    EXPECT_EQ(seed_probe->possible_outputs[0].ref_kind, "sp_probe_run");
    EXPECT_FALSE(seed_probe->hidden);
    EXPECT_TRUE(seed_probe->standalone_launchable);
    EXPECT_TRUE(seed_probe->unit_variant.empty());
    EXPECT_TRUE(seed_probe->breakpoint_profile_key.empty());
    EXPECT_EQ(validate_root->standalone_presentation_family_key,
        "tas_movie_validation");
    EXPECT_EQ(validate_tree->standalone_presentation_family_key,
        "tas_movie_validation");
    EXPECT_EQ(validate_root->standalone_presentation_family_display_name,
        "TAS Movie Validation");
    EXPECT_EQ(validate_tree->standalone_presentation_family_display_name,
        "TAS Movie Validation");
    EXPECT_EQ(validate_tree->display_name,
        "Validate Recorded TAS Branch");
    const auto standalone = BuildStandalonePresentationEntries(
        registry.ListUnits());
    const auto validation_entry = std::ranges::find(
        standalone,
        std::string("tas_movie_validation"),
        &WorkflowStandalonePresentationEntry::presentation_key);
    ASSERT_NE(validation_entry, standalone.end());
    EXPECT_EQ(validation_entry->display_name, "TAS Movie Validation");
    ASSERT_EQ(validation_entry->members.size(), 2u);
    EXPECT_TRUE(std::ranges::any_of(
        validation_entry->members,
        [](const auto& member) {
            return member.unit_kind == "tas_movie_validate_root";
        }));
    EXPECT_TRUE(std::ranges::any_of(
        validation_entry->members,
        [](const auto& member) {
            return member.unit_kind == "tas_movie_validate_tree";
        }));
    EXPECT_TRUE(std::ranges::any_of(
        standalone,
        [](const auto& entry) {
            return entry.presentation_key == "seed_probe" &&
                entry.members.size() == 1u &&
                entry.members.front().unit_kind == "seed_probe";
        }));
    ASSERT_EQ(battle->required_inputs.size(), 2u);
    ASSERT_EQ(battle->authored_refs.size(), 1u);
    EXPECT_EQ(battle->authored_refs[0].ref_kind, "authoring.battle_plan");
    EXPECT_EQ(battle->required_inputs[0].data_kind,
        "analysis.seed_probe_run");
    EXPECT_EQ(battle->required_inputs[1].data_kind,
        "analysis_battle.battle_context_id");
    EXPECT_TRUE(std::any_of(
        battle->possible_outputs.begin(),
        battle->possible_outputs.end(),
        [](const auto& output) {
            return output.key == "battle_set"
                && output.data_kind == "analysis_battle.battle_set"
                && output.ref_kind == "analysis_battle.battle_set";
        }));

    for (const auto& unit : registry.ListUnits()) {
        for (const auto& input : unit.required_inputs) EXPECT_FALSE(input.ref_kind.empty());
        for (const auto& output : unit.possible_outputs) EXPECT_FALSE(output.ref_kind.empty());
    }
}

TEST(Stage5WorkflowComposition, ValidatesTasSeedProbeBattleCompatibility) {
    using namespace savor::db::execution::workflow;

    const auto registry = BuildDefaultWorkflowUnitRegistry();
    const WorkflowCompositionService service(&registry);

    WorkflowCompositionSpec composition;
    composition.nodes = {
        { .node_key = "tas_1", .unit_kind = "tas_movie_establish_root_cursor" },
    };
    composition.external_inputs = {
        { .node_key = "tas_1", .input_key = "root_dtm", .data_kind = "state_artifact.dtm_artifact_id", .ref_kind = "state_artifact", .ref_id = 10 },
    };

    const auto preview = service.Preview(composition);
    EXPECT_TRUE(preview.valid);
    EXPECT_TRUE(preview.issues.empty());
    ASSERT_EQ(preview.nodes.size(), 1u);
    EXPECT_EQ(preview.nodes[0].resolved_inputs.size(), 1u);
}

TEST(Stage5WorkflowComposition, AcceptsOnlyOutputPresentGuardWithoutValue) {
    using namespace savor::db::execution::workflow;

    const auto registry = BuildDefaultWorkflowUnitRegistry();
    const WorkflowCompositionService service(&registry);
    WorkflowCompositionSpec composition;
    composition.nodes = {
        { .node_key = "validate", .unit_kind = "tas_movie_validate_root" },
        { .node_key = "sterilize", .unit_kind = "tas_movie_checkpoint_sterilize" },
        { .node_key = "probe", .unit_kind = "seed_probe" },
    };
    composition.external_inputs = {{
        .node_key = "validate",
        .input_key = "root_establishment",
        .data_kind = "analysis.tas_movie_root_establishment_attempt_id",
        .ref_kind = "tmv_root_establishment_attempt",
        .ref_id = 7,
    }};
    composition.output_bindings = {{
        .from_node_key = "validate",
        .output_key = "validated_checkpoint_savestate",
        .to_node_key = "sterilize",
        .input_key = "paired_checkpoint_savestate",
        .guard_kind = std::string(savor::db::kWorkflowOutputPresentGuard),
    }, {
        .from_node_key = "sterilize",
        .output_key = "sterilized_checkpoint_savestate",
        .to_node_key = "probe",
        .input_key = "entry_savestate",
        .guard_kind = std::string(savor::db::kWorkflowOutputPresentGuard),
    }};
    EXPECT_TRUE(service.Preview(composition).valid);

    composition.output_bindings.back().guard_kind = "unknown_guard";
    EXPECT_FALSE(service.Preview(composition).valid);
    composition.output_bindings.back().guard_kind =
        std::string(savor::db::kWorkflowOutputPresentGuard);
    composition.output_bindings.back().guard_value = "illegal";
    EXPECT_FALSE(service.Preview(composition).valid);
}

TEST(Stage5WorkflowComposition, ReportsUnresolvedAndMismatchedInputs) {
    using namespace savor::db::execution::workflow;

    const auto registry = BuildDefaultWorkflowUnitRegistry();
    const WorkflowCompositionService service(&registry);

    WorkflowCompositionSpec unresolved;
    unresolved.nodes = {
        { .node_key = "battle_1", .unit_kind = "battle" },
    };
    auto preview = service.Preview(unresolved);
    EXPECT_FALSE(preview.valid);
    ASSERT_EQ(preview.issues.size(), 2u);

    WorkflowCompositionSpec mismatched;
    mismatched.nodes = {
        { .node_key = "probe_1", .unit_kind = "seed_probe" },
        { .node_key = "battle_1", .unit_kind = "battle" },
    };
    mismatched.external_inputs = {
        { .node_key = "probe_1", .input_key = "entry_savestate", .data_kind = "state.movie_inactive_savestate_id", .ref_kind = "state.savestate", .ref_id = 99 },
    };
    mismatched.output_bindings = {
        { .from_node_key = "probe_1", .output_key = "seed_probe_run", .to_node_key = "battle_1", .input_key = "battle_context" },
    };

    preview = service.Preview(mismatched);
    EXPECT_FALSE(preview.valid);
    ASSERT_FALSE(preview.issues.empty());
    EXPECT_TRUE(std::any_of(
        preview.issues.begin(),
        preview.issues.end(),
        [](const auto& issue) {
            return issue.message.find("type mismatch") != std::string::npos;
        }));
}

TEST(Stage5WorkflowLaunchContract, NormalizesDefaultsAndRejectsMalformedTypedLaunches) {
    using namespace savor::db;
    using namespace savor::db::execution::workflow;

    const auto registry = BuildDefaultWorkflowUnitRegistry();
    const auto make_graph = [](const WorkflowUnitDefinition& unit) {
        savor::db::WorkflowGraphSnapshot graph{};
        WorkflowGraphNodeSnapshot node{};
        node.node_key = "unit_1";
        node.unit_kind = unit.unit_kind;
        node.display_name = unit.display_name;
        if (!unit.authored_refs.empty()) {
            node.authored_ref_kind = unit.authored_refs.front().ref_kind;
            node.authored_ref_id = 17;
        }
        for (const auto& input : unit.required_inputs) {
            node.inputs.push_back({ input.key, input.data_kind, input.ref_kind, input.display_name, input.required });
        }
        for (const auto& output : unit.possible_outputs) {
            node.possible_outputs.push_back({ output.key, output.data_kind, output.ref_kind, output.display_name });
        }
        for (const auto& argument : unit.launch_arguments) {
            std::string value_type;
            switch (argument.value_type) {
            case WorkflowLaunchArgumentValueType::Integer: value_type = "integer"; break;
            case WorkflowLaunchArgumentValueType::Text: value_type = "text"; break;
            case WorkflowLaunchArgumentValueType::Boolean: value_type = "boolean"; break;
            case WorkflowLaunchArgumentValueType::Json: value_type = "json"; break;
            case WorkflowLaunchArgumentValueType::Choice: value_type = "choice"; break;
            }
            WorkflowGraphNodeArgumentSnapshot persisted_argument{
                argument.key,
                argument.display_name,
                value_type,
                argument.required,
                argument.default_value,
                argument.minimum_integer,
                argument.maximum_integer,
            };
            for (const auto& choice : argument.choices) {
                persisted_argument.choices.push_back({ choice.value, choice.display_name });
            }
            node.arguments.push_back(std::move(persisted_argument));
        }
        for (const auto& constraint : unit.launch_argument_constraints) {
            node.argument_constraints.push_back({
                constraint.lesser_or_equal_key,
                constraint.greater_or_equal_key,
                constraint.message,
            });
        }
        graph.nodes.push_back(std::move(node));
        return graph;
    };

    const auto* seed = registry.Find("seed_probe");
    ASSERT_NE(seed, nullptr);
    auto seed_graph = make_graph(*seed);
    std::vector<WorkflowLaunchInputValue> seed_inputs{{
        "unit_1", "entry_savestate", "state.movie_inactive_savestate_id",
        "state.savestate", 91, "external",
    }};
    auto result = WorkflowLaunchContractValidator::Validate(seed_graph, registry, seed_inputs, {});
    ASSERT_TRUE(result.valid);
    ASSERT_EQ(result.normalized_arguments.size(), 1u);
    EXPECT_EQ(result.normalized_arguments.front().argument_key, "samples_per_axis");
    EXPECT_EQ(result.normalized_arguments.front().integer_value, 5);

    auto wrong_ref = seed_inputs;
    wrong_ref.front().ref_kind = "state_artifact";
    EXPECT_FALSE(WorkflowLaunchContractValidator::Validate(seed_graph, registry, wrong_ref, {}).valid);
    auto duplicate = seed_inputs;
    duplicate.push_back(seed_inputs.front());
    EXPECT_FALSE(WorkflowLaunchContractValidator::Validate(seed_graph, registry, duplicate, {}).valid);
    EXPECT_FALSE(WorkflowLaunchContractValidator::Validate(seed_graph, registry, seed_inputs, {{
        "unit_1", "unknown", "integer", 1, std::nullopt, "test",
    }}).valid);

    const auto* validate = registry.Find("tas_movie_validate_root");
    ASSERT_NE(validate, nullptr);
    auto validate_graph = make_graph(*validate);
    auto& rtc = *std::find_if(validate_graph.nodes.front().arguments.begin(),
        validate_graph.nodes.front().arguments.end(), [](const auto& argument) {
            return argument.argument_key == "rtc";
        });
    rtc.binding_mode = SaveWorkflowGraphNodeArgumentCommand::BindingMode::Constant;
    rtc.constant_value = "0";
    const std::vector<WorkflowLaunchInputValue> validate_inputs{{
        "unit_1", "root_establishment",
        "analysis.tas_movie_root_establishment_attempt_id",
        "tmv_root_establishment_attempt", 44, "external",
    }};
    const auto constant_result = WorkflowLaunchContractValidator::Validate(
        validate_graph, registry, validate_inputs, {});
    ASSERT_TRUE(constant_result.valid);
    ASSERT_EQ(constant_result.normalized_arguments.size(), 1u);
    EXPECT_EQ(constant_result.normalized_arguments.front().integer_value, 0);
    EXPECT_EQ(constant_result.normalized_arguments.front().source_kind,
        "graph_constant");
    EXPECT_FALSE(WorkflowLaunchContractValidator::Validate(
        validate_graph, registry, validate_inputs, {{
            "unit_1", "rtc", "integer", 5, std::nullopt, "test",
        }}).valid);

    const auto* battle = registry.Find("battle");
    ASSERT_NE(battle, nullptr);
    auto battle_graph = make_graph(*battle);
    std::vector<WorkflowLaunchInputValue> battle_inputs;
    for (const auto& input : battle->required_inputs) {
        battle_inputs.push_back({ "unit_1", input.key, input.data_kind, input.ref_kind, 42, "external" });
    }
    EXPECT_FALSE(WorkflowLaunchContractValidator::Validate(
        battle_graph, registry, battle_inputs, {}).valid);
    EXPECT_FALSE(WorkflowLaunchContractValidator::Validate(battle_graph, registry, battle_inputs, {
        { "unit_1", "fake_attack_min", "integer", 5, std::nullopt, "test" },
        { "unit_1", "fake_attack_max", "integer", 4, std::nullopt, "test" },
        { "unit_1", "continuation_mode", "choice", std::nullopt, std::string("automatic_best_per_ending_rng"), "test" },
    }).valid);
    EXPECT_FALSE(WorkflowLaunchContractValidator::Validate(battle_graph, registry, battle_inputs, {
        { "unit_1", "continuation_mode", "choice", std::nullopt, std::string("unknown"), "test" },
    }).valid);
    const auto valid_battle = WorkflowLaunchContractValidator::Validate(battle_graph, registry, battle_inputs, {
        { "unit_1", "continuation_mode", "choice", std::nullopt, std::string("manual_selection"), "test" },
    });
    ASSERT_TRUE(valid_battle.valid);
    const auto continuation = std::ranges::find(
        valid_battle.normalized_arguments,
        std::string("continuation_mode"),
        &WorkflowLaunchArgumentValue::argument_key);
    ASSERT_NE(continuation, valid_battle.normalized_arguments.end());
    EXPECT_EQ(continuation->value_type, "choice");
    EXPECT_EQ(continuation->text_value.value_or(""), "manual_selection");

    const auto* validate_root = registry.Find("tas_movie_validate_root");
    ASSERT_NE(validate_root, nullptr);
    const auto validate_root_graph = make_graph(*validate_root);
    const std::vector<WorkflowLaunchInputValue> validate_root_inputs{{
        "unit_1", "root_establishment", "analysis.tas_movie_root_establishment_attempt_id",
        "tmv_root_establishment_attempt", 7, "external",
    }};
    EXPECT_FALSE(WorkflowLaunchContractValidator::Validate(validate_root_graph, registry, validate_root_inputs, {}).valid);
    EXPECT_FALSE(WorkflowLaunchContractValidator::Validate(validate_root_graph, registry, validate_root_inputs, {{
        "unit_1", "rtc", "integer", -1, std::nullopt, "test",
    }}).valid);
    EXPECT_TRUE(WorkflowLaunchContractValidator::Validate(validate_root_graph, registry, validate_root_inputs, {{
        "unit_1", "rtc", "integer", 555, std::nullopt, "test",
    }}).valid);
}

TEST(Stage3cCoordinatorBridge, DeduplicatesTerminalSignalsAndSchedulesReadySteps) {
    using namespace savor::runner::parallel::savordb;

    int terminal_count = 0;
    WorkflowCoordinatorBridge bridge;
    bridge.SetTerminalCallback([&](const TerminalJobSetSignal&) {
        ++terminal_count;
    });

    const TerminalJobSetSignal signal{
        .workflow_instance_id = 1,
        .workflow_step_id = 2,
        .job_set_id = 3,
        .terminal_state = "FAILED",
    };

    EXPECT_TRUE(bridge.NotifyTerminal(signal));
    EXPECT_FALSE(bridge.NotifyTerminal(signal));
    EXPECT_EQ(terminal_count, 1);

    WorkflowSchedulerAdapter adapter([](const WorkflowReadyStep& step) {
        return ScheduledJobSet{ .job_set_id = 1234, .workflow_step_id = step.workflow_step_id };
    });
    const auto scheduled = adapter.MaterializeReadyStep({ .workflow_instance_id = 9, .workflow_step_id = 44, .step_key = "Grid", .step_kind = "seedprobe.grid", .priority = 10 });
    EXPECT_EQ(scheduled.job_set_id, 1234);
    EXPECT_EQ(scheduled.workflow_step_id, 44);
}


TEST(WorkflowStepSettlementGate, MismatchThenTerminalFail) {
    using namespace savor::db::execution::workflow;

    StepSettlementGateService gate;
    const StepSettlementSnapshot snapshot{
        .workflow_step_id = 500,
        .job_set_id = 900,
        .expected_total = 10,
        .discovered_total = 9,
        .settled_total = 9,
    };

    const auto first = gate.Evaluate(snapshot);
    EXPECT_FALSE(first.can_transition);
    EXPECT_FALSE(first.workflow_fail);
    EXPECT_EQ(first.blocked_reason.value_or(""), "STEP_BLOCKED_COUNT_MISMATCH");

    const auto second = gate.Evaluate(snapshot);
    EXPECT_FALSE(second.can_transition);
    EXPECT_TRUE(second.workflow_fail);
    EXPECT_EQ(second.blocked_reason.value_or(""), "STEP_BLOCKED_COUNT_MISMATCH_TERMINAL_FAIL");
}

TEST(WorkflowStepSettlementGate, ParksFailureWhenAllJobsSettle) {
    using namespace savor::db::execution::workflow;

    StepSettlementGateService gate;
    const StepSettlementSnapshot snapshot{
        .workflow_step_id = 501,
        .job_set_id = 901,
        .expected_total = 10,
        .discovered_total = 10,
        .settled_total = 10,
        .succeeded_total = 9,
        .failed_total = 1,
    };

    const auto decision = gate.Evaluate(snapshot);
    EXPECT_TRUE(decision.can_transition);
    EXPECT_TRUE(decision.workflow_fail);
    EXPECT_FALSE(decision.blocked_reason.has_value());
}

TEST(WorkflowStepSettlementGate, SeparatesSupersessionFromFailureAndSuccess) {
    using namespace savor::db::execution::workflow;

    StepSettlementGateService gate;
    const auto mixed = gate.Evaluate({
        .workflow_step_id = 502,
        .job_set_id = 902,
        .expected_total = 4,
        .discovered_total = 4,
        .settled_total = 4,
        .succeeded_total = 1,
        .superseded_total = 3,
    });
    EXPECT_TRUE(mixed.can_transition);
    EXPECT_FALSE(mixed.workflow_fail);

    const auto all_superseded = gate.Evaluate({
        .workflow_step_id = 503,
        .job_set_id = 903,
        .expected_total = 4,
        .discovered_total = 4,
        .settled_total = 4,
        .superseded_total = 4,
    });
    EXPECT_TRUE(all_superseded.can_transition);
    EXPECT_TRUE(all_superseded.workflow_fail);
    EXPECT_EQ(all_superseded.blocked_reason.value_or(""),
        "STEP_NO_SUCCESSFUL_JOBS");
}

TEST(WorkflowStepSettlementGate, ParksInterruptedWorkSeparately) {
    using namespace savor::db::execution::workflow;

    StepSettlementGateService gate;
    const auto decision = gate.Evaluate({
        .workflow_step_id = 504,
        .job_set_id = 904,
        .expected_total = 2,
        .discovered_total = 2,
        .settled_total = 2,
        .succeeded_total = 1,
        .interrupted_total = 1,
    });
    EXPECT_TRUE(decision.can_transition);
    EXPECT_TRUE(decision.workflow_fail);
}

TEST(Stage1StepInputAggregation, AllInputsRequiredGatingAndEventSequence) {
    using namespace savor::runner::parallel::savordb;

    std::vector<std::string> events;
    StepInputAggregationService svc(
        StepInputAggregationConfig{ .timeout = std::chrono::milliseconds(50), .max_timeout_retries = 1 },
        [&](const WorkflowReadyStep&, const std::string& event_kind, const std::optional<std::string>&, const std::optional<std::string>&, const std::optional<std::string>&) {
            events.push_back(event_kind);
        });

    const WorkflowReadyStep step{
        .workflow_instance_id = 1,
        .workflow_step_id = 10,
        .step_key = "Neutral",
        .step_kind = "seedprobe.neutral",
        .priority = 1,
    };
    const auto t0 = std::chrono::steady_clock::time_point{};
    const auto first = svc.Evaluate(step, t0, false);
    EXPECT_FALSE(first.input_complete);

    const auto second = svc.Evaluate(step, t0 + std::chrono::milliseconds(6), true);
    EXPECT_TRUE(second.input_complete);
    ASSERT_GE(events.size(), 4u);
    EXPECT_EQ(events[0], "Execution.WorkflowStepInputRequested.v1");
    EXPECT_EQ(events[1], "Execution.WorkflowStepInputRequested.v1");
    EXPECT_EQ(events[2], "Execution.WorkflowStepInputFragmentReady.v1");
    EXPECT_EQ(events.back(), "Execution.WorkflowStepInputComplete.v1");
}

TEST(Stage1StepInputAggregation, DuplicateFragmentIsIdempotentAndScopedByInstanceAndStepKey) {
    using namespace savor::runner::parallel::savordb;

    int fragment_ready_count = 0;
    StepInputAggregationService svc(
        StepInputAggregationConfig{ .timeout = std::chrono::milliseconds(50), .max_timeout_retries = 1 },
        [&](const WorkflowReadyStep&, const std::string& event_kind, const std::optional<std::string>&, const std::optional<std::string>&, const std::optional<std::string>&) {
            if (event_kind == "Execution.WorkflowStepInputFragmentReady.v1") {
                ++fragment_ready_count;
            }
        });

    const auto t0 = std::chrono::steady_clock::time_point{};
    const WorkflowReadyStep a{ .workflow_instance_id = 7, .workflow_step_id = 70, .step_key = "Grid", .step_kind = "seedprobe.grid", .priority = 1 };
    const WorkflowReadyStep b{ .workflow_instance_id = 8, .workflow_step_id = 71, .step_key = "Grid", .step_kind = "seedprobe.grid", .priority = 1 };

    (void)svc.Evaluate(a, t0, false);
    EXPECT_TRUE(svc.SubmitFragment(a, "sync", std::nullopt, t0)); // duplicate sync should be idempotent
    EXPECT_TRUE(svc.SubmitFragment(a, "sync", std::nullopt, t0));
    (void)svc.Evaluate(b, t0, false);
    EXPECT_TRUE(svc.SubmitFragment(b, "sync", std::nullopt, t0));
    EXPECT_EQ(fragment_ready_count, 2);
}

TEST(Stage1StepInputAggregation, TimeoutRetriesOnceThenMarksTerminalFailureReady) {
    using namespace savor::runner::parallel::savordb;

    int requested_count = 0;
    StepInputAggregationService svc(
        StepInputAggregationConfig{ .timeout = std::chrono::milliseconds(10), .max_timeout_retries = 1 },
        [&](const WorkflowReadyStep&, const std::string& event_kind, const std::optional<std::string>&, const std::optional<std::string>&, const std::optional<std::string>&) {
            if (event_kind == "Execution.WorkflowStepInputRequested.v1") {
                ++requested_count;
            }
        });

    const WorkflowReadyStep step{
        .workflow_instance_id = 2,
        .workflow_step_id = 20,
        .step_key = "Unique",
        .step_kind = "seedprobe.unique",
        .priority = 1,
    };
    const auto t0 = std::chrono::steady_clock::time_point{};
    const auto collecting = svc.Evaluate(step, t0, false);
    EXPECT_FALSE(collecting.input_complete);

    const auto after_first_timeout = svc.Evaluate(step, t0 + std::chrono::milliseconds(11), false);
    EXPECT_FALSE(after_first_timeout.input_complete);
    EXPECT_TRUE(after_first_timeout.timed_out);
    EXPECT_FALSE(after_first_timeout.failure_ready);

    const auto after_second_timeout = svc.Evaluate(step, t0 + std::chrono::milliseconds(22), false);
    EXPECT_FALSE(after_second_timeout.input_complete);
    EXPECT_TRUE(after_second_timeout.timed_out);
    EXPECT_TRUE(after_second_timeout.failure_ready);
    EXPECT_GE(requested_count, 4); // initial (2) + retry (2)
}

TEST(Stage3cEventContracts, CanonicalEventTypeFormatValidationAcceptsAndRejectsExpectedShapes) {
    using namespace savor::db::events;

    std::string error;
    EXPECT_TRUE(ValidateEventTypeFormat("Execution.WorkflowStepFailed.v1", 1, &error)) << error;
    EXPECT_TRUE(ValidateEventTypeFormat("AnalysisSeedProbe.RunCompleted.v1", 1, &error)) << error;

    EXPECT_FALSE(ValidateEventTypeFormat("Execution.WorkflowStepFailed", 1, &error));
    EXPECT_FALSE(ValidateEventTypeFormat("Execution.WorkflowStepFailed.v2", 1, &error));
    EXPECT_FALSE(ValidateEventTypeFormat("Execution.Workflow.StepFailed.v1", 1, &error));
    EXPECT_FALSE(ValidateEventTypeFormat("Execution.WorkflowStepFailed.v1", 0, &error));
}

TEST(Stage3cEventContracts, PayloadDispatchAndValidationRejectVersionSuffixMismatches) {
    using namespace savor::db::events;

    const auto exact = ResolvePayloadResolverContract("Execution.WorkflowStepFailed.v1", 1);
    ASSERT_TRUE(exact.has_value());
    EXPECT_EQ(*exact, PayloadResolverContract::ExecutionWorkflowJobV1);

    const auto suffix_mismatch = ResolvePayloadResolverContract("Execution.WorkflowStepFailed.v2", 1);
    EXPECT_FALSE(suffix_mismatch.has_value());

    EventEnvelope envelope{};
    envelope.event_type = "Execution.WorkflowStepFailed.v2";
    envelope.event_version = 1;
    envelope.context_name = "Execution";
    envelope.aggregate_kind = "workflow_instance";
    envelope.payload_ref_kind = "workflow_event";
    envelope.payload_ref_id = 42;

    std::string error;
    EXPECT_FALSE(ValidateExecutionWorkflowJobPayloadV1(envelope, &error));
    EXPECT_EQ(error, "event_type must end with .v<event_version>");
}

TEST(Stage3cEventContracts, SeedProbeValidationRequiresConcretePayloadRefKinds) {
    using namespace savor::db::events;

    EventEnvelope envelope{};
    envelope.event_type = "AnalysisSeedProbe.RunCompleted.v1";
    envelope.event_version = 1;
    envelope.context_name = "AnalysisSeedProbe";
    envelope.aggregate_kind = "probe_run";
    envelope.payload_ref_kind = "seed_probe_event";
    envelope.payload_ref_id = 42;

    std::string error;
    EXPECT_FALSE(ValidateAnalysisSeedProbePayloadV1(envelope, &error));
    EXPECT_EQ(error, "payload_ref_kind must be probe_run for SeedProbe run events");

    envelope.payload_ref_kind = "probe_run";
    EXPECT_TRUE(ValidateAnalysisSeedProbePayloadV1(envelope, &error)) << error;
}

TEST(Stage3cEventContracts, AnalysisSpineValidationRequiresConcretePayloadRefKinds) {
    using namespace savor::db::events;

    EventEnvelope envelope{};
    envelope.event_type = "AnalysisSpine.StateRefRegistered.v1";
    envelope.event_version = 1;
    envelope.context_name = "AnalysisSpine";
    envelope.aggregate_kind = "run";
    envelope.payload_ref_kind = "spine_ref";
    envelope.payload_ref_id = 22;

    std::string error;
    EXPECT_FALSE(ValidateAnalysisSpinePayloadV1(envelope, &error));
    EXPECT_EQ(error, "payload_ref_kind must be state_ref for AnalysisSpine.StateRefRegistered.v1");

    envelope.payload_ref_kind = "state_ref";
    EXPECT_TRUE(ValidateAnalysisSpinePayloadV1(envelope, &error)) << error;
}

TEST(Stage3cEventContracts, BattleValidationRequiresConcretePayloadRefKinds) {
    using namespace savor::db::events;

    struct Case {
        const char* event_type;
        const char* required_payload_kind;
    };
    constexpr std::array<Case, 4> cases{ {
        { "AnalysisBattle.TurnJobRecorded.v1", "turn_job" },
        { "AnalysisBattle.TurnJobResultUpdated.v1", "turn_job" },
        { "AnalysisBattle.TurnWaveStatusUpdated.v1", "turn_wave" },
        { "AnalysisBattle.BattleSetStatusUpdated.v1", "battle_set" },
    } };

    for (const auto& test_case : cases) {
        const auto contract = ResolvePayloadResolverContract(test_case.event_type, 1);
        ASSERT_TRUE(contract.has_value()) << test_case.event_type;
        EXPECT_EQ(*contract, PayloadResolverContract::AnalysisBattleV1) << test_case.event_type;

        EventEnvelope envelope{};
        envelope.event_type = test_case.event_type;
        envelope.event_version = 1;
        envelope.context_name = "AnalysisBattle";
        envelope.aggregate_kind = "battle_set";
        envelope.payload_ref_kind = "battle_event";
        envelope.payload_ref_id = 77;

        std::string error;
        EXPECT_FALSE(ValidateAnalysisBattlePayloadV1(envelope, &error)) << test_case.event_type;
        EXPECT_EQ(
            error,
            std::string("payload_ref_kind must be ") + test_case.required_payload_kind + " for " + test_case.event_type);

        envelope.payload_ref_kind = test_case.required_payload_kind;
        EXPECT_TRUE(ValidateAnalysisBattlePayloadV1(envelope, &error)) << test_case.event_type << ": " << error;
    }
}

TEST(Stage3cEventContracts, AnalysisSpineFamilyDispatchRoutesToSpineContractV1) {
    using namespace savor::db::events;

    constexpr std::array<std::string_view, 4> kSpineEventTypes{ {
        "AnalysisSpine.RunCreated.v1",
        "AnalysisSpine.StateRefRegistered.v1",
        "AnalysisSpine.LineageEdgeAdded.v1",
        "AnalysisSpine.ArtifactLinked.v1",
    } };

    for (const auto event_type : kSpineEventTypes) {
        const auto contract = ResolvePayloadResolverContract(event_type, 1);
        ASSERT_TRUE(contract.has_value()) << event_type;
        EXPECT_EQ(*contract, PayloadResolverContract::AnalysisSpineV1) << event_type;
    }
}

TEST(Stage3cEventContracts, AuthoringFamilyDispatchRoutesToAuthoringContractV1) {
    using namespace savor::db::events;

    constexpr std::array<std::string_view, 5> kAuthoringEventTypes{ {
        "Authoring.SeedProbeSpecSaved.v1",
        "Authoring.PlanSaved.v1",
        "Authoring.BattlePlanActionPresetSaved.v1",
        "Authoring.BattlePlanActionPresetRenamed.v1",
        "Authoring.WorkflowGraphSaved.v1",
    } };

    for (const auto event_type : kAuthoringEventTypes) {
        const auto contract = ResolvePayloadResolverContract(event_type, 1);
        ASSERT_TRUE(contract.has_value()) << event_type;
        EXPECT_EQ(*contract, PayloadResolverContract::AuthoringV1) << event_type;
    }
}

TEST(Stage3cEventContracts, AuthoringCatalogEntriesRemainDispatched) {
    using namespace savor::db::events;

    constexpr std::string_view kAuthoringPrefix = "Authoring.";
    constexpr std::size_t kPrefixLength = 10;
    std::size_t authoring_entries = 0;

    for (const auto event_type : kEventCatalogV1) {
        if (event_type.substr(0, kPrefixLength) != kAuthoringPrefix) {
            continue;
        }
        ++authoring_entries;

        const auto contract = ResolvePayloadResolverContract(event_type, 1);
        ASSERT_TRUE(contract.has_value()) << event_type;
        EXPECT_EQ(*contract, PayloadResolverContract::AuthoringV1) << event_type;
    }

    EXPECT_EQ(authoring_entries, 6u);
}

} // namespace savordb
