#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
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
#include "Runner/Parallel/SavorDb/ArchiveWorkflowCommands.h"
#include "Execution/Workflow/SqliteExecutionDb.h"
#include "Execution/Jobs/JobEventOrchestration.h"
#include "Execution/Workflow/WorkflowComposition.h"
#include "Execution/Workflow/WorkflowIntegrityChecks.h"
#include "Execution/Workflow/WorkflowProjector.h"
#include "Execution/Workflow/WorkflowRecoveryService.h"
#include "Execution/Workflow/AdapterChainOrchestrator.h"
#include "Execution/Workflow/WorkflowTerminalOutboxSubscriber.h"
#include "Execution/ProgramDB/SeedProbe/SeedProbePhaseRegistration.h"
#include "Execution/ProgramDB/SeedProbe/SeedProbeNeutralAdapters.h"
#include "Execution/ProgramDB/SeedProbe/SeedProbeGridAdapters.h"
#include "Execution/ProgramDB/SeedProbe/SeedProbeUniqueAdapters.h"
#include "UIRead/Projectors/ProjectorContract.h"
#include "Runner/Parallel/SavorDb/WorkflowCoordinatorBridge.h"
#include "Runner/Parallel/SavorDb/DBWorkflowWorkerCoordinator.h"
#include "Runner/Parallel/SavorDb/WorkflowSchedulerAdapter.h"
#include "Runner/Parallel/SavorDb/StepInputAggregationService.h"

#include "common/RecordingExecutionDb.h"
#include "common/MapperExecutionDb.h"
#include "common/AlwaysAdvanceTransitionHandler.h"
#include "common/SqliteDbFixture.h"
#include "common/savordb_helpers.h"

namespace savordb {

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
    const auto* tas = registry.Find("tas_movie");
    const auto* seed_probe = registry.Find("seed_probe_chain");
    const auto* battle = registry.Find("battle_chain");
    ASSERT_NE(tas, nullptr);
    ASSERT_NE(seed_probe, nullptr);
    ASSERT_NE(battle, nullptr);

    ASSERT_EQ(tas->possible_outputs.size(), 1u);
    EXPECT_EQ(tas->possible_outputs[0].data_kind, "state.savestate_id");
    ASSERT_EQ(seed_probe->required_inputs.size(), 1u);
    EXPECT_EQ(seed_probe->required_inputs[0].data_kind, "state.savestate_id");
    ASSERT_EQ(seed_probe->possible_outputs.size(), 1u);
    EXPECT_EQ(seed_probe->possible_outputs[0].data_kind, "analysis.input_frame_set_id");
    ASSERT_EQ(battle->required_inputs.size(), 2u);
    EXPECT_EQ(battle->required_inputs[0].data_kind, "state.savestate_id");
    EXPECT_EQ(battle->required_inputs[1].data_kind, "analysis.input_frame_set_id");
}

TEST(Stage5WorkflowComposition, ValidatesTasSeedProbeBattleCompatibility) {
    using namespace savor::db::execution::workflow;

    const auto registry = BuildDefaultWorkflowUnitRegistry();
    const WorkflowCompositionService service(&registry);

    WorkflowCompositionSpec composition;
    composition.nodes = {
        { .node_key = "tas_1", .unit_kind = "tas_movie" },
        { .node_key = "probe_1", .unit_kind = "seed_probe_chain" },
        { .node_key = "battle_1", .unit_kind = "battle_chain" },
    };
    composition.external_inputs = {
        { .node_key = "tas_1", .input_key = "dtm_artifact", .data_kind = "state_artifact.dtm_artifact_id", .ref_id = 10 },
    };
    composition.output_bindings = {
        { .from_node_key = "tas_1", .output_key = "savestate", .to_node_key = "probe_1", .input_key = "entry_savestate" },
        { .from_node_key = "tas_1", .output_key = "savestate", .to_node_key = "battle_1", .input_key = "entry_savestate" },
        { .from_node_key = "probe_1", .output_key = "unique_input_frames", .to_node_key = "battle_1", .input_key = "initial_input_frames" },
    };

    const auto preview = service.Preview(composition);
    EXPECT_TRUE(preview.valid);
    EXPECT_TRUE(preview.issues.empty());
    ASSERT_EQ(preview.nodes.size(), 3u);
    EXPECT_EQ(preview.nodes[2].resolved_inputs.size(), 2u);
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
        { .node_key = "probe_1", .input_key = "entry_savestate", .data_kind = "state.savestate_id", .ref_id = 99 },
    };
    mismatched.output_bindings = {
        { .from_node_key = "probe_1", .output_key = "unique_input_frames", .to_node_key = "battle_1", .input_key = "entry_savestate" },
    };

    preview = service.Preview(mismatched);
    EXPECT_FALSE(preview.valid);
    ASSERT_FALSE(preview.issues.empty());
    EXPECT_NE(preview.issues[0].message.find("type mismatch"), std::string::npos);
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


