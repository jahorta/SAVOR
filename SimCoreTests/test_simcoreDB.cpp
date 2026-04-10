#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
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
#include "SimCoreDB.h"
#include "Analysis/SqliteAnalysisDb.h"
#include "Archive/SqliteArchiveDb.h"
#include "Archive/ArchivePackageService.h"
#include "Archive/RehydrateExecutor.h"
#include "UIRead/SqliteUiReadDb.h"
#include "Runner/Parallel/SimCoreDB/ArchiveWorkflowCommands.h"
#include "Execution/Workflow/SqliteExecutionDb.h"
#include "Execution/Jobs/JobEventOrchestration.h"
#include "Execution/Workflow/SeedProbeWorkflowDefinition.h"
#include "Execution/Workflow/WorkflowEngine.h"
#include "Execution/Workflow/WorkflowIntegrityChecks.h"
#include "Execution/Workflow/WorkflowModeProvider.h"
#include "Execution/Workflow/WorkflowParityDiagnostics.h"
#include "Execution/Workflow/WorkflowParityStore.h"
#include "Execution/Workflow/WorkflowProjector.h"
#include "Execution/Workflow/WorkflowRecoveryService.h"
#include "Execution/Workflow/AdapterChainOrchestrator.h"
#include "Execution/Workflow/WorkflowTerminalOutboxSubscriber.h"
#include "Execution/ProgramDB/SeedProbe/SeedProbePhaseRegistration.h"
#include "Execution/ProgramDB/SeedProbe/SeedProbeNeutralAdapters.h"
#include "Execution/ProgramDB/SeedProbe/SeedProbeGridAdapters.h"
#include "Execution/ProgramDB/SeedProbe/SeedProbeUniqueAdapters.h"
#include "UIRead/Projectors/ProjectorContract.h"
#include "Runner/Parallel/SimCoreDB/WorkflowCoordinatorBridge.h"
#include "Runner/Parallel/SimCoreDB/DBWorkflowWorkerCoordinator.h"
#include "Runner/Parallel/SimCoreDB/WorkflowSchedulerAdapter.h"
#include "Runner/Parallel/SimCoreDB/StepInputAggregationService.h"

#include "common/RecordingExecutionDb.h"
#include "common/MapperExecutionDb.h"
#include "common/AlwaysAdvanceTransitionHandler.h"
#include "common/SqliteDbFixture.h"
#include "common/simcoredb_helpers.h"

namespace simcoreDB {
TEST(DbMigrateMigrationsIntegration, DISABLED_FilesystemSourceHasMigrationPerContext) {
    namespace fs = std::filesystem;
    using namespace simcore::db::migrations;

    const auto root = fs::weakly_canonical(fs::path("../../SimCoreDB/migration"));
    const MigrationSourceOptions filesystem_options{
        .source_kind = MigrationSourceKind::Filesystem,
        .filesystem_root = root,
    };

    for (const auto context : ListAllMigrationContexts()) {
        const auto entries = LoadContextMigrations(context, filesystem_options);
        ASSERT_FALSE(entries.empty()) << "Expected at least one migration in context " << ToString(context);
    }
}

TEST(Stage3cSeedProbeDefinition, ValidatesAndRejectsCycleDefinitions) {
    using namespace simcore::db::execution::workflow;

    auto definition = BuildSeedProbeChainDefinition();
    std::string err;
    EXPECT_TRUE(ValidateWorkflowDefinition(definition, &err)) << err;

    definition.steps[0].dependencies.push_back("Done");
    EXPECT_FALSE(ValidateWorkflowDefinition(definition, &err));
    EXPECT_NE(err.find("cycle"), std::string::npos);
}

TEST(Stage3cSeedProbeDefinition, ValidatesStepContracts) {
    using namespace simcore::db::execution::workflow;

    auto definition = BuildSeedProbeChainDefinition();
    std::string err;
    EXPECT_TRUE(ValidateWorkflowDefinition(definition, &err)) << err;
    ASSERT_EQ(definition.initial_inputs.size(), 1u);
    EXPECT_EQ(definition.initial_inputs[0], "general.transition_savestate");
    ASSERT_FALSE(definition.steps[0].required_inputs.empty());
    EXPECT_EQ(definition.steps[0].required_inputs[0], "general.transition_savestate");
    ASSERT_FALSE(definition.steps[2].provided_outputs.empty());
    EXPECT_EQ(definition.steps[2].provided_outputs[0], "general.input_frame_list");

    definition.steps[2].required_inputs.push_back("seedprobe.grid.extra_artifact");
    EXPECT_FALSE(ValidateWorkflowDefinition(definition, &err));
    EXPECT_NE(err.find("unsatisfied required_inputs"), std::string::npos);
    EXPECT_NE(err.find("Unique"), std::string::npos);
    EXPECT_NE(err.find("seedprobe.grid.extra_artifact"), std::string::npos);
}

TEST(Stage3cSeedProbeDefinition, RegistryRegistersDefaultsAndRejectsDuplicates) {
    using namespace simcore::db::execution::workflow;

    WorkflowDefinitionRegistry registry;
    std::string err;
    EXPECT_TRUE(registry.RegisterSeedProbeDefaults(&err)) << err;

    const auto* found = registry.Find("SEED_PROBE_CHAIN");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->steps.size(), 4u);

