#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Execution/DBWorkflowWorkerCoordinator.h"
#include "Runner/Runtime/RuntimeTypes.h"
#include "Utils/ModulePath.h"
#include "Worker/WorkerCapabilityPreflight.h"
#include "common/RecordingExecutionDb.h"

namespace {

using savor::runner::parallel::savordb::CoordinatorIntegrationConfig;
using savor::runner::parallel::savordb::CoordinatorStartStatus;
using savor::runner::parallel::savordb::CoordinatorWorkerCapabilityPreflightResult;
using savor::runner::parallel::savordb::DBWorkflowWorkerCoordinator;
using savor::runner::parallel::savordb::DBWorkflowWorkerCoordinatorConfig;

class GateRecordingExecutionDb final : public RecordingExecutionDb
{
public:
    std::vector<savor::db::ClaimedExecutionJob> ClaimBatchReadyExecutionJobs(
        std::string_view,
        int requested_jobs,
        std::int64_t,
        std::string* error_out = nullptr) override
    {
        last_claim_budget.store(requested_jobs, std::memory_order_release);
        claim_calls.fetch_add(1, std::memory_order_acq_rel);
        if (!claim_signaled.exchange(true, std::memory_order_acq_rel))
            first_claim.set_value();
        if (error_out)
            error_out->clear();
        return {};
    }

    bool RequeueExpiredClaimedExecutionJobs(
        int* rows_requeued_out = nullptr,
        std::string* error_out = nullptr) override
    {
        recovery_calls.fetch_add(1, std::memory_order_acq_rel);
        if (rows_requeued_out)
            *rows_requeued_out = 0;
        if (error_out)
            error_out->clear();
        return true;
    }

    [[nodiscard]] int DataPlaneCallCount() const
    {
        return claim_calls.load(std::memory_order_acquire) +
            recovery_calls.load(std::memory_order_acquire);
    }

    std::atomic<int> claim_calls{0};
    std::atomic<int> recovery_calls{0};
    std::atomic<int> last_claim_budget{0};
    std::promise<void> first_claim;

private:
    std::atomic<bool> claim_signaled{false};
};

DBWorkflowWorkerCoordinatorConfig BaseConfig()
{
    DBWorkflowWorkerCoordinatorConfig config;
    config.desired_workers = 1;
    config.controller_sleep_ms = 1;
    config.max_concurrent_worker_starts = 1;
    return config;
}

std::filesystem::path FindBuiltWorker()
{
    const auto test_directory = utils::getExecutablePath();
    const std::array candidates{
        test_directory / "SavorWorker.exe",
        test_directory.parent_path().parent_path() /
            "bin" /
            test_directory.parent_path().filename() /
            test_directory.filename() /
            "SavorWorker.exe",
    };
    std::error_code error;
    for (const auto& candidate : candidates)
    {
        if (std::filesystem::is_regular_file(candidate, error) && !error)
            return candidate;
        error.clear();
    }
    return {};
}

template <typename StartDbCallback>
savor::WorkerCapabilityPreflightResult RunCallerStylePreflight(
    const std::filesystem::path& worker_path,
    StartDbCallback&& start_db)
{
    const auto result = savor::RunWorkerCapabilityPreflight(
        savor::WorkerCapabilityPreflightRequest{
            .worker_exe_path = worker_path.string(),
            .timeout_ms = 10000,
            .required_capabilities = savor::runtime::CapabilityMask(
                savor::runtime::WorkerCapability::ProgramInvocation),
        });
    if (result)
        std::forward<StartDbCallback>(start_db)();
    return result;
}

} // namespace