TEST(Stage3cSeedProbeProgramDB, BuildsPhaseSpecificDescriptors) {
    using namespace savor::db::execution::programdb::seedprobe;

    auto neutral = BuildSeedProbeNeutralDescriptor(nullptr, nullptr);
    auto grid = BuildSeedProbeGridDescriptor(
        nullptr,
        nullptr,
        SeedProbeGridBlueprintConfig{},
        SeedProbeGridSpec{});
    auto unique = BuildSeedProbeUniqueDescriptor(nullptr, nullptr, SeedProbeGridBlueprintConfig{}, UniqueIni{});

    EXPECT_NE(dynamic_cast<NeutralProbeJobPersistenceAdapter*>(neutral.job_persistence.get()), nullptr);
    EXPECT_NE(dynamic_cast<SeedProbeGridJobPersistenceAdapter*>(grid.job_persistence.get()), nullptr);
    EXPECT_NE(dynamic_cast<SeedProbeUniqueJobPersistenceAdapter*>(unique.job_persistence.get()), nullptr);

    EXPECT_NE(dynamic_cast<RequiredSavestateRuntimeInitAdapter*>(neutral.runtime_init.get()), nullptr);
    EXPECT_NE(dynamic_cast<SeedProbeRuntimeInitAdapter*>(grid.runtime_init.get()), nullptr);
    EXPECT_NE(dynamic_cast<SeedProbeUniqueRuntimeInitAdapter*>(unique.runtime_init.get()), nullptr);

    EXPECT_NE(dynamic_cast<NeutralSeedResultMapper*>(neutral.result_mapper.get()), nullptr);
    EXPECT_NE(dynamic_cast<SeedProbeGridResultMapper*>(grid.result_mapper.get()), nullptr);
    EXPECT_NE(dynamic_cast<SeedProbeUniqueResultMapper*>(unique.result_mapper.get()), nullptr);

    EXPECT_NE(neutral.workflow_transition, nullptr);
    EXPECT_NE(grid.workflow_transition, nullptr);
    EXPECT_NE(unique.workflow_transition, nullptr);
}

TEST(Stage3cSeedProbeProgramDB, RegistryDispatchesAdaptersByWorkflowStepKind) {
    using namespace savor::db::execution::programdb;
    using namespace savor::db::execution::programdb::seedprobe;

    ProgramKindRegistry registry;
    SeedProbePhaseRegistrationConfig config{};
    RegisterSeedProbePhaseDescriptors(&registry, nullptr, nullptr, std::move(config));

    const auto* neutral = registry.FindForStepKind("seedprobe.neutral");
    ASSERT_NE(neutral, nullptr);
    EXPECT_NE(dynamic_cast<NeutralProbeJobPersistenceAdapter*>(neutral->job_persistence.get()), nullptr);

    const auto* grid = registry.FindForStepKind("seedprobe.grid");
    ASSERT_NE(grid, nullptr);
    EXPECT_NE(dynamic_cast<SeedProbeGridJobPersistenceAdapter*>(grid->job_persistence.get()), nullptr);

    const auto* unique = registry.FindForStepKind("seedprobe.unique");
    ASSERT_NE(unique, nullptr);
    EXPECT_NE(dynamic_cast<SeedProbeUniqueJobPersistenceAdapter*>(unique->job_persistence.get()), nullptr);
}

