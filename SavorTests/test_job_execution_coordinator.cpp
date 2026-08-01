#include <gtest/gtest.h>

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "Execution/JobExecutionCoordinator.h"
#include "Execution/ProgramDB/ProgramKindRegistry.h"
#include "Execution/WorkerCoordinator.h"
#include "Execution/WorkerResultBlobStore.h"
#include "common/RecordingExecutionDb.h"

namespace {

using savor::runner::parallel::savordb::
    JobExecutionCoordinator;
using savor::runner::parallel::savordb::
    JobExecutionCoordinatorConfig;
using savor::runner::parallel::savordb::WorkerCoordinator;
using savor::runner::parallel::savordb::WorkerCoordinatorConfig;
using savor::runner::parallel::savordb::
    WorkerCoordinatorStartStatus;
using savor::runner::parallel::savordb::WorkerExecutionTarget;
using savor::runner::parallel::savordb::WorkerSubmitDisposition;

bool WaitUntil(
    const std::function<bool()>& predicate,
    std::chrono::milliseconds timeout =
        std::chrono::seconds(2)) {
    const auto deadline =
        std::chrono::steady_clock::now() + timeout;
    while (!predicate()
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

savor::runtime::WorkerRuntimeManifest TestWorkerManifest(
    std::uint32_t item_credits = 2) {
    savor::runtime::WorkerRuntimeManifest manifest{};
    manifest.catalog_status =
        savor::runtime::RuntimeCatalogStatus::CompleteExact;
    manifest.runtime_profile_sha256 = std::string(64, '1');
    manifest.dependency_manifest_sha256 =
        std::string(64, '2');
    manifest.limits.maximum_items_per_workset = item_credits;
    manifest.limits.maximum_item_credits = item_credits;
    manifest.limits.maximum_active_and_staged_items =
        item_credits;
    manifest.limits.maximum_encoded_workset_bytes = 1024;
    manifest.modules.push_back(
        {
            .module = {
                .canonical_id = "soa.seed_probe",
                .revision = 1,
                .canonical_hash = std::string(64, '3'),
            },
            .entrypoints = {"probe"},
            .dependency_manifest_sha256 =
                manifest.dependency_manifest_sha256,
            .development_only = false,
        });
    manifest.catalog_sha256 =
        savor::runtime::ComputeRuntimeCatalogHash(
            manifest.modules,
            manifest.catalog_status);
    return manifest;
}

savor::runtime::WorkerCapabilityMask TestWorkerCapabilities() {
    return savor::runtime::CapabilityMask(
        savor::runtime::WorkerCapability::WorksetDispatch);
}

savor::runtime::WorkerWorksetDefinition TestTargetedWorkset(
    std::string module_id = "soa.seed_probe") {
    savor::runtime::WorkerWorksetDefinition workset{};
    workset.workset_id = savor::runtime::WorkerWorksetId{1};
    workset.execution_key.module =
        {
            .canonical_id = std::move(module_id),
            .revision = 1,
            .canonical_hash = std::string(64, '3'),
        };
    workset.execution_key.entrypoint = "probe";
    workset.execution_key.verified_dependency_sha256 =
        std::string(64, '2');
    workset.execution_key.runtime_profile_sha256 =
        std::string(64, '1');
    workset.execution_key.baseline.sha256 =
        std::string(64, '4');
    workset.execution_key.canonical_sha256 =
        std::string(64, '5');
    workset.items.push_back(
        {
            .item_id = savor::runtime::WorkerWorksetItemId{1},
            .ordinal = 0,
        });
    return workset;
}

void UpdateMaximum(
    std::atomic<int>* maximum,
    int candidate) {
    auto observed = maximum->load(std::memory_order_relaxed);
    while (candidate > observed
           && !maximum->compare_exchange_weak(
               observed,
               candidate,
               std::memory_order_relaxed)) {
    }
}

class TemporaryCoordinatorDirectory final {
public:
    TemporaryCoordinatorDirectory() {
        static std::atomic<std::uint64_t> counter = 0;
        root_ = std::filesystem::temp_directory_path()
            / ("savor-job-execution-coordinator-"
               + std::to_string(
                   std::chrono::steady_clock::now()
                       .time_since_epoch()
                       .count())
               + "-" + std::to_string(counter.fetch_add(1)));
    }

    ~TemporaryCoordinatorDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }

private:
    std::filesystem::path root_;
};

class TrackingExecutionDb : public RecordingExecutionDb {
public:
    std::vector<savor::db::ClaimedPublishedWorkset>
    ClaimPublishedWorksetBatch(
        const savor::db::ClaimPublishedWorksetBatchCommand& command,
        std::string* error_out) override {
        ++claim_calls;
        UpdateMaximum(
            &maximum_requested_worksets,
            static_cast<int>(command.requested_workset_count));
        if (error_out != nullptr) {
            error_out->clear();
        }
        return {};
    }

    bool RenewWorksetDispatchLease(
        const savor::db::RenewWorksetDispatchLeaseCommand& command,
        savor::db::WorksetDispatchLeaseReceipt* receipt_out,
        std::string* error_out) override {
        ++renew_calls;
        if (receipt_out != nullptr) {
            *receipt_out = {
                .disposition =
                    savor::db::ExecutionDbOperationDisposition::Applied,
                .dispatch_attempt_id = command.dispatch_attempt_id,
                .lease_expires_at_utc =
                    CurrentUtcMs() + command.lease_duration_ms,
            };
        }
        if (error_out != nullptr) {
            error_out->clear();
        }
        return true;
    }

    bool ReleaseWorksetDispatch(
        const savor::db::ReleaseWorksetDispatchCommand& command,
        savor::db::WorksetDispatchMutationReceipt* receipt_out,
        std::string* error_out) override {
        ++release_calls;
        last_release_reason = command.reason_code;
        if (receipt_out != nullptr) {
            *receipt_out = {
                .disposition =
                    savor::db::ExecutionDbOperationDisposition::Applied,
                .dispatch_attempt_id = command.dispatch_attempt_id,
                .jobs_requeued = 1,
                .dispatch_closed = true,
            };
        }
        if (error_out != nullptr) {
            error_out->clear();
        }
        return true;
    }

    bool StageWorkerTerminal(
        const savor::db::StageWorkerTerminalCommand&,
        savor::db::StageWorkerTerminalReceipt* receipt_out,
        std::string* error_out) override {
        ++stage_calls;
        if (receipt_out != nullptr) {
            *receipt_out = {
                .disposition =
                    savor::db::ExecutionDbOperationDisposition::Applied,
            };
        }
        if (error_out != nullptr) {
            error_out->clear();
        }
        return true;
    }

    bool RecoverExpiredWorksetDispatches(
        int,
        int* dispatches_recovered_out,
        std::string* error_out) override {
        if (dispatches_recovered_out != nullptr) {
            *dispatches_recovered_out = 0;
        }
        if (error_out != nullptr) {
            error_out->clear();
        }
        return true;
    }

    static std::int64_t CurrentUtcMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    std::atomic<int> claim_calls{0};
    std::atomic<int> maximum_requested_worksets{0};
    std::atomic<int> renew_calls{0};
    std::atomic<int> release_calls{0};
    std::atomic<int> stage_calls{0};
    std::string last_release_reason;
};

class ScriptedBatchExecutionDb final : public TrackingExecutionDb {
public:
    std::vector<savor::db::ClaimedPublishedWorkset>
    ClaimPublishedWorksetBatch(
        const savor::db::ClaimPublishedWorksetBatchCommand& command,
        std::string* error_out) override {
        ++claim_calls;
        UpdateMaximum(
            &maximum_requested_worksets,
            static_cast<int>(command.requested_workset_count));
        if (error_out) error_out->clear();
        if (returned_.exchange(true)) return {};
        std::vector<savor::db::ClaimedPublishedWorkset> result;
        const auto count =
            std::min<std::size_t>(4, command.requested_workset_count);
        result.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            savor::db::ClaimedPublishedWorkset claimed{};
            claimed.workset_id =
                static_cast<std::int64_t>(1000 + index);
            claimed.dispatch_attempt_id =
                static_cast<std::int64_t>(2000 + index);
            claimed.job_set_id = 10;
            claimed.workflow_step_id = 11;
            claimed.root_job_set_id = 10;
            claimed.workset_key =
                "scripted-" + std::to_string(index);
            claimed.program_kind = 1;
            claimed.program_version = 1;
            claimed.compatibility.compatibility_key = "test";
            claimed.compatibility.module_canonical_id =
                "soa.seed_probe";
            claimed.compatibility.module_version = 1;
            claimed.compatibility.module_sha256 =
                std::string(64, '3');
            claimed.compatibility.entrypoint = "probe";
            claimed.compatibility.verified_dependency_sha256 =
                std::string(64, '2');
            claimed.compatibility.runtime_profile_sha256 =
                std::string(64, '1');
            claimed.compatibility.estimated_payload_bytes = 1;
            claimed.claim_token =
                command.batch_nonce + "-"
                + std::to_string(index + 1);
            claimed.lease_expires_at_utc =
                CurrentUtcMs() + command.lease_duration_ms;
            claimed.items.push_back(
                {
                    .job_id =
                        static_cast<std::int64_t>(3000 + index),
                    .job_set_id = 10,
                    .program_kind = 1,
                    .program_version = 1,
                    .program_ref_kind = "test",
                    .program_ref_id = 1,
                    .fingerprint =
                        "scripted-job-" + std::to_string(index),
                    .attempts = 0,
                    .max_attempts = 2,
                    .item_ordinal = 0,
                    .dispatch_item_ordinal = 0,
                    .reserved_attempt_id = 1,
                });
            result.push_back(std::move(claimed));
        }
        return result;
    }

private:
    std::atomic<bool> returned_{false};
};

class BlockingReconstructionAdapter final
    : public savor::db::execution::programdb::
          IWorksetReconstructionAdapter {
public:
    explicit BlockingReconstructionAdapter(
        std::shared_future<void> release)
        : release_(std::move(release)) {}

    std::optional<
        savor::db::execution::programdb::
            WorksetReconstructionResult>
    Reconstruct(
        const savor::db::execution::programdb::
            WorksetReconstructionContext&,
        std::string* error_out) const override {
        const auto active = active_.fetch_add(1) + 1;
        UpdateMaximum(&maximum_active_, active);
        ++entered_;
        release_.wait();
        --active_;
        if (error_out) {
            *error_out = "intentional blocked-reconstructor failure";
        }
        return std::nullopt;
    }

    [[nodiscard]] int entered() const { return entered_.load(); }
    [[nodiscard]] int maximum_active() const {
        return maximum_active_.load();
    }

private:
    std::shared_future<void> release_;
    mutable std::atomic<int> active_{0};
    mutable std::atomic<int> entered_{0};
    mutable std::atomic<int> maximum_active_{0};
};

savor::runtime::StateCompatibilityToken TestCompatibility() {
    return {
        .game_id = "TEST00",
        .iso_sha256 = std::string(64, '0'),
        .emulator_build = "test-emulator",
        .runtime_revision = "job-execution-coordinator-test",
    };
}

JobExecutionCoordinatorConfig TestConfig() {
    return {
        .poll_interval = std::chrono::milliseconds(2),
        .workset_lease_duration = std::chrono::milliseconds(60),
        .recovery_interval = std::chrono::hours(1),
        .terminal_retry_interval = std::chrono::milliseconds(1),
        .terminal_retry_max_interval = std::chrono::milliseconds(4),
        .blob_readiness_retry_interval =
            std::chrono::milliseconds(1),
        .blob_readiness_retry_max_interval =
            std::chrono::milliseconds(4),
        .state_compatibility = TestCompatibility(),
    };
}

savor::runtime::DurableWorkerTerminalEnvelope MakeTerminal(
    std::int64_t dispatch_attempt_id,
    std::uint64_t process_generation = 9) {
    return {
        .wrms_protocol_version = savor::wrms::ProtocolVersion,
        .worker_id = 5,
        .process_generation = process_generation,
        .terminal =
            {
                .workset_id =
                    static_cast<std::uint64_t>(dispatch_attempt_id),
                .item_id = 1,
                .item_ordinal = 0,
                .invocation_id = 2,
                .attempt_id = 7,
                .terminal_id = 3,
                .terminal_order = 1,
                .status =
                    savor::wrms::InvocationTerminalStatus::Succeeded,
                .session_disposition =
                    savor::wrms::SessionDispositionCode::Clean,
                .state_epoch = 11,
                .result = {1, 2, 3},
            },
    };
}

TEST(
    WorkerCoordinator,
    ReportsNonretryableWorkerStartupExhaustionWithoutElapsedWait) {
    WorkerCoordinatorConfig config{
        .desired_workers = 1,
        .max_worker_start_attempts = 3,
    };
    config.worker_capability_preflight =
        [](
            std::size_t,
            const WorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>&) {
            return savor::runner::parallel::savordb::
                WorkerCoordinatorCapabilityPreflightResult{
                    .process_ready = false,
                    .retryable = false,
                    .error = "nonretryable startup failure",
                };
        };
    WorkerCoordinator coordinator(std::move(config));

    const auto result = coordinator.Start();
    EXPECT_EQ(
        result.status,
        WorkerCoordinatorStartStatus::StartupExhausted);
    EXPECT_FALSE(result.started());
    EXPECT_NE(
        result.diagnostic.find("nonretryable startup failure"),
        std::string::npos)
        << result.diagnostic;

    coordinator.Stop();
}

TEST(
    WorkerCoordinator,
    ReportsStartupExhaustionWhenRetryBudgetIsAlreadySpent) {
    WorkerCoordinatorConfig config{
        .desired_workers = 1,
        .max_worker_start_attempts = 1,
    };
    config.worker_capability_preflight =
        [](
            std::size_t,
            const WorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>&) {
            return savor::runner::parallel::savordb::
                WorkerCoordinatorCapabilityPreflightResult{
                    .process_ready = false,
                    .retryable = true,
                    .error = "retryable startup failure",
                };
        };
    WorkerCoordinator coordinator(std::move(config));

    const auto result = coordinator.Start();
    EXPECT_EQ(
        result.status,
        WorkerCoordinatorStartStatus::StartupExhausted);
    EXPECT_FALSE(result.started());
    EXPECT_NE(
        result.diagnostic.find("retryable startup failure"),
        std::string::npos)
        << result.diagnostic;

    coordinator.Stop();
}

TEST(
    WorkerCoordinator,
    ReturnsAfterFirstReadyWorkerAndBoundsSecondaryStartupConcurrency) {
    std::promise<void> release_promise;
    auto release = release_promise.get_future().share();
    std::atomic<int> active_secondary_starts{0};
    std::atomic<int> maximum_secondary_starts{0};
    std::atomic<int> secondary_starts_entered{0};

    WorkerCoordinatorConfig config{
        .desired_workers = 3,
        .controller_sleep_ms = 250,
        .max_concurrent_worker_starts = 2,
    };
    config.worker_capability_preflight =
        [&](std::size_t worker_id,
            const WorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>&) {
            if (worker_id != 0) {
                const auto active =
                    active_secondary_starts.fetch_add(1) + 1;
                UpdateMaximum(
                    &maximum_secondary_starts,
                    active);
                ++secondary_starts_entered;
                release.wait();
                --active_secondary_starts;
            }
            return savor::runner::parallel::savordb::
                WorkerCoordinatorCapabilityPreflightResult{
                    .process_ready = true,
                    .capabilities = TestWorkerCapabilities(),
                    .runtime_manifest = TestWorkerManifest(),
                };
        };
    WorkerCoordinator coordinator(std::move(config));

    const auto start = coordinator.Start();
    const bool secondaries_started = WaitUntil([&]() {
        return secondary_starts_entered.load() == 2;
    });
    const auto fleet = coordinator.SnapshotFleetStartup();

    EXPECT_EQ(start.status, WorkerCoordinatorStartStatus::Started)
        << start.diagnostic;
    EXPECT_EQ(start.ready_workers, 1u);
    EXPECT_TRUE(secondaries_started);
    EXPECT_EQ(maximum_secondary_starts.load(), 2);
    EXPECT_EQ(fleet.desired, 3u);
    EXPECT_EQ(fleet.ready, 1u);
    EXPECT_EQ(fleet.starting, 2u);
    EXPECT_EQ(fleet.slots.size(), 3u);
    if (!fleet.slots.empty()) {
        EXPECT_EQ(fleet.slots[0].attempt_count, 1u);
    }

    release_promise.set_value();
    coordinator.Stop();
}

TEST(
    WorkerCoordinator,
    StopJoinsInProgressStartupThreads) {
    std::promise<void> release_promise;
    auto release = release_promise.get_future().share();
    std::atomic<bool> secondary_entered{false};

    WorkerCoordinatorConfig config{
        .desired_workers = 2,
        .controller_sleep_ms = 50,
        .max_concurrent_worker_starts = 1,
    };
    config.worker_capability_preflight =
        [&](std::size_t worker_id,
            const WorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>&) {
            if (worker_id == 1) {
                secondary_entered = true;
                release.wait();
            }
            return savor::runner::parallel::savordb::
                WorkerCoordinatorCapabilityPreflightResult{
                    .process_ready = true,
                    .capabilities = TestWorkerCapabilities(),
                    .runtime_manifest = TestWorkerManifest(),
                };
        };
    WorkerCoordinator coordinator(std::move(config));
    ASSERT_TRUE(coordinator.Start().started());
    const bool entered = WaitUntil(
        [&]() { return secondary_entered.load(); });

    auto stop = std::async(
        std::launch::async,
        [&]() { coordinator.Stop(); });
    EXPECT_EQ(
        stop.wait_for(std::chrono::milliseconds(100)),
        std::future_status::timeout);

    release_promise.set_value();
    stop.get();
    EXPECT_TRUE(entered);
    EXPECT_FALSE(coordinator.IsStarted());
}

TEST(
    WorkerCoordinator,
    PartialStartupExhaustionPreservesExactSlotDiagnostic) {
    WorkerCoordinatorConfig config{
        .desired_workers = 2,
        .controller_sleep_ms = 250,
        .max_worker_start_attempts = 3,
        .max_concurrent_worker_starts = 1,
    };
    config.worker_capability_preflight =
        [](std::size_t worker_id,
            const WorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>&) {
            if (worker_id == 1) {
                return savor::runner::parallel::savordb::
                    WorkerCoordinatorCapabilityPreflightResult{
                        .process_ready = false,
                        .retryable = false,
                        .error =
                            "worker 1 exact startup diagnostic",
                    };
            }
            return savor::runner::parallel::savordb::
                WorkerCoordinatorCapabilityPreflightResult{
                    .process_ready = true,
                    .capabilities = TestWorkerCapabilities(),
                    .runtime_manifest = TestWorkerManifest(),
                };
        };
    WorkerCoordinator coordinator(std::move(config));
    const auto start = coordinator.Start();
    const bool observed_exhaustion = WaitUntil([&]() {
        return coordinator.SnapshotFleetStartup().exhausted == 1;
    });
    const auto fleet = coordinator.SnapshotFleetStartup();

    EXPECT_TRUE(start.started()) << start.diagnostic;
    EXPECT_TRUE(observed_exhaustion);
    EXPECT_EQ(fleet.ready, 1u);
    EXPECT_EQ(fleet.exhausted, 1u);
    EXPECT_TRUE(fleet.full_pool_impossible());
    ASSERT_EQ(fleet.slots.size(), 2u);
    EXPECT_EQ(fleet.slots[1].attempt_count, 1u);
    EXPECT_EQ(
        fleet.slots[1].terminal_diagnostic,
        "worker 1 exact startup diagnostic");

    coordinator.Stop();
}

TEST(
    WorkerCoordinator,
    ExplicitTargetRejectsStaleGenerationAndIncompatibleContractBeforeWrite) {
    WorkerCoordinatorConfig config{
        .desired_workers = 1,
    };
    config.worker_capability_preflight =
        [](
            std::size_t,
            const WorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>&) {
            return savor::runner::parallel::savordb::
                WorkerCoordinatorCapabilityPreflightResult{
                    .process_ready = true,
                    .capabilities = TestWorkerCapabilities(),
                    .runtime_manifest = TestWorkerManifest(),
                };
        };
    WorkerCoordinator coordinator(std::move(config));
    ASSERT_TRUE(coordinator.Start().started());
    const auto ready = coordinator.SnapshotReadyWorkers();
    ASSERT_EQ(ready.size(), 1u);

    const auto stale = coordinator.SubmitWorksetToWorker(
        WorkerExecutionTarget{
            .worker_id = ready[0].worker_id,
            .process_generation =
                ready[0].process_generation + 1,
        },
        TestTargetedWorkset());
    EXPECT_EQ(
        stale.disposition,
        WorkerSubmitDisposition::StaleGeneration);

    const auto incompatible = coordinator.SubmitWorksetToWorker(
        WorkerExecutionTarget{
            .worker_id = ready[0].worker_id,
            .process_generation =
                ready[0].process_generation,
        },
        TestTargetedWorkset("other.module"));
    EXPECT_EQ(
        incompatible.disposition,
        WorkerSubmitDisposition::IncompatibleWorkset);
    EXPECT_EQ(
        coordinator.SnapshotTelemetry().submit_accepted,
        0u);
    coordinator.Stop();
}

TEST(
    JobExecutionCoordinator,
    ClaimsWithFirstReadyWorkerWhileRemainingFleetStarts) {
    TemporaryCoordinatorDirectory temporary;
    std::promise<void> release_promise;
    auto release = release_promise.get_future().share();
    std::atomic<bool> secondary_entered{false};

    WorkerCoordinatorConfig worker_config{
        .desired_workers = 2,
        .controller_sleep_ms = 250,
        .max_concurrent_worker_starts = 1,
    };
    worker_config.worker_capability_preflight =
        [&](std::size_t worker_id,
            const WorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>&) {
            if (worker_id == 1) {
                secondary_entered = true;
                release.wait();
            }
            return savor::runner::parallel::savordb::
                WorkerCoordinatorCapabilityPreflightResult{
                    .process_ready = true,
                    .capabilities = TestWorkerCapabilities(),
                    .runtime_manifest = TestWorkerManifest(),
                };
    };
    WorkerCoordinator workers(std::move(worker_config));
    const auto worker_start = workers.Start();
    const bool secondary_started = WaitUntil(
        [&]() { return secondary_entered.load(); });
    EXPECT_TRUE(worker_start.started());
    EXPECT_TRUE(secondary_started);
    if (!worker_start.started() || !secondary_started) {
        release_promise.set_value();
        workers.Stop();
        return;
    }

    TrackingExecutionDb execution_db;
    savor::db::execution::programdb::ProgramKindRegistry registry;
    EXPECT_TRUE(registry.Register(
        {
            .program_kind = 1,
            .program_name = "test-seed-probe",
        }));
    savor::db::execution::WorkerResultBlobStore blob_store(
        temporary.root() / "object_store");
    JobExecutionCoordinator coordinator(
        &execution_db,
        &registry,
        &workers,
        &blob_store,
        TestConfig());
    std::string error;
    const bool execution_started = coordinator.Start(&error);
    EXPECT_TRUE(execution_started) << error;
    if (!execution_started) {
        release_promise.set_value();
        workers.Stop();
        return;
    }

    const bool claimed = WaitUntil(
        [&]() { return execution_db.claim_calls.load() > 0; },
        std::chrono::milliseconds(100));
    const auto fleet = workers.SnapshotFleetStartup();
    EXPECT_TRUE(claimed);
    EXPECT_FALSE(fleet.full_pool_ready());
    EXPECT_EQ(fleet.ready, 1u);
    EXPECT_EQ(fleet.starting, 1u);

    release_promise.set_value();
    coordinator.Stop();
    workers.Stop();
}

TEST(
    JobExecutionCoordinator,
    ExplicitStartupBarrierPausePreventsClaimsUntilReleased) {
    TemporaryCoordinatorDirectory temporary;
    std::promise<void> release_promise;
    auto release = release_promise.get_future().share();
    std::atomic<bool> secondary_entered{false};

    WorkerCoordinatorConfig worker_config{
        .desired_workers = 2,
        .controller_sleep_ms = 250,
        .max_concurrent_worker_starts = 1,
    };
    worker_config.worker_capability_preflight =
        [&](std::size_t worker_id,
            const WorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>&) {
            if (worker_id == 1) {
                secondary_entered = true;
                release.wait();
            }
            return savor::runner::parallel::savordb::
                WorkerCoordinatorCapabilityPreflightResult{
                    .process_ready = true,
                    .capabilities = TestWorkerCapabilities(),
                    .runtime_manifest = TestWorkerManifest(),
                };
    };
    WorkerCoordinator workers(std::move(worker_config));
    const auto worker_start = workers.Start();
    const bool secondary_started = WaitUntil(
        [&]() { return secondary_entered.load(); });
    EXPECT_TRUE(worker_start.started());
    EXPECT_TRUE(secondary_started);
    if (!worker_start.started() || !secondary_started) {
        release_promise.set_value();
        workers.Stop();
        return;
    }

    TrackingExecutionDb execution_db;
    savor::db::execution::programdb::ProgramKindRegistry registry;
    EXPECT_TRUE(registry.Register(
        {
            .program_kind = 1,
            .program_name = "test-seed-probe",
        }));
    savor::db::execution::WorkerResultBlobStore blob_store(
        temporary.root() / "object_store");
    JobExecutionCoordinator coordinator(
        &execution_db,
        &registry,
        &workers,
        &blob_store,
        TestConfig());
    coordinator.SetPaused(true);
    std::string error;
    const bool execution_started = coordinator.Start(&error);
    EXPECT_TRUE(execution_started) << error;
    if (!execution_started) {
        release_promise.set_value();
        workers.Stop();
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    EXPECT_EQ(execution_db.claim_calls.load(), 0);

    release_promise.set_value();
    const bool fleet_ready = WaitUntil([&]() {
        return workers.SnapshotFleetStartup().full_pool_ready();
    });
    EXPECT_TRUE(fleet_ready);
    EXPECT_EQ(execution_db.claim_calls.load(), 0);

    coordinator.SetPaused(false);
    EXPECT_TRUE(WaitUntil(
        [&]() { return execution_db.claim_calls.load() > 0; },
        std::chrono::milliseconds(100)));

    coordinator.Stop();
    workers.Stop();
}

TEST(
    JobExecutionCoordinator,
    IdenticalReadyWorkersBatchTheirCombinedQueueCapacity) {
    TemporaryCoordinatorDirectory temporary;
    WorkerCoordinatorConfig worker_config{
        .desired_workers = 3,
        .controller_sleep_ms = 250,
        .max_concurrent_worker_starts = 3,
    };
    worker_config.worker_capability_preflight =
        [](
            std::size_t,
            const WorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>&) {
            return savor::runner::parallel::savordb::
                WorkerCoordinatorCapabilityPreflightResult{
                    .process_ready = true,
                    .capabilities = TestWorkerCapabilities(),
                    .runtime_manifest = TestWorkerManifest(),
                };
    };
    WorkerCoordinator workers(std::move(worker_config));
    ASSERT_TRUE(workers.Start().started());
    ASSERT_TRUE(WaitUntil([&]() {
        return workers.SnapshotFleetStartup().full_pool_ready();
    }));

    TrackingExecutionDb execution_db;
    savor::db::execution::programdb::ProgramKindRegistry registry;
    ASSERT_TRUE(registry.Register(
        {
            .program_kind = 1,
            .program_name = "test-seed-probe",
        }));
    savor::db::execution::WorkerResultBlobStore blob_store(
        temporary.root() / "object_store");
    JobExecutionCoordinator coordinator(
        &execution_db,
        &registry,
        &workers,
        &blob_store,
        TestConfig());
    std::string error;
    ASSERT_TRUE(coordinator.Start(&error)) << error;
    EXPECT_TRUE(WaitUntil(
        [&]() {
            return execution_db.maximum_requested_worksets.load()
                == 6;
        },
        std::chrono::milliseconds(250)));
    EXPECT_EQ(coordinator.SnapshotWorkerLanes().size(), 3u);
    EXPECT_GT(
        coordinator.SnapshotTelemetry().claim_batches,
        0u);

    coordinator.Stop();
    workers.Stop();
}

TEST(
    JobExecutionCoordinator,
    BlockedSingleReconstructorExposesBacklogWhileHeartbeatContinues) {
    TemporaryCoordinatorDirectory temporary;
    WorkerCoordinatorConfig worker_config{
        .desired_workers = 2,
        .controller_sleep_ms = 250,
        .max_concurrent_worker_starts = 2,
    };
    worker_config.worker_capability_preflight =
        [](
            std::size_t,
            const WorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>&) {
            return savor::runner::parallel::savordb::
                WorkerCoordinatorCapabilityPreflightResult{
                    .process_ready = true,
                    .capabilities = TestWorkerCapabilities(),
                    .runtime_manifest = TestWorkerManifest(),
                };
        };
    WorkerCoordinator workers(std::move(worker_config));
    ASSERT_TRUE(workers.Start().started());
    ASSERT_TRUE(WaitUntil([&]() {
        return workers.SnapshotFleetStartup().full_pool_ready();
    }));

    std::promise<void> reconstruction_release_promise;
    auto reconstruction_release =
        reconstruction_release_promise.get_future().share();
    auto adapter = std::make_shared<BlockingReconstructionAdapter>(
        reconstruction_release);
    ScriptedBatchExecutionDb execution_db;
    savor::db::execution::programdb::ProgramKindRegistry registry;
    savor::db::execution::programdb::ProgramKindDescriptor descriptor{};
    descriptor.program_kind = 1;
    descriptor.program_name = "blocked-reconstruction-test";
    descriptor.workset_reconstruction = adapter;
    ASSERT_TRUE(registry.Register(descriptor));
    savor::db::execution::WorkerResultBlobStore blob_store(
        temporary.root() / "object_store");
    JobExecutionCoordinator coordinator(
        &execution_db,
        &registry,
        &workers,
        &blob_store,
        TestConfig());
    std::string error;
    ASSERT_TRUE(coordinator.Start(&error)) << error;

    ASSERT_TRUE(WaitUntil([&]() {
        const auto telemetry = coordinator.SnapshotTelemetry();
        return adapter->entered() == 1
            && telemetry.reconstruction_active
            && telemetry.reconstruction_queue_depth == 3;
    }));
    const auto blocked = coordinator.SnapshotTelemetry();
    const auto lanes = coordinator.SnapshotWorkerLanes();
    std::size_t reservations = 0;
    std::size_t reconstructing = 0;
    for (const auto& lane : lanes) {
        reservations += lane.reservations;
        reconstructing += lane.reconstructing;
    }
    EXPECT_EQ(lanes.size(), 2u);
    EXPECT_EQ(reservations, 4u);
    EXPECT_EQ(reconstructing, 4u);
    EXPECT_EQ(blocked.reconstruction_queue_high_water, 3u);
    EXPECT_EQ(adapter->maximum_active(), 1);
    EXPECT_TRUE(WaitUntil(
        [&]() { return execution_db.renew_calls.load() > 0; }));
    EXPECT_EQ(adapter->maximum_active(), 1);

    reconstruction_release_promise.set_value();
    EXPECT_TRUE(WaitUntil([&]() {
        return coordinator.SnapshotTelemetry().invariant_paused;
    }));
    coordinator.Stop();
    workers.Stop();
}

TEST(
    JobExecutionCoordinator,
    StartupRejectsUnavailableBlobStoreBeforeAnyClaim) {
    TemporaryCoordinatorDirectory temporary;
    const auto object_store = temporary.root() / "object_store";
    ASSERT_TRUE(std::filesystem::create_directories(object_store));
    {
        std::ofstream blocker(
            object_store / "worker_results",
            std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(blocker);
        blocker << "not a directory";
    }

    TrackingExecutionDb execution_db;
    savor::db::execution::programdb::ProgramKindRegistry registry;
    WorkerCoordinator workers(
        WorkerCoordinatorConfig{.desired_workers = 0});
    ASSERT_TRUE(workers.Start().started());
    savor::db::execution::WorkerResultBlobStore blob_store(
        object_store);
    JobExecutionCoordinator coordinator(
        &execution_db,
        &registry,
        &workers,
        &blob_store,
        TestConfig());

    std::string error;
    EXPECT_FALSE(coordinator.Start(&error));
    EXPECT_NE(error.find("worker result"), std::string::npos)
        << error;
    EXPECT_EQ(execution_db.claim_calls.load(), 0);
    EXPECT_EQ(execution_db.stage_calls.load(), 0);
    EXPECT_FALSE(coordinator.SnapshotTelemetry().blob_store_ready);
    workers.Stop();
}

} // namespace