    EXPECT_FALSE(registry.RegisterSeedProbeDefaults(&err));
    EXPECT_NE(err.find("already registered"), std::string::npos);
}

TEST(Stage3cSeedProbeDefinition, RegistryRejectsInvalidContractDefinition) {
    using namespace simcore::db::execution::workflow;

    WorkflowDefinitionRegistry registry;
    auto definition = BuildSeedProbeChainDefinition();
    definition.steps[1].required_inputs.push_back("seedprobe.missing.contract");

    std::string err;
    EXPECT_FALSE(registry.RegisterDefinition(std::move(definition), &err));
    EXPECT_NE(err.find("invalid workflow definition"), std::string::npos);
    EXPECT_NE(err.find("seedprobe.missing.contract"), std::string::npos);
    EXPECT_NE(err.find("Grid"), std::string::npos);
}

TEST(Stage3cWorkflowEngine, ResolveReadinessAndRecoveryTransitions) {
    using namespace simcore::db::execution::workflow;

    WorkflowGraphSnapshot snapshot;
    snapshot.instance.workflow_instance_id = 44;
    snapshot.steps = {
        WorkflowStepRecord{ .workflow_step_id = 1, .workflow_instance_id = 44, .step_key = "Neutral", .step_kind = "seedprobe.neutral", .state = WorkflowStepState::Completed },
        WorkflowStepRecord{ .workflow_step_id = 2, .workflow_instance_id = 44, .step_key = "Grid", .step_kind = "seedprobe.grid", .state = WorkflowStepState::Waiting },
        WorkflowStepRecord{ .workflow_step_id = 3, .workflow_instance_id = 44, .step_key = "Unique", .step_kind = "seedprobe.unique", .state = WorkflowStepState::Materialized, .job_set_id = 555 },
        WorkflowStepRecord{ .workflow_step_id = 4, .workflow_instance_id = 44, .step_key = "Done", .step_kind = "seedprobe.done", .state = WorkflowStepState::Waiting },
    };

    const auto definition = BuildSeedProbeChainDefinition();
    const auto ready_result = ResolveReadiness(snapshot, definition, {});
    ASSERT_EQ(ready_result.transitions.size(), 1);
    EXPECT_EQ(ready_result.transitions[0].workflow_step_id, 2);
    EXPECT_EQ(ready_result.transitions[0].to, WorkflowStepState::Ready);

    const auto reconcile_result = ReconcileRunningSteps(snapshot, { { 555, "FAILED" } });
    ASSERT_EQ(reconcile_result.transitions.size(), 1);
    EXPECT_EQ(reconcile_result.transitions[0].workflow_step_id, 3);
    EXPECT_EQ(reconcile_result.transitions[0].to, WorkflowStepState::Failed);
}