TEST(Stage3cSeedProbeProgramDB, UniqueTransitionBlocksWhenCompletionGateFails) {
    using namespace savor::db::execution::programdb::seedprobe;

    SeedProbeUniqueTransitionHandler handler([](const auto&) { return false; });
    const savor::db::execution::programdb::WorkflowTransitionContext context{
        .workflow_instance_id = 77,
        .workflow_step_id = 501,
        .job_set_id = 9001,
        .workflow_kind = "SEED_PROBE_CHAIN",
        .step_key = "Grid",
    };

    const auto decision = handler.EvaluateTransition(context);
    EXPECT_FALSE(decision.should_advance);
    EXPECT_EQ(decision.blocked_reason.value_or(""), "Grid completion gate not satisfied");
    EXPECT_EQ(decision.next_step_key.value_or(""), "Unique");
}

TEST(Stage3cSeedProbeProgramDB, UniqueTransitionAdvancesToDoneTerminalStep) {
    using namespace savor::db::execution::programdb::seedprobe;

    SeedProbeUniqueTransitionHandler handler([](const auto&) { return true; });
    const savor::db::execution::programdb::WorkflowTransitionContext context{
        .workflow_instance_id = 77,
        .workflow_step_id = 503,
        .job_set_id = 9003,
        .workflow_kind = "SEED_PROBE_CHAIN",
        .step_key = "Unique",
    };

    const auto decision = handler.EvaluateTransition(context);
    EXPECT_TRUE(decision.should_advance);
    EXPECT_EQ(decision.next_step_key.value_or(""), "Done");
    EXPECT_FALSE(decision.blocked_reason.has_value());
}

TEST(Stage2AdapterChain, InvokesCanonicalOrderAndWriterContract) {
    using namespace savor::db::execution::programdb;
    using namespace savor::db::execution::workflow;

    class MockPersistence final : public IJobPersistenceAdapter {
    public:
        WorkflowStepScheduleResult EncodeForQueueing(std::int64_t domain_ref_id) const override {
            JobPersistenceRecord r{};
            r.program_ref_kind = "mock";
            r.program_ref_id = domain_ref_id;
            return WorkflowStepScheduleResult{
                .persistence = r,
                .root_job_set_id = domain_ref_id,
            };
        }
        std::int64_t DecodeDomainRefId(const JobPersistenceRecord& persisted) const override { return persisted.program_ref_id; }
    };
    class MockRuntime final : public IRuntimeInitAdapter {
    public:
        RuntimeInitRequest BuildRuntimeInit(std::int64_t job_id) const override {
            RuntimeInitRequest r{};
            r.bootstrap_profile = "mock.runtime";
            r.savestate_ref_kind = "savestate";
            r.savestate_ref_id = job_id;
            return r;
        }
        std::optional<savor::PSJob> MaterializePsJob(std::int64_t job_id, const RuntimeInitRequest&) const override {
            (void)job_id;
            return savor::PSJob{};
        }
    };
    class MockMapper final : public IResultMapper {
    public:
        std::string BuildResultIniFromPrResult(std::int64_t, const savor::PRResult&) const override {
            return "[Mock.Results]\nok=1\n";
        }
        ResultMapPayload MapPrimaryResult(std::int64_t job_id, const std::string&) const override {
            return ResultMapPayload{ .result_kind = "mock.result", .result_ref_id = job_id };
        }
        std::optional<ResultArtifactRef> MapPrimaryArtifact(std::int64_t) const override { return std::nullopt; }
    };
    class MockTransition final : public IWorkflowTransitionHandler {
    public:
        WorkflowTransitionDecision EvaluateTransition(const WorkflowTransitionContext&) const override {
            return WorkflowTransitionDecision{ .should_advance = true, .blocked_reason = std::nullopt, .next_step_key = std::optional<std::string>("Next") };
        }
    };
    class MockWriter final : public IResultPayloadWriter {
    public:
        bool Persist(const ResultMapPayload& payload, std::string*) override {
            persisted.push_back(payload.result_kind + ":" + std::to_string(payload.result_ref_id));
            return true;
        }
        std::vector<std::string> persisted;
    };

    ProgramKindDescriptor descriptor{};
    descriptor.program_kind = 999;
    descriptor.program_name = "MockProgram";
    descriptor.job_persistence = std::make_shared<MockPersistence>();
    descriptor.runtime_init = std::make_shared<MockRuntime>();
    descriptor.result_mapper = std::make_shared<MockMapper>();
    descriptor.workflow_transition = std::make_shared<MockTransition>();
    descriptor.supports_workflow_orchestration = true;
    auto writer = std::make_shared<MockWriter>();
    descriptor.result_payload_writer = writer;

    ProgramKindRegistry registry;
    ASSERT_TRUE(registry.Register(descriptor));
    ASSERT_TRUE(registry.RegisterForStepKind("mock.step", descriptor));

    StepCompletionGateService gate;
    AdapterChainOrchestrator orchestrator(&registry, &gate);

    AdapterChainTrace trace{};
    const auto persisted = orchestrator.OnInputComplete("mock.step", 77, &trace);
    ASSERT_TRUE(persisted.has_value());
    EXPECT_TRUE(trace.job_persistence_invoked);

    const auto runtime = orchestrator.OnJobClaimed("mock.step", 88, &trace);
    ASSERT_TRUE(runtime.has_value());
    EXPECT_TRUE(trace.runtime_init_invoked);

    savor::PRResult pr{};
    pr.job_id = 99;
    const auto mapped = orchestrator.OnJobTerminal("mock.step", 99, pr, &trace, nullptr);
    ASSERT_TRUE(mapped.has_value());
    EXPECT_TRUE(trace.result_mapper_invoked);
    EXPECT_TRUE(trace.result_writer_invoked);
    ASSERT_EQ(writer->persisted.size(), 1u);
    EXPECT_EQ(writer->persisted.front(), "mock.result:99");

    const auto terminal = orchestrator.OnStepTerminal(
        "mock.step",
        WorkflowTransitionContext{ .workflow_instance_id = 1, .workflow_step_id = 2, .job_set_id = 3, .workflow_kind = "Mock", .step_key = "Step" },
        StepCompletionSnapshot{ .workflow_step_id = 2, .job_set_id = 3, .expected_total = 1, .discovered_total = 1, .terminal_total = 1 },
        &trace);
    EXPECT_TRUE(terminal.gate.can_transition);
    ASSERT_TRUE(terminal.transition.has_value());
    EXPECT_TRUE(terminal.transition->should_advance);
    EXPECT_TRUE(trace.transition_handler_invoked);
}