TEST(WorkerCapabilityGate, SessionOnlyCapabilitiesFailClosedBeforeAnyDataPlaneWork)
{
    GateRecordingExecutionDb execution_db;
    std::atomic<int> preflight_calls{0};
    std::atomic<int> callback_calls{0};
    std::atomic<int> db_calls_observed_by_preflight{-1};

    auto config = BaseConfig();
    config.worker_capability_preflight =
        [&](size_t worker_id,
            const DBWorkflowWorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>& worker) {
            EXPECT_EQ(worker_id, 0u);
            EXPECT_NE(worker, nullptr);
            db_calls_observed_by_preflight.store(
                execution_db.DataPlaneCallCount(),
                std::memory_order_release);
            preflight_calls.fetch_add(1, std::memory_order_acq_rel);
            return CoordinatorWorkerCapabilityPreflightResult{
                .process_ready = true,
                .capabilities = savor::runtime::kSlice1ProductionCapabilities,
                .error = "canonical ProgramRuntime is unavailable",
            };
        };

    DBWorkflowWorkerCoordinator coordinator(
        &execution_db,
        std::move(config),
        CoordinatorIntegrationConfig{});
    coordinator.SetWorkflowMaterializationCallback(
        [&](std::int64_t, std::int64_t) { ++callback_calls; });
    coordinator.SetWorkflowTerminalCallback(
        [&](const auto&) { ++callback_calls; });
    coordinator.SetWorkflowCreatedCallback(
        [&](const auto&) { ++callback_calls; });
    coordinator.SetProgressCallback(
        [&](const auto&) { ++callback_calls; });
    coordinator.SetResultCallback(
        [&](const auto&) { ++callback_calls; });
    coordinator.SetResultMapEventCallback(
        [&](const auto&) { ++callback_calls; });

    const auto first = coordinator.Start();
    EXPECT_FALSE(first);
    EXPECT_EQ(first.status, CoordinatorStartStatus::ProgramInvocationUnavailable);
    EXPECT_TRUE(first.non_retryable);
    EXPECT_EQ(first.ready_invocation_capable_workers, 0u);
    EXPECT_NE(first.error.find("ProgramInvocation"), std::string::npos);
    EXPECT_FALSE(coordinator.IsDataPlaneEnabled());
    EXPECT_EQ(coordinator.ReadyInvocationCapableWorkerCount(), 0u);
    EXPECT_EQ(coordinator.ActiveWorkerCount(), 0u);

    const auto second = coordinator.Start();
    EXPECT_EQ(second.status, first.status);
    EXPECT_EQ(second.non_retryable, first.non_retryable);
    EXPECT_EQ(second.error, first.error);
    EXPECT_EQ(preflight_calls.load(), 1);
    EXPECT_EQ(db_calls_observed_by_preflight.load(), 0);

    EXPECT_FALSE(coordinator.PublishWorkflowCreated({.workflow_instance_id = 1}));
    EXPECT_FALSE(coordinator.PublishTerminalJobSet({
        .workflow_instance_id = 1,
        .workflow_step_id = 2,
        .job_set_id = 3,
        .terminal_state = "COMPLETED",
    }));
    coordinator.EnqueueReadyStep({
        .workflow_instance_id = 1,
        .workflow_step_id = 2,
        .step_key = "blocked",
        .step_kind = "test.blocked",
    });
    EXPECT_FALSE(coordinator.MaterializeWorkflowStep({
        .workflow_instance_id = 1,
        .workflow_step_id = 2,
        .step_key = "blocked",
        .step_kind = "test.blocked",
    }).has_value());
    coordinator.EnqueueProgressForTest({
        .worker_id = 0,
        .job_id = 4,
        .text = "must not drain",
    });
    coordinator.EnqueueResultForTest({
        .job_id = 4,
        .worker_id = 0,
        .accepted = true,
    });

    EXPECT_EQ(coordinator.SnapshotStatus().queued_jobs, 0u);
    EXPECT_EQ(execution_db.DataPlaneCallCount(), 0);
    EXPECT_EQ(callback_calls.load(), 0);
    EXPECT_TRUE(execution_db.command_service.materialized_calls.empty());
    EXPECT_TRUE(execution_db.command_service.lifecycle_events.empty());
    EXPECT_TRUE(execution_db.command_service.terminal_calls.empty());

    coordinator.Stop();
}