TEST(Stage3cWorkflowParityDiagnostics, ClassifiesMissingAndMismatchedOutcomes) {
    using namespace simcore::db::execution::workflow;

    const auto report = CompareLegacyAndWorkflowOutcomes(
        {
            WorkflowOutcomeItem{ .step_key = "Neutral", .outcome = "COMPLETED" },
            WorkflowOutcomeItem{ .step_key = "Grid", .outcome = "COMPLETED" },
        },
        {
            WorkflowOutcomeItem{ .step_key = "Neutral", .outcome = "FAILED" },
            WorkflowOutcomeItem{ .step_key = "Unique", .outcome = "COMPLETED" },
        });

    EXPECT_EQ(report.compared_steps, 3);
    EXPECT_EQ(report.matched_steps, 0);
    EXPECT_EQ(report.mismatches.size(), 3);
}

TEST(Stage3cWorkflowParityDiagnostics, NormalizesLegacyAndWorkflowOutcomeVocabulary) {
    using namespace simcore::db::execution::workflow;

    const auto report = CompareLegacyAndWorkflowOutcomes(
        {
            WorkflowOutcomeItem{ .step_key = "neutral", .outcome = "SUCCEEDED" },
            WorkflowOutcomeItem{ .step_key = "Grid", .outcome = "SUCCEEDED_WINNER" },
            WorkflowOutcomeItem{ .step_key = "Unique", .outcome = "CANCELED" },
        },
        {
            WorkflowOutcomeItem{ .step_key = "NEUTRAL", .outcome = "COMPLETED" },
            WorkflowOutcomeItem{ .step_key = "grid", .outcome = "COMPLETED" },
            WorkflowOutcomeItem{ .step_key = "unique", .outcome = "FAILED" },
        });

    EXPECT_EQ(report.compared_steps, 3);
    EXPECT_EQ(report.matched_steps, 3);
    EXPECT_TRUE(report.mismatches.empty());
}