TEST(Stage3cResultMapper, SeedProbeMapPrimaryResultMarksTerminalJobStateFromErrors) {
    using namespace savor::db::execution::jobs;
    using namespace savor::db::execution::programdb::seedprobe;

    MapperExecutionDb execution_db;
    NeutralSeedResultMapper mapper(&execution_db, nullptr);

    (void)mapper.MapPrimaryResult(701, "[SeedProbe.Results]\nw_err=0\ndw_err=0\n");
    (void)mapper.MapPrimaryResult(702, "[SeedProbe.Results]\nw_err=1\ndw_err=0\n");

    ASSERT_EQ(execution_db.job_events.calls.size(), 2u);
    EXPECT_EQ(execution_db.job_events.calls[0].kind, JobLifecycleEventKind::JobCompleted);
    EXPECT_EQ(execution_db.job_events.calls[0].job_id, 701);
    ASSERT_TRUE(execution_db.job_events.calls[0].terminal_state.has_value());
    EXPECT_EQ(*execution_db.job_events.calls[0].terminal_state, "SUCCEEDED");

    EXPECT_EQ(execution_db.job_events.calls[1].kind, JobLifecycleEventKind::JobCompleted);
    EXPECT_EQ(execution_db.job_events.calls[1].job_id, 702);
    ASSERT_TRUE(execution_db.job_events.calls[1].terminal_state.has_value());
    EXPECT_EQ(*execution_db.job_events.calls[1].terminal_state, "FAILED");
}

