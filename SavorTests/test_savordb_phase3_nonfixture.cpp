#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <future>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include "Common/Events/OutboxRelay.h"
#include "Common/Performance/DbPerfReport.h"
#include "Common/Migrations/MigrationRunner.h"
#include "Analysis/QueuedAnalysisDb.h"
#include "Archive/QueuedArchiveDb.h"
#include "Authoring/QueuedAuthoringDb.h"
#include "Execution/QueuedExecutionDb.h"
#include "Execution/Workflow/SqliteExecutionDb.h"
#include "Execution/Workflow/WorkflowCoordinatorService.h"
#include "Execution/Workflow/WorkflowRecoveryService.h"
#include "Execution/DBWorkflowWorkerCoordinator.h"
#include "Execution/JobMaterializationService.h"
#include "Execution/WorkflowDispatchCoordinator.h"
#include "Execution/WorkflowSchedulerAdapter.h"
#include "State/QueuedStateDb.h"
#include "UIRead/QueuedUiReadDb.h"
#include "Worker/ProcessWorker.h"
#include "Worker/WorkerStatusRegistry.h"
#include "common/DbPreparer.h"
#include "common/RecordingExecutionDb.h"
#include "common/RecordingJobEventCommandService.h"
#include "common/savordb_helpers.h"

namespace savordb {
    namespace {
        bool QueryText(sqlite3* db, const char* sql, std::string* value_out) {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
                return false;
            }
            const int rc = sqlite3_step(st);
            if (rc != SQLITE_ROW) {
                sqlite3_finalize(st);
                return false;
            }
            const auto* text = sqlite3_column_text(st, 0);
            *value_out = text != nullptr ? reinterpret_cast<const char*>(text) : "";
            sqlite3_finalize(st);
            return true;
        }

