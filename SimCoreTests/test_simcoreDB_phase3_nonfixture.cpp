#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include <gtest/gtest.h>

#include "Execution/Workflow/WorkflowModeProvider.h"
#include "Runner/Parallel/SimCoreDB/DBWorkflowWorkerCoordinator.h"
#include "Runner/Parallel/SimCoreDB/WorkflowDispatchCoordinator.h"
#include "Runner/Parallel/SimCoreDB/WorkflowMaterializationService.h"
#include "Runner/Parallel/SimCoreDB/WorkflowSchedulerAdapter.h"

namespace simcoreDB {
    TEST(Stage3Phase3Telemetry, CapturesMaterializationAndDispatchCounters) {
        using namespace simcore::runner::parallel::simcoredb;
        using namespace simcore::db::execution::workflow;

        StaticWorkflowModeProvider mode_provider({ .mode = WorkflowExecutionMode::Workflow, .source = "phase3-telemetry-test" });

        DBWorkflowWorkerCoordinator coordinator(
            nullptr,
            &mode_provider,
            DBWorkflowWorkerCoordinatorConfig{
                .desired_workers = 0,
                .controller_sleep_ms = 1,
            },
            CoordinatorIntegrationConfig{},
            [](const WorkflowReadyStep& step) {
                return ScheduledJobSet{
                    .job_set_id = 13000 + step.workflow_step_id,
                    .workflow_step_id = step.workflow_step_id,
                };
            });

        coordinator.EnqueueReadyStep({
            .workflow_instance_id = 10,
            .workflow_step_id = 20,
            .step_key = "Neutral",
            .step_kind = "seedprobe.neutral",
            .priority = 1,
            });

        coordinator.Start();
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        coordinator.Stop();

        const auto telemetry = coordinator.SnapshotTelemetry();
        EXPECT_GE(telemetry.materialization_count, 1);
        EXPECT_GE(telemetry.last_materialization_latency_ms, 0);
        EXPECT_GE(telemetry.max_materialization_latency_ms, 0);
        EXPECT_GE(telemetry.dispatch_attempt_count, 0);
        EXPECT_GE(telemetry.dispatch_miss_count, 0);
        EXPECT_GE(telemetry.dispatch_miss_rate_basis_points, 0);
    }

    TEST(Stage3Phase3Batching, ProgressCallbacksDrainInBatchesAndTerminalResultsAreNotBlocked) {
        using namespace simcore::runner::parallel::simcoredb;
        using namespace simcore::db::execution::workflow;

        StaticWorkflowModeProvider mode_provider({ .mode = WorkflowExecutionMode::Workflow, .source = "phase3-batching-test" });
        DBWorkflowWorkerCoordinator coordinator(
            nullptr,
            &mode_provider,
            DBWorkflowWorkerCoordinatorConfig{
                .desired_workers = 0,
                .controller_sleep_ms = 1,
            },
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
        coordinator.SetProgressCallback([&](const simcore::PRProgress&) {
            ++progress_seen;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            });
        coordinator.SetResultCallback([&](const simcore::PRResult&) {
            terminal_seen_promise.set_value();
            });

        coordinator.Start();
        for (int i = 0; i < 200; ++i) {
            coordinator.EnqueueProgressForTest(simcore::PRProgress{
                .worker_id = 0,
                .job_id = static_cast<std::uint64_t>(1000 + i),
                .text = "progress",
                });
        }
        coordinator.EnqueueResultForTest(simcore::PRResult{
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
    }

    TEST(Stage3Phase3DispatchGuard, ClaimedJobsAreNotDispatchedBeforeMaterialization) {
        using namespace simcore::runner::parallel::simcoredb;

        WorkflowSchedulerAdapter scheduler([](const WorkflowReadyStep& step) {
            return ScheduledJobSet{
                .job_set_id = 15000 + step.workflow_step_id,
                .workflow_step_id = step.workflow_step_id,
            };
            });

        std::vector<ClaimedJobSeed> claimed{
            ClaimedJobSeed{
                .step = WorkflowReadyStep{
                    .workflow_instance_id = 1,
                    .workflow_step_id = 2,
                    .step_key = "Neutral",
                    .step_kind = "seedprobe.neutral",
                    .priority = 1,
                },
                .job_set_id = 42,
                .job_id = 4242,
            },
        };

        WorkflowMaterializationService materialization(
            &scheduler,
            [&]() { return claimed; },
            [](std::int64_t job_id, const WorkflowReadyStep&) -> std::optional<simcore::PSJob> {
                simcore::PSJob job{};
                (void)job_id;
                return job;
            },
            {},
            {});

        int dispatch_calls = 0;
        WorkflowDispatchCoordinator dispatch(
            &materialization,
            [&](std::size_t, const ClaimedJobRecord&) {
                ++dispatch_calls;
                return true;
            });

        const auto now = std::chrono::steady_clock::now();
        EXPECT_EQ(materialization.ClaimJobs(now), 1u);
        EXPECT_FALSE(dispatch.DispatchNextEligibleForWorker(0, std::nullopt, now));
        EXPECT_EQ(dispatch_calls, 0);

        EXPECT_TRUE(materialization.MaterializeClaimedJobPayload(now));
        EXPECT_TRUE(dispatch.DispatchNextEligibleForWorker(0, std::nullopt, now));
        EXPECT_EQ(dispatch_calls, 1);
    }

}