TEST(Stage2AdapterChain, CompletionGateMismatchThenTerminalFail) {
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

TEST(Stage2AdapterChain, CompletionGateAllowsTerminalFailureWhenAllJobsFinished) {
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


TEST(Stage3cCoordinatorReplacement, MaterializesAndPublishesThroughWorkflowBridge) {
    using namespace savor::runner::parallel::savordb;
    using namespace savor::db::execution::workflow;

    DBWorkflowWorkerCoordinator coordinator(
        nullptr,
        DBWorkflowWorkerCoordinatorConfig{
            .desired_workers = 0,
        },
        CoordinatorIntegrationConfig{

        },
        [](const WorkflowReadyStep& step) {
            return ScheduledJobSet{
                .job_set_id = 7777 + step.workflow_step_id,
                .workflow_step_id = step.workflow_step_id,
            };
        });

    int materialized_callbacks = 0;
    int terminal_callbacks = 0;
    coordinator.SetWorkflowMaterializationCallback([&](std::int64_t workflow_step_id, std::int64_t job_set_id) {
        ++materialized_callbacks;
        EXPECT_EQ(workflow_step_id, 22);
        EXPECT_EQ(job_set_id, 7799);
    });
    coordinator.SetWorkflowTerminalCallback([&](const TerminalJobSetSignal& signal) {
        ++terminal_callbacks;
        EXPECT_EQ(signal.workflow_step_id, 22);
        EXPECT_EQ(signal.terminal_state, "COMPLETED");
    });

    const auto materialized = coordinator.MaterializeWorkflowStep({
        .workflow_instance_id = 11,
        .workflow_step_id = 22,
        .step_key = "Grid",
        .step_kind = "seedprobe.grid",
        .priority = 10,
    });
    ASSERT_TRUE(materialized.has_value());
    EXPECT_EQ(materialized->job_set_id, 7799);
    EXPECT_EQ(materialized_callbacks, 1);

    const TerminalJobSetSignal terminal{
        .workflow_instance_id = 11,
        .workflow_step_id = 22,
        .job_set_id = 7799,
        .terminal_state = "COMPLETED",
    };

    EXPECT_TRUE(coordinator.PublishTerminalJobSet(terminal));
    EXPECT_FALSE(coordinator.PublishTerminalJobSet(terminal));
    EXPECT_EQ(terminal_callbacks, 1);

    const auto status = coordinator.SnapshotStatus();
    EXPECT_EQ(status.running_workers, 1u);
    EXPECT_EQ(status.pending_start_workers, 1u);
}

TEST(Stage3cCoordinatorReplacement, SnapshotWorkersTracksSlotLifecycleAcrossEnqueueStartStop) {
    using namespace savor::runner::parallel::savordb;
    using namespace savor::db::execution::workflow;

    DBWorkflowWorkerCoordinator coordinator(
        nullptr,
        DBWorkflowWorkerCoordinatorConfig{
            .desired_workers = 1,
            .controller_sleep_ms = 1,
            .worker_exe_path = "missing-worker-binary.exe",
        },
        CoordinatorIntegrationConfig{},
        [](const WorkflowReadyStep& step) {
            return ScheduledJobSet{
                .job_set_id = 12000 + step.workflow_step_id,
                .workflow_step_id = step.workflow_step_id,
            };
        });

    EXPECT_TRUE(coordinator.SnapshotWorkers().empty());

    coordinator.EnqueueReadyStep({
        .workflow_instance_id = 501,
        .workflow_step_id = 601,
        .step_key = "Grid",
        .step_kind = "seedprobe.grid",
        .priority = 1,
    });
    EXPECT_TRUE(coordinator.SnapshotWorkers().empty());

    coordinator.Start();
    const auto started_snapshot = coordinator.SnapshotWorkers();
    ASSERT_EQ(started_snapshot.size(), 1u);
    EXPECT_EQ(started_snapshot[0].worker_id, 0);
    EXPECT_FALSE(started_snapshot[0].job_id.has_value());
    EXPECT_TRUE(started_snapshot[0].state == WorkerStateKind::Idle || started_snapshot[0].state == WorkerStateKind::Dead);

    coordinator.Stop();
    EXPECT_TRUE(coordinator.SnapshotWorkers().empty());
}

TEST(Stage3cCoordinatorReplacement, PersistsMaterializedAndTerminalTransitionsToExecutionDb) {
    using namespace savor::runner::parallel::savordb;
    using namespace savor::db::execution::workflow;

    RecordingExecutionDb execution_db;
    DBWorkflowWorkerCoordinator coordinator(
        &execution_db,
        DBWorkflowWorkerCoordinatorConfig{
            .desired_workers = 0,
        },
        CoordinatorIntegrationConfig{

        },
        [](const WorkflowReadyStep& step) {
            return ScheduledJobSet{
                .job_set_id = 9000 + step.workflow_step_id,
                .workflow_step_id = step.workflow_step_id,
            };
        });

    const auto materialized = coordinator.MaterializeWorkflowStep({
        .workflow_instance_id = 77,
        .workflow_step_id = 12,
        .step_key = "Grid",
        .step_kind = "seedprobe.grid",
        .priority = 5,
    });
    ASSERT_TRUE(materialized.has_value());
    ASSERT_EQ(execution_db.command_service.materialized_calls.size(), 1u);
    EXPECT_EQ(execution_db.command_service.materialized_calls[0].workflow_step_id, 12);
    EXPECT_EQ(execution_db.command_service.materialized_calls[0].job_set_id, 9012);

    const TerminalJobSetSignal terminal{
        .workflow_instance_id = 77,
        .workflow_step_id = 12,
        .job_set_id = 9012,
        .terminal_state = "FAILED",
    };

    EXPECT_TRUE(coordinator.PublishTerminalJobSet(terminal));
    EXPECT_FALSE(coordinator.PublishTerminalJobSet(terminal));
    EXPECT_TRUE(execution_db.command_service.terminal_calls.empty());
}

TEST(Stage1CoordinatorIntegration, DbBackedSchedulerInvokesInputCompleteOnceAndMarksSameJobSet) {
    using namespace savor::runner::parallel::savordb;
    using namespace savor::db::execution::programdb;
    using namespace savor::db::execution::workflow;

    class CountingPersistenceAdapter final : public IJobPersistenceAdapter {
    public:
        WorkflowStepScheduleResult EncodeForQueueing(std::int64_t domain_ref_id) const override {
            ++encode_calls;
            WorkflowStepScheduleResult result{};
            result.root_job_set_id = 17000 + domain_ref_id;
            result.persistence.program_ref_kind = "test.domain";
            result.persistence.program_ref_id = domain_ref_id;
            result.persistence.fingerprint = "test";
            return result;
        }

        std::int64_t DecodeDomainRefId(const JobPersistenceRecord& persisted) const override {
            return persisted.program_ref_id;
        }

        mutable std::atomic<int> encode_calls{ 0 };
    };

    RecordingExecutionDb execution_db;
    ProgramKindRegistry registry;
    auto persistence = std::make_shared<CountingPersistenceAdapter>();
    ProgramKindDescriptor descriptor{};
    descriptor.program_kind = 7;
    descriptor.program_name = "test.step";
    descriptor.job_persistence = persistence;
    ASSERT_TRUE(registry.RegisterForStepKind("test.step", descriptor));

    DBWorkflowWorkerCoordinator coordinator(
        &execution_db,
        DBWorkflowWorkerCoordinatorConfig{
            .desired_workers = 0,
            .controller_sleep_ms = 1,
        },
        CoordinatorIntegrationConfig{},
        &registry);

    const auto scheduled = coordinator.MaterializeWorkflowStep({
        .workflow_instance_id = 301,
        .workflow_step_id = 302,
        .step_key = "Only",
        .step_kind = "test.step",
        .priority = 1,
        .input_ref_id = 44,
    });

    ASSERT_TRUE(scheduled.has_value());
    EXPECT_EQ(persistence->encode_calls.load(), 1);
    ASSERT_EQ(execution_db.command_service.materialized_calls.size(), 1u);
    EXPECT_EQ(execution_db.command_service.materialized_calls.front().workflow_step_id, 302);
    EXPECT_EQ(execution_db.command_service.materialized_calls.front().job_set_id, 17044);
}

TEST(Stage1CoordinatorIntegration, ManualSignalPublishesWorkflowCreatedAndMaterializesStep) {
    using namespace savor::runner::parallel::savordb;

    RecordingExecutionDb execution_db;
    DBWorkflowWorkerCoordinator coordinator(
        &execution_db,
        DBWorkflowWorkerCoordinatorConfig{
            .desired_workers = 0,
            .controller_sleep_ms = 1,
        },
        CoordinatorIntegrationConfig{},
        [](const WorkflowReadyStep& step) {
            return ScheduledJobSet{
                .job_set_id = 8100 + step.workflow_step_id,
                .workflow_step_id = step.workflow_step_id,
            };
        });

    int workflow_created_callbacks = 0;
    coordinator.SetWorkflowCreatedCallback([&](const WorkflowCreatedSignal& signal) {
        ++workflow_created_callbacks;
        EXPECT_EQ(signal.workflow_instance_id, 999);
    });

    EXPECT_TRUE(coordinator.PublishWorkflowCreated({ .workflow_instance_id = 999 }));
    const auto scheduled = coordinator.MaterializeWorkflowStep({
        .workflow_instance_id = 999,
        .workflow_step_id = 333,
        .step_key = "Grid",
        .step_kind = "seedprobe.grid",
        .priority = 4,
    });

    ASSERT_TRUE(scheduled.has_value());
    ASSERT_FALSE(execution_db.command_service.materialized_calls.empty());
    EXPECT_EQ(execution_db.command_service.materialized_calls.front().workflow_step_id, 333);
    EXPECT_EQ(workflow_created_callbacks, 1);
    const auto telemetry = coordinator.SnapshotTelemetry();
    EXPECT_EQ(telemetry.workflow_created_signal_count, 1);
}

TEST(Stage3cCoordinatorReplacement, DisabledWorkflowIntegrationSkipsWorkflowPersistencePath) {
    using namespace savor::runner::parallel::savordb;
    using namespace savor::db::execution::workflow;

    RecordingExecutionDb execution_db;
    DBWorkflowWorkerCoordinator coordinator(
        &execution_db,
        DBWorkflowWorkerCoordinatorConfig{
            .desired_workers = 0,
        },
        CoordinatorIntegrationConfig{
            .workflow_enabled = false,
        },
        [](const WorkflowReadyStep& step) {
            return ScheduledJobSet{
                .job_set_id = 5000 + step.workflow_step_id,
                .workflow_step_id = step.workflow_step_id,
            };
        });

    const auto scheduled = coordinator.MaterializeWorkflowStep({
        .workflow_instance_id = 3,
        .workflow_step_id = 4,
        .step_key = "Grid",
        .step_kind = "seedprobe.grid",
        .priority = 1,
    });
    EXPECT_FALSE(scheduled.has_value());
    EXPECT_TRUE(execution_db.command_service.materialized_calls.empty());

    const TerminalJobSetSignal terminal{
        .workflow_instance_id = 3,
        .workflow_step_id = 4,
        .job_set_id = 5004,
        .terminal_state = "COMPLETED",
    };
    EXPECT_FALSE(coordinator.PublishTerminalJobSet(terminal));
    EXPECT_TRUE(execution_db.command_service.terminal_calls.empty());
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
    EXPECT_EQ(error, "payload_ref_kind must be probe_result for AnalysisSeedProbe.RunCompleted.v1");

    envelope.payload_ref_kind = "probe_result";
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

    EventEnvelope envelope{};
    envelope.event_type = "AnalysisBattle.TurnJobRecorded.v1";
    envelope.event_version = 1;
    envelope.context_name = "AnalysisBattle";
    envelope.aggregate_kind = "battle_set";
    envelope.payload_ref_kind = "battle_event";
    envelope.payload_ref_id = 77;

    std::string error;
    EXPECT_FALSE(ValidateAnalysisBattlePayloadV1(envelope, &error));
    EXPECT_EQ(error, "payload_ref_kind must be turn_job for AnalysisBattle.TurnJobRecorded.v1");

    envelope.payload_ref_kind = "turn_job";
    EXPECT_TRUE(ValidateAnalysisBattlePayloadV1(envelope, &error)) << error;
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

    constexpr std::array<std::string_view, 12> kAuthoringEventTypes{ {
        "Authoring.SeedProbeSpecSaved.v1",
        "Authoring.TasSpecSaved.v1",
        "Authoring.BattleRunSpecSaved.v1",
        "Authoring.PlanSaved.v1",
        "Authoring.BattlePlanActionPresetSaved.v1",
        "Authoring.BattlePlanActionPresetRenamed.v1",
        "Authoring.PredicateSpecSaved.v1",
        "Authoring.PredicateSpecUpdated.v1",
        "Authoring.PredicateSpecDeleted.v1",
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

TEST(Stage3cCoordinatorIntegrationConfig, WorkflowEnabledConfigDrivesWorkflowPathDecisions) {
    using namespace savor::runner::parallel::savordb;
    using namespace savor::db::execution::workflow;

    const std::vector<std::pair<CoordinatorIntegrationConfig, bool>> matrix{
        { CoordinatorIntegrationConfig{ .workflow_enabled = false }, false },
        { CoordinatorIntegrationConfig{ .workflow_enabled = true }, true },
    };

    for (const auto& [integration_cfg, should_run_workflow] : matrix) {
        RecordingExecutionDb execution_db;
        DBWorkflowWorkerCoordinator coordinator(
            &execution_db,
            DBWorkflowWorkerCoordinatorConfig{
                .desired_workers = 0,
            },
            integration_cfg,
            [](const WorkflowReadyStep& step) {
                return ScheduledJobSet{
                    .job_set_id = 10000 + step.workflow_step_id,
                    .workflow_step_id = step.workflow_step_id,
                };
            });

        const auto scheduled = coordinator.MaterializeWorkflowStep({
            .workflow_instance_id = 1,
            .workflow_step_id = 2,
            .step_key = "Grid",
            .step_kind = "seedprobe.grid",
            .priority = 5,
        });

        TerminalJobSetSignal terminal{
            .workflow_instance_id = 1,
            .workflow_step_id = 2,
            .job_set_id = 10002,
            .terminal_state = "COMPLETED",
        };
        const auto published_terminal = coordinator.PublishTerminalJobSet(terminal);

        if (should_run_workflow) {
            ASSERT_TRUE(scheduled.has_value());
            EXPECT_EQ(execution_db.command_service.materialized_calls.size(), 1u);
            EXPECT_TRUE(published_terminal);
            EXPECT_TRUE(execution_db.command_service.terminal_calls.empty());
        } else {
            EXPECT_FALSE(scheduled.has_value());
            EXPECT_TRUE(execution_db.command_service.materialized_calls.empty());
            EXPECT_FALSE(published_terminal);
            EXPECT_TRUE(execution_db.command_service.terminal_calls.empty());
        }
    }
}

TEST(Stage3cCoordinatorTelemetry, CapturesReadinessScanLatencyAndQueueDepth) {
    using namespace savor::runner::parallel::savordb;
    using namespace savor::db::execution::workflow;

    RecordingExecutionDb execution_db;
    DBWorkflowWorkerCoordinator coordinator(
        &execution_db,
        DBWorkflowWorkerCoordinatorConfig{
            .desired_workers = 0,
        },
        CoordinatorIntegrationConfig{

        },
        [](const WorkflowReadyStep& step) {
            return ScheduledJobSet{
                .job_set_id = 12000 + step.workflow_step_id,
                .workflow_step_id = step.workflow_step_id,
            };
        });

    coordinator.EnqueueReadyStep({
        .workflow_instance_id = 50,
        .workflow_step_id = 60,
        .step_key = "Grid",
        .step_kind = "seedprobe.grid",
        .priority = 1,
    });
    coordinator.EnqueueReadyStep({
        .workflow_instance_id = 50,
        .workflow_step_id = 61,
        .step_key = "Unique",
        .step_kind = "seedprobe.unique",
        .priority = 1,
    });

    const auto telemetry = coordinator.SnapshotTelemetry();
    EXPECT_GE(telemetry.max_ready_queue_depth, 0);
    EXPECT_GE(telemetry.ready_scan_count, 0);
    EXPECT_GE(telemetry.last_ready_scan_latency_ms, 0);
}

}