        bool QueryInt64(sqlite3* db, const char* sql, std::int64_t* value_out) {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
                return false;
            }
            const int rc = sqlite3_step(st);
            if (rc != SQLITE_ROW) {
                sqlite3_finalize(st);
                return false;
            }
            *value_out = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
            return true;
        }

        bool WaitForCondition(std::function<bool()> condition, std::chrono::milliseconds timeout = std::chrono::milliseconds{ 1000 }) {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline) {
                if (condition()) {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{ 5 });
            }
            return condition();
        }

        savor::runtime::WorkerRuntimeManifest CompleteWorksetTestManifest() {
            static constexpr std::array<
                std::pair<std::string_view, std::string_view>,
                9>
                kModules{{
                    {"soa.seed_probe", "probe"},
                    {"soa.navigation.context", "capture"},
                    {"soa.tas_movie", "play_and_checkpoint"},
                    {"soa.tas_frame_detector", "detect"},
                    {"soa.battle.context", "capture"},
                    {"soa.battle.macro_probe", "probe"},
                    {"soa.battle.single_turn", "execute"},
                    {"soa.battle.completion", "complete"},
                    {"soa.battle.results_screen", "advance"},
                }};
            savor::runtime::WorkerRuntimeManifest manifest;
            manifest.catalog_status =
                savor::runtime::RuntimeCatalogStatus::CompleteExact;
            manifest.catalog_sha256 =
                "phase3-workset-test-catalog";
            manifest.runtime_profile_sha256 =
                "phase3-workset-test-runtime";
            manifest.dependency_manifest_sha256 =
                "phase3-workset-test-dependencies";
            for (const auto& [module_id, entrypoint] : kModules) {
                manifest.modules.push_back(
                    savor::runtime::RuntimeModuleManifestEntry{
                        .module = {
                            .canonical_id = std::string(module_id),
                            .revision = 1,
                            .canonical_hash =
                                "phase3-workset-test:" +
                                std::string(module_id),
                        },
                        .entrypoints = {
                            std::string(entrypoint)},
                        .dependency_manifest_sha256 =
                            "phase3-workset-test-module-dependencies",
                    });
            }
            return manifest;
        }

        void ConfigureWorksetTestGate(
            savor::runner::parallel::savordb::
                DBWorkflowWorkerCoordinatorConfig& config) {
            using namespace savor::runner::parallel::savordb;
            config.expected_catalog_sha256 =
                "phase3-workset-test-catalog";
            config.expected_runtime_profile_sha256 =
                "phase3-workset-test-runtime";
            config.expected_dependency_manifest_sha256 =
                "phase3-workset-test-dependencies";
            config.workset_definition_builder =
                [](std::size_t,
                   const std::vector<ClaimedJobRecord>&,
                   const savor::runtime::WorkerRuntimeManifest&,
                   std::string*)
                -> std::optional<
                    savor::runtime::WorkerWorksetDefinition> {
                    return std::nullopt;
                };
            config.workset_terminal_decoder =
                [](const ClaimedJobRecord&,
                   const savor::wrms::
                       WorksetItemTerminalPayload&,
                   std::string*)
                -> std::optional<savor::PRResult> {
                    return savor::PRResult{};
                };
            config.worker_capability_preflight =
                [](std::size_t,
                   const DBWorkflowWorkerCoordinatorConfig&,
                   const std::shared_ptr<
                       savor::ProcessWorker>&) {
                    return
                        CoordinatorWorkerCapabilityPreflightResult{
                            .process_ready = true,
                            .capabilities =
                                savor::runtime::AddCapability(
                                    savor::runtime::
                                        kSlice1ProductionCapabilities,
                                    savor::runtime::
                                        WorkerCapability::
                                            WorksetDispatch),
                            .runtime_manifest =
                                CompleteWorksetTestManifest(),
                        };
                };
        }

        class TestProgramJobMaterializer final
            : public savor::db::execution::programdb::
                  IProgramJobMaterializer {
        public:
            explicit TestProgramJobMaterializer(
                savor::db::IExecutionDb* execution_db,
                std::shared_ptr<std::atomic<int>>
                    continuation_count = {},
                savor::db::execution::programdb::
                    ProgramJobContinuationDisposition
                        continuation_disposition =
                            savor::db::execution::programdb::
                                ProgramJobContinuationDisposition::
                                    Complete)
                : execution_db_(execution_db)
                , continuation_count_(
                      std::move(continuation_count))
                , continuation_disposition_(
                      continuation_disposition) {
            }

            bool Materialize(
                const savor::db::execution::programdb::
                    ProgramJobMaterializationContext& context,
                savor::db::execution::programdb::
                    WorkflowStepScheduleResult* result_out,
                std::string* error_out) const override {
                using savor::db::ExecutionDbOperationDisposition;
                if (result_out == nullptr || execution_db_ == nullptr) {
                    if (error_out != nullptr) {
                        *error_out =
                            "workflow coordinator test materializer is "
                            "unavailable";
                    }
                    return false;
                }
                *result_out = {};

                const auto accepted = [](auto disposition) {
                    return disposition
                            == ExecutionDbOperationDisposition::Applied
                        || disposition
                            == ExecutionDbOperationDisposition::
                                AlreadyApplied;
                };
                const auto fail = [&](std::string fallback) {
                    if (error_out != nullptr && error_out->empty()) {
                        *error_out = std::move(fallback);
                    }
                    return false;
                };
                const auto materialization_key =
                    "workflow-coordinator-test-"
                    + std::to_string(
                        context.step.workflow_step_id);
                const auto fingerprint =
                    materialization_key + ".job";
                const auto set_result =
                    [&](std::int64_t job_set_id) {
                        result_out->root_job_set_id =
                            job_set_id;
                        result_out->persistence = {
                            .program_ref_kind = "unit.input",
                            .program_ref_id =
                                context.step.domain_ref_id,
                            .fingerprint = fingerprint,
                            .program_version = 1,
                        };
                    };

                savor::db::EnsureMaterializingJobSetReceipt
                    job_set{};
                if (!execution_db_->EnsureMaterializingJobSet(
                    {
                        .materialization_key =
                            materialization_key,
                        .parent_job_set_id = std::nullopt,
                        .program_kind = 1,
                        .purpose = "workflow-test",
                        .created_by =
                            std::string(
                                "WorkflowCoordinatorServiceTest"),
                        .created_at_utc = 0,
                        .priority_boost = 0,
                        .expected_total = 1,
                        .domain_ref_kind =
                            std::string("unit.input"),
                        .domain_ref_id =
                            context.step.domain_ref_id,
                    },
                    &job_set,
                    error_out)
                    || !accepted(job_set.disposition)
                    || job_set.job_set_id <= 0) {
                    return fail(
                        "failed ensuring workflow coordinator test "
                        "job set");
                }
                if (job_set.materialization_state
                    == "WORKSET_PUBLICATION_COMPLETE") {
                    set_result(job_set.job_set_id);
                    if (error_out != nullptr) {
                        error_out->clear();
                    }
                    return true;
                }

                std::int64_t job_id = 0;
                if (job_set.materialization_state
                    == "MATERIALIZING") {
                    savor::db::CreatePendingJobReceipt job{};
                    if (!execution_db_->CreatePendingJob(
                        {
                            .job_set_id =
                                job_set.job_set_id,
                            .program_kind = 1,
                            .program_version = 1,
                            .program_ref_kind = "unit.input",
                            .program_ref_id =
                                context.step.domain_ref_id,
                            .fingerprint = fingerprint,
                            .priority =
                                context.step.step_priority,
                            .max_attempts = 1,
                        },
                        &job,
                        error_out)
                        || !accepted(job.disposition)
                        || job.job_id <= 0) {
                        return fail(
                            "failed creating workflow coordinator "
                            "test job");
                    }
                    job_id = job.job_id;
                } else {
                    const auto jobs =
                        execution_db_->ListJobsInJobSet(
                            job_set.job_set_id);
                    if (jobs.size() != 1) {
                        return fail(
                            "workflow coordinator test job set does "
                            "not contain exactly one job");
                    }
                    job_id = jobs.front().job_id;
                }

                savor::db::SealJobPopulationReceipt seal{};
                if (!execution_db_->SealJobPopulation(
                    {
                        .job_set_id = job_set.job_set_id,
                        .expected_job_count = 1,
                        .requested_by =
                            "WorkflowCoordinatorServiceTest",
                    },
                    &seal,
                    error_out)
                    || !accepted(seal.disposition)) {
                    return fail(
                        "failed sealing workflow coordinator test "
                        "job population");
                }

                savor::db::PublishWorksetWaveReceipt wave{};
                if (!execution_db_->PublishWorksetWave(
                    {
                        .job_set_id = job_set.job_set_id,
                        .expected_job_count = 1,
                        .worksets = {{
                            .job_set_id = job_set.job_set_id,
                            .workset_key = materialization_key + ".workset",
                            .program_kind = 1,
                            .program_version = 1,
                            .compatibility = {
                                .compatibility_key = "workflow-coordinator-test",
                                .module_canonical_id = "workflow.coordinator.test",
                                .module_version = 1,
                                .module_sha256 = std::string(64, '1'),
                                .entrypoint = "execute",
                                .verified_dependency_sha256 = std::string(64, '2'),
                                .runtime_profile_sha256 = std::string(64, '3'),
                                .required_capability_mask = 0,
                                .estimated_payload_bytes = 1,
                            },
                            .priority = context.step.step_priority,
                            .ordered_job_ids = {job_id},
                            .requested_by = "WorkflowCoordinatorServiceTest",
                        }},
                        .requested_by =
                            "WorkflowCoordinatorServiceTest",
                    },
                    &wave,
                    error_out)
                    || !accepted(wave.disposition)
                    || wave.worksets.size() != 1
                    || wave.worksets.front().workset_id <= 0) {
                    return fail(
                        "failed publishing workflow coordinator test "
                        "workset");
                }

                set_result(job_set.job_set_id);
                if (error_out != nullptr) {
                    error_out->clear();
                }
                return true;
            }

            bool Continue(
                const savor::db::execution::programdb::
                    ProgramJobContinuationContext&,
                savor::db::execution::programdb::
                    ProgramJobContinuationResult* result_out,
                std::string* error_out) const override {
                if (result_out == nullptr) {
                    if (error_out != nullptr) {
                        *error_out =
                            "workflow coordinator test continuation "
                            "result is required";
                    }
                    return false;
                }
                if (continuation_count_ != nullptr) {
                    continuation_count_->fetch_add(
                        1,
                        std::memory_order_relaxed);
                }
                *result_out = {};
                result_out->disposition =
                    continuation_disposition_;
                if (continuation_disposition_
                    == savor::db::execution::programdb::
                        ProgramJobContinuationDisposition::Failed) {
                    result_out->failure_code =
                        "TEST_CONTINUATION_FAILED";
                    result_out->failure_text =
                        "test continuation rejected completed work";
                }
                if (error_out != nullptr) {
                    error_out->clear();
                }
                return true;
            }

        private:
            savor::db::IExecutionDb* execution_db_ = nullptr;
            std::shared_ptr<std::atomic<int>>
                continuation_count_;
            savor::db::execution::programdb::
                ProgramJobContinuationDisposition
                    continuation_disposition_;
        };

        class TestWorksetReconstructionAdapter final
            : public savor::db::execution::programdb::
                  IWorksetReconstructionAdapter {
        public:
            std::optional<
                savor::db::execution::programdb::
                    WorksetReconstructionResult>
            Reconstruct(
                const savor::db::execution::programdb::
                    WorksetReconstructionContext&,
                std::string*) const override {
                return std::nullopt;
            }
        };

        class TestProgramResultHandler final
            : public savor::db::execution::programdb::
                  IProgramResultHandler {
        public:
            savor::db::execution::programdb::
                ProgramResultDecision
            Process(
                const savor::db::execution::programdb::
                    ProgramResultProcessingContext&)
                const override {
                return {
                    .final_job_state = "SUCCEEDED",
                };
            }
        };

        class AdvanceToNextStepHandler final : public savor::db::execution::programdb::IWorkflowTransitionHandler {
        public:
            savor::db::execution::programdb::WorkflowTransitionDecision EvaluateTransition(
                const savor::db::execution::programdb::WorkflowTransitionContext&) const override {
                savor::db::execution::programdb::WorkflowTransitionDecision decision{};
                decision.should_advance = true;
                decision.next_step_key = "Next";
                return decision;
            }
        };

        savor::db::execution::programdb::ProgramKindRegistry BuildWorkflowCoordinatorTestRegistry(
            savor::db::IExecutionDb* execution_db,
            bool include_transition = false,
            std::shared_ptr<std::atomic<int>>
                continuation_count = {},
            savor::db::execution::programdb::
                ProgramJobContinuationDisposition
                    continuation_disposition =
                        savor::db::execution::programdb::
                            ProgramJobContinuationDisposition::
                                Complete) {
            savor::db::execution::programdb::ProgramKindRegistry registry;
            savor::db::execution::programdb::ProgramKindDescriptor descriptor{};
            descriptor.program_kind = 1;
            descriptor.program_name = "WorkflowCoordinatorServiceTest";
            descriptor.job_materializer =
                std::make_shared<TestProgramJobMaterializer>(
                    execution_db,
                    std::move(continuation_count),
                    continuation_disposition);
            descriptor.workset_reconstruction =
                std::make_shared<
                    TestWorksetReconstructionAdapter>();
            descriptor.result_handler =
                std::make_shared<TestProgramResultHandler>();
            if (include_transition) {
                descriptor.workflow_transition = std::make_shared<AdvanceToNextStepHandler>();
            }
            descriptor.supports_workflow_orchestration = true;
            (void)registry.RegisterForStepKind("unit.ready", descriptor);
            (void)registry.RegisterForStepKind("unit.step", descriptor);
            return registry;
        }

        savor::db::execution::workflow::WorkflowCoordinatorConfig FastWorkflowCoordinatorConfig() {
            return savor::db::execution::workflow::WorkflowCoordinatorConfig{
                .workflow_enabled = true,
                .strict_smoke_terminal_on_failure = false,
                .poll_interval = std::chrono::milliseconds{ 5 },
                .ready_scan_limit = 16,
                .terminal_scan_limit = 16,
            };
        }

        bool InsertActiveWorkflow(
            sqlite3* db,
            std::int64_t workflow_instance_id,
            std::int64_t workflow_step_id,
            const std::string& workflow_state = "RUNNING",
            const std::string& step_state = "MATERIALIZED") {
            return ExecSql(
                db,
                ("INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc) "
                 "VALUES(" + std::to_string(workflow_instance_id) + ", 'workflow_coordinator_test', '" + workflow_state + "', 'manual', 'test', unixepoch()*1000, unixepoch()*1000);"
                 "INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, attempts, max_attempts, created_at_utc, started_at_utc) "
                 "VALUES(" + std::to_string(workflow_step_id) + ", " + std::to_string(workflow_instance_id) + ", 'Active', 'unit.step', '" + step_state + "', 0, 1, unixepoch()*1000, unixepoch()*1000);")
                    .c_str());
        }

        bool InsertReadyWorkflow(
            sqlite3* db,
            std::int64_t workflow_instance_id,
            std::int64_t workflow_step_id,
            std::int64_t input_ref_id) {
            return ExecSql(
                db,
                ("INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc) "
                 "VALUES(" + std::to_string(workflow_instance_id) + ", 'workflow_coordinator_test', 'RUNNING', 'manual', 'test', unixepoch()*1000, unixepoch()*1000);"
                 "INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, input_ref_kind, input_ref_id, attempts, max_attempts, created_at_utc, ready_at_utc) "
                 "VALUES(" + std::to_string(workflow_step_id) + ", " + std::to_string(workflow_instance_id) + ", 'Ready', 'unit.ready', 'READY', 'unit.input', "
                 + std::to_string(input_ref_id) + ", 0, 1, unixepoch()*1000, unixepoch()*1000);")
                    .c_str());
        }
    }

    TEST(Stage3Phase2Contracts, DbLifecyclePreservesCanonicalEventOrderingAndOutboxContracts) {
        using namespace savor::db::execution::workflow;
        using namespace savor::db::migrations;

        sqlite3* db = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &db));
        const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
        std::string err;
        ASSERT_TRUE(ApplyContextMigrations(db, MigrationContext::Execution, embedded_options, &err)) << err;

        ASSERT_TRUE(ExecSql(db, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(2501, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'test', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, attempts, max_attempts, created_at_utc, ready_at_utc)
VALUES(2502, 2501, 'Neutral', 'seedprobe.neutral', 'READY', 0, 2, unixepoch()*1000, unixepoch()*1000);
)SQL"));

        SqliteExecutionDb execution_db(db);
        auto* commands = execution_db.WorkflowCommandService();
        ASSERT_NE(commands, nullptr);

        ASSERT_TRUE(commands->MarkStepMaterialized({ .workflow_step_id = 2502, .job_set_id = 8801, .requested_by = "SavorTests" }, &err)) << err;
        ASSERT_TRUE(commands->MarkStepTerminal({ .workflow_step_id = 2502, .terminal_state = "COMPLETED", .requested_by = "SavorTests" }, &err)) << err;

        sqlite3_stmt* st = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db,
            "SELECT event_kind FROM exec_workflow_event WHERE workflow_step_id=2502 ORDER BY workflow_event_id;",
            -1, &st, nullptr));
        std::vector<std::string> lifecycle_events;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const auto* text = sqlite3_column_text(st, 0);
            lifecycle_events.emplace_back(text != nullptr ? reinterpret_cast<const char*>(text) : "");
        }
        sqlite3_finalize(st);
        EXPECT_EQ(lifecycle_events, (std::vector<std::string>{
            "Execution.WorkflowStepMaterialized.v1",
            "Execution.WorkflowStepCompleted.v1",
            }));

        ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(
            db,
            "SELECT COUNT(1) FROM exec_outbox_message WHERE aggregate_kind='workflow_instance' AND payload_ref_kind='workflow_event';",
            -1,
            &st,
            nullptr));
        ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
        EXPECT_EQ(sqlite3_column_int(st, 0), 2);
        sqlite3_finalize(st);

        sqlite3_close(db);
    }

    TEST(Stage3Phase2Contracts, FailedTerminalStepDoesNotAllowInvalidDownstreamAdvance) {
        using namespace savor::db::execution::workflow;
        using namespace savor::db::migrations;

        sqlite3* db = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &db));
        const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
        std::string err;
        ASSERT_TRUE(ApplyContextMigrations(db, MigrationContext::Execution, embedded_options, &err)) << err;

        ASSERT_TRUE(ExecSql(db, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(2701, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'test', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, attempts, max_attempts, created_at_utc, ready_at_utc)
VALUES
    (2702, 2701, 'Neutral', 'seedprobe.neutral', 'READY', 0, 2, unixepoch()*1000, unixepoch()*1000),
    (2703, 2701, 'Grid', 'seedprobe.grid', 'WAITING', 0, 2, unixepoch()*1000, NULL);
)SQL"));

        SqliteExecutionDb execution_db(db);
        auto* commands = execution_db.WorkflowCommandService();
        ASSERT_NE(commands, nullptr);
        ASSERT_TRUE(commands->MarkStepMaterialized({ .workflow_step_id = 2702, .job_set_id = 9101, .requested_by = "SavorTests" }, &err)) << err;
        ASSERT_TRUE(commands->MarkStepTerminal({ .workflow_step_id = 2702, .terminal_state = "FAILED", .requested_by = "SavorTests" }, &err)) << err;

        EXPECT_FALSE(commands->MarkStepMaterialized({ .workflow_step_id = 2703, .job_set_id = 9102, .requested_by = "SavorTests" }, &err));

        sqlite3_stmt* st = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db, "SELECT state FROM exec_workflow_step WHERE workflow_step_id=2702;", -1, &st, nullptr));
        ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
        ASSERT_NE(sqlite3_column_text(st, 0), nullptr);
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 0))), "FAILED");
        sqlite3_finalize(st);
        sqlite3_close(db);
    }

    TEST(Stage3Phase2Contracts, TransitionTwoStepSuccessUsesSavestateSignalAndAdvancesGridToReady) {
        using namespace savor::db::execution::workflow;
        using namespace savor::db::migrations;

        const auto temp_dir = MakeTempPhase4Dir("phase2-two-step-savestate");
        const auto savestate_path = temp_dir / "input_transition.sav";
        {
            std::ofstream out(savestate_path, std::ios::binary);
            out << "dummy-savestate";
        }
        ASSERT_TRUE(std::filesystem::exists(savestate_path));

        sqlite3* db = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &db));
        const MigrationSourceOptions embedded_options{ .source_kind = MigrationSourceKind::Embedded };
        std::string err;
        ASSERT_TRUE(ApplyContextMigrations(db, MigrationContext::Execution, embedded_options, &err)) << err;

        ASSERT_TRUE(ExecSql(db, R"SQL(
INSERT INTO exec_workflow_instance(
    workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc
)
VALUES(2601, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'test', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, attempts, max_attempts, input_ref_kind, created_at_utc, ready_at_utc)
VALUES
    (2602, 2601, 'Neutral', 'seedprobe.neutral', 'READY', 0, 2, 'sp_probe_run.probe_run_id', unixepoch()*1000, unixepoch()*1000),
    (2603, 2601, 'Grid', 'seedprobe.grid', 'WAITING', 0, 2, 'seedprobe.neutral.seed_context', unixepoch()*1000, NULL);
INSERT INTO exec_workflow_edge(workflow_edge_id, workflow_instance_id, from_step_id, to_step_id, created_at_utc)
VALUES(2604, 2601, 2602, 2603, unixepoch()*1000);
)SQL"));

        SqliteExecutionDb execution_db(db);
        auto* commands = execution_db.WorkflowCommandService();
        auto* queries = execution_db.WorkflowQueryService();
        ASSERT_NE(commands, nullptr);
        ASSERT_NE(queries, nullptr);

        ASSERT_TRUE(commands->MarkStepMaterialized({ .workflow_step_id = 2602, .job_set_id = 9001, .requested_by = "SavorTests" }, &err)) << err;
        ASSERT_TRUE(commands->MarkStepTerminal({ .workflow_step_id = 2602, .terminal_state = "COMPLETED", .requested_by = "SavorTests" }, &err)) << err;
        ASSERT_TRUE(ExecSql(db, "UPDATE exec_workflow_step SET state='READY', ready_at_utc=unixepoch()*1000 WHERE workflow_step_id=2603 AND state='WAITING';"));

        const auto graph = queries->GetWorkflowGraph(2601);
        ASSERT_TRUE(graph.has_value());
        bool neutral_completed = false;
        bool grid_ready = false;
        for (const auto& step : graph->steps) {
            if (step.step_key == "Neutral" && step.state == WorkflowStepState::Completed) {
                neutral_completed = true;
            }
            if (step.step_key == "Grid" && step.state == WorkflowStepState::Ready) {
                grid_ready = true;
            }
        }
        EXPECT_TRUE(neutral_completed);
        EXPECT_TRUE(grid_ready);

        sqlite3_close(db);
        std::error_code ec;
        std::filesystem::remove_all(temp_dir, ec);
    }

    TEST(WorkflowCoordinatorService, MaterializesReadyStepWithoutWorkerCoordinator) {
        using namespace savor::db::execution::workflow;
        using namespace savor::db::migrations;

        sqlite3* db = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &db));
        std::string err;
        ASSERT_TRUE(ApplyContextMigrations(db, MigrationContext::Execution, { .source_kind = MigrationSourceKind::Embedded }, &err)) << err;
        ASSERT_TRUE(ExecSql(db, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(6101, 'workflow_coordinator_test', 'RUNNING', 'manual', 'test', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, input_ref_kind, input_ref_id, attempts, max_attempts, created_at_utc, ready_at_utc)
VALUES(6102, 6101, 'Ready', 'unit.ready', 'READY', 'unit.input', 6103, 0, 1, unixepoch()*1000, unixepoch()*1000);
)SQL"));

        SqliteExecutionDb execution_db(db);
        auto registry = BuildWorkflowCoordinatorTestRegistry(&execution_db, true);
        WorkflowCoordinatorService service(&execution_db, &registry, FastWorkflowCoordinatorConfig());
        ASSERT_TRUE(service.Start(&err)) << err;
        EXPECT_TRUE(WaitForCondition([&]() {
            std::string state;
            std::int64_t job_count = 0;
            return QueryText(db, "SELECT state FROM exec_workflow_step WHERE workflow_step_id=6102;", &state)
                && state == "MATERIALIZED"
                && QueryInt64(db, "SELECT COUNT(1) FROM exec_job WHERE job_set_id=(SELECT job_set_id FROM exec_workflow_step WHERE workflow_step_id=6102);", &job_count)
                && job_count == 1;
        }));
        service.Stop();

        const auto telemetry = service.SnapshotTelemetry();
        EXPECT_GE(telemetry.materialization_count, 1);
        sqlite3_close(db);
    }

    TEST(WorkflowCoordinatorService, CountsDistinctActiveMaterializedWorkflows) {
        using namespace savor::db::execution::workflow;
        using namespace savor::db::migrations;

        sqlite3* db = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &db));
        std::string err;
        ASSERT_TRUE(ApplyContextMigrations(db, MigrationContext::Execution, { .source_kind = MigrationSourceKind::Embedded }, &err)) << err;
        ASSERT_TRUE(ExecSql(db, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES
    (6501, 'workflow_coordinator_test', 'RUNNING', 'manual', 'test', unixepoch()*1000, unixepoch()*1000),
    (6502, 'workflow_coordinator_test', 'PENDING', 'manual', 'test', unixepoch()*1000, NULL),
    (6503, 'workflow_coordinator_test', 'COMPLETED', 'manual', 'test', unixepoch()*1000, unixepoch()*1000),
    (6504, 'workflow_coordinator_test', 'RUNNING', 'manual', 'test', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, attempts, max_attempts, created_at_utc, started_at_utc)
VALUES
    (6511, 6501, 'ActiveA', 'unit.step', 'MATERIALIZED', 0, 1, unixepoch()*1000, unixepoch()*1000),
    (6512, 6501, 'ActiveB', 'unit.step', 'RUNNING', 0, 1, unixepoch()*1000, unixepoch()*1000),
    (6521, 6502, 'PendingActive', 'unit.step', 'RUNNING', 0, 1, unixepoch()*1000, unixepoch()*1000),
    (6531, 6503, 'CompletedIgnored', 'unit.step', 'RUNNING', 0, 1, unixepoch()*1000, unixepoch()*1000),
    (6541, 6504, 'ReadyIgnored', 'unit.step', 'READY', 0, 1, unixepoch()*1000, unixepoch()*1000);
)SQL"));

        SqliteExecutionDb execution_db(db);
        ASSERT_NE(execution_db.WorkflowQueryService(), nullptr);
        EXPECT_EQ(execution_db.WorkflowQueryService()->CountActiveMaterializedWorkflows(), 2);
        sqlite3_close(db);
    }

    TEST(WorkflowCoordinatorService, MaterializesReadyWorkRegardlessOfActiveWorkflowCount) {
        using namespace savor::db::execution::workflow;
        using namespace savor::db::migrations;

        sqlite3* db = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &db));
        std::string err;
        ASSERT_TRUE(ApplyContextMigrations(db, MigrationContext::Execution, { .source_kind = MigrationSourceKind::Embedded }, &err)) << err;
        for (int i = 0; i < 30; ++i) {
            ASSERT_TRUE(InsertActiveWorkflow(db, 6600 + i, 6700 + i));
        }
        ASSERT_TRUE(InsertReadyWorkflow(db, 6801, 6802, 6803));

        SqliteExecutionDb execution_db(db);
        auto registry = BuildWorkflowCoordinatorTestRegistry(&execution_db, true);
        auto config = FastWorkflowCoordinatorConfig();
        config.max_active_materialized_workflows = 30;
        WorkflowCoordinatorService service(&execution_db, &registry, config);
        ASSERT_TRUE(service.Start(&err)) << err;
        EXPECT_TRUE(WaitForCondition([&]() {
            std::string state;
            return QueryText(
                       db,
                       "SELECT state FROM exec_workflow_step WHERE "
                       "workflow_step_id=6802;",
                       &state)
                && state == "MATERIALIZED";
        }));
        service.Stop();

        std::string ready_state;
        std::int64_t created_job_sets = 0;
        ASSERT_TRUE(QueryText(db, "SELECT state FROM exec_workflow_step WHERE workflow_step_id=6802;", &ready_state));
        ASSERT_TRUE(QueryInt64(db, "SELECT COUNT(1) FROM exec_job_set WHERE created_by='WorkflowCoordinatorServiceTest';", &created_job_sets));
        EXPECT_EQ(ready_state, "MATERIALIZED");
        EXPECT_EQ(created_job_sets, 1);
        const auto telemetry = service.SnapshotTelemetry();
        EXPECT_GE(telemetry.active_materialized_workflow_count, 30);
        EXPECT_EQ(telemetry.materialization_throttle_count, 0);
        EXPECT_GE(telemetry.materialization_count, 1);
        sqlite3_close(db);
    }

    TEST(WorkflowCoordinatorService, MaterializesAllReadyWorkRegardlessOfActiveWorkflowCount) {
        using namespace savor::db::execution::workflow;
        using namespace savor::db::migrations;

        sqlite3* db = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &db));
        std::string err;
        ASSERT_TRUE(ApplyContextMigrations(db, MigrationContext::Execution, { .source_kind = MigrationSourceKind::Embedded }, &err)) << err;
        for (int i = 0; i < 29; ++i) {
            ASSERT_TRUE(InsertActiveWorkflow(db, 6900 + i, 7000 + i));
        }
        ASSERT_TRUE(InsertReadyWorkflow(db, 7101, 7102, 7103));
        ASSERT_TRUE(InsertReadyWorkflow(db, 7111, 7112, 7113));
        ASSERT_TRUE(InsertReadyWorkflow(db, 7121, 7122, 7123));

        SqliteExecutionDb execution_db(db);
        auto registry = BuildWorkflowCoordinatorTestRegistry(&execution_db, true);
        auto config = FastWorkflowCoordinatorConfig();
        config.max_active_materialized_workflows = 30;
        WorkflowCoordinatorService service(&execution_db, &registry, config);
        ASSERT_TRUE(service.Start(&err)) << err;
        EXPECT_TRUE(WaitForCondition([&]() {
            std::int64_t materialized_ready_steps = 0;
            return QueryInt64(db, "SELECT COUNT(1) FROM exec_workflow_step WHERE workflow_instance_id IN (7101,7111,7121) AND state='MATERIALIZED';", &materialized_ready_steps)
                && materialized_ready_steps == 3;
        }));
        service.Stop();

        std::int64_t materialized_ready_steps = 0;
        std::int64_t ready_steps = 0;
        std::int64_t created_job_sets = 0;
        ASSERT_TRUE(QueryInt64(db, "SELECT COUNT(1) FROM exec_workflow_step WHERE workflow_instance_id IN (7101,7111,7121) AND state='MATERIALIZED';", &materialized_ready_steps));
        ASSERT_TRUE(QueryInt64(db, "SELECT COUNT(1) FROM exec_workflow_step WHERE workflow_instance_id IN (7101,7111,7121) AND state='READY';", &ready_steps));
        ASSERT_TRUE(QueryInt64(db, "SELECT COUNT(1) FROM exec_job_set WHERE created_by='WorkflowCoordinatorServiceTest';", &created_job_sets));
        EXPECT_EQ(materialized_ready_steps, 3);
        EXPECT_EQ(ready_steps, 0);
        EXPECT_EQ(created_job_sets, 3);
        const auto telemetry = service.SnapshotTelemetry();
        EXPECT_EQ(telemetry.materialization_throttle_count, 0);
        EXPECT_GE(telemetry.materialization_count, 3);
        sqlite3_close(db);
    }

    TEST(WorkflowCoordinatorService, TerminalReconciliationRunsAlongsideActiveMaterializedWorkflows) {
        using namespace savor::db::execution::workflow;
        using namespace savor::db::migrations;

        sqlite3* db = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &db));
        std::string err;
        ASSERT_TRUE(ApplyContextMigrations(db, MigrationContext::Execution, { .source_kind = MigrationSourceKind::Embedded }, &err)) << err;
        for (int i = 0; i < 29; ++i) {
            ASSERT_TRUE(InsertActiveWorkflow(db, 7200 + i, 7300 + i));
        }
        ASSERT_TRUE(ExecSql(db, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(7401, 'workflow_coordinator_test', 'RUNNING', 'manual', 'test', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, expected_total, created_at_utc, materialization_state)
VALUES(7402, 1, 'workflow-test-empty', 0, unixepoch()*1000, 'WORKSET_PUBLICATION_COMPLETE');
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, attempts, max_attempts, created_at_utc, started_at_utc)
VALUES(7403, 7401, 'Current', 'unit.step', 'MATERIALIZED', 7402, 0, 1, unixepoch()*1000, unixepoch()*1000);
)SQL"));

        SqliteExecutionDb execution_db(db);
        auto registry = BuildWorkflowCoordinatorTestRegistry(&execution_db);
        auto config = FastWorkflowCoordinatorConfig();
        config.max_active_materialized_workflows = 30;
        WorkflowCoordinatorService service(&execution_db, &registry, config);
        ASSERT_TRUE(service.Start(&err)) << err;
        EXPECT_TRUE(WaitForCondition([&]() {
            std::string step_state;
            return QueryText(db, "SELECT state FROM exec_workflow_step WHERE workflow_step_id=7403;", &step_state)
                && step_state == "COMPLETED";
        }));
        service.Stop();

        const auto telemetry = service.SnapshotTelemetry();
        EXPECT_GE(telemetry.terminal_empty_step_count, 1);
        sqlite3_close(db);
    }

    TEST(WorkflowCoordinatorService, TerminalScanMarksSucceededMaterializedStepAndAdvancesGraph) {
        using namespace savor::db::execution::workflow;
        using namespace savor::db::migrations;

        sqlite3* db = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &db));
        std::string err;
        ASSERT_TRUE(ApplyContextMigrations(db, MigrationContext::Execution, { .source_kind = MigrationSourceKind::Embedded }, &err)) << err;
        ASSERT_TRUE(ExecSql(db, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(6201, 'workflow_coordinator_test', 'RUNNING', 'manual', 'test', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, expected_total, created_at_utc, materialization_state)
VALUES(6202, 1, 'workflow-test', 1, unixepoch()*1000, 'WORKSET_PUBLICATION_COMPLETE');
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc, ended_at_utc)
VALUES(6203, 6202, 1, 1, 'unit.input', 6203, 'workflow-terminal-success', 0, 'COMPLETED', 1, 1, unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, attempts, max_attempts, created_at_utc, started_at_utc)
VALUES(6204, 6201, 'Current', 'unit.step', 'MATERIALIZED', 6202, 0, 1, unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, attempts, max_attempts, created_at_utc)
VALUES(6205, 6201, 'Next', 'unit.ready', 'WAITING', 0, 1, unixepoch()*1000);
)SQL"));

        SqliteExecutionDb execution_db(db);
        auto registry = BuildWorkflowCoordinatorTestRegistry(&execution_db, true);
        WorkflowCoordinatorService service(&execution_db, &registry, FastWorkflowCoordinatorConfig());
        ASSERT_TRUE(service.Start(&err)) << err;
        EXPECT_TRUE(WaitForCondition([&]() {
            std::string current_state;
            std::string next_state;
            return QueryText(db, "SELECT state FROM exec_workflow_step WHERE workflow_step_id=6204;", &current_state)
                && QueryText(db, "SELECT state FROM exec_workflow_step WHERE workflow_step_id=6205;", &next_state)
                && current_state == "COMPLETED"
                && next_state == "READY";
        }));
        service.Stop();

        const auto telemetry = service.SnapshotTelemetry();
        EXPECT_GE(telemetry.terminal_completed_step_count, 1);
        EXPECT_GE(telemetry.transition_advanced_count, 1);
        sqlite3_close(db);
    }

    TEST(WorkflowCoordinatorService, TargetedTerminalNotificationWaitsForEveryDescendantJob) {
        using namespace savor::db::execution::workflow;
        using namespace savor::db::migrations;

        sqlite3* db = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &db));
        std::string err;
        ASSERT_TRUE(ApplyContextMigrations(
            db,
            MigrationContext::Execution,
            {.source_kind = MigrationSourceKind::Embedded},
            &err))
            << err;
        ASSERT_TRUE(ExecSql(db, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(7501, 'workflow_coordinator_test', 'RUNNING', 'manual', 'test', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, parent_job_set_id, program_kind, purpose, expected_total, created_at_utc, materialization_state)
VALUES
    (7502, NULL, 1, 'workflow-test-root', 2, unixepoch()*1000, 'WORKSET_PUBLICATION_COMPLETE'),
    (7504, 7502, 1, 'workflow-test-child', 1, unixepoch()*1000, 'WORKSET_PUBLICATION_COMPLETE');
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc, ended_at_utc)
VALUES
    (7503, 7502, 1, 1, 'unit.input', 7503, 'workflow-targeted-terminal', 0, 'COMPLETED', 1, 1, unixepoch()*1000, unixepoch()*1000),
    (7507, 7502, 1, 1, 'unit.input', 7507, 'workflow-root-still-running', 0, 'QUEUED', 0, 1, unixepoch()*1000, NULL),
    (7505, 7504, 1, 1, 'unit.input', 7505, 'workflow-child-still-running', 0, 'QUEUED', 0, 1, unixepoch()*1000, NULL);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, attempts, max_attempts, created_at_utc, started_at_utc)