TEST(WorkerCapabilityGate, InvocationCapableButErrorfulPreflightFailsClosed)
{
    GateRecordingExecutionDb execution_db;
    auto config = BaseConfig();
    config.worker_capability_preflight =
        [](size_t,
            const DBWorkflowWorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>&) {
            return CoordinatorWorkerCapabilityPreflightResult{
                .process_ready = true,
                .capabilities = savor::runtime::AddCapability(
                    savor::runtime::kSlice1ProductionCapabilities,
                    savor::runtime::WorkerCapability::ProgramInvocation),
                .error = "session negotiation did not complete",
            };
        };

    DBWorkflowWorkerCoordinator coordinator(
        &execution_db,
        std::move(config),
        CoordinatorIntegrationConfig{});

    const auto result = coordinator.Start();
    EXPECT_FALSE(result);
    EXPECT_EQ(
        result.status,
        CoordinatorStartStatus::CapabilityPreflightFailed);
    EXPECT_NE(
        result.error.find("session negotiation did not complete"),
        std::string::npos);
    EXPECT_FALSE(coordinator.IsDataPlaneEnabled());
    EXPECT_EQ(coordinator.ReadyInvocationCapableWorkerCount(), 0u);
    EXPECT_EQ(execution_db.DataPlaneCallCount(), 0);

    coordinator.Stop();
}

TEST(WorkerCapabilityGate, VisualDebugRequiresInteractiveCapability)
{
    GateRecordingExecutionDb execution_db;
    auto config = BaseConfig();
    config.visual_debug_workers = true;
    config.worker_capability_preflight =
        [](size_t,
            const DBWorkflowWorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>&) {
            return CoordinatorWorkerCapabilityPreflightResult{
                .process_ready = true,
                .capabilities = savor::runtime::AddCapability(
                    savor::runtime::kSlice1ProductionCapabilities,
                    savor::runtime::WorkerCapability::ProgramInvocation),
            };
        };

    DBWorkflowWorkerCoordinator coordinator(
        &execution_db,
        std::move(config),
        CoordinatorIntegrationConfig{});

    const auto result = coordinator.Start();
    EXPECT_FALSE(result);
    EXPECT_EQ(
        result.status,
        CoordinatorStartStatus::InteractiveVisualDebugUnavailable);
    EXPECT_TRUE(result.non_retryable);
    EXPECT_NE(
        result.error.find("InteractiveVisualDebug"),
        std::string::npos);
    EXPECT_FALSE(coordinator.IsDataPlaneEnabled());
    EXPECT_EQ(coordinator.ReadyInvocationCapableWorkerCount(), 0u);
    EXPECT_EQ(execution_db.DataPlaneCallCount(), 0);

    coordinator.Stop();
}

TEST(WorkerCapabilityGate, InvocationCapablePreflightEnablesDataPlaneAndReadyCapacity)
{
    GateRecordingExecutionDb execution_db;
    auto first_claim = execution_db.first_claim.get_future();
    std::atomic<int> preflight_calls{0};
    std::atomic<int> db_calls_observed_by_preflight{-1};

    auto config = BaseConfig();
    config.worker_capability_preflight =
        [&](size_t worker_id,
            const DBWorkflowWorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>& worker) {
            EXPECT_EQ(worker_id, 0u);
            EXPECT_NE(worker, nullptr);
            db_calls_observed_by_preflight.store(
                execution_db.DataPlaneCallCount(),
                std::memory_order_release);
            preflight_calls.fetch_add(1, std::memory_order_acq_rel);
            return CoordinatorWorkerCapabilityPreflightResult{
                .process_ready = true,
                .capabilities = savor::runtime::AddCapability(
                    savor::runtime::kSlice1ProductionCapabilities,
                    savor::runtime::WorkerCapability::ProgramInvocation),
            };
        };

    DBWorkflowWorkerCoordinator coordinator(
        &execution_db,
        std::move(config),
        CoordinatorIntegrationConfig{});

    const auto first = coordinator.Start();
    ASSERT_TRUE(first) << first.error;
    EXPECT_EQ(first.status, CoordinatorStartStatus::Started);
    EXPECT_FALSE(first.non_retryable);
    EXPECT_EQ(first.ready_invocation_capable_workers, 1u);
    EXPECT_TRUE(coordinator.IsDataPlaneEnabled());
    EXPECT_EQ(coordinator.ReadyInvocationCapableWorkerCount(), 1u);
    EXPECT_EQ(coordinator.SnapshotStatus().ready_workers, 1u);

    const auto second = coordinator.Start();
    EXPECT_EQ(second.status, first.status);
    EXPECT_EQ(second.ready_invocation_capable_workers, 1u);
    EXPECT_EQ(preflight_calls.load(), 1);
    EXPECT_EQ(db_calls_observed_by_preflight.load(), 0);

    ASSERT_EQ(
        first_claim.wait_for(std::chrono::seconds(2)),
        std::future_status::ready);
    EXPECT_GE(execution_db.claim_calls.load(), 1);
    EXPECT_EQ(execution_db.last_claim_budget.load(), 1);

    coordinator.Stop();
    EXPECT_FALSE(coordinator.IsDataPlaneEnabled());
    EXPECT_EQ(coordinator.ReadyInvocationCapableWorkerCount(), 0u);
}

