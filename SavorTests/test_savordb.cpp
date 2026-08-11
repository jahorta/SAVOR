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
#include "Execution/Workflow/WorkflowIntegrityChecks.h"
#include "Execution/Workflow/WorkflowProjector.h"
#include "Execution/Workflow/WorkflowRecoveryService.h"
#include "Execution/Workflow/WorkflowStepCompletionGate.h"
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
    const auto* seed_probe = registry.Find("seed_probe_chain");
    const auto* battle = registry.Find("battle_chain");
    ASSERT_NE(establish, nullptr);
    ASSERT_NE(validate_root, nullptr);
    ASSERT_NE(validate_tree, nullptr);
    ASSERT_NE(sterilize, nullptr);
    ASSERT_NE(seed_probe, nullptr);
    ASSERT_NE(battle, nullptr);

    ASSERT_EQ(establish->required_inputs.size(), 1u);
    EXPECT_EQ(establish->required_inputs[0].data_kind, "state_artifact.dtm_artifact_id");
    ASSERT_EQ(establish->possible_outputs.size(), 2u);
    EXPECT_TRUE(std::any_of(
        establish->possible_outputs.begin(),
        establish->possible_outputs.end(),
        [](const auto& output) {
            return output.key == "established_root_cursor_attempt"
                && output.data_kind
                    == "analysis.tas_movie_validation_attempt_id";
        }));
    ASSERT_EQ(validate_root->required_inputs.size(), 1u);
    EXPECT_EQ(validate_root->required_inputs[0].data_kind, "analysis.tas_movie_validation_attempt_id");
    EXPECT_TRUE(std::any_of(
        validate_root->possible_outputs.begin(),
        validate_root->possible_outputs.end(),
        [](const auto& output) {
            return output.key == "validated_checkpoint_savestate"
                && output.data_kind == "state.movie_paired_savestate_id";
        }));
    ASSERT_EQ(validate_tree->required_inputs.size(), 1u);
    EXPECT_EQ(validate_tree->required_inputs[0].data_kind, "state.tas_movie_tree_id");
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
    ASSERT_EQ(battle->required_inputs.size(), 2u);
    EXPECT_EQ(battle->required_inputs[0].data_kind,
        "analysis.seed_probe_run");
    EXPECT_EQ(battle->required_inputs[1].data_kind,
        "analysis_battle.battle_context_id");
    EXPECT_TRUE(std::any_of(
        battle->possible_outputs.begin(),
        battle->possible_outputs.end(),
        [](const auto& output) {
            return output.key == "battle_set"
                && output.data_kind == "analysis_battle.battle_set";
        }));
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
        { .node_key = "tas_1", .input_key = "root_dtm", .data_kind = "state_artifact.dtm_artifact_id", .ref_id = 10 },
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
        { .node_key = "probe", .unit_kind = "battle_seed_probe" },
    };
    composition.external_inputs = {{
        .node_key = "validate",
        .input_key = "root_establishment",
        .data_kind = "analysis.tas_movie_validation_attempt_id",
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
        { .node_key = "battle_1", .unit_kind = "battle_chain" },
    };
    auto preview = service.Preview(unresolved);
    EXPECT_FALSE(preview.valid);
    ASSERT_EQ(preview.issues.size(), 2u);

    WorkflowCompositionSpec mismatched;
    mismatched.nodes = {
        { .node_key = "probe_1", .unit_kind = "seed_probe_chain" },
        { .node_key = "battle_1", .unit_kind = "battle_chain" },
    };
    mismatched.external_inputs = {
        { .node_key = "probe_1", .input_key = "entry_savestate", .data_kind = "state.movie_inactive_savestate_id", .ref_id = 99 },
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


TEST(WorkflowStepCompletionGate, MismatchThenTerminalFail) {
    using namespace savor::db::execution::workflow;

    StepCompletionGateService gate;
    const StepCompletionSnapshot snapshot{
        .workflow_step_id = 500,
        .job_set_id = 900,
        .expected_total = 10,
        .discovered_total = 9,
        .terminal_total = 9,
    };

    const auto first = gate.Evaluate(snapshot);
    EXPECT_FALSE(first.can_transition);
    EXPECT_FALSE(first.terminal_fail);
    EXPECT_EQ(first.blocked_reason.value_or(""), "STEP_BLOCKED_COUNT_MISMATCH");

    const auto second = gate.Evaluate(snapshot);
    EXPECT_FALSE(second.can_transition);
    EXPECT_TRUE(second.terminal_fail);
    EXPECT_EQ(second.blocked_reason.value_or(""), "STEP_BLOCKED_COUNT_MISMATCH_TERMINAL_FAIL");
}

TEST(WorkflowStepCompletionGate, AllowsTerminalFailureWhenAllJobsFinished) {
    using namespace savor::db::execution::workflow;

    StepCompletionGateService gate;
    const StepCompletionSnapshot snapshot{
        .workflow_step_id = 501,
        .job_set_id = 901,
        .expected_total = 10,
        .discovered_total = 10,
        .terminal_total = 10,
        .failed_total = 1,
    };

    const auto decision = gate.Evaluate(snapshot);
    EXPECT_TRUE(decision.can_transition);
    EXPECT_TRUE(decision.terminal_fail);
    EXPECT_FALSE(decision.blocked_reason.has_value());
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
    EXPECT_FALSE(after_first_timeout.terminal_failure_ready);

    const auto after_second_timeout = svc.Evaluate(step, t0 + std::chrono::milliseconds(22), false);
    EXPECT_FALSE(after_second_timeout.input_complete);
    EXPECT_TRUE(after_second_timeout.timed_out);
    EXPECT_TRUE(after_second_timeout.terminal_failure_ready);
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

    constexpr std::array<std::string_view, 9> kAuthoringEventTypes{ {
        "Authoring.SeedProbeSpecSaved.v1",
        "Authoring.TasSpecSaved.v1",
        "Authoring.BattleRunSpecSaved.v1",
        "Authoring.PlanSaved.v1",
        "Authoring.BattlePlanActionPresetSaved.v1",
        "Authoring.BattlePlanActionPresetRenamed.v1",
        "Authoring.SettingsSaved.v1",
        "Authoring.BattleChainSpecSaved.v1",
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

    EXPECT_EQ(authoring_entries, 12u);
}

} // namespace savordb