VALUES(7506, 7501, 'Current', 'unit.step', 'MATERIALIZED', 7502, 0, 1, unixepoch()*1000, unixepoch()*1000);
)SQL"));

        SqliteExecutionDb execution_db(db);
        auto* queries = execution_db.WorkflowQueryService();
        ASSERT_NE(queries, nullptr);
        EXPECT_FALSE(
            queries->GetStepTerminalSnapshotForJob(7503)
                .has_value());

        auto continuation_count =
            std::make_shared<std::atomic<int>>(0);
        auto registry = BuildWorkflowCoordinatorTestRegistry(
            &execution_db,
            false,
            continuation_count);
        auto config = FastWorkflowCoordinatorConfig();
        config.terminal_repair_interval =
            std::chrono::hours(1);
        WorkflowCoordinatorService service(
            &execution_db,
            &registry,
            config);
        ASSERT_TRUE(service.Start(&err)) << err;
        ASSERT_TRUE(WaitForCondition([&]() {
            return service.SnapshotTelemetry()
                       .ready_scan_count
                >= 1;
        }));
        const auto ready_scans_before_notification =
            service.SnapshotTelemetry().ready_scan_count;

        ASSERT_TRUE(service.PublishTerminalCommit({
            .commit_sequence = 1,
            .workflow_step_id = 7506,
            .job_id = 7503,
        }));
        ASSERT_TRUE(WaitForCondition([&]() {
            return service.SnapshotTelemetry()
                       .ready_scan_count
                > ready_scans_before_notification;
        }));
        EXPECT_EQ(
            continuation_count->load(
                std::memory_order_relaxed),
            0);
        std::string step_state;
        ASSERT_TRUE(QueryText(
            db,
            "SELECT state FROM exec_workflow_step WHERE "
            "workflow_step_id=7506;",
            &step_state));
        EXPECT_EQ(step_state, "MATERIALIZED");

        ASSERT_TRUE(ExecSql(db, R"SQL(
UPDATE exec_job
SET state='COMPLETED', attempts=1, ended_at_utc=unixepoch()*1000
WHERE job_id IN (7505,7507);
)SQL"));
        ASSERT_TRUE(
            queries->GetStepTerminalSnapshotForJob(7507)
                .has_value());
        ASSERT_TRUE(service.PublishTerminalCommit({
            .commit_sequence = 2,
            .workflow_step_id = 7506,
            .job_id = 7507,
        }));
        EXPECT_TRUE(WaitForCondition([&]() {
            std::string current_state;
            return continuation_count->load(
                       std::memory_order_relaxed)
                    == 1
                && QueryText(
                    db,
                    "SELECT state FROM exec_workflow_step WHERE "
                    "workflow_step_id=7506;",
                    &current_state)
                && current_state == "COMPLETED";
        }));
        service.Stop();

        EXPECT_EQ(
            continuation_count->load(
                std::memory_order_relaxed),
            1);
        const auto telemetry = service.SnapshotTelemetry();
        EXPECT_EQ(
            telemetry.targeted_terminal_notification_count,
            2);
        EXPECT_EQ(
            telemetry.targeted_terminal_advancement_count,
            1);
        sqlite3_close(db);
    }

    TEST(WorkflowCoordinatorService, ContinuationFailureCommitsStepAndWorkflowTogether) {
        using namespace savor::db::execution::programdb;
        using namespace savor::db::execution::workflow;
        using namespace savor::db::migrations;

        sqlite3* db = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &db));
        std::string err;
        ASSERT_TRUE(ApplyContextMigrations(
            db,
            MigrationContext::Execution,
            {.source_kind = MigrationSourceKind::Embedded},
            &err))
            << err;
        ASSERT_TRUE(ExecSql(db, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES
    (7601, 'workflow_coordinator_test', 'RUNNING', 'manual', 'test', unixepoch()*1000, unixepoch()*1000),
    (7611, 'workflow_coordinator_test', 'RUNNING', 'manual', 'test', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, expected_total, created_at_utc, materialization_state)
VALUES
    (7602, 1, 'workflow-test', 1, unixepoch()*1000, 'WORKSET_PUBLICATION_COMPLETE'),
    (7612, 1, 'workflow-test', 1, unixepoch()*1000, 'WORKSET_PUBLICATION_COMPLETE');
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc, ended_at_utc)
VALUES
    (7603, 7602, 1, 1, 'unit.input', 7603, 'workflow-continuation-failed', 0, 'COMPLETED', 1, 1, unixepoch()*1000, unixepoch()*1000),
    (7613, 7612, 1, 1, 'unit.input', 7613, 'workflow-continuation-rollback', 0, 'COMPLETED', 1, 1, unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, attempts, max_attempts, created_at_utc, started_at_utc)
VALUES
    (7604, 7601, 'Current', 'unit.step', 'MATERIALIZED', 7602, 0, 1, unixepoch()*1000, unixepoch()*1000),
    (7614, 7611, 'Current', 'unit.step', 'MATERIALIZED', 7612, 0, 1, unixepoch()*1000, unixepoch()*1000);
CREATE TRIGGER reject_test_workflow_failure
BEFORE UPDATE OF state ON exec_workflow_instance
WHEN OLD.workflow_instance_id=7611 AND NEW.state='FAILED'
BEGIN
    SELECT RAISE(ABORT, 'injected workflow failure');
END;
)SQL"));

        SqliteExecutionDb execution_db(db);
        auto continuation_count =
            std::make_shared<std::atomic<int>>(0);
        auto registry = BuildWorkflowCoordinatorTestRegistry(
            &execution_db,
            false,
            continuation_count,
            ProgramJobContinuationDisposition::Failed);
        WorkflowCoordinatorService service(
            &execution_db,
            &registry,
            FastWorkflowCoordinatorConfig());
        ASSERT_TRUE(service.Start(&err)) << err;
        EXPECT_TRUE(WaitForCondition([&]() {
            std::string committed_step_state;
            std::string committed_workflow_state;
            std::int64_t rollback_failure_events = 0;
            return QueryText(
                       db,
                       "SELECT state FROM exec_workflow_step WHERE "
                       "workflow_step_id=7604;",
                       &committed_step_state)
                && QueryText(
                    db,
                    "SELECT state FROM exec_workflow_instance WHERE "
                    "workflow_instance_id=7601;",
                    &committed_workflow_state)
                && QueryInt64(
                    db,
                    "SELECT COUNT(1) FROM exec_workflow_event WHERE "
                    "workflow_step_id=7614 AND "
                    "event_kind='Execution.WorkflowStepCoordinatorFailure.v1';",
                    &rollback_failure_events)
                && committed_step_state == "FAILED"
                && committed_workflow_state == "FAILED"
                && rollback_failure_events >= 1;
        }));
        service.Stop();

        std::string committed_failure_code;
        std::string rolled_back_step_state;
        std::string rolled_back_workflow_state;
        std::int64_t committed_step_failure_events = 0;
        std::int64_t rolled_back_step_failure_events = 0;
        ASSERT_TRUE(QueryText(
            db,
            "SELECT failure_code FROM exec_workflow_instance WHERE "
            "workflow_instance_id=7601;",
            &committed_failure_code));
        ASSERT_TRUE(QueryText(
            db,
            "SELECT state FROM exec_workflow_step WHERE "
            "workflow_step_id=7614;",
            &rolled_back_step_state));
        ASSERT_TRUE(QueryText(
            db,
            "SELECT state FROM exec_workflow_instance WHERE "
            "workflow_instance_id=7611;",
            &rolled_back_workflow_state));
        ASSERT_TRUE(QueryInt64(
            db,
            "SELECT COUNT(1) FROM exec_workflow_event WHERE "
            "workflow_step_id=7604 AND "
            "event_kind='Execution.WorkflowStepFailed.v1';",
            &committed_step_failure_events));
        ASSERT_TRUE(QueryInt64(
            db,
            "SELECT COUNT(1) FROM exec_workflow_event WHERE "
            "workflow_step_id=7614 AND "
            "event_kind='Execution.WorkflowStepFailed.v1';",
            &rolled_back_step_failure_events));

        EXPECT_EQ(committed_failure_code, "TEST_CONTINUATION_FAILED");
        EXPECT_EQ(rolled_back_step_state, "MATERIALIZED");
        EXPECT_EQ(rolled_back_workflow_state, "RUNNING");
        EXPECT_EQ(committed_step_failure_events, 1);
        EXPECT_EQ(rolled_back_step_failure_events, 0);
        EXPECT_GE(
            continuation_count->load(std::memory_order_relaxed),
            2);
        EXPECT_GE(
            service.SnapshotTelemetry().continuation_failure_count,
            1);
        sqlite3_close(db);
    }

    TEST(WorkflowCoordinatorService, TerminalScanMarksFailedWhenAnyDescendantJobFails) {
        using namespace savor::db::execution::workflow;
        using namespace savor::db::migrations;

        sqlite3* db = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &db));
        std::string err;
        ASSERT_TRUE(ApplyContextMigrations(db, MigrationContext::Execution, { .source_kind = MigrationSourceKind::Embedded }, &err)) << err;
        ASSERT_TRUE(ExecSql(db, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(6301, 'workflow_coordinator_test', 'RUNNING', 'manual', 'test', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, expected_total, created_at_utc, materialization_state)
VALUES(6302, 1, 'workflow-test', 2, unixepoch()*1000, 'WORKSET_PUBLICATION_COMPLETE');
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc, ended_at_utc)
VALUES
    (6303, 6302, 1, 1, 'unit.input', 6303, 'workflow-terminal-failed-1', 0, 'COMPLETED', 1, 1, unixepoch()*1000, unixepoch()*1000),
    (6304, 6302, 1, 1, 'unit.input', 6304, 'workflow-terminal-failed-2', 0, 'FAILED', 1, 1, unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, attempts, max_attempts, created_at_utc, started_at_utc)
VALUES(6305, 6301, 'Current', 'unit.step', 'MATERIALIZED', 6302, 0, 1, unixepoch()*1000, unixepoch()*1000);
)SQL"));

        SqliteExecutionDb execution_db(db);
        auto registry = BuildWorkflowCoordinatorTestRegistry(&execution_db);
        WorkflowCoordinatorService service(&execution_db, &registry, FastWorkflowCoordinatorConfig());
        ASSERT_TRUE(service.Start(&err)) << err;
        EXPECT_TRUE(WaitForCondition([&]() {
            std::string step_state;
            std::string workflow_state;
            return QueryText(db, "SELECT state FROM exec_workflow_step WHERE workflow_step_id=6305;", &step_state)
                && QueryText(db, "SELECT state FROM exec_workflow_instance WHERE workflow_instance_id=6301;", &workflow_state)
                && step_state == "FAILED"
                && workflow_state == "FAILED";
        }));
        service.Stop();

        const auto telemetry = service.SnapshotTelemetry();
        EXPECT_GE(telemetry.terminal_failed_step_count, 1);
        EXPECT_GE(telemetry.workflow_failed_count, 1);
        sqlite3_close(db);
    }

    TEST(WorkflowCoordinatorService, TerminalScanMarksMaterializedEmptyJobSetCompletedWithEmptyEvent) {
        using namespace savor::db::execution::workflow;
        using namespace savor::db::migrations;

        sqlite3* db = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &db));
        std::string err;
        ASSERT_TRUE(ApplyContextMigrations(db, MigrationContext::Execution, { .source_kind = MigrationSourceKind::Embedded }, &err)) << err;
        ASSERT_TRUE(ExecSql(db, R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(6401, 'workflow_coordinator_test', 'RUNNING', 'manual', 'test', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, expected_total, created_at_utc, materialization_state)
VALUES(6402, 1, 'workflow-test-empty', 0, unixepoch()*1000, 'WORKSET_PUBLICATION_COMPLETE');
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, attempts, max_attempts, created_at_utc, started_at_utc)
VALUES(6403, 6401, 'Current', 'unit.step', 'MATERIALIZED', 6402, 0, 1, unixepoch()*1000, unixepoch()*1000);
)SQL"));

        SqliteExecutionDb execution_db(db);
        auto registry = BuildWorkflowCoordinatorTestRegistry(&execution_db);
        const auto snapshots = execution_db.WorkflowQueryService()->ListTerminalReadyStepSnapshots(16);
        ASSERT_EQ(snapshots.size(), 1u);
        EXPECT_EQ(snapshots.front().workflow_step_id, 6403);
        EXPECT_EQ(snapshots.front().discovered_total, 0);
        WorkflowCoordinatorService service(&execution_db, &registry, FastWorkflowCoordinatorConfig());
        ASSERT_TRUE(service.Start(&err)) << err;
        const bool completed_empty_step = WaitForCondition([&]() {
            std::string step_state;
            std::int64_t empty_events = 0;
            return QueryText(db, "SELECT state FROM exec_workflow_step WHERE workflow_step_id=6403;", &step_state)
                && QueryInt64(db, "SELECT COUNT(1) FROM exec_workflow_event WHERE workflow_step_id=6403 AND event_kind='Execution.WorkflowStepEmpty.v1';", &empty_events)
                && step_state == "COMPLETED"
                && empty_events == 1;
        });
        service.Stop();

        std::string failure_message;
        std::string final_step_state;
        std::int64_t final_empty_events = 0;
        (void)QueryText(db, "SELECT state FROM exec_workflow_step WHERE workflow_step_id=6403;", &final_step_state);
        (void)QueryInt64(db,
            "SELECT COUNT(1) FROM exec_workflow_event WHERE workflow_step_id=6403 AND event_kind='Execution.WorkflowStepEmpty.v1';",
            &final_empty_events);
        (void)QueryText(db,
            "SELECT COALESCE(message, '') FROM exec_workflow_event "
            "WHERE workflow_step_id=6403 AND event_kind='Execution.WorkflowStepCoordinatorFailure.v1' "
            "ORDER BY workflow_event_id DESC LIMIT 1;",
            &failure_message);

        const auto telemetry = service.SnapshotTelemetry();
        EXPECT_TRUE(completed_empty_step)
            << "state=" << final_step_state
            << " empty_events=" << final_empty_events
            << " terminal_scans=" << telemetry.terminal_scan_count
            << " terminal_steps=" << telemetry.terminal_step_count
            << " failure=" << failure_message;
        EXPECT_GE(telemetry.terminal_empty_step_count, 1);
        sqlite3_close(db);
    }

    TEST(Stage3Phase3Telemetry, CapturesDispatchCountersWithoutWorkflowMaterialization) {
        using namespace savor::runner::parallel::savordb;
        using namespace savor::db::execution::workflow;

        DBWorkflowWorkerCoordinatorConfig cfg{
            .desired_workers = 1,
            .controller_sleep_ms = 1,
        };
        ConfigureWorksetTestGate(cfg);
        DBWorkflowWorkerCoordinator coordinator(
            nullptr,
            std::move(cfg),
            CoordinatorIntegrationConfig{},
            [](const WorkflowReadyStep& step) {
                return ScheduledJobSet{
                    .job_set_id = 13000 + step.workflow_step_id,
                    .workflow_step_id = step.workflow_step_id,
                };
            });

        coordinator.Start();
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        coordinator.Stop();

        const auto telemetry = coordinator.SnapshotTelemetry();
        EXPECT_EQ(telemetry.materialization_count, 0);
        EXPECT_GE(telemetry.dispatch_attempt_count, 0);
        EXPECT_GE(telemetry.dispatch_success_count, 0);
        EXPECT_GE(telemetry.dispatch_miss_count, 0);
        EXPECT_GE(telemetry.dispatch_miss_rate_basis_points, 0);
    }

    TEST(ProcessWorkerShutdown, StopIsIdempotentWithoutStartedChild) {
        savor::ProcessWorker worker;

        worker.stop();
        const auto first = worker.last_stop_snapshot();
        EXPECT_FALSE(first.already_stopping);
        EXPECT_FALSE(first.was_running);
        EXPECT_FALSE(first.stdin_close_attempted);
        EXPECT_FALSE(first.termination_attempted);
        EXPECT_FALSE(first.cancel_pipe_attempted);
        EXPECT_FALSE(first.cancel_reader_attempted);
        EXPECT_FALSE(first.reader_joined);

        worker.stop();
        const auto second = worker.last_stop_snapshot();
        EXPECT_TRUE(second.already_stopping);
    }

    TEST(Stage3Phase3Shutdown, StopIsIdempotentAndEmitsOneCompletePhase) {
        using namespace savor::runner::parallel::savordb;
        using namespace savor::db::execution::workflow;

        DBWorkflowWorkerCoordinatorConfig cfg{
            .desired_workers = 1,
            .controller_sleep_ms = 1,
        };
        ConfigureWorksetTestGate(cfg);
        DBWorkflowWorkerCoordinator coordinator(
            nullptr,
            std::move(cfg),
            CoordinatorIntegrationConfig{},
            [](const WorkflowReadyStep& step) {
                return ScheduledJobSet{
                    .job_set_id = 17000 + step.workflow_step_id,
                    .workflow_step_id = step.workflow_step_id,
                };
            });

        std::mutex event_mtx;
        std::vector<std::string> events;
        coordinator.SetResultMapEventCallback([&](const std::string& line) {
            std::lock_guard<std::mutex> lock(event_mtx);
            events.push_back(line);
        });

        const auto start = coordinator.Start();
        ASSERT_TRUE(start) << start.error;
        coordinator.Stop();
        coordinator.Stop();

        std::lock_guard<std::mutex> lock(event_mtx);
        const auto complete_count = std::count_if(events.begin(), events.end(), [](const std::string& line) {
            return line.find("[workflow-coordinator-shutdown] phase=stop_complete") != std::string::npos;
        });
        EXPECT_EQ(complete_count, 1);
    }

    TEST(Stage3Phase3Shutdown, StopDrainsQueuedResultBeforeClosingResultQueue) {
        using namespace savor::runner::parallel::savordb;
        using namespace savor::db::execution::workflow;

        DBWorkflowWorkerCoordinatorConfig cfg{
            .desired_workers = 1,
            .controller_sleep_ms = 1,
        };
        ConfigureWorksetTestGate(cfg);
        DBWorkflowWorkerCoordinator coordinator(
            nullptr,
            std::move(cfg),
            CoordinatorIntegrationConfig{},
            [](const WorkflowReadyStep& step) {
                return ScheduledJobSet{
                    .job_set_id = 17100 + step.workflow_step_id,
                    .workflow_step_id = step.workflow_step_id,
                };
            });

        std::atomic<int> results_seen{ 0 };
        coordinator.SetResultCallback([&](const savor::PRResult&) {
            ++results_seen;
        });

        const auto start = coordinator.Start();
        ASSERT_TRUE(start) << start.error;
        coordinator.EnqueueResultForTest(savor::PRResult{
            .job_id = 777,
            .epoch = 1,
            .worker_id = 0,
            .accepted = true,
        });
        coordinator.Stop();

        EXPECT_EQ(results_seen.load(), 1);
        EXPECT_GE(coordinator.SnapshotTelemetry().results_received_count, 1);
    }

    TEST(Stage3Phase3Shutdown, StopCompletesAfterStartupInProgressIsReleased) {
        using namespace savor::runner::parallel::savordb;
        using namespace savor::db::execution::workflow;

        auto startup_entered = std::make_shared<std::promise<void>>();
        auto startup_release = std::make_shared<std::promise<void>>();
        auto release_future = startup_release->get_future().share();
        auto startup_signalled =
            std::make_shared<std::atomic<bool>>(false);

        DBWorkflowWorkerCoordinatorConfig cfg{
            .desired_workers = 2,
            .controller_sleep_ms = 1,
            .max_concurrent_worker_starts = 1,
        };
        ConfigureWorksetTestGate(cfg);
        cfg.worker_capability_preflight =
            [startup_entered,
             release_future,
             startup_signalled](
            size_t worker_id,
            const DBWorkflowWorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>&) mutable {
            if (worker_id != 0) {
                if (!startup_signalled->exchange(true)) {
                    startup_entered->set_value();
                }
                release_future.wait();
                return CoordinatorWorkerCapabilityPreflightResult{
                    .error = "released by shutdown test"};
            }
            return CoordinatorWorkerCapabilityPreflightResult{
                .process_ready = true,
                .capabilities = savor::runtime::AddCapability(
                    savor::runtime::kSlice1ProductionCapabilities,
                    savor::runtime::WorkerCapability::
                        WorksetDispatch),
                .runtime_manifest =
                    CompleteWorksetTestManifest(),
            };
        };

        DBWorkflowWorkerCoordinator coordinator(
            nullptr,
            std::move(cfg),
            CoordinatorIntegrationConfig{},
            [](const WorkflowReadyStep& step) {
                return ScheduledJobSet{
                    .job_set_id = 17200 + step.workflow_step_id,
                    .workflow_step_id = step.workflow_step_id,
                };
            });

        const auto start = coordinator.Start();
        ASSERT_TRUE(start) << start.error;
        ASSERT_EQ(
            startup_entered->get_future().wait_for(
                std::chrono::milliseconds(1000)),
            std::future_status::ready);

        auto stop_future = std::async(std::launch::async, [&]() {
            coordinator.Stop();
        });
        EXPECT_NE(stop_future.wait_for(std::chrono::milliseconds(30)), std::future_status::ready);
        startup_release->set_value();
        EXPECT_EQ(stop_future.wait_for(std::chrono::milliseconds(2000)), std::future_status::ready);
        coordinator.Stop();
    }

    TEST(Stage3Phase3Shutdown, WorkerStopPhasePrecedesCoordinatorThreadJoins) {
        using namespace savor::runner::parallel::savordb;
        using namespace savor::db::execution::workflow;

        DBWorkflowWorkerCoordinatorConfig cfg{
            .desired_workers = 1,
            .controller_sleep_ms = 1,
        };
        ConfigureWorksetTestGate(cfg);
        DBWorkflowWorkerCoordinator coordinator(
            nullptr,
            std::move(cfg),
            CoordinatorIntegrationConfig{},
            [](const WorkflowReadyStep& step) {
                return ScheduledJobSet{
                    .job_set_id = 17300 + step.workflow_step_id,
                    .workflow_step_id = step.workflow_step_id,
                };
            });

        std::mutex event_mtx;
        std::vector<std::string> events;
        coordinator.SetResultMapEventCallback([&](const std::string& line) {
            std::lock_guard<std::mutex> lock(event_mtx);
            events.push_back(line);
        });

        const auto start = coordinator.Start();
        ASSERT_TRUE(start) << start.error;
        ASSERT_TRUE(WaitForCondition([&]() {
            return !coordinator.SnapshotWorkers().empty();
        }, std::chrono::milliseconds{ 1000 }));
        coordinator.Stop();

        std::lock_guard<std::mutex> lock(event_mtx);
        const auto worker_stop_it = std::find_if(events.begin(), events.end(), [](const std::string& line) {
            return line.find("phase=worker_stop_begin") != std::string::npos;
        });
        const auto join_it = std::find_if(events.begin(), events.end(), [](const std::string& line) {
            return line.find("phase=join_begin thread=worker_job") != std::string::npos;
        });
        const auto worker_stop_end_it = std::find_if(events.begin(), events.end(), [](const std::string& line) {
            return line.find("phase=worker_stop_end") != std::string::npos;
        });
        ASSERT_NE(worker_stop_it, events.end());
        ASSERT_NE(join_it, events.end());
        ASSERT_NE(worker_stop_end_it, events.end());
        EXPECT_LT(std::distance(events.begin(), worker_stop_it), std::distance(events.begin(), join_it));
        EXPECT_NE(worker_stop_end_it->find("terminate="), std::string::npos);
        EXPECT_NE(worker_stop_end_it->find("cancel_pipe="), std::string::npos);
        EXPECT_NE(worker_stop_end_it->find("cancel_reader="), std::string::npos);
        EXPECT_NE(worker_stop_end_it->find("reader_joined="), std::string::npos);
    }

    TEST(Stage3Phase3ClaimAccounting, DistinguishesCleanZeroClaimFromClaimErrorAndPartialClaim) {
        using namespace savor::runner::parallel::savordb;

        class ClaimAccountingExecutionDb final : public RecordingExecutionDb {
        public:
            std::vector<savor::db::ClaimedExecutionJob> claims;
            std::string error;

            std::vector<savor::db::ClaimedExecutionJob> ClaimBatchReadyExecutionJobs(
                std::string_view,
                int,
                std::int64_t,
                std::string* error_out = nullptr) override {
                if (error_out != nullptr) {
                    *error_out = error;
                }
                return claims;
            }
        };

        savor::db::execution::programdb::ProgramKindRegistry registry;
        const auto now = std::chrono::steady_clock::now();

        ClaimAccountingExecutionDb clean_zero_db;
        JobMaterializationService clean_zero_materializer(&clean_zero_db, &registry);
        const auto clean_zero = clean_zero_materializer.ClaimJobsDetailed(3, now);
        EXPECT_TRUE(clean_zero.attempted);
        EXPECT_EQ(clean_zero.requested, 3u);
        EXPECT_EQ(clean_zero.claimed, 0u);
        EXPECT_FALSE(clean_zero.error);
        EXPECT_TRUE(clean_zero.error_message.empty());
        EXPECT_EQ(clean_zero_materializer.CountMaterializedJobs(), 0u);

        ClaimAccountingExecutionDb error_db;
        error_db.error = "synthetic claim failure";
        JobMaterializationService error_materializer(&error_db, &registry);
        const auto failed_claim = error_materializer.ClaimJobsDetailed(3, now);
        EXPECT_TRUE(failed_claim.attempted);
        EXPECT_EQ(failed_claim.claimed, 0u);
        EXPECT_TRUE(failed_claim.error);
        EXPECT_EQ(failed_claim.error_message, "synthetic claim failure");

        ClaimAccountingExecutionDb partial_db;
        partial_db.claims = {
            savor::db::ClaimedExecutionJob{
                .job_id = 11,
                .job_set_id = 10,
                .workflow_instance_id = 1,
                .workflow_step_id = 101,
                .workflow_step_key = "step-a",
                .workflow_step_kind = "test.step",
                .workflow_step_priority = 1,
            },
            savor::db::ClaimedExecutionJob{
                .job_id = 12,
                .job_set_id = 10,
                .workflow_instance_id = 1,
                .workflow_step_id = 101,
                .workflow_step_key = "step-a",
                .workflow_step_kind = "test.step",
                .workflow_step_priority = 1,
            },
        };
        JobMaterializationService partial_materializer(&partial_db, &registry);
        const auto partial_claim = partial_materializer.ClaimJobsDetailed(3, now);
        EXPECT_TRUE(partial_claim.attempted);
        EXPECT_EQ(partial_claim.requested, 3u);
        EXPECT_EQ(partial_claim.claimed, 2u);
        EXPECT_FALSE(partial_claim.error);
    }

    TEST(Stage3Phase3PerfReport, WritesWorkerCoordinatorMetricsToSummaryAndMarkdown) {
        namespace perf = savor::db::perf;

        const auto report_dir = std::filesystem::temp_directory_path()
            / ("savor-worker-coordinator-report-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(report_dir);

        perf::PerfRunReport report{};
        report.scenario = "e2e-replay";
        report.measured_workload = "synthetic";
        report.worker_coordinator.available = true;
        report.worker_coordinator.dispatch_attempts = 10;
        report.worker_coordinator.dispatch_successes = 9;
        report.worker_coordinator.dispatch_misses = 1;
        report.worker_coordinator.dispatch_miss_rate_basis_points = 1000;
        report.worker_coordinator.worker_dispatch_min = 4;
        report.worker_coordinator.worker_dispatch_max = 5;
        report.worker_coordinator.worker_dispatch_avg = 4.5;
        report.worker_coordinator.active_samples = 3;
        report.worker_coordinator.avg_running_workers = 1.5;
        report.worker_coordinator.avg_idle_workers = 0.5;
        report.worker_coordinator.enough_work_samples = 2;
        report.worker_coordinator.enough_work_full_utilization_samples = 1;
        report.worker_coordinator.enough_work_full_utilization_pct = 50.0;
        report.worker_coordinator.claim_target = 2;
        report.worker_coordinator.claim_attempts = 4;
        report.worker_coordinator.claimed_jobs = 9;
        report.worker_coordinator.clean_zero_claims = 1;
        report.worker_coordinator.partial_claims = 1;
        report.worker_coordinator.max_program_kind_switches = 4;
        report.worker_coordinator.workers_over_program_kind_switch_limit = 1;
        report.worker_coordinator.workers_over_program_kind_switch_limit_ids = { 7 };
        report.worker_coordinator.progress_batches = 8;
        report.worker_coordinator.max_progress_batch_size = 3;
        report.worker_coordinator.results_received = 9;
        report.worker_coordinator.workers = {
            perf::WorkerCoordinatorWorkerMetric{
                .worker_id = 7,
                .dispatch_success_count = 5,
                .program_kind_switch_count = 4,
            },
        };

        perf::WriteSummaryJson(report_dir, report);
        perf::WriteMarkdownReport(report_dir, report);

        const auto summary = [&] {
            std::ifstream in(report_dir / "perf-summary.json", std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }();
        const auto markdown = [&] {
            std::ifstream in(report_dir / "perf-report.md", std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }();

        EXPECT_NE(summary.find("\"worker_coordinator\""), std::string::npos);
        EXPECT_NE(summary.find("\"workers_over_3\":1"), std::string::npos);
        EXPECT_NE(summary.find("\"worker_enough_work_full_utilization_pct\": 50"), std::string::npos);
        EXPECT_NE(markdown.find("## Worker Coordinator Telemetry"), std::string::npos);
        EXPECT_NE(markdown.find("| Claim pool | Claim attempts | 4 |"), std::string::npos);
        EXPECT_NE(markdown.find("| Program kind | Workers over 3 switches | 1 |"), std::string::npos);

        std::error_code ec;
        std::filesystem::remove_all(report_dir, ec);
    }

    TEST(WorkerStatusRegistry, ConcurrentMutationsProduceConsistentSnapshots) {
        WorkerStatusRegistry registry;
        constexpr int kWorkerCount = 4;
        constexpr int kIterations = 200;
        for (int worker = 0; worker < kWorkerCount; ++worker) {
            registry.RegisterWorker(worker, "localhost", 1000 + worker, "test");
        }

        std::atomic<bool> stop_snapshots{ false };
        std::thread snapshot_thread([&]() {
            while (!stop_snapshots.load()) {
                const auto snapshot = registry.GetClusterSnapshot();
                EXPECT_LE(snapshot.size(), static_cast<std::size_t>(kWorkerCount));
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

        std::vector<std::thread> workers;
        for (int worker = 0; worker < kWorkerCount; ++worker) {
            workers.emplace_back([&, worker]() {
                for (int i = 0; i < kIterations; ++i) {
                    registry.UpdateState(worker, (i % 2) == 0 ? WorkerStateKind::Running : WorkerStateKind::Idle);
                    registry.SetCurrentJob(worker, 10000 + worker, worker);
                    registry.RecordProgress(worker, "progress-" + std::to_string(i), 10000 + worker);
                    registry.RecordHeartbeat(worker);
                    registry.RecordDbSuccess(worker);
                }
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
        stop_snapshots.store(true);
        snapshot_thread.join();

        const auto snapshot = registry.GetClusterSnapshot();
        ASSERT_EQ(snapshot.size(), static_cast<std::size_t>(kWorkerCount));
        for (const auto& worker : snapshot) {
            EXPECT_TRUE(worker.job_id.has_value());
            EXPECT_FALSE(worker.last_progress.empty());
            EXPECT_GT(worker.last_heartbeat_mono_ns, 0);
        }
    }

    TEST(Stage3Phase3Batching, ProgressSnapshotIsNotBlockedBySlowWorkerStartup) {
        using namespace savor::runner::parallel::savordb;
        using namespace savor::db::execution::workflow;

        auto startup_entered = std::make_shared<std::promise<void>>();
        auto startup_release = std::make_shared<std::promise<void>>();
        auto release_future = startup_release->get_future().share();
        auto startup_signalled =
            std::make_shared<std::atomic<bool>>(false);

        DBWorkflowWorkerCoordinatorConfig cfg{
            .desired_workers = 2,
            .controller_sleep_ms = 1,
            .max_concurrent_worker_starts = 1,
        };
        ConfigureWorksetTestGate(cfg);
        cfg.worker_capability_preflight =
            [startup_entered,
             release_future,
             startup_signalled](
            size_t worker_id,
            const DBWorkflowWorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>&) mutable {
            if (worker_id != 0) {
                if (!startup_signalled->exchange(true)) {
                    startup_entered->set_value();
                }
                release_future.wait();
                return CoordinatorWorkerCapabilityPreflightResult{
                    .error = "blocked secondary startup test"};
            }
            return CoordinatorWorkerCapabilityPreflightResult{
                .process_ready = true,
                .capabilities = savor::runtime::AddCapability(
                    savor::runtime::kSlice1ProductionCapabilities,
                    savor::runtime::WorkerCapability::
                        WorksetDispatch),
                .runtime_manifest =
                    CompleteWorksetTestManifest(),
            };
        };

        DBWorkflowWorkerCoordinator coordinator(
            nullptr,
            std::move(cfg),
            CoordinatorIntegrationConfig{},
            [](const WorkflowReadyStep& step) {
                return ScheduledJobSet{
                    .job_set_id = 16000 + step.workflow_step_id,
                    .workflow_step_id = step.workflow_step_id,
                };
            });

        const auto start = coordinator.Start();
        ASSERT_TRUE(start) << start.error;
        const auto secondary_start =
            startup_entered->get_future().wait_for(
                std::chrono::milliseconds(1000));
        if (secondary_start != std::future_status::ready) {
            startup_release->set_value();
            coordinator.Stop();
            FAIL() << "Secondary worker startup did not begin";
        }

        coordinator.EnqueueProgressForTest(savor::PRProgress{
            .worker_id = 1,
            .job_id = 77,
            .text = "progress while worker 0 startup is blocked",
        });

        EXPECT_TRUE(WaitForCondition([&]() {
            const auto snapshot = coordinator.SnapshotWorkers();
            const auto it = std::find_if(snapshot.begin(), snapshot.end(), [](const WorkerSnapshot& worker) {
                return worker.worker_id == 1;
            });
            return it != snapshot.end()
                && it->last_progress == "progress while worker 0 startup is blocked";
        }, std::chrono::milliseconds{ 1000 }));

        startup_release->set_value();
        coordinator.Stop();
    }

    TEST(Stage3Phase3Batching, ProgressCallbacksDrainInBatchesAndTerminalResultsAreNotBlocked) {
        using namespace savor::runner::parallel::savordb;
        using namespace savor::db::execution::workflow;

        DBWorkflowWorkerCoordinatorConfig cfg{
            .desired_workers = 1,
            .controller_sleep_ms = 1,
        };
        ConfigureWorksetTestGate(cfg);
        DBWorkflowWorkerCoordinator coordinator(
            nullptr,
            std::move(cfg),
            CoordinatorIntegrationConfig{},
            [](const WorkflowReadyStep& step) {
                return ScheduledJobSet{
                    .job_set_id = 14000 + step.workflow_step_id,
                    .workflow_step_id = step.workflow_step_id,
                };
            });

        std::atomic<int> progress_seen{ 0 };
        std::promise<void> terminal_seen_promise;
        auto terminal_seen_future = terminal_seen_promise.get_future();
        coordinator.SetProgressCallback([&](const savor::PRProgress&) {
            ++progress_seen;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            });
        coordinator.SetResultCallback([&](const savor::PRResult&) {
            terminal_seen_promise.set_value();
            });

        const auto start = coordinator.Start();
        ASSERT_TRUE(start) << start.error;
        for (int i = 0; i < 200; ++i) {
            coordinator.EnqueueProgressForTest(savor::PRProgress{
                .worker_id = 0,
                .job_id = static_cast<std::uint64_t>(1000 + i),
                .text = "progress",
                });
        }
        coordinator.EnqueueResultForTest(savor::PRResult{
            .job_id = 999,
            .epoch = 1,
            .worker_id = 0,
            .accepted = true,
            });

        EXPECT_EQ(terminal_seen_future.wait_for(std::chrono::milliseconds(250)), std::future_status::ready);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        coordinator.Stop();

        const auto telemetry = coordinator.SnapshotTelemetry();
        EXPECT_GT(progress_seen.load(), 0);
        EXPECT_GT(telemetry.progress_batch_count, 0);
        EXPECT_GT(telemetry.max_progress_batch_size, 0);
        EXPECT_GE(telemetry.results_received_count, 1);
    }

    TEST(Stage3Phase3Batching, ConsecutiveDuplicateProgressRowsAreWarnedAndSummarized) {
        using namespace savor::runner::parallel::savordb;
        using namespace savor::db::execution::workflow;

        class ProgressRecordingExecutionDb final : public RecordingExecutionDb {
        public:
            savor::db::execution::jobs::IJobEventCommandService* JobCommandService() override {
                return &job_events;
            }

            RecordingJobEventCommandService job_events;
        };

        ProgressRecordingExecutionDb execution_db;
        DBWorkflowWorkerCoordinatorConfig cfg{
            .desired_workers = 1,
            .controller_sleep_ms = 1,
        };
        ConfigureWorksetTestGate(cfg);
        DBWorkflowWorkerCoordinator coordinator(
            &execution_db,
            std::move(cfg),
            CoordinatorIntegrationConfig{},
            [](const WorkflowReadyStep& step) {
                return ScheduledJobSet{
                    .job_set_id = 15000 + step.workflow_step_id,
                    .workflow_step_id = step.workflow_step_id,
                };
            });

        std::atomic<int> progress_seen{ 0 };
        coordinator.SetProgressCallback([&](const savor::PRProgress&) {
            ++progress_seen;
        });

        const auto start = coordinator.Start();
        ASSERT_TRUE(start) << start.error;
        coordinator.EnqueueProgressForTest(savor::PRProgress{ .worker_id = 3, .job_id = 42, .text = "same" });
        coordinator.EnqueueProgressForTest(savor::PRProgress{ .worker_id = 3, .job_id = 42, .text = "same" });
        coordinator.EnqueueProgressForTest(savor::PRProgress{ .worker_id = 3, .job_id = 42, .text = "same" });
        coordinator.EnqueueProgressForTest(savor::PRProgress{ .worker_id = 3, .job_id = 42, .text = "next" });
        EXPECT_TRUE(WaitForCondition([&] {
            return progress_seen.load() == 2;
        }));
        coordinator.Stop();

        std::vector<std::string> messages;
        for (const auto& call : execution_db.job_events.calls) {
            messages.push_back(call.message.value_or(""));
        }

        const auto exact_same_count = std::count(
            messages.begin(),
            messages.end(),
            std::string("worker=3 progress=same"));
        EXPECT_EQ(exact_same_count, 1);
        EXPECT_NE(std::find(messages.begin(), messages.end(), "worker=3 progress=next"), messages.end());
        EXPECT_TRUE(std::any_of(messages.begin(), messages.end(), [](const std::string& message) {
            return message.find("progress_warning=duplicate consecutive progress suppressed") != std::string::npos;
        }));
        EXPECT_TRUE(std::any_of(messages.begin(), messages.end(), [](const std::string& message) {
            return message.find("progress_summary=suppressed 2 duplicate consecutive progress line(s)") != std::string::npos;
        }));
        EXPECT_EQ(progress_seen.load(), 2);

        const auto warnings = coordinator.SnapshotWarnings();
        ASSERT_EQ(warnings.size(), 1);
        EXPECT_EQ(warnings.front().worker_id, 3);
        EXPECT_EQ(warnings.front().job_id, 42);
        EXPECT_EQ(warnings.front().message, "Duplicate progress suppressed");
        EXPECT_NE(warnings.front().detail.find("progress_warning=duplicate consecutive progress suppressed"), std::string::npos);
    }

    TEST(Stage3Phase3DispatchGuard, ClaimedJobsMoveThroughMaterializedQueueBeforeDispatch) {
        using namespace savor::runner::parallel::savordb;
        using namespace savor::db::execution::programdb;

        class QueueClaimExecutionDb final : public RecordingExecutionDb {
        public:
            std::vector<savor::db::ClaimedExecutionJob> ClaimBatchReadyExecutionJobs(
                std::string_view claimed_by_token,
                int requested_jobs,
                std::int64_t lease_duration_ms,
                std::string* error_out = nullptr) override {
                if (error_out) {
                    error_out->clear();
                }
                std::vector<savor::db::ClaimedExecutionJob> out;
                while (!claims.empty() && static_cast<int>(out.size()) < requested_jobs) {
                    auto claimed = claims.front();
                    claimed.claimed_by_token =
                        std::string(claimed_by_token) + ":"
                        + std::to_string(claimed.job_id);
                    claimed.lease_expires_at_utc = lease_duration_ms;
                    out.push_back(claimed);
                    savor::db::ExecutionJobRecord record{};
                    record.job_id = claimed.job_id;
                    record.job_set_id = claimed.job_set_id;
                    record.state = "CLAIMED";
                    record.claimed_by_token = claimed.claimed_by_token;
                    record.lease_expires_at_utc = lease_duration_ms;
                    records[record.job_id] = record;
                    claims.erase(claims.begin());
                }
                return out;
            }

            bool RenewExecutionJobLease(
                std::int64_t job_id,
                std::string_view claimed_by_token,
                std::int64_t lease_duration_ms,
                bool* renewed_out = nullptr,
                std::string* error_out = nullptr) override {
                if (error_out) {
                    error_out->clear();
                }
                auto it = records.find(job_id);
                const bool renewed = it != records.end()
                    && it->second.claimed_by_token.has_value()
                    && *it->second.claimed_by_token == claimed_by_token;
                if (renewed) {
                    it->second.lease_expires_at_utc = lease_duration_ms;
                    ++lease_renewals;
                }
                if (renewed_out) {
                    *renewed_out = renewed;
                }
                return true;
            }

            std::optional<savor::db::ExecutionJobRecord> GetJob(std::int64_t job_id) const override {
                const auto it = records.find(job_id);
                if (it == records.end()) {
                    return std::nullopt;
                }
                return it->second;
            }

            std::vector<savor::db::ClaimedExecutionJob> claims;
            std::unordered_map<std::int64_t, savor::db::ExecutionJobRecord> records;
            int lease_renewals = 0;
        };

        class QueueRuntime final : public IRuntimeInitAdapter {
        public:
            RuntimeInitRequest BuildRuntimeInit(std::int64_t) const override {
                return RuntimeInitRequest{
                    .savestate_ref_kind = "test_savestate",
                    .savestate_ref_id = 123,
                    .bootstrap_profile = "test.runtime",
                };
            }

            std::optional<savor::PSJob> MaterializePsJob(std::int64_t job_id, const RuntimeInitRequest&) const override {
                savor::PSJob job{};
                job.payload.push_back(7);
                job.payload.push_back(static_cast<std::uint8_t>(job_id & 0xff));
                (void)job_id;
                return job;
            }
        };

        ProgramKindDescriptor descriptor{};
        descriptor.program_kind = 7;
        descriptor.program_name = "QueueRuntime";
        descriptor.runtime_init = std::make_shared<QueueRuntime>();

        ProgramKindRegistry registry;
        ASSERT_TRUE(registry.Register(descriptor));
        ASSERT_TRUE(registry.RegisterForStepKind("test.step", descriptor));

        QueueClaimExecutionDb execution_db;
        JobMaterializationService materialization(&execution_db, &registry);
        materialization.ResetForStart();

        int dispatch_calls = 0;
        WorkflowDispatchCoordinator dispatch(
            &materialization,
            [&](std::size_t, const ClaimedJobRecord& job) {
                ++dispatch_calls;
                EXPECT_EQ(job.job_id, 4242);
                EXPECT_EQ(job.program_kind, 7);
                EXPECT_TRUE(job.payload.has_value());
                EXPECT_TRUE(job.affinity.program_runtime_affinity_key.has_value());
                if (job.affinity.program_runtime_affinity_key.has_value()) {
                    EXPECT_EQ(*job.affinity.program_runtime_affinity_key, "test.runtime");
                }
                return true;
            });

        const auto now = std::chrono::steady_clock::now();
        EXPECT_FALSE(dispatch.DispatchNextEligibleForWorker(0, std::nullopt, now));
        EXPECT_EQ(dispatch_calls, 0);

        std::atomic<bool> stop_requested{ false };
        std::thread materializer([&]() {
            materialization.MaterializeClaimedJobPayloadLoop(stop_requested);
        });

        execution_db.claims.push_back(savor::db::ClaimedExecutionJob{
            .job_id = 4242,
            .job_set_id = 42,
            .workflow_instance_id = 1,
            .workflow_step_id = 2,
            .workflow_step_key = "Neutral",
            .workflow_step_kind = "test.step",
            .workflow_step_priority = 1,
        });

        EXPECT_EQ(materialization.ClaimJobs(1, now), 1u);
        const auto lease_maintenance = materialization.RenewActiveClaimLeases(std::chrono::milliseconds(30000));
        EXPECT_EQ(lease_maintenance.attempted, 1u);
        EXPECT_EQ(lease_maintenance.renewed, 1u);
        EXPECT_EQ(lease_maintenance.failed, 0u);
        EXPECT_EQ(execution_db.lease_renewals, 1);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while (dispatch_calls == 0 && std::chrono::steady_clock::now() < deadline) {
            (void)dispatch.DispatchNextEligibleForWorker(0, std::nullopt, std::chrono::steady_clock::now());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        stop_requested.store(true);
        materialization.StopMaterializationLoop();
        if (materializer.joinable()) {
            materializer.join();
        }

        EXPECT_EQ(dispatch_calls, 1);
    }

    TEST(Stage3Phase3DispatchGuard, MaterializedReadySetUsesStableDurableOrderBeforeWorkerAffinity) {
        using namespace savor::runner::parallel::savordb;
        using namespace savor::db::execution::programdb;

        class QueueClaimExecutionDb final : public RecordingExecutionDb {
        public:
            std::vector<savor::db::ClaimedExecutionJob> ClaimBatchReadyExecutionJobs(
                std::string_view claimed_by_token,
                int requested_jobs,
                std::int64_t lease_duration_ms,
                std::string* error_out = nullptr) override {
                if (error_out) {
                    error_out->clear();
                }
                std::vector<savor::db::ClaimedExecutionJob> out;
                while (!claims.empty() && static_cast<int>(out.size()) < requested_jobs) {
                    auto claimed = claims.front();
                    claimed.claimed_by_token =
                        std::string(claimed_by_token) + ":"
                        + std::to_string(claimed.job_id);
                    claimed.lease_expires_at_utc = lease_duration_ms;
                    out.push_back(claimed);
                    savor::db::ExecutionJobRecord record{};
                    record.job_id = claimed.job_id;
                    record.job_set_id = claimed.job_set_id;
                    record.state = "CLAIMED";
                    record.claimed_by_token = claimed.claimed_by_token;
                    record.lease_expires_at_utc = lease_duration_ms;
                    records[record.job_id] = record;
                    claims.erase(claims.begin());
                }
                return out;
            }

            bool RenewExecutionJobLease(
                std::int64_t job_id,
                std::string_view claimed_by_token,
                std::int64_t lease_duration_ms,
                bool* renewed_out = nullptr,
                std::string* error_out = nullptr) override {
                if (error_out) {
                    error_out->clear();
                }
                auto it = records.find(job_id);
                const bool renewed = it != records.end()
                    && it->second.claimed_by_token.has_value()
                    && *it->second.claimed_by_token == claimed_by_token;
                if (renewed) {
                    it->second.lease_expires_at_utc = lease_duration_ms;
                    ++lease_renewals;
                }
                if (renewed_out) {
                    *renewed_out = renewed;
                }
                return true;
            }

            std::optional<savor::db::ExecutionJobRecord> GetJob(std::int64_t job_id) const override {
                const auto it = records.find(job_id);
                if (it == records.end()) {
                    return std::nullopt;
                }
                return it->second;
            }

            std::vector<savor::db::ClaimedExecutionJob> claims;
            std::unordered_map<std::int64_t, savor::db::ExecutionJobRecord> records;
            int lease_renewals = 0;
        };

        class AffinityRuntime final : public IRuntimeInitAdapter {
        public:
            AffinityRuntime(std::int64_t savestate_ref_id, std::string bootstrap_profile)
                : savestate_ref_id_(savestate_ref_id)
                , bootstrap_profile_(std::move(bootstrap_profile)) {
            }

            RuntimeInitRequest BuildRuntimeInit(std::int64_t) const override {
                return RuntimeInitRequest{
                    .savestate_ref_kind = "test_savestate",
                    .savestate_ref_id = savestate_ref_id_,
                    .bootstrap_profile = bootstrap_profile_,
                    .workset_execution_key =
                        bootstrap_profile_ + ":"
                        + std::to_string(savestate_ref_id_),
                };
            }

            std::optional<savor::PSJob> MaterializePsJob(std::int64_t job_id, const RuntimeInitRequest&) const override {
                savor::PSJob job{};
                job.payload.push_back(static_cast<std::uint8_t>(job_id & 0xff));
                return job;
            }

        private:
            std::int64_t savestate_ref_id_ = 0;
            std::string bootstrap_profile_;
        };

        const auto register_program = [](
            ProgramKindRegistry& registry,
            std::int32_t program_kind,
            const std::string& step_kind,
            std::int64_t savestate_ref_id,
            const std::string& bootstrap_profile) {
            ProgramKindDescriptor descriptor{};
            descriptor.program_kind = program_kind;
            descriptor.program_name = step_kind;
            descriptor.runtime_init = std::make_shared<AffinityRuntime>(savestate_ref_id, bootstrap_profile);
            ASSERT_TRUE(registry.Register(descriptor));
            ASSERT_TRUE(registry.RegisterForStepKind(step_kind, descriptor));
        };

        ProgramKindRegistry registry;
        register_program(registry, 7, "test.program7", 111, "old-runtime");
        register_program(registry, 8, "test.program8", 123, "other-runtime");
        register_program(registry, 9, "test.program9", 333, "warm-runtime");
        register_program(registry, 10, "test.program10", 444, "cold-runtime");

        QueueClaimExecutionDb execution_db;
        execution_db.claims = {
            savor::db::ClaimedExecutionJob{
                .job_id = 1001,
                .job_set_id = 1,
                .workflow_instance_id = 1,
                .workflow_step_id = 1,
                .workflow_step_key = "Program7",
                .workflow_step_kind = "test.program7",
                .workflow_step_priority = 1,
            },
            savor::db::ClaimedExecutionJob{
                .job_id = 1002,
                .job_set_id = 1,
                .workflow_instance_id = 1,
                .workflow_step_id = 2,
                .workflow_step_key = "Program8",
                .workflow_step_kind = "test.program8",
                .workflow_step_priority = 1,
            },
            savor::db::ClaimedExecutionJob{
                .job_id = 1003,
                .job_set_id = 1,
                .workflow_instance_id = 1,
                .workflow_step_id = 3,
                .workflow_step_key = "Program9",
                .workflow_step_kind = "test.program9",
                .workflow_step_priority = 1,
            },
            savor::db::ClaimedExecutionJob{
                .job_id = 1004,
                .job_set_id = 1,
                .workflow_instance_id = 1,
                .workflow_step_id = 4,
                .workflow_step_key = "Program10",
                .workflow_step_kind = "test.program10",
                .workflow_step_priority = 1,
            },
            savor::db::ClaimedExecutionJob{
                .job_id = 1005,
                .job_set_id = 1,
                .workflow_instance_id = 1,
                .workflow_step_id = 5,
                .workflow_step_key = "Program9Again",
                .workflow_step_kind = "test.program9",
                .workflow_step_priority = 1,
            },
        };

        JobMaterializationService materialization(&execution_db, &registry);
        materialization.ResetForStart();
        const auto now = std::chrono::steady_clock::now();
        EXPECT_EQ(materialization.ClaimJobs(5, now), 5u);
        EXPECT_TRUE(materialization.MaterializeClaimedJobPayload(now));

        ClaimedJobRecord selected{};
        EXPECT_TRUE(materialization.TrySelectMaterializedJobForWorker(
            MaterializedJobSelectionAffinity{
                .savestate_affinity_key = std::string("123"),
                .program_kind = 7,
                .program_runtime_affinity_key = std::string("old-runtime"),
            },
            &selected));
        EXPECT_EQ(selected.job_id, 1001);
        EXPECT_EQ(selected.program_kind, 7);

        EXPECT_TRUE(materialization.TrySelectMaterializedJobForWorker(
            MaterializedJobSelectionAffinity{
                .program_kind = 7,
                .program_runtime_affinity_key = std::string("cold-runtime"),
            },
            &selected));
        EXPECT_EQ(selected.job_id, 1002);
        EXPECT_EQ(selected.program_kind, 8);

        ClaimedJobRecord anchor{};
        ASSERT_TRUE(materialization.PeekMaterializedAnchor(&anchor));
        ASSERT_EQ(anchor.job_id, 1003);
        ASSERT_GT(anchor.encoded_input_bytes, 0u);

        std::vector<ClaimedJobRecord> workset;
        EXPECT_TRUE(materialization.TrySelectMaterializedWorksetForWorker(
            MaterializedJobSelectionAffinity{},
            MaterializedWorksetSelectionLimits{
                .max_items = 16,
                .lookahead_items = 16,
                .max_selected_bytes = anchor.encoded_input_bytes,
                .lookahead_bytes =
                    (std::numeric_limits<std::size_t>::max)(),
            },
            &workset));
        ASSERT_EQ(workset.size(), 1u);
        EXPECT_EQ(workset.front().job_id, 1003);
        ASSERT_TRUE(materialization.RequeueMaterializedJob(1003));

        workset.clear();
        EXPECT_TRUE(materialization.TrySelectMaterializedWorksetForWorker(
            MaterializedJobSelectionAffinity{},
            MaterializedWorksetSelectionLimits{
                .max_items = 16,
                .lookahead_items = 16,
                .max_selected_bytes =
                    (std::numeric_limits<std::size_t>::max)(),
                .lookahead_bytes = anchor.encoded_input_bytes,
            },
            &workset));
        ASSERT_EQ(workset.size(), 1u);
        EXPECT_EQ(workset.front().job_id, 1003);
        ASSERT_TRUE(materialization.RequeueMaterializedJob(1003));

        workset.clear();
        EXPECT_TRUE(materialization.TrySelectMaterializedWorksetForWorker(
            MaterializedJobSelectionAffinity{
                .program_kind = 99,
                .program_runtime_affinity_key = std::string("warm-runtime"),
            },
            MaterializedWorksetSelectionLimits{
                .max_items = 16,
                .lookahead_items = 16,
            },
            &workset));
        ASSERT_EQ(workset.size(), 2u);
        EXPECT_EQ(workset[0].job_id, 1003);
        EXPECT_EQ(workset[1].job_id, 1005);
        EXPECT_EQ(workset[0].workset_execution_key,
            workset[1].workset_execution_key);
    }

    TEST(Stage3Phase3Contracts, DedupeIsolationIsScopedPerCoordinatorBridgeInstance) {
        using namespace savor::runner::parallel::savordb;

        WorkflowCoordinatorBridge service_a;
        WorkflowCoordinatorBridge service_b;
        int service_a_seen = 0;
        int service_b_seen = 0;
        service_a.SetTerminalCallback([&](const TerminalJobSetSignal&) { ++service_a_seen; });
        service_b.SetTerminalCallback([&](const TerminalJobSetSignal&) { ++service_b_seen; });

        const TerminalJobSetSignal signal{
            .workflow_instance_id = 77,
            .workflow_step_id = 88,
            .job_set_id = 99,
            .terminal_state = "COMPLETED",
        };

        const bool a_first = service_a.NotifyTerminal(signal);
        const bool a_dup = service_a.NotifyTerminal(signal);
        const bool b_first = service_b.NotifyTerminal(signal);
        const bool b_dup = service_b.NotifyTerminal(signal);

        EXPECT_TRUE(a_first);
        EXPECT_FALSE(a_dup);
        EXPECT_TRUE(b_first);
        EXPECT_FALSE(b_dup);
        EXPECT_EQ(service_a_seen, 1);
        EXPECT_EQ(service_b_seen, 1);
    }

    TEST(Stage3Phase0Replay, BackfillRelayResolvesLegacyAndWorkflowInputPayloadRefs) {
        using namespace savor::db::migrations;
        using namespace savor::db::events;

        sqlite3* db = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &db));
        const MigrationSourceOptions embedded_options{
            .source_kind = MigrationSourceKind::Embedded,
        };
        std::string migration_error;
        ASSERT_TRUE(ApplyContextMigrations(db, MigrationContext::Execution, embedded_options, &migration_error))
            << migration_error;

        ASSERT_TRUE(ExecSql(db, R"SQL(
INSERT INTO exec_workflow_instance(
    workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc
)
VALUES(301, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'test', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_step(
    workflow_step_id, workflow_instance_id, step_key, step_kind, state, created_at_utc, ready_at_utc
)
VALUES(302, 301, 'seedprobe.neutral', 'seedprobe.neutral', 'READY', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_event(workflow_event_id, workflow_instance_id, workflow_step_id, event_kind, event_ts_utc, message)
VALUES(303, 301, 302, 'Execution.WorkflowStepReady.v1', unixepoch()*1000, 'ready');
INSERT INTO exec_outbox_message(
    outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id
)
VALUES
    (305,'evt-v-1','Execution.WorkflowStepReady.v1',1,'Execution','workflow_step','302','corr-301','cause-301',unixepoch()*1000,'workflow_event',303);
)SQL"));

        savor::db::execution::workflow::SqliteExecutionDb execution_db(db);
        const auto legacy_payload = execution_db.ResolveExecutionWorkflowJobPayload(
            "Execution.WorkflowStepReady.v1", 1, "workflow_event", 303);
        ASSERT_TRUE(legacy_payload.has_value());
        EXPECT_EQ(legacy_payload->workflow_instance_id, 301);
        EXPECT_EQ(legacy_payload->workflow_step_id, 302);

        int unresolved_count = 0;
        OutboxRelay relay({
            .db = db,
            .outbox_table = "exec_outbox_message",
            .context_name = "Execution",
        });

        std::vector<OutboxRelayDispatchBinding> bindings;
        bindings.push_back({
            .key = { .event_type = "Execution.WorkflowStepReady.v1", .event_version = 1 },
            .handler = [&](const EventEnvelope& envelope, std::string* handler_error) {
                const auto payload = execution_db.ResolveExecutionWorkflowJobPayload(envelope);
                if (!payload.has_value()) {
                    ++unresolved_count;
                    if (handler_error != nullptr) {
                        *handler_error = "unresolved payload";
                    }
                    return false;
                }
                return true;
            },
        });
        OutboxRelayResult relay_result{};
        std::string err;
        ASSERT_TRUE(relay.RelayBatch(0, 10, bindings, &relay_result, &err)) << err;
        EXPECT_EQ(relay_result.failure_count, 0);
        EXPECT_EQ(unresolved_count, 0);
        EXPECT_EQ(relay_result.published_count, 1);

        sqlite3_close(db);
    }

    TEST(Stage3Phase3Replay, ReplayCursorRobustnessAcrossSequentialRelayPasses) {
        using namespace savor::db::events;
        using namespace savor::db::migrations;

        sqlite3* db = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &db));
        const MigrationSourceOptions embedded_options{
            .source_kind = MigrationSourceKind::Embedded,
        };
        std::string err;
        ASSERT_TRUE(ApplyContextMigrations(db, MigrationContext::Execution, embedded_options, &err)) << err;

        ASSERT_TRUE(ExecSql(db, R"SQL(
INSERT INTO exec_outbox_message(
    outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id
) VALUES
    (3901,'evt-p3-r1','Execution.WorkflowStepReady.v1',1,'Execution','workflow_step','901','corr-p3','cause-p3',unixepoch()*1000,'workflow_event',1);
)SQL"));

        OutboxRelay relay({
            .db = db,
            .outbox_table = "exec_outbox_message",
            .context_name = "Execution",
        });
        int handled = 0;
        std::vector<OutboxRelayDispatchBinding> bindings{ {
            .key = { .event_type = "Execution.WorkflowStepReady.v1", .event_version = 1 },
            .handler = [&](const EventEnvelope&, std::string*) { ++handled; return true; },
        } };

        OutboxRelayResult first{};
        ASSERT_TRUE(relay.RelayBatchFromCursor(0, 8, bindings, &first, &err)) << err;
        OutboxRelayResult second{};
        ASSERT_TRUE(relay.RelayBatchFromCursor(first.last_scanned_outbox_id, 8, bindings, &second, &err)) << err;

        EXPECT_EQ(first.published_count, 1);
        EXPECT_EQ(second.published_count, 0);
        EXPECT_EQ(handled, 1);
        sqlite3_close(db);
    }

    TEST(Stage3Phase3Replay, ReplayRobustnessSupportsSavestateOverrideAndJsonlFixtureSeeding) {
        using namespace savor::db::events;

        const auto migration_root = ResolveMigrationRootForTests();
        sqlite3* db = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &db));

        DbPreparer preparer(db, migration_root);
        std::string err;
        ASSERT_TRUE(preparer.InitializeRequiredMigrations(&err)) << err;

        const auto temp_dir = MakeTempPhase4Dir("phase3-robustness-jsonl");
        const auto savestate_path = temp_dir / "fixture_phase3.sav";
        {
            std::ofstream sav(savestate_path, std::ios::binary);
            sav << "savestate-bytes";
        }
        ASSERT_TRUE(preparer.SeedSavestateArtifactAndOverride(savestate_path, &err)) << err;

        const std::string default_rows_json = R"JSON({
  "state_artifact": [
    {
      "artifact_id": 101,
      "sha256": "phase3-placeholder-sha256",
      "size_bytes": 1,
      "compression_kind": 0,
      "filename": "placeholder_phase3.sav",
      "file_ext": ".sav",
      "artifact_kind": "SAV",
      "created_at_utc": 1712304000000
    }
  ]
})JSON";
        ASSERT_TRUE(preparer.SeedRowsFromJsonObject(default_rows_json, &err)) << err;

        const auto jsonl_dir = temp_dir / "jsonl";
        std::filesystem::create_directories(jsonl_dir);
        {
            std::ofstream out(jsonl_dir / "exec_outbox_message.jsonl");
            out << R"JSON({"outbox_id":3901,"event_id":"evt-p3-r1","event_type":"Execution.WorkflowStepReady.v1","event_version":1,"context_name":"Execution","aggregate_kind":"workflow_step","aggregate_id":"901","correlation_id":"corr-p3","causation_id":"cause-p3","occurred_at_utc":1712304000000,"payload_ref_kind":"workflow_event","payload_ref_id":1})JSON"
                << "\n";
        }
        ASSERT_TRUE(preparer.SeedRowsFromJsonlDirectory(jsonl_dir, &err)) << err;

        sqlite3_stmt* st = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db, "SELECT filename FROM state_artifact WHERE artifact_id=101;", -1, &st, nullptr));
        ASSERT_EQ(SQLITE_ROW, sqlite3_step(st));
        ASSERT_NE(sqlite3_column_text(st, 0), nullptr);
        const std::string filename = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
        EXPECT_EQ(std::filesystem::path(filename).extension().string(), ".sav");

        OutboxRelay relay({
            .db = db,
            .outbox_table = "exec_outbox_message",
            .context_name = "Execution",
        });
        int handled = 0;
        std::vector<OutboxRelayDispatchBinding> bindings{ {
            .key = { .event_type = "Execution.WorkflowStepReady.v1", .event_version = 1 },
            .handler = [&](const EventEnvelope&, std::string*) { ++handled; return true; },
        } };
        OutboxRelayResult first{};
        ASSERT_TRUE(relay.RelayBatchFromCursor(0, 8, bindings, &first, &err)) << err;
        OutboxRelayResult second{};
        ASSERT_TRUE(relay.RelayBatchFromCursor(first.last_scanned_outbox_id, 8, bindings, &second, &err)) << err;
        EXPECT_EQ(first.published_count, 1);
        EXPECT_EQ(second.published_count, 0);
        EXPECT_EQ(handled, 1);

        sqlite3_close(db);
        std::error_code ec;
        std::filesystem::remove_all(temp_dir, ec);
    }

    TEST(Stage3Phase3Relay, DeadLetterAndLagPreviewReadinessSignals) {
        using namespace savor::db::events;
        using namespace savor::db::migrations;
        using namespace savor::db::retention;

        sqlite3* db = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_open(":memory:", &db));
        const MigrationSourceOptions embedded_options{
            .source_kind = MigrationSourceKind::Embedded,
        };
        std::string err;
        ASSERT_TRUE(ApplyContextMigrations(db, MigrationContext::Execution, embedded_options, &err)) << err;

        ASSERT_TRUE(ExecSql(db, R"SQL(
INSERT INTO exec_outbox_message(
    outbox_id,event_id,event_type,event_version,context_name,aggregate_kind,aggregate_id,correlation_id,causation_id,occurred_at_utc,payload_ref_kind,payload_ref_id
) VALUES
    (3951,'evt-p3-lag','Execution.WorkflowStepReady.v1',1,'Execution','workflow_step','1','corr','cause',unixepoch()*1000,'workflow_event',1);
)SQL"));

        OutboxRelay relay({
            .db = db,
            .outbox_table = "exec_outbox_message",
            .context_name = "Execution",
            .max_attempts = 1,
        });
        std::vector<OutboxRelayDispatchBinding> bindings{ {
            .key = { .event_type = "Execution.WorkflowStepReady.v1", .event_version = 1 },
            .handler = [&](const EventEnvelope&, std::string* handler_error) {
                if (handler_error != nullptr) {
                    *handler_error = "intentional-failure";
                }
                return false;
            },
        } };

        OutboxRelayResult relay_result{};
        ASSERT_TRUE(relay.RelayBatchFromCursor(0, 10, bindings, &relay_result, &err)) << err;
        EXPECT_EQ(relay_result.dead_lettered_count, 1);

        savor::db::execution::workflow::SqliteExecutionDb execution_db(db);
        const auto preview = execution_db.PreviewOutboxRetention(
            std::vector<OutboxSubscriptionSnapshot>{
                OutboxSubscriptionSnapshot{
                    .projector_name = "phase3-test-subscriber",
                    .last_outbox_id = 0,
                    .updated_at_utc = savor::db::types::UtcNow(),
                    .status = "ACTIVE",
                },
            },
            savor::db::types::UtcNow(),
            OutboxRetentionPolicy{});

        ASSERT_FALSE(preview.lag_per_subscription.empty());
        EXPECT_GE(preview.lag_per_subscription.front().lag_outbox_rows, 1);
        sqlite3_close(db);
    }

    TEST(Stage4Recovery, InvariantViolationRemediationSequencePersistsLifecycle) {
        using savor::db::core::DBService;
        using savor::db::execution::workflow::SqliteExecutionDb;
        using savor::db::execution::workflow::WorkflowInvariantRemediationCommand;

        std::filesystem::path temp_dir;
        std::unique_ptr<DBService> service;
        SqliteExecutionDb* execution_db = nullptr;
        std::string err;
        ASSERT_TRUE(OpenPhase4ExecutionDb("remediation", &temp_dir, &service, &execution_db, &err)) << err;

        ASSERT_TRUE(execution_db->ValidationExecuteSql(R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(4101, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'phase4-test', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, expected_total, created_at_utc)
VALUES(4102, 1, 'workflow', 2, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, attempts, max_attempts, created_at_utc, started_at_utc)
VALUES(4103, 4101, 'Grid', 'seedprobe.grid', 'RUNNING', 4102, 1, 2, unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_job(job_id, job_set_id, program_kind, program_version, program_ref_kind, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at_utc, ended_at_utc)
VALUES
(4104, 4102, 1, 1, 'seedprobe', 1, 'phase4-r1', 1, 'COMPLETED', 1, 2, unixepoch()*1000, unixepoch()*1000),
(4105, 4102, 1, 1, 'seedprobe', 1, 'phase4-r2', 1, 'FAILED', 1, 2, unixepoch()*1000, unixepoch()*1000);
)SQL", &err))
            << err;

        bool reopened = false;
        ASSERT_TRUE(execution_db->ValidationExecuteInvariantRemediation(
            WorkflowInvariantRemediationCommand{
                .workflow_instance_id = 4101,
                .workflow_step_id = 4103,
                .violation_reason = "STEP_BLOCKED_COUNT_MISMATCH",
                .requested_by = "phase4-test",
            },
            &reopened,
            &err))
            << err;
        EXPECT_TRUE(reopened);

        std::string instance_state;
        ASSERT_TRUE(execution_db->ValidationQueryText(
            "SELECT state FROM exec_workflow_instance WHERE workflow_instance_id=4101;",
            &instance_state,
            &err))
            << err;
        EXPECT_EQ(instance_state, "RUNNING");

        std::int64_t violation_events = 0;
        std::int64_t repair_events = 0;
        std::int64_t reopen_events = 0;
        ASSERT_TRUE(execution_db->ValidationQueryInt(
                        "SELECT COUNT(1) FROM exec_workflow_event WHERE event_kind='Execution.WorkflowInvariantViolation.v1';",
                        &violation_events,
                        &err)
                    && execution_db->ValidationQueryInt(
                        "SELECT COUNT(1) FROM exec_workflow_event WHERE event_kind='Execution.WorkflowRemediationRepairExecuted.v1';",
                        &repair_events,
                        &err)
                    && execution_db->ValidationQueryInt(
                        "SELECT COUNT(1) FROM exec_workflow_event WHERE event_kind='Execution.WorkflowRemediationReopened.v1';",
                        &reopen_events,
                        &err))
            << err;
        EXPECT_EQ(violation_events, 1);
        EXPECT_EQ(repair_events, 1);
        EXPECT_EQ(reopen_events, 1);

        CleanupPhase4Db(service, temp_dir);
    }

    TEST(Stage4Queues, DBServiceExecutionDbSerializesConcurrentWorkflowLifecycleWrites) {
        using savor::db::core::DBService;
        using savor::db::execution::QueuedExecutionDb;
        using savor::db::execution::workflow::SqliteExecutionDb;

        std::filesystem::path temp_dir;
        std::unique_ptr<DBService> service;
        SqliteExecutionDb* raw_execution_db = nullptr;
        std::string err;
        ASSERT_TRUE(OpenPhase4ExecutionDb("queued-execution", &temp_dir, &service, &raw_execution_db, &err)) << err;

        auto* queued_execution_db = dynamic_cast<QueuedExecutionDb*>(service->ExecutionDb());
        ASSERT_NE(queued_execution_db, nullptr);
        ASSERT_NE(queued_execution_db->WorkflowCommandService(), nullptr);

        ASSERT_TRUE(raw_execution_db->ValidationExecuteSql(R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(4701, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'queue-test', unixepoch()*1000, unixepoch()*1000);
)SQL", &err))
            << err;

        constexpr int kThreadCount = 8;
        constexpr int kWritesPerThread = 25;
        std::atomic<int> failures{0};
        std::vector<std::thread> threads;
        threads.reserve(kThreadCount);
        for (int thread_idx = 0; thread_idx < kThreadCount; ++thread_idx) {
            threads.emplace_back([&, thread_idx]() {
                for (int write_idx = 0; write_idx < kWritesPerThread; ++write_idx) {
                    std::string write_err;
                    const bool ok = queued_execution_db->WorkflowCommandService()->AppendLifecycleEvent(
                        {
                            .workflow_instance_id = 4701,
                            .event_kind = "Execution.WorkflowTransitionEvaluated.v1",
                            .message = std::string("thread=") + std::to_string(thread_idx) + " write=" + std::to_string(write_idx),
                            .requested_by = "queue-test",
                        },
                        &write_err);
                    if (!ok) {
                        ++failures;
                    }
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }

        EXPECT_EQ(failures.load(), 0);

        std::int64_t event_count = 0;
        ASSERT_TRUE(raw_execution_db->ValidationQueryInt(
            "SELECT COUNT(1) FROM exec_workflow_event WHERE workflow_instance_id=4701 AND event_kind='Execution.WorkflowTransitionEvaluated.v1';",
            &event_count,
            &err))
            << err;
        EXPECT_EQ(event_count, kThreadCount * kWritesPerThread);

        const auto telemetry = queued_execution_db->GetTelemetrySnapshot();
        EXPECT_GE(telemetry.write_enqueued, static_cast<std::uint64_t>(kThreadCount * kWritesPerThread));
        EXPECT_EQ(telemetry.write_rejected, 0u);

        CleanupPhase4Db(service, temp_dir);
    }

    TEST(Stage4Queues, DBServiceExposesQueuedFacadesForAllDatabases) {
        using savor::db::QueuedArchiveDb;
        using savor::db::QueuedAuthoringDb;
        using savor::db::QueuedUiReadDb;
        using savor::db::analysis::QueuedAnalysisDb;
        using savor::db::core::DBService;
        using savor::db::execution::QueuedExecutionDb;
        using savor::db::execution::workflow::SqliteExecutionDb;
        using savor::db::state::QueuedStateDb;

        std::filesystem::path temp_dir;
        std::unique_ptr<DBService> service;
        SqliteExecutionDb* raw_execution_db = nullptr;
        std::string err;
        ASSERT_TRUE(OpenPhase4ExecutionDb("queued-all-dbs", &temp_dir, &service, &raw_execution_db, &err)) << err;

        auto* execution = dynamic_cast<QueuedExecutionDb*>(service->ExecutionDb());
        auto* state = dynamic_cast<QueuedStateDb*>(service->StateDb());
        auto* analysis = dynamic_cast<QueuedAnalysisDb*>(service->AnalysisDb());
        auto* authoring = dynamic_cast<QueuedAuthoringDb*>(service->AuthoringDb());
        auto* ui_read = dynamic_cast<QueuedUiReadDb*>(service->UiReadDb());
        auto* archive = dynamic_cast<QueuedArchiveDb*>(service->ArchiveDb());

        ASSERT_NE(execution, nullptr);
        ASSERT_NE(state, nullptr);
        ASSERT_NE(analysis, nullptr);
        ASSERT_NE(authoring, nullptr);
        ASSERT_NE(ui_read, nullptr);
        ASSERT_NE(archive, nullptr);

        EXPECT_TRUE(execution->IsRunning());
        EXPECT_TRUE(state->IsRunning());
        EXPECT_TRUE(analysis->IsRunning());
        EXPECT_TRUE(authoring->IsRunning());
        EXPECT_TRUE(ui_read->IsRunning());
        EXPECT_TRUE(archive->IsRunning());

        CleanupPhase4Db(service, temp_dir);
    }

    TEST(Stage4Recovery, ClaimedJobMaterializationIsIdempotentAcrossRestart) {
        using savor::db::core::DBService;
        using savor::db::execution::workflow::SqliteExecutionDb;

        std::filesystem::path temp_dir;
        std::unique_ptr<DBService> service;
        SqliteExecutionDb* execution_db = nullptr;
        std::string err;
        ASSERT_TRUE(OpenPhase4ExecutionDb("powerloss", &temp_dir, &service, &execution_db, &err)) << err;

        ASSERT_TRUE(execution_db->ValidationExecuteSql(R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(4201, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'phase4-test', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, attempts, max_attempts, created_at_utc, ready_at_utc)
VALUES(4202, 4201, 'Neutral', 'seedprobe.neutral', 'READY', 0, 2, unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, expected_total, created_at_utc)
VALUES(4203, 1, 'workflow', 1, unixepoch()*1000);
)SQL", &err))
            << err;

        ASSERT_TRUE(execution_db->WorkflowCommandService()->MarkStepMaterialized(
            { .workflow_step_id = 4202, .job_set_id = 4203, .requested_by = "phase4-test" }, &err))
            << err;

        service->Stop();
        using namespace savor::db::migrations;
        auto restarted = std::make_unique<DBService>(
            MakePhase4DbPaths(temp_dir),
            MigrationSourceOptions{ .source_kind = MigrationSourceKind::Embedded });
        ASSERT_TRUE(restarted->Start(&err)) << err;
        auto* restarted_execution = restarted->RawExecutionDbForValidation();
        ASSERT_NE(restarted_execution, nullptr);

        ASSERT_TRUE(restarted_execution->WorkflowCommandService()->MarkStepMaterialized(
            { .workflow_step_id = 4202, .job_set_id = 4203, .requested_by = "phase4-test-restart" }, &err))
            << err;

        std::int64_t materialized_events = 0;
        ASSERT_TRUE(restarted_execution->ValidationQueryInt(
            "SELECT COUNT(1) FROM exec_workflow_event WHERE workflow_step_id=4202 AND event_kind='Execution.WorkflowStepMaterialized.v1';",
            &materialized_events,
            &err))
            << err;
        EXPECT_EQ(materialized_events, 1);

        CleanupPhase4Db(restarted, temp_dir);
    }

    TEST(Stage4Recovery, DuplicateTerminalReplayWritesSingleTerminalEvent) {
        using savor::db::core::DBService;
        using savor::db::execution::workflow::SqliteExecutionDb;

        std::filesystem::path temp_dir;
        std::unique_ptr<DBService> service;
        SqliteExecutionDb* execution_db = nullptr;
        std::string err;
        ASSERT_TRUE(OpenPhase4ExecutionDb("dup-terminal", &temp_dir, &service, &execution_db, &err)) << err;

        ASSERT_TRUE(execution_db->ValidationExecuteSql(R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(4301, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'phase4-test', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, expected_total, created_at_utc)
VALUES(4302, 1, 'workflow', 1, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, job_set_id, attempts, max_attempts, created_at_utc, started_at_utc)
VALUES(4303, 4301, 'Unique', 'seedprobe.unique', 'MATERIALIZED', 4302, 1, 2, unixepoch()*1000, unixepoch()*1000);
)SQL", &err))
            << err;

        ASSERT_TRUE(execution_db->WorkflowCommandService()->MarkStepTerminal(
            { .workflow_step_id = 4303, .terminal_state = "COMPLETED", .requested_by = "phase4-test" }, &err))
            << err;
        ASSERT_TRUE(execution_db->WorkflowCommandService()->MarkStepTerminal(
            { .workflow_step_id = 4303, .terminal_state = "COMPLETED", .requested_by = "phase4-test-replay" }, &err))
            << err;

        std::int64_t completed_events = 0;
        ASSERT_TRUE(execution_db->ValidationQueryInt(
            "SELECT COUNT(1) FROM exec_workflow_event WHERE workflow_step_id=4303 AND event_kind='Execution.WorkflowStepCompleted.v1';",
            &completed_events,
            &err))
            << err;
        EXPECT_EQ(completed_events, 1);

        CleanupPhase4Db(service, temp_dir);
    }

    TEST(Stage4Recovery, PartialWriterFailurePreservesCommittedState) {
        using savor::db::core::DBService;
        using savor::db::execution::workflow::SqliteExecutionDb;

        std::filesystem::path temp_dir;
        std::unique_ptr<DBService> service;
        SqliteExecutionDb* execution_db = nullptr;
        std::string err;
        ASSERT_TRUE(OpenPhase4ExecutionDb("partial-writer", &temp_dir, &service, &execution_db, &err)) << err;

        ASSERT_TRUE(execution_db->ValidationExecuteSql(R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(4401, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'phase4-test', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_job_set(job_set_id, program_kind, purpose, expected_total, created_at_utc)
VALUES(4402, 1, 'workflow', 1, unixepoch()*1000),
      (4403, 1, 'workflow', 1, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, attempts, max_attempts, created_at_utc, ready_at_utc)
VALUES(4404, 4401, 'Neutral', 'seedprobe.neutral', 'READY', 0, 2, unixepoch()*1000, unixepoch()*1000);
)SQL", &err))
            << err;

        ASSERT_TRUE(execution_db->WorkflowCommandService()->MarkStepMaterialized(
            { .workflow_step_id = 4404, .job_set_id = 4402, .requested_by = "phase4-test" }, &err))
            << err;
        EXPECT_FALSE(execution_db->WorkflowCommandService()->MarkStepMaterialized(
            { .workflow_step_id = 4404, .job_set_id = 4403, .requested_by = "phase4-test-conflict" }, &err));

        std::int64_t mapped_job_set_id = 0;
        std::int64_t materialized_events = 0;
        ASSERT_TRUE(execution_db->ValidationQueryInt(
                        "SELECT job_set_id FROM exec_workflow_step WHERE workflow_step_id=4404;",
                        &mapped_job_set_id,
                        &err)
                    && execution_db->ValidationQueryInt(
                        "SELECT COUNT(1) FROM exec_workflow_event WHERE workflow_step_id=4404 AND event_kind='Execution.WorkflowStepMaterialized.v1';",
                        &materialized_events,
                        &err))
            << err;
        EXPECT_EQ(mapped_job_set_id, 4402);
        EXPECT_EQ(materialized_events, 1);

        CleanupPhase4Db(service, temp_dir);
    }

    TEST(Stage4Recovery, MissingDecisionResultRerunBackfillsExactlyOneDecisionEvent) {
        using savor::db::core::DBService;
        using savor::db::execution::workflow::SqliteExecutionDb;

        std::filesystem::path temp_dir;
        std::unique_ptr<DBService> service;
        SqliteExecutionDb* execution_db = nullptr;
        std::string err;
        ASSERT_TRUE(OpenPhase4ExecutionDb("missing-decision", &temp_dir, &service, &execution_db, &err)) << err;

        ASSERT_TRUE(execution_db->ValidationExecuteSql(R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(4501, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'phase4-test', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, attempts, max_attempts, created_at_utc, ready_at_utc)
VALUES(4502, 4501, 'Grid', 'seedprobe.grid', 'RUNNING', 1, 2, unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_event(workflow_instance_id, workflow_step_id, event_kind, event_ts_utc, message)
VALUES(4501, 4502, 'Execution.WorkflowTransitionEvaluated.v1', unixepoch()*1000, 'decision_evaluated_without_result');
)SQL", &err))
            << err;

        std::int64_t existing_decisions = 0;
        ASSERT_TRUE(execution_db->ValidationQueryInt(
            "SELECT COUNT(1) FROM exec_workflow_event WHERE workflow_instance_id=4501 AND workflow_step_id=4502 AND event_kind IN ('Execution.WorkflowTransitionAdvanced.v1','Execution.WorkflowTransitionBlocked.v1');",
            &existing_decisions,
            &err))
            << err;
        EXPECT_EQ(existing_decisions, 0);

        ASSERT_TRUE(execution_db->WorkflowCommandService()->AppendLifecycleEvent(
            {
                .workflow_instance_id = 4501,
                .workflow_step_id = 4502,
                .event_kind = "Execution.WorkflowTransitionBlocked.v1",
                .message = std::optional<std::string>("restart_rerun_backfilled_missing_decision_result"),
                .requested_by = "phase4-test",
            },
            &err))
            << err;

        std::int64_t backfilled_decisions = 0;
        ASSERT_TRUE(execution_db->ValidationQueryInt(
            "SELECT COUNT(1) FROM exec_workflow_event WHERE workflow_instance_id=4501 AND workflow_step_id=4502 AND event_kind='Execution.WorkflowTransitionBlocked.v1';",
            &backfilled_decisions,
            &err))
            << err;
        EXPECT_EQ(backfilled_decisions, 1);

        CleanupPhase4Db(service, temp_dir);
    }

    TEST(Stage4Readiness, ObservabilityRetentionSignalsAndPolicyThresholdsAreCoherent) {
        using savor::db::core::DBService;
        using savor::db::execution::workflow::SqliteExecutionDb;

        std::filesystem::path temp_dir;
        std::unique_ptr<DBService> service;
        SqliteExecutionDb* execution_db = nullptr;
        std::string err;
        ASSERT_TRUE(OpenPhase4ExecutionDb("observability", &temp_dir, &service, &execution_db, &err)) << err;

        ASSERT_TRUE(execution_db->ValidationExecuteSql(R"SQL(
INSERT INTO exec_workflow_instance(workflow_instance_id, workflow_kind, state, root_scope_kind, created_by, created_at_utc, started_at_utc)
VALUES(4601, 'SEED_PROBE_CHAIN', 'RUNNING', 'manual', 'phase4-test', unixepoch()*1000, unixepoch()*1000);
INSERT INTO exec_workflow_step(workflow_step_id, workflow_instance_id, step_key, step_kind, state, attempts, max_attempts, created_at_utc, ready_at_utc, blocked_reason)
VALUES
(4602, 4601, 'Neutral', 'seedprobe.neutral', 'RUNNING', 1, 2, unixepoch()*1000, unixepoch()*1000, NULL),
(4603, 4601, 'Grid', 'seedprobe.grid', 'RUNNING', 1, 2, unixepoch()*1000, unixepoch()*1000, 'STEP_BLOCKED_COUNT_MISMATCH'),
(4604, 4601, 'Unique', 'seedprobe.unique', 'RUNNING', 1, 2, unixepoch()*1000, unixepoch()*1000, 'STEP_BLOCKED_COUNT_MISMATCH');
INSERT INTO exec_workflow_event(workflow_instance_id, workflow_step_id, event_kind, event_ts_utc, message)
VALUES
(4601, 4602, 'Execution.WorkflowTransitionEvaluated.v1', unixepoch()*1000, 'eval-1'),
(4601, 4602, 'Execution.WorkflowTransitionEvaluated.v1', unixepoch()*1000, 'eval-2'),
(4601, 4602, 'Execution.WorkflowTransitionEvaluated.v1', unixepoch()*1000, 'eval-3'),
(4601, 4602, 'Execution.WorkflowTransitionEvaluated.v1', unixepoch()*1000, 'eval-4'),
(4601, 4603, 'Execution.WorkflowTransitionEvaluated.v1', unixepoch()*1000, 'eval-5');
INSERT INTO exec_handler_dedupe(handler_name, event_id, semantic_key, first_seen_at_utc, last_seen_at_utc)
VALUES
('workflow_terminal_subscriber', 'evt-1', 'workflow_step:4602:terminal:COMPLETED', unixepoch()*1000-6*3600*1000, unixepoch()*1000-6*3600*1000),
('workflow_terminal_subscriber', 'evt-2', 'workflow_step:4603:terminal:FAILED', unixepoch()*1000-5*3600*1000, unixepoch()*1000-5*3600*1000),
('workflow_terminal_subscriber', 'evt-3', 'workflow_step:4604:terminal:FAILED', unixepoch()*1000-4*3600*1000, unixepoch()*1000-4*3600*1000),
('workflow_terminal_subscriber', 'evt-4', 'workflow_step:4605:terminal:FAILED', unixepoch()*1000-1*3600*1000, unixepoch()*1000-1*3600*1000),
('workflow_terminal_subscriber', 'evt-5', 'workflow_step:4606:terminal:FAILED', unixepoch()*1000-30*60*1000, unixepoch()*1000-30*60*1000),
('workflow_terminal_subscriber', 'evt-6', 'workflow_step:4607:terminal:FAILED', unixepoch()*1000-20*60*1000, unixepoch()*1000-20*60*1000),
('workflow_terminal_subscriber', 'evt-7', 'workflow_step:4608:terminal:FAILED', unixepoch()*1000-10*60*1000, unixepoch()*1000-10*60*1000),
('workflow_terminal_subscriber', 'evt-8', 'workflow_step:4609:terminal:FAILED', unixepoch()*1000-5*60*1000, unixepoch()*1000-5*60*1000);
)SQL", &err))
            << err;

        std::int64_t completion_checks = 0;
        std::int64_t completion_mismatches = 0;
        std::int64_t replay_loop_steps = 0;
        std::int64_t historical_dedupe_rows = 0;
        std::int64_t current_dedupe_rows = 0;
        ASSERT_TRUE(execution_db->ValidationQueryInt(
                        "SELECT COUNT(1) FROM exec_workflow_event WHERE event_kind='Execution.WorkflowTransitionEvaluated.v1';",
                        &completion_checks,
                        &err)
                    && execution_db->ValidationQueryInt(
                        "SELECT COUNT(1) FROM exec_workflow_step WHERE blocked_reason='STEP_BLOCKED_COUNT_MISMATCH';",
                        &completion_mismatches,
                        &err)
                    && execution_db->ValidationQueryInt(
                        R"SQL(SELECT COUNT(1) FROM (
                                SELECT workflow_step_id, COUNT(1) AS attempts
                                FROM exec_workflow_event
                                WHERE event_kind='Execution.WorkflowTransitionEvaluated.v1'
                                GROUP BY workflow_step_id
                                HAVING attempts >= 3
                            );)SQL",
                        &replay_loop_steps,
                        &err)
                    && execution_db->ValidationQueryInt(
                        "SELECT COUNT(1) FROM exec_handler_dedupe WHERE last_seen_at_utc < unixepoch()*1000-3600*1000;",
                        &historical_dedupe_rows,
                        &err)
                    && execution_db->ValidationQueryInt(
                        "SELECT COUNT(1) FROM exec_handler_dedupe WHERE last_seen_at_utc >= unixepoch()*1000-3600*1000;",
                        &current_dedupe_rows,
                        &err))
            << err;

        ASSERT_GT(completion_checks, 0);
        ASSERT_GE(completion_mismatches, 0);
        ASSERT_LE(completion_mismatches, completion_checks);
        const double completion_gate_mismatch_frequency =
            static_cast<double>(completion_mismatches) / static_cast<double>(completion_checks);

        ASSERT_GT(historical_dedupe_rows, 0);
        ASSERT_GE(current_dedupe_rows, 0);
        const double dedupe_growth_ratio =
            static_cast<double>(current_dedupe_rows) / static_cast<double>(historical_dedupe_rows);

        EXPECT_GE(kPhase4DedupeTtlHours, kPhase4MinDedupeTtlHours);
        EXPECT_LE(kPhase4DedupeTtlHours, kPhase4MaxDedupeTtlHours);
        EXPECT_GE(kPhase4ClaimedJobStagingCleanupHours, kPhase4MinClaimedJobCleanupHours);
        EXPECT_LE(kPhase4ClaimedJobStagingCleanupHours, kPhase4MaxClaimedJobCleanupHours);

        const bool escalation_policy_present = kPhase4CompletionGateMismatchWarnFrequency > 0.0
            && kPhase4CompletionGateMismatchPageFrequency > kPhase4CompletionGateMismatchWarnFrequency
            && kPhase4ReplayLoopWarnCount > 0
            && kPhase4ReplayLoopPageCount > kPhase4ReplayLoopWarnCount
            && kPhase4DedupeGrowthWarnRatio > 1.0
            && kPhase4DedupeGrowthPageRatio > kPhase4DedupeGrowthWarnRatio;
        EXPECT_TRUE(escalation_policy_present);

        EXPECT_TRUE(std::isfinite(completion_gate_mismatch_frequency));
        EXPECT_GE(completion_gate_mismatch_frequency, 0.0);
        EXPECT_LE(completion_gate_mismatch_frequency, 1.0);
        EXPECT_GE(replay_loop_steps, 0);
        EXPECT_TRUE(std::isfinite(dedupe_growth_ratio));
        EXPECT_GT(dedupe_growth_ratio, 0.0);

        CleanupPhase4Db(service, temp_dir);
    }

    TEST(WorkflowUnitActivation, SchemaCreatesActivationTablesAndStepOwnershipColumn) {
        std::filesystem::path temp_dir;
        std::unique_ptr<savor::db::core::DBService> service;
        savor::db::execution::workflow::SqliteExecutionDb* execution_db = nullptr;
        std::string err;
        ASSERT_TRUE(OpenPhase4ExecutionDb("unit-activation-schema", &temp_dir, &service, &execution_db, &err)) << err;

        std::int64_t activation_tables = 0;
        std::int64_t step_activation_columns = 0;
        ASSERT_TRUE(execution_db->ValidationQueryInt(
            "SELECT COUNT(1) FROM sqlite_master "
            "WHERE type='table' AND name IN ('exec_workflow_unit_activation','exec_workflow_unit_activation_edge');",
            &activation_tables,
            &err))
            << err;
        ASSERT_TRUE(execution_db->ValidationQueryInt(
            "SELECT COUNT(1) FROM pragma_table_info('exec_workflow_step') WHERE name='workflow_unit_activation_id';",
            &step_activation_columns,
            &err))
            << err;
        EXPECT_EQ(activation_tables, 2);
        EXPECT_EQ(step_activation_columns, 1);

        CleanupPhase4Db(service, temp_dir);
    }

    TEST(WorkflowUnitActivation, CreateInstanceOwnsStepsAndDerivesActivationState) {
        using namespace savor::db::execution::workflow;

        std::filesystem::path temp_dir;
        std::unique_ptr<savor::db::core::DBService> service;
        SqliteExecutionDb* execution_db = nullptr;
        std::string err;
        ASSERT_TRUE(OpenPhase4ExecutionDb("unit-activation-create", &temp_dir, &service, &execution_db, &err)) << err;

        auto* commands = execution_db->WorkflowCommandService();
        auto* queries = execution_db->WorkflowQueryService();
        ASSERT_NE(commands, nullptr);
        ASSERT_NE(queries, nullptr);

        WorkflowCreateUnitStepSpec child{};
        child.step_key = "probe_1";
        child.step_kind = "seed_probe_chain";
        child.priority = 0;
        child.max_attempts = 1;

        WorkflowCreateUnitActivationSpec activation{};
        activation.activation_key = "probe_1";
        activation.graph_node_key = "probe_1";
        activation.unit_kind = "battle_seed_probe";
        activation.display_name = "Battle Seed Probe";
        activation.activation_params_json = "{\"breakpoint_profile_key\":\"seedprobe.battle\"}";
        activation.authored_ref_kind = "seed_probe_spec";
        activation.authored_ref_id = 42;
        activation.steps.push_back(child);

        WorkflowCreateInstanceCommand command{};
        command.workflow_kind = "workflow_graph";
        command.workflow_graph_revision_id = 7;
        command.root_scope_kind = "manual";
        command.created_by = "SavorTests";
        command.unit_activations.push_back(activation);

        std::int64_t workflow_instance_id = 0;
        ASSERT_TRUE(commands->CreateWorkflowInstance(command, &workflow_instance_id, &err)) << err;
        ASSERT_GT(workflow_instance_id, 0);

        auto graph = queries->GetWorkflowGraph(workflow_instance_id);
        ASSERT_TRUE(graph.has_value());
        ASSERT_EQ(graph->unit_activations.size(), 1u);
        ASSERT_EQ(graph->steps.size(), 1u);
        EXPECT_EQ(graph->unit_activations.front().activation_key, "probe_1");
        EXPECT_EQ(graph->unit_activations.front().unit_kind, "battle_seed_probe");
        EXPECT_EQ(graph->unit_activations.front().state, WorkflowUnitActivationState::Ready);
        ASSERT_TRUE(graph->steps.front().workflow_unit_activation_id.has_value());
        EXPECT_EQ(*graph->steps.front().workflow_unit_activation_id, graph->unit_activations.front().workflow_unit_activation_id);

        std::int64_t job_set_id = 0;
        ASSERT_TRUE(execution_db->CreateJobSet(
            {
                .program_kind = 1,
                .purpose = "workflow-unit-activation-test",
                .created_by = std::string("SavorTests"),
                .priority_boost = 0,
                .expected_total = 1,
                .domain_ref_kind = std::string("unit.input"),
                .domain_ref_id = 9001,
            },
            &job_set_id,
            &err))
            << err;
        ASSERT_TRUE(commands->MarkStepMaterialized(
            {
                .workflow_step_id = graph->steps.front().workflow_step_id,
                .job_set_id = job_set_id,
                .requested_by = "SavorTests",
            },
            &err))
            << err;

        graph = queries->GetWorkflowGraph(workflow_instance_id);
        ASSERT_TRUE(graph.has_value());
        ASSERT_EQ(graph->unit_activations.size(), 1u);
        EXPECT_EQ(graph->unit_activations.front().state, WorkflowUnitActivationState::Running);

        ASSERT_TRUE(commands->MarkStepTerminal(
            {
                .workflow_step_id = graph->steps.front().workflow_step_id,
                .terminal_state = "COMPLETED",
                .requested_by = "SavorTests",
            },
            &err))
            << err;

        graph = queries->GetWorkflowGraph(workflow_instance_id);
        ASSERT_TRUE(graph.has_value());
        ASSERT_EQ(graph->unit_activations.size(), 1u);
        EXPECT_EQ(graph->unit_activations.front().state, WorkflowUnitActivationState::Completed);

        CleanupPhase4Db(service, temp_dir);
    }

    TEST(WorkflowUnitActivation, ScheduleUnitActivationAppendsRuntimeGroupWithEdgeAndOwnedStep) {
        using namespace savor::db::execution::workflow;

        std::filesystem::path temp_dir;
        std::unique_ptr<savor::db::core::DBService> service;
        SqliteExecutionDb* execution_db = nullptr;
        std::string err;
        ASSERT_TRUE(OpenPhase4ExecutionDb("unit-activation-schedule", &temp_dir, &service, &execution_db, &err)) << err;

        auto* commands = execution_db->WorkflowCommandService();
        auto* queries = execution_db->WorkflowQueryService();
        ASSERT_NE(commands, nullptr);
        ASSERT_NE(queries, nullptr);

        WorkflowCreateUnitStepSpec source_child{};
        source_child.step_key = "tas_1";
        source_child.step_kind = "tas_movie";
        source_child.priority = 0;
        source_child.max_attempts = 1;

        WorkflowCreateUnitActivationSpec source{};
        source.activation_key = "tas_1";
        source.graph_node_key = "tas_1";
        source.unit_kind = "tas_movie";
        source.display_name = "TAS Movie";
        source.steps.push_back(source_child);

        WorkflowCreateInstanceCommand create{};
        create.workflow_kind = "workflow_graph";
        create.workflow_graph_revision_id = 8;
        create.root_scope_kind = "manual";
        create.created_by = "SavorTests";
        create.unit_activations.push_back(source);

        std::int64_t workflow_instance_id = 0;
        ASSERT_TRUE(commands->CreateWorkflowInstance(create, &workflow_instance_id, &err)) << err;
        auto graph = queries->GetWorkflowGraph(workflow_instance_id);
        ASSERT_TRUE(graph.has_value());
        ASSERT_EQ(graph->unit_activations.size(), 1u);
        const auto source_activation_id = graph->unit_activations.front().workflow_unit_activation_id;

        WorkflowCreateUnitStepSpec scheduled_child{};
        scheduled_child.step_key = "dungeon_1";
        scheduled_child.step_kind = "dungeon_explorer";
        scheduled_child.priority = 0;
        scheduled_child.max_attempts = 1;

        WorkflowScheduleUnitActivationCommand schedule{};
        schedule.workflow_instance_id = workflow_instance_id;
        schedule.source_workflow_unit_activation_id = source_activation_id;
        schedule.activation_key = "dungeon_1";
        schedule.graph_node_key = "dungeon_1";
        schedule.unit_kind = "dungeon_explorer";
        schedule.display_name = "Dungeon Explorer";
        schedule.activation_params_json = "{\"reason\":\"decider\"}";
        schedule.steps.push_back(scheduled_child);

        ASSERT_TRUE(commands->ScheduleUnitActivation(schedule, &err)) << err;

        graph = queries->GetWorkflowGraph(workflow_instance_id);
        ASSERT_TRUE(graph.has_value());
        ASSERT_EQ(graph->unit_activations.size(), 2u);
        const auto scheduled_activation_it = std::find_if(
            graph->unit_activations.begin(),
            graph->unit_activations.end(),
            [](const auto& activation) { return activation.activation_key == "dungeon_1"; });
        ASSERT_NE(scheduled_activation_it, graph->unit_activations.end());
        const auto scheduled_activation_id = scheduled_activation_it->workflow_unit_activation_id;
        ASSERT_EQ(graph->unit_activation_edges.size(), 1u);
        EXPECT_EQ(graph->unit_activation_edges.front().from_workflow_unit_activation_id, source_activation_id);
        EXPECT_EQ(graph->unit_activation_edges.front().to_workflow_unit_activation_id, scheduled_activation_id);

        const auto child_it = std::find_if(
            graph->steps.begin(),
            graph->steps.end(),
            [](const auto& step) { return step.step_key == "dungeon_1"; });
        ASSERT_NE(child_it, graph->steps.end());
        ASSERT_TRUE(child_it->workflow_unit_activation_id.has_value());
        EXPECT_EQ(*child_it->workflow_unit_activation_id, scheduled_activation_id);

        CleanupPhase4Db(service, temp_dir);
    }

}