TEST(WorkerCapabilityGate, StopSerializesWithInFlightStart)
{
    GateRecordingExecutionDb execution_db;
    std::promise<void> preflight_entered_promise;
    auto preflight_entered = preflight_entered_promise.get_future();
    std::promise<void> release_preflight_promise;
    auto release_preflight = release_preflight_promise.get_future().share();

    auto config = BaseConfig();
    config.worker_capability_preflight =
        [&](size_t,
            const DBWorkflowWorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>&) {
            preflight_entered_promise.set_value();
            release_preflight.wait();
            return CoordinatorWorkerCapabilityPreflightResult{
                .process_ready = true,
                .capabilities = savor::runtime::AddCapability(
                    savor::runtime::kSlice1ProductionCapabilities,
                    savor::runtime::WorkerCapability::ProgramInvocation),
            };
        };

    DBWorkflowWorkerCoordinator coordinator(
        &execution_db,
        std::move(config),
        CoordinatorIntegrationConfig{});

    auto start = std::async(
        std::launch::async,
        [&]() { return coordinator.Start(); });
    ASSERT_EQ(
        preflight_entered.wait_for(std::chrono::seconds(2)),
        std::future_status::ready);

    auto stop = std::async(
        std::launch::async,
        [&]() { coordinator.Stop(); });
    EXPECT_EQ(
        stop.wait_for(std::chrono::milliseconds(0)),
        std::future_status::timeout);

    release_preflight_promise.set_value();
    const auto start_result = start.get();
    EXPECT_TRUE(start_result) << start_result.error;
    stop.get();

    EXPECT_FALSE(coordinator.IsDataPlaneEnabled());
    EXPECT_EQ(coordinator.ActiveWorkerCount(), 0u);
    EXPECT_EQ(coordinator.ReadyInvocationCapableWorkerCount(), 0u);
}

TEST(WorkerCapabilityGate, ScaleUpRevalidatesCapabilitiesAndStopJoinsPreflight)
{
    GateRecordingExecutionDb execution_db;
    std::promise<void> scale_up_entered_promise;
    auto scale_up_entered = scale_up_entered_promise.get_future();
    std::promise<void> release_scale_up_promise;
    auto release_scale_up = release_scale_up_promise.get_future().share();
    std::atomic<int> preflight_calls{0};

    auto config = BaseConfig();
    config.worker_capability_preflight =
        [&](size_t worker_id,
            const DBWorkflowWorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>&) {
            preflight_calls.fetch_add(1, std::memory_order_acq_rel);
            if (worker_id == 1) {
                scale_up_entered_promise.set_value();
                release_scale_up.wait();
            }
            return CoordinatorWorkerCapabilityPreflightResult{
                .process_ready = true,
                .capabilities = savor::runtime::AddCapability(
                    savor::runtime::kSlice1ProductionCapabilities,
                    savor::runtime::WorkerCapability::ProgramInvocation),
            };
        };

    DBWorkflowWorkerCoordinator coordinator(
        &execution_db,
        std::move(config),
        CoordinatorIntegrationConfig{});

    const auto start = coordinator.Start();
    ASSERT_TRUE(start) << start.error;
    coordinator.SetDesiredWorkerCount(2);
    ASSERT_EQ(
        scale_up_entered.wait_for(std::chrono::seconds(2)),
        std::future_status::ready);

    auto stop = std::async(
        std::launch::async,
        [&]() { coordinator.Stop(); });
    EXPECT_EQ(
        stop.wait_for(std::chrono::milliseconds(0)),
        std::future_status::timeout);

    release_scale_up_promise.set_value();
    stop.get();

    EXPECT_GE(preflight_calls.load(std::memory_order_acquire), 2);
    EXPECT_FALSE(coordinator.IsDataPlaneEnabled());
    EXPECT_EQ(coordinator.ActiveWorkerCount(), 0u);
    EXPECT_EQ(coordinator.ReadyInvocationCapableWorkerCount(), 0u);
}