TEST(Stage3cWorkflowPromotionGate, EvaluatesDecisionFromLegacyAndWorkflowPathsAndBuildsArtifact) {
    using namespace simcore::db::execution::workflow;

    const std::vector<WorkflowOutcomeItem> legacy_pass_path{
        { .step_key = "Neutral", .outcome = "SUCCEEDED" },
        { .step_key = "Grid", .outcome = "SUCCEEDED_WINNER" },
        { .step_key = "Unique", .outcome = "SUCCEEDED" },
        { .step_key = "Done", .outcome = "COMPLETED" },
    };
    const std::vector<WorkflowOutcomeItem> workflow_pass_path{
        { .step_key = "neutral", .outcome = "COMPLETED" },
        { .step_key = "grid", .outcome = "COMPLETED" },
        { .step_key = "unique", .outcome = "COMPLETED" },
        { .step_key = "done", .outcome = "COMPLETED" },
    };
    const auto pass_parity = CompareLegacyAndWorkflowOutcomes(legacy_pass_path, workflow_pass_path);

    const WorkflowPromotionEvidence pass_evidence{
        .parity_compared_steps = pass_parity.compared_steps,
        .parity_matched_steps = pass_parity.matched_steps,
        .recovery_passed = true,
        .integrity_passed = true,
        .readiness_scan_p95_ms = 5.0,
        .readiness_scan_threshold_ms = 20.0,
    };

    const auto pass_decision = EvaluateWorkflowPromotionGate(pass_evidence);
    EXPECT_TRUE(pass_decision.approved);
    EXPECT_TRUE(pass_decision.blockers.empty());

    const auto pass_json = BuildWorkflowPromotionDecisionJson(pass_evidence, pass_decision);
    EXPECT_NE(pass_json.find("\"approved\":true"), std::string::npos);
    EXPECT_NE(pass_json.find("\"blockers\":[]"), std::string::npos);

    const std::vector<WorkflowOutcomeItem> legacy_fail_path{
        { .step_key = "Neutral", .outcome = "SUCCEEDED" },
        { .step_key = "Grid", .outcome = "SUCCEEDED" },
        { .step_key = "Unique", .outcome = "SUCCEEDED" },
        { .step_key = "Done", .outcome = "SUCCEEDED" },
    };
    const std::vector<WorkflowOutcomeItem> workflow_fail_path{
        { .step_key = "Neutral", .outcome = "FAILED" },
        { .step_key = "Grid", .outcome = "FAILED" },
        { .step_key = "Unique", .outcome = "FAILED" },
        { .step_key = "Done", .outcome = "FAILED" },
    };
    const auto fail_parity = CompareLegacyAndWorkflowOutcomes(legacy_fail_path, workflow_fail_path);

    const WorkflowPromotionEvidence fail_evidence{
        .parity_compared_steps = fail_parity.compared_steps,
        .parity_matched_steps = fail_parity.matched_steps,
        .recovery_passed = false,
        .integrity_passed = true,
        .readiness_scan_p95_ms = 50.0,
        .readiness_scan_threshold_ms = 20.0,
    };

    const auto fail_decision = EvaluateWorkflowPromotionGate(fail_evidence);
    EXPECT_FALSE(fail_decision.approved);
    EXPECT_GE(fail_decision.blockers.size(), 3u);

    const auto fail_json = BuildWorkflowPromotionDecisionJson(fail_evidence, fail_decision);
    EXPECT_NE(fail_json.find("parity_below_99_percent"), std::string::npos);
    EXPECT_NE(fail_json.find("recovery_failed"), std::string::npos);
    EXPECT_NE(fail_json.find("readiness_latency_above_threshold"), std::string::npos);

    const std::vector<std::string> required_json_fields{
        "\"approved\":",
        "\"parity_percent\":",
        "\"parity_compared_steps\":",
        "\"parity_matched_steps\":",
        "\"recovery_passed\":",
        "\"integrity_passed\":",
        "\"readiness_scan_p95_ms\":",
        "\"readiness_scan_threshold_ms\":",
        "\"blockers\":[",
    };

    auto has_all_required_fields = [&](const std::string& json) {
        for (const auto& token : required_json_fields) {
            if (json.find(token) == std::string::npos) {
                return false;
            }
        }
        return true;
    };

    EXPECT_TRUE(has_all_required_fields(pass_json));
    EXPECT_TRUE(has_all_required_fields(fail_json));

    const auto pass_parity_percent =
        (static_cast<double>(pass_evidence.parity_matched_steps) / static_cast<double>(pass_evidence.parity_compared_steps)) * 100.0;
    const auto fail_parity_percent =
        (static_cast<double>(fail_evidence.parity_matched_steps) / static_cast<double>(fail_evidence.parity_compared_steps)) * 100.0;

    std::ostringstream detail_summary;
    detail_summary << "Item18PromotionGate\n";
    detail_summary << "RequiredDataFieldsPresent(pass_json)="
                   << (has_all_required_fields(pass_json) ? "YES" : "NO") << "\n";
    detail_summary << "RequiredDataFieldsPresent(fail_json)="
                   << (has_all_required_fields(fail_json) ? "YES" : "NO") << "\n";
    detail_summary << "PassEvidence: "
                   << "approved=" << (pass_decision.approved ? "true" : "false")
                   << ", parity_percent=" << pass_parity_percent
                   << ", parity_compared_steps=" << pass_evidence.parity_compared_steps
                   << ", parity_matched_steps=" << pass_evidence.parity_matched_steps
                   << ", recovery_passed=" << (pass_evidence.recovery_passed ? "true" : "false")
                   << ", integrity_passed=" << (pass_evidence.integrity_passed ? "true" : "false")
                   << ", readiness_scan_p95_ms=" << pass_evidence.readiness_scan_p95_ms
                   << ", readiness_scan_threshold_ms=" << pass_evidence.readiness_scan_threshold_ms
                   << ", blockers_count=" << pass_decision.blockers.size() << "\n";
    detail_summary << "FailEvidence: "
                   << "approved=" << (fail_decision.approved ? "true" : "false")
                   << ", parity_percent=" << fail_parity_percent
                   << ", parity_compared_steps=" << fail_evidence.parity_compared_steps
                   << ", parity_matched_steps=" << fail_evidence.parity_matched_steps
                   << ", recovery_passed=" << (fail_evidence.recovery_passed ? "true" : "false")
                   << ", integrity_passed=" << (fail_evidence.integrity_passed ? "true" : "false")
                   << ", readiness_scan_p95_ms=" << fail_evidence.readiness_scan_p95_ms
                   << ", readiness_scan_threshold_ms=" << fail_evidence.readiness_scan_threshold_ms
                   << ", blockers_count=" << fail_decision.blockers.size() << "\n";
    detail_summary << "PassPathSampleSize(legacy|workflow)="
                   << legacy_pass_path.size() << "|" << workflow_pass_path.size() << "\n";
    detail_summary << "FailPathSampleSize(legacy|workflow)="
                   << legacy_fail_path.size() << "|" << workflow_fail_path.size() << "\n";
    detail_summary << "FailBlockers=";
    for (size_t i = 0; i < fail_decision.blockers.size(); ++i) {
        if (i > 0) {
            detail_summary << "|";
        }
        detail_summary << fail_decision.blockers[i];
    }

    ::testing::Test::RecordProperty("TestDetailSummary", detail_summary.str());
}