TEST(WorkerCapabilityGate, BuiltSliceOneWorkerBlocksE2EAndPredictBeforeDbStart)
{
    const auto worker_path = FindBuiltWorker();
    if (worker_path.empty())
        GTEST_SKIP() << "SavorWorker.exe was not built beside the test outputs";

    for (const std::string_view caller : {"SavorE2E", "SavorPredict"})
    {
        SCOPED_TRACE(caller);
        std::atomic<int> db_start_calls{0};

        const auto result = RunCallerStylePreflight(
            worker_path,
            [&]() { db_start_calls.fetch_add(1, std::memory_order_acq_rel); });

        EXPECT_EQ(
            result.status,
            savor::WorkerCapabilityPreflightStatus::RuntimeUnavailable);
        EXPECT_TRUE(result.non_retryable);
        EXPECT_TRUE(savor::runtime::HasCapability(
            result.missing_capabilities,
            savor::runtime::WorkerCapability::ProgramInvocation));
        EXPECT_NE(result.message.find("RuntimeUnavailable"), std::string::npos);
        EXPECT_EQ(db_start_calls.load(std::memory_order_acquire), 0);
    }
}

TEST(WorkerCapabilityGate, VisualDebugReplayReportsSliceThreeDeferralWithoutDataPlaneWork)
{
    GateRecordingExecutionDb execution_db;
    std::atomic<int> preflight_calls{0};

    auto config = BaseConfig();
    config.worker_capability_preflight =
        [&](size_t,
            const DBWorkflowWorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>&) {
            preflight_calls.fetch_add(1, std::memory_order_acq_rel);
            return CoordinatorWorkerCapabilityPreflightResult{
                .process_ready = true,
                .capabilities = savor::runtime::kSlice1ProductionCapabilities,
                .error = "canonical ProgramRuntime is unavailable",
            };
        };

    DBWorkflowWorkerCoordinator coordinator(
        &execution_db,
        std::move(config),
        CoordinatorIntegrationConfig{});

    const auto start = coordinator.Start();
    ASSERT_FALSE(start);
    ASSERT_EQ(start.status, CoordinatorStartStatus::ProgramInvocationUnavailable);
    ASSERT_FALSE(coordinator.IsDataPlaneEnabled());

    std::string error;
    EXPECT_FALSE(coordinator.StartVisualDebugReplay(1, 1, {}, &error));
    EXPECT_EQ(
        error,
        "interactive visual debugging is unavailable until Dependency Slice 3");
    EXPECT_FALSE(coordinator.IsDataPlaneEnabled());
    EXPECT_EQ(preflight_calls.load(std::memory_order_acquire), 1);
    EXPECT_EQ(execution_db.DataPlaneCallCount(), 0);
    EXPECT_TRUE(execution_db.command_service.materialized_calls.empty());
    EXPECT_TRUE(execution_db.command_service.lifecycle_events.empty());
    EXPECT_TRUE(execution_db.command_service.terminal_calls.empty());

    coordinator.Stop();
}