TEST(Stage3cWorkflowModeProvider, ParsesAndReturnsSelectedModes) {
    using namespace simcore::db::execution::workflow;

    StaticWorkflowModeProvider provider({ .mode = WorkflowExecutionMode::Workflow, .source = "unit-test" });
    const auto selection = provider.GetModeSelection();
    EXPECT_EQ(selection.mode, WorkflowExecutionMode::Workflow);
    EXPECT_EQ(selection.source, "unit-test");

    EXPECT_EQ(ParseWorkflowExecutionMode("Workflow", WorkflowExecutionMode::Workflow), WorkflowExecutionMode::Workflow);
    EXPECT_EQ(ParseWorkflowExecutionMode("invalid", WorkflowExecutionMode::Workflow), WorkflowExecutionMode::Workflow);

    const auto policy = BuildWorkflowAuthorityPolicy(WorkflowExecutionMode::Workflow);
    EXPECT_TRUE(policy.run_workflow);
}

TEST(Stage3cCoordinatorBridge, DeduplicatesTerminalSignalsAndSchedulesReadySteps) {
    using namespace simcore::runner::parallel::simcoredb;

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
    using namespace simcore::db::execution::programdb::seedprobe;

    auto neutral = BuildSeedProbeNeutralDescriptor(nullptr, nullptr);
    auto grid = BuildSeedProbeGridDescriptor(
        nullptr,
        nullptr,
        SeedProbeGridBlueprintConfig{},
        SeedProbeGridSpec{},
        [](std::int64_t) -> std::optional<GridResultContext> { return std::nullopt; });
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
    using namespace simcore::db::execution::programdb;
    using namespace simcore::db::execution::programdb::seedprobe;

    ProgramKindRegistry registry;
    SeedProbePhaseRegistrationConfig config{};
    config.grid_context_lookup = [](std::int64_t) -> std::optional<GridResultContext> { return std::nullopt; };
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
    using namespace simcore::db::execution::programdb::seedprobe;

    SeedProbeUniqueTransitionHandler handler([](const auto&) { return false; });
    const simcore::db::execution::programdb::WorkflowTransitionContext context{
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

TEST(Stage2AdapterChain, InvokesCanonicalOrderAndWriterContract) {
    using namespace simcore::db::execution::programdb;
    using namespace simcore::db::execution::workflow;

    class MockPersistence final : public IJobPersistenceAdapter {
    public:
        JobPersistenceRecord EncodeForQueueing(std::int64_t domain_ref_id) const override {
            JobPersistenceRecord r{};
            r.program_ref_kind = "mock";
            r.program_ref_id = domain_ref_id;
            return r;
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
    };
    class MockMapper final : public IResultMapper {
    public:
        std::string BuildResultIniFromPrResult(std::int64_t, const simcore::PRResult&) const override {
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

    simcore::PRResult pr{};
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
    using namespace simcore::db::execution::jobs;
    using namespace simcore::db::execution::programdb::seedprobe;

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
    using namespace simcore::db::execution::workflow;

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

TEST(Stage1StepInputAggregation, AllInputsRequiredGatingAndEventSequence) {
    using namespace simcore::runner::parallel::simcoredb;

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
    using namespace simcore::runner::parallel::simcoredb;

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
    using namespace simcore::runner::parallel::simcoredb;

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
    using namespace simcore::runner::parallel::simcoredb;
    using namespace simcore::db::execution::workflow;

    StaticWorkflowModeProvider mode_provider({ .mode = WorkflowExecutionMode::Workflow, .source = "unit-test" });

    DBWorkflowWorkerCoordinator coordinator(
        nullptr,
        &mode_provider,
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

TEST(Stage3cCoordinatorReplacement, PersistsMaterializedAndTerminalTransitionsToExecutionDb) {
    using namespace simcore::runner::parallel::simcoredb;
    using namespace simcore::db::execution::workflow;

    RecordingExecutionDb execution_db;
    StaticWorkflowModeProvider mode_provider({ .mode = WorkflowExecutionMode::Workflow, .source = "unit-test" });

    DBWorkflowWorkerCoordinator coordinator(
        &execution_db,
        &mode_provider,
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

TEST(Stage1CoordinatorIntegration, AggregationGatesMaterializationAndEmitsInputEvents) {
    using namespace simcore::runner::parallel::simcoredb;
    using namespace simcore::db::execution::workflow;

    RecordingExecutionDb execution_db;
    StaticWorkflowModeProvider mode_provider({ .mode = WorkflowExecutionMode::Workflow, .source = "stage1-test" });

    DBWorkflowWorkerCoordinator coordinator(
        &execution_db,
        &mode_provider,
        DBWorkflowWorkerCoordinatorConfig{
            .desired_workers = 0,
            .controller_sleep_ms = 1,
        },
        CoordinatorIntegrationConfig{},
        [](const WorkflowReadyStep& step) {
            return ScheduledJobSet{
                .job_set_id = 7000 + step.workflow_step_id,
                .workflow_step_id = step.workflow_step_id,
            };
        });

    coordinator.EnqueueReadyStep({
        .workflow_instance_id = 101,
        .workflow_step_id = 202,
        .step_key = "Neutral",
        .step_kind = "seedprobe.neutral",
        .priority = 1,
    });

    coordinator.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    coordinator.Stop();

    ASSERT_EQ(execution_db.command_service.materialized_calls.size(), 1u);
    EXPECT_EQ(execution_db.command_service.materialized_calls.front().workflow_step_id, 202);
    EXPECT_FALSE(execution_db.command_service.input_events.empty());
    bool saw_requested = false;
    bool saw_fragment = false;
    bool saw_complete = false;
    for (const auto& event : execution_db.command_service.input_events) {
        if (event.event_kind == "Execution.WorkflowStepInputRequested.v1") saw_requested = true;
        if (event.event_kind == "Execution.WorkflowStepInputFragmentReady.v1") saw_fragment = true;
        if (event.event_kind == "Execution.WorkflowStepInputComplete.v1") saw_complete = true;
    }
    EXPECT_TRUE(saw_requested);
    EXPECT_TRUE(saw_fragment);
    EXPECT_TRUE(saw_complete);

    const auto telemetry = coordinator.SnapshotTelemetry();
    EXPECT_GE(telemetry.input_complete_count, 1);
    EXPECT_GE(telemetry.last_input_latency_ms, 0);
}

TEST(Stage3cCoordinatorReplacement, DisabledWorkflowModeSkipsWorkflowPersistencePath) {
    using namespace simcore::runner::parallel::simcoredb;
    using namespace simcore::db::execution::workflow;

    RecordingExecutionDb execution_db;
    StaticWorkflowModeProvider mode_provider({ .mode = WorkflowExecutionMode::Workflow, .source = "unit-test" });

    DBWorkflowWorkerCoordinator coordinator(
        &execution_db,
        &mode_provider,
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
    using namespace simcore::db::events;

    std::string error;
    EXPECT_TRUE(ValidateEventTypeFormat("Execution.WorkflowStepFailed.v1", 1, &error)) << error;
    EXPECT_TRUE(ValidateEventTypeFormat("AnalysisSeedProbe.RunCompleted.v1", 1, &error)) << error;

    EXPECT_FALSE(ValidateEventTypeFormat("Execution.WorkflowStepFailed", 1, &error));
    EXPECT_FALSE(ValidateEventTypeFormat("Execution.WorkflowStepFailed.v2", 1, &error));
    EXPECT_FALSE(ValidateEventTypeFormat("Execution.Workflow.StepFailed.v1", 1, &error));
    EXPECT_FALSE(ValidateEventTypeFormat("Execution.WorkflowStepFailed.v1", 0, &error));
}

TEST(Stage3cEventContracts, PayloadDispatchAndValidationRejectVersionSuffixMismatches) {
    using namespace simcore::db::events;

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

TEST(Stage3cEventContracts, WorkflowInputEventsRouteToExecutionContractAndRequireWorkflowInputPayloadFamily) {
    using namespace simcore::db::events;

    constexpr std::array<std::string_view, 3> kWorkflowInputEvents{ {
        "Execution.WorkflowStepInputRequested.v1",
        "Execution.WorkflowStepInputFragmentReady.v1",
        "Execution.WorkflowStepInputComplete.v1",
    } };

    for (const auto event_type : kWorkflowInputEvents) {
        const auto contract = ResolvePayloadResolverContract(event_type, 1);
        ASSERT_TRUE(contract.has_value()) << event_type;
        EXPECT_EQ(*contract, PayloadResolverContract::ExecutionWorkflowJobV1) << event_type;

        EventEnvelope valid{};
        valid.event_type = std::string(event_type);
        valid.event_version = 1;
        valid.context_name = "Execution";
        valid.aggregate_kind = "workflow_step";
        valid.payload_ref_kind = "workflow_input_event";
        valid.payload_ref_id = 123;
        std::string error;
        EXPECT_TRUE(ValidateExecutionWorkflowJobPayloadV1(valid, &error)) << event_type << ": " << error;

        valid.payload_ref_kind = "workflow_event";
        EXPECT_FALSE(ValidateExecutionWorkflowJobPayloadV1(valid, &error));
        EXPECT_EQ(error, "payload_ref_kind must be workflow_input_event for Execution.WorkflowStepInput* event");
    }
}


TEST(Stage3cEventContracts, SeedProbeValidationRequiresConcretePayloadRefKinds) {
    using namespace simcore::db::events;

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
    using namespace simcore::db::events;

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
    using namespace simcore::db::events;

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
    using namespace simcore::db::events;

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
    using namespace simcore::db::events;

    constexpr std::array<std::string_view, 7> kAuthoringEventTypes{ {
        "Authoring.SeedProbeSpecSaved.v1",
        "Authoring.TasSpecSaved.v1",
        "Authoring.BattleRunSpecSaved.v1",
        "Authoring.PlanSaved.v1",
        "Authoring.PredicateSpecSaved.v1",
        "Authoring.SettingsSaved.v1",
        "Authoring.TemplateSaved.v1",
    } };

    for (const auto event_type : kAuthoringEventTypes) {
        const auto contract = ResolvePayloadResolverContract(event_type, 1);
        ASSERT_TRUE(contract.has_value()) << event_type;
        EXPECT_EQ(*contract, PayloadResolverContract::AuthoringV1) << event_type;
    }
}

TEST(Stage3cEventContracts, AuthoringCatalogEntriesRemainDispatched) {
    using namespace simcore::db::events;

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

    EXPECT_EQ(authoring_entries, 7u);
}

TEST(Stage3cCoordinatorModes, ModeMatrixPoliciesDriveWorkflowPathDecisions) {
    using namespace simcore::runner::parallel::simcoredb;
    using namespace simcore::db::execution::workflow;

    const std::vector<std::pair<CoordinatorIntegrationConfig, bool>> matrix{
        { CoordinatorIntegrationConfig{ .workflow_enabled = false }, false },
        { CoordinatorIntegrationConfig{ .workflow_enabled = true }, true },
    };

    for (const auto& [integration_cfg, should_run_workflow] : matrix) {
        RecordingExecutionDb execution_db;
        StaticWorkflowModeProvider mode_provider({ .mode = WorkflowExecutionMode::Workflow, .source = "mode-matrix" });

        DBWorkflowWorkerCoordinator coordinator(
            &execution_db,
            &mode_provider,
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
    using namespace simcore::runner::parallel::simcoredb;
    using namespace simcore::db::execution::workflow;

    RecordingExecutionDb execution_db;
    StaticWorkflowModeProvider mode_provider({ .mode = WorkflowExecutionMode::Workflow, .source = "telemetry-test" });

    DBWorkflowWorkerCoordinator coordinator(
        &execution_db,
        &mode_provider,
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
