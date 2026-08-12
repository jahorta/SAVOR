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
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "Execution/JobExecutionCoordinator.h"
#include "Execution/ProgramDB/ProgramKindRegistry.h"
#include "Execution/QueuedExecutionDb.h"
#include "Execution/WorkerCoordinator.h"
#include "Execution/WorkerResultBlobStore.h"
#include "Runner/Runtime/Worksets/WorksetWireCodec.h"
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

savor::runtime::WorkerRuntimeContractV1 TestRuntimeContract(
    std::uint32_t item_credits = 0) {
    auto contract =
        savor::runtime::BuildProductionWorkerRuntimeContractV1();
    if (item_credits != 0) {
        contract.limits.maximum_item_credits = item_credits;
        contract.canonical_sha256 =
            savor::runtime::ComputeWorkerRuntimeContractHashV1(contract);
    }
    return contract;
}

savor::db::ExecutionWorksetObservationBindingV1 TestObservationBinding()
{
    savor::db::ExecutionWorksetObservationBindingV1 result;
    std::optional<savor::runtime::WorksetCaptureBindingV1> capture;
    savor::runtime::progress::ProgressPlanV1 progress;
    if (!savor::runtime::EncodeWorksetCaptureBindingV1(
            capture,
            result.capture_binding_payload) ||
        !savor::runtime::EncodeProgressPlanV1(
            progress,
            result.progress_plan_payload))
    {
        throw std::logic_error(
            "empty workset observation binding could not be encoded");
    }
    result.capture_binding_sha256 =
        savor::runtime::EmptyWorksetCaptureBindingHashV1();
    result.progress_plan_sha256 = progress.content_sha256;
    return result;
}

savor::db::ExecutionWorksetDerivedStateBindingV1 TestDerivedStateBinding()
{
    savor::db::ExecutionWorksetDerivedStateBindingV1 result;
    const savor::runtime::derived::WorksetDerivedStateBindingV1 binding;
    if (!savor::runtime::EncodeWorksetDerivedStateBindingV1(
            binding, result.binding_payload))
    {
        throw std::logic_error(
            "empty workset derived-state binding could not be encoded");
    }
    result.binding_sha256 = binding.content_sha256;
    return result;
}

savor::runtime::WorkerWorksetDefinition TestTargetedWorkset(
    std::string module_id = {}) {
    const auto* phase = savor::runtime::fullphase::
        ProductionRegistry().Find(1);
    if (phase == nullptr) {
        throw std::logic_error(
            "PK_SeedProbe is absent from the production FullPhase registry");
    }
    const auto& contract = phase->runtime_contract();
    savor::runtime::WorkerWorksetDefinition workset{};
    workset.workset_id = savor::runtime::WorkerWorksetId{1};
    workset.phase_invocation = {
        .invocation_id = {1, 1},
        .program_package =
            savor::runtime::fullphase::BuildFullPhaseProgramPackage(
                *phase),
        .common_input =
            savor::runtime::fullphase::MakeFullPhaseCommonInput(
                "soa.seed_probe.CommonInput", 1),
    };
    workset.execution_key.module = contract.module;
    if (!module_id.empty()) {
        workset.execution_key.module.canonical_id =
            std::move(module_id);
    }
    workset.execution_key.entrypoint = contract.entrypoint;
    workset.execution_key.verified_dependency_sha256 =
        contract.verified_dependency_sha256;
    workset.execution_key.runtime_profile_sha256 =
        contract.runtime_profile_sha256;
    workset.execution_key.baseline.sha256 =
        std::string(64, '4');
    workset.execution_key.program_package_sha256 =
        workset.phase_invocation.program_package.canonical_sha256;
    workset.execution_key.common_input_sha256 =
        workset.phase_invocation.common_input.content_sha256;
    workset.execution_key.derived_state_binding_sha256 =
        workset.derived_state.content_sha256;
    workset.execution_key.capture_binding_sha256 =
        savor::runtime::EmptyWorksetCaptureBindingHashV1();
    workset.execution_key.progress_plan_sha256 =
        workset.progress_plan.content_sha256;
    workset.execution_key.canonical_sha256 =
        std::string(64, '5');
    workset.items.push_back(
        {
            .item_id = savor::runtime::WorkerWorksetItemId{1},
            .ordinal = 0,
        });
    return workset;
}

savor::db::execution::programdb::ProgramKindDescriptor
TestProgramDescriptor(std::string name) {
    const auto* phase = savor::runtime::fullphase::
        ProductionRegistry().Find(1);
    if (phase == nullptr) {
        throw std::logic_error(
            "PK_SeedProbe is absent from the production FullPhase registry");
    }
    savor::db::execution::programdb::ProgramKindDescriptor descriptor{};
    descriptor.program_kind = 1;
    descriptor.program_name = std::move(name);
    descriptor.full_phase_identity = phase->identity();
    descriptor.default_progress_library_ids =
        std::vector<std::string>{};
    descriptor.default_derived_state_block_ids =
        std::vector<std::string>{};
    return descriptor;
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
    bool PublishWorksetWave(
        const savor::db::PublishWorksetWaveCommand& command,
        savor::db::PublishWorksetWaveReceipt* receipt_out,
        std::string* error_out) override {
        if (!publish_updates_availability.load()) {
            if (error_out) error_out->clear();
            return false;
        }
        ready_present.store(true);
        ready_generation.fetch_add(1);
        if (receipt_out) {
            receipt_out->disposition =
                savor::db::ExecutionDbOperationDisposition::Applied;
            receipt_out->durable_workset_count =
                static_cast<int>(command.worksets.size());
            receipt_out->durable_job_count = command.expected_job_count;
            receipt_out->ready_workset_availability_changed = true;
        }
        if (error_out) error_out->clear();
        return true;
    }

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

    std::vector<savor::db::WorksetDispatchLeaseReceipt>
    RenewActiveWorksetLeases(
        const savor::db::RenewActiveWorksetLeasesCommand& command,
        std::string* error_out) override {
        ++renew_calls;
        UpdateMaximum(
            &maximum_renewal_batch_size,
            static_cast<int>(command.requests.size()));
        std::vector<savor::db::WorksetDispatchLeaseReceipt> receipts;
        for (const auto& request : command.requests) {
            receipts.push_back({
                .disposition =
                    savor::db::ExecutionDbOperationDisposition::Applied,
                .dispatch_attempt_id = request.dispatch_attempt_id,
                .lease_expires_at_utc =
                    CurrentUtcMs() + command.lease_duration_ms,
            });
        }
        if (error_out != nullptr) {
            error_out->clear();
        }
        return receipts;
    }

    bool MarkWorksetActive(
        const savor::db::MarkWorksetActiveCommand& command,
        savor::db::WorksetDispatchMutationReceipt* receipt_out,
        std::string* error_out) override {
        if (receipt_out != nullptr) {
            *receipt_out = {
                .disposition = savor::db::
                    ExecutionDbOperationDisposition::Applied,
                .dispatch_attempt_id = command.dispatch_attempt_id,
                .lease_expires_at_utc =
                    CurrentUtcMs() + command.lease_duration_ms,
            };
        }
        if (error_out != nullptr) error_out->clear();
        return true;
    }

    bool MarkWorksetDraining(
        const savor::db::MarkWorksetDrainingCommand& command,
        savor::db::WorksetDispatchMutationReceipt* receipt_out,
        std::string* error_out) override {
        if (receipt_out != nullptr) {
            *receipt_out = {
                .disposition = savor::db::
                    ExecutionDbOperationDisposition::Applied,
                .dispatch_attempt_id = command.dispatch_attempt_id,
            };
        }
        if (error_out != nullptr) error_out->clear();
        return true;
    }

    std::optional<savor::db::ExecutionWorkAvailabilitySnapshot>
    GetExecutionWorkAvailability(
        std::string* error_out) const override {
        if (error_out) error_out->clear();
        return savor::db::ExecutionWorkAvailabilitySnapshot{
            .generation = ready_generation.load(),
            .has_ready_worksets = ready_present.load(),
            .changed_at_utc = CurrentUtcMs(),
        };
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

    bool PersistWorkerExecutionEventsBatch(
        const savor::db::PersistWorkerExecutionEventsBatchCommand& command,
        savor::db::PersistWorkerExecutionEventsBatchReceipt* receipt_out,
        std::string* error_out) override {
        stage_calls.fetch_add(static_cast<int>(command.events.size()));
        if (receipt_out != nullptr) {
            receipt_out->events.clear();
            for (const auto& event : command.events) {
                if (std::holds_alternative<
                        savor::db::StageWorkerTerminalCommand>(event)) {
                    receipt_out->events.emplace_back(
                        savor::db::StageWorkerTerminalReceipt{
                            .disposition = savor::db::
                                ExecutionDbOperationDisposition::Applied,
                        });
                } else {
                    receipt_out->events.emplace_back(
                        savor::db::WorksetJobStartReceipt{
                            .disposition = savor::db::
                                ExecutionDbOperationDisposition::Applied,
                        });
                }
            }
        }
        if (error_out != nullptr) {
            error_out->clear();
        }
        return true;
    }

    bool RecoverInterruptedWorksetDispatches(
        savor::db::RecoverInterruptedWorksetDispatchesReceipt* receipt_out,
        std::string* error_out) override {
        if (receipt_out != nullptr) *receipt_out = {};
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
    std::atomic<int> maximum_renewal_batch_size{0};
    std::atomic<int> release_calls{0};
    std::atomic<int> stage_calls{0};
    std::atomic<std::uint64_t> ready_generation{1};
    std::atomic<bool> ready_present{false};
    std::atomic<bool> publish_updates_availability{false};
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
            claimed.contract.contract_key = "test";
            claimed.contract.module_canonical_id =
                "soa.seed_probe";
            claimed.contract.module_version = 1;
            claimed.contract.module_sha256 =
                std::string(64, '3');
            claimed.contract.entrypoint = "probe";
            claimed.contract.verified_dependency_sha256 =
                std::string(64, '2');
            claimed.contract.runtime_profile_sha256 =
                std::string(64, '1');
            claimed.contract.program_package_sha256 =
                std::string(64, '6');
            claimed.contract.estimated_payload_bytes = 1;
            claimed.derived_state = TestDerivedStateBinding();
            claimed.observation = TestObservationBinding();
            claimed.claim_token =
                command.batch_nonce + "-"
                + std::to_string(index + 1);
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

savor::runtime::ArtifactCompatibilityToken TestCompatibility() {
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
        .workset_lease_renewal_point = std::chrono::milliseconds(30),
        .workset_lease_retry_interval = std::chrono::milliseconds(5),
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
                .workset_epoch = 11,
                .result = {1, 2, 3},
            },
    };
}

TEST(
    JobExecutionCoordinatorLifecycle,
    ActiveAndUnpersistedDrainingDispatchesCannotRetire) {
    using savor::runner::parallel::savordb::detail::
        DispatchReadyToRetire;
    using savor::runner::parallel::savordb::detail::
        DispatchRetirementAuthority;
    using savor::runner::parallel::savordb::detail::
        DispatchRetirementFacts;

    DispatchRetirementFacts facts{
        .authority = DispatchRetirementAuthority::None,
        .summary_observed = true,
        .executable_items = 16,
        .staged_items = 16,
        .acknowledged_items = 16,
        .sidecar_persisted = true,
    };
    EXPECT_FALSE(DispatchReadyToRetire(facts));

    facts.authority = DispatchRetirementAuthority::DrainingPending;
    EXPECT_FALSE(DispatchReadyToRetire(facts));
}

TEST(
    JobExecutionCoordinatorLifecycle,
    PersistedDrainingReevaluatesEveryTerminalFactOrdering) {
    using savor::runner::parallel::savordb::detail::
        DispatchReadyToRetire;
    using savor::runner::parallel::savordb::detail::
        DispatchRetirementAuthority;
    using savor::runner::parallel::savordb::detail::
        DispatchRetirementFacts;

    DispatchRetirementFacts facts{
        .authority = DispatchRetirementAuthority::DrainingPending,
        .summary_observed = true,
        .executable_items = 16,
        .staged_items = 16,
        .acknowledged_items = 16,
        .sidecar_persisted = true,
    };
    EXPECT_FALSE(DispatchReadyToRetire(facts));
    facts.authority = DispatchRetirementAuthority::DrainingPersisted;
    EXPECT_TRUE(DispatchReadyToRetire(facts));

    facts.summary_observed = false;
    EXPECT_FALSE(DispatchReadyToRetire(facts));
    facts.summary_observed = true;
    EXPECT_TRUE(DispatchReadyToRetire(facts));

    facts.acknowledged_items = 15;
    EXPECT_FALSE(DispatchReadyToRetire(facts));
    facts.acknowledged_items = 16;
    EXPECT_TRUE(DispatchReadyToRetire(facts));

    facts.staged_items = 15;
    EXPECT_FALSE(DispatchReadyToRetire(facts));
    facts.staged_items = 16;
    EXPECT_TRUE(DispatchReadyToRetire(facts));

    facts.sidecar_persisted = false;
    EXPECT_FALSE(DispatchReadyToRetire(facts));
    facts.sidecar_persisted = true;
    EXPECT_TRUE(DispatchReadyToRetire(facts));
}

TEST(
    JobExecutionCoordinatorLifecycle,
    ReleasedDrainingRetainsItsExistingRetirementRules) {
    using savor::runner::parallel::savordb::detail::
        DispatchReadyToRetire;
    using savor::runner::parallel::savordb::detail::
        DispatchRetirementAuthority;
    using savor::runner::parallel::savordb::detail::
        DispatchRetirementFacts;

    DispatchRetirementFacts facts{
        .authority = DispatchRetirementAuthority::ReleasedDraining,
        .summary_observed = true,
        .executable_items = 16,
        .staged_items = 0,
        .acknowledged_items = 16,
        .sidecar_persisted = true,
    };
    EXPECT_TRUE(DispatchReadyToRetire(facts));

    facts.acknowledged_items = 15;
    EXPECT_FALSE(DispatchReadyToRetire(facts));
    facts.acknowledged_items = 16;
    facts.summary_observed = false;
    EXPECT_FALSE(DispatchReadyToRetire(facts));
}

TEST(
    WorkerCoordinator,
    ReportsNonretryableWorkerStartupExhaustionWithoutElapsedWait) {
    WorkerCoordinatorConfig config{
        .desired_workers = 1,
        .max_worker_start_attempts = 3,
    };
        config.worker_runtime_preflight =
        [](
            std::size_t,
            const WorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>&) {
            return savor::runner::parallel::savordb::
                WorkerCoordinatorRuntimePreflightResult{
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
        config.worker_runtime_preflight =
        [](
            std::size_t,
            const WorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>&) {
            return savor::runner::parallel::savordb::
                WorkerCoordinatorRuntimePreflightResult{
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
        config.worker_runtime_preflight =
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
                WorkerCoordinatorRuntimePreflightResult{
                    .process_ready = true,
                                        .runtime_contract = TestRuntimeContract(),
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
    EXPECT_EQ(fleet.worker_slots.size(), 3u);
    if (!fleet.worker_slots.empty()) {
        EXPECT_EQ(fleet.worker_slots[0].attempt_count, 1u);
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
        config.worker_runtime_preflight =
        [&](std::size_t worker_id,
            const WorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>&) {
            if (worker_id == 1) {
                secondary_entered = true;
                release.wait();
            }
            return savor::runner::parallel::savordb::
                WorkerCoordinatorRuntimePreflightResult{
                    .process_ready = true,
                                        .runtime_contract = TestRuntimeContract(),
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
        config.worker_runtime_preflight =
        [](std::size_t worker_id,
            const WorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>&) {
            if (worker_id == 1) {
                return savor::runner::parallel::savordb::
                    WorkerCoordinatorRuntimePreflightResult{
                        .process_ready = false,
                        .retryable = false,
                        .error =
                            "worker 1 exact startup diagnostic",
                    };
            }
            return savor::runner::parallel::savordb::
                WorkerCoordinatorRuntimePreflightResult{
                    .process_ready = true,
                                        .runtime_contract = TestRuntimeContract(),
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
    ASSERT_EQ(fleet.worker_slots.size(), 2u);
    EXPECT_EQ(fleet.worker_slots[1].attempt_count, 1u);
    EXPECT_EQ(
        fleet.worker_slots[1].terminal_diagnostic,
        "worker 1 exact startup diagnostic");

    coordinator.Stop();
}

TEST(
    WorkerCoordinator,
    ExplicitTargetRejectsStaleGenerationAndInvalidPackageBeforeWrite) {
    WorkerCoordinatorConfig config{
        .desired_workers = 1,
    };
        config.worker_runtime_preflight =
        [](
            std::size_t,
            const WorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>&) {
            return savor::runner::parallel::savordb::
                WorkerCoordinatorRuntimePreflightResult{
                    .process_ready = true,
                                        .runtime_contract = TestRuntimeContract(),
                };
        };
    WorkerCoordinator coordinator(std::move(config));
    ASSERT_TRUE(coordinator.Start().started());
    const auto ready = coordinator.SnapshotReadyWorkers();
    ASSERT_EQ(ready.size(), 1u);

    const auto stale_workset = TestTargetedWorkset();
    const auto stale = coordinator.SubmitWorksetToWorker(
        WorkerExecutionTarget{
            .worker_id = ready[0].worker_id,
            .process_generation =
                ready[0].process_generation + 1,
        },
        stale_workset,
        savor::runtime::InitialWorksetCancellationSidecarV1{
            .workset_id = stale_workset.workset_id,
        });
    EXPECT_EQ(
        stale.disposition,
        WorkerSubmitDisposition::StaleGeneration);

    const auto invalid_workset = TestTargetedWorkset("other.module");
    const auto invalid = coordinator.SubmitWorksetToWorker(
        WorkerExecutionTarget{
            .worker_id = ready[0].worker_id,
            .process_generation =
                ready[0].process_generation,
        },
        invalid_workset,
        savor::runtime::InitialWorksetCancellationSidecarV1{
            .workset_id = invalid_workset.workset_id,
        });
    EXPECT_EQ(
        invalid.disposition,
        WorkerSubmitDisposition::InvalidWorkset);
    EXPECT_EQ(
        coordinator.SnapshotTelemetry().submit_accepted,
        0u);
    coordinator.Stop();
}

TEST(
    WorkerCoordinator,
    VisualModeRemainsInTheHomogeneousPool) {
    WorkerCoordinatorConfig config{
        .desired_workers = 1,
        .worker_mode = savor::runtime::WorkerMode::Visual,
    };
        config.worker_runtime_preflight =
        [](
            std::size_t,
            const WorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>&) {
            return savor::runner::parallel::savordb::
                WorkerCoordinatorRuntimePreflightResult{
                    .process_ready = true,
                    .runtime_contract = TestRuntimeContract(),
                };
        };
    WorkerCoordinator coordinator(std::move(config));
    ASSERT_TRUE(coordinator.Start().started());
    const auto ready = coordinator.SnapshotReadyWorkers();
    ASSERT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready.front().mode, savor::runtime::WorkerMode::Visual);
    EXPECT_EQ(
        ready.front().runtime_contract_sha256,
        TestRuntimeContract().canonical_sha256);
    coordinator.Stop();
}

TEST(
    WorkerCoordinator,
    VisualDebugModeIsExcludedFromScheduledWorkerPools) {
    WorkerCoordinatorConfig config{
        .desired_workers = 1,
        .worker_mode = savor::runtime::WorkerMode::VisualDebug,
    };
    WorkerCoordinator coordinator(std::move(config));
    const auto started = coordinator.Start();
    EXPECT_EQ(
        started.status,
        WorkerCoordinatorStartStatus::StartupExhausted);
    EXPECT_TRUE(coordinator.SnapshotReadyWorkers().empty());
    EXPECT_NE(
        started.diagnostic.find("Headless or Visual"),
        std::string::npos)
        << started.diagnostic;
}

TEST(
    WorkerCoordinator,
    UnderqualifiedWorkerNeverBecomesReady) {
    WorkerCoordinatorConfig config{
        .desired_workers = 1,
        .max_worker_start_attempts = 1,
    };
        config.worker_runtime_preflight =
        [](
            std::size_t,
            const WorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>&) {
            auto contract = TestRuntimeContract();
            contract.limits.maximum_items_per_workset = 15;
            contract.canonical_sha256 =
                savor::runtime::ComputeWorkerRuntimeContractHashV1(
                    contract);
            return savor::runner::parallel::savordb::
                WorkerCoordinatorRuntimePreflightResult{
                    .process_ready = true,
                    .runtime_contract = std::move(contract),
                };
        };
    WorkerCoordinator coordinator(std::move(config));
    const auto start = coordinator.Start();
    EXPECT_EQ(
        start.status,
        WorkerCoordinatorStartStatus::StartupExhausted);
    EXPECT_TRUE(coordinator.SnapshotReadyWorkers().empty());
    EXPECT_NE(
        start.diagnostic.find("expected homogeneous runtime identity"),
        std::string::npos)
        << start.diagnostic;
    coordinator.Stop();
}

TEST(
    QueuedExecutionDb,
    ExecutionWorkCallbacksAreImmediateAndWatcherSeesExternalGeneration) {
    TrackingExecutionDb inner;
    inner.publish_updates_availability = true;
    savor::db::execution::QueuedExecutionDb queued(
        &inner,
        savor::db::execution::ExecutionQueueConfig{
            .availability_watch_interval =
                std::chrono::milliseconds(20),
        });
    std::string error;
    ASSERT_TRUE(queued.Start(&error)) << error;
    std::mutex observed_mutex;
    std::vector<savor::db::ExecutionWorkAvailabilitySnapshot> observed;
    const auto subscription = queued.SubscribeExecutionWorkAvailability(
        [&](const auto& snapshot) {
            std::lock_guard lock(observed_mutex);
            observed.push_back(snapshot);
        });
    ASSERT_NE(subscription, 0u);

    savor::db::PublishWorksetWaveReceipt published{};
    ASSERT_TRUE(queued.PublishWorksetWave({}, &published, &error)) << error;
    EXPECT_EQ(
        published.disposition,
        savor::db::ExecutionDbOperationDisposition::Applied);
    ASSERT_TRUE(WaitUntil([&]() {
        std::lock_guard lock(observed_mutex);
        return !observed.empty()
            && observed.back().has_ready_worksets;
    }));

    inner.ready_present = false;
    inner.ready_generation.fetch_add(1);
    ASSERT_TRUE(WaitUntil([&]() {
        std::lock_guard lock(observed_mutex);
        return observed.size() >= 2
            && !observed.back().has_ready_worksets;
    }));
    {
        std::lock_guard lock(observed_mutex);
        EXPECT_GT(
            observed.back().generation,
            observed.front().generation);
    }
    const auto queue_telemetry = queued.GetTelemetrySnapshot();
    EXPECT_GT(queue_telemetry.availability_watcher_reads, 0u);
    EXPECT_GE(queue_telemetry.availability_signal_transitions, 3u);
    EXPECT_GE(queue_telemetry.availability_callback_wakes, 2u);
    queued.UnsubscribeExecutionWorkAvailability(subscription);
    queued.Stop();
    EXPECT_EQ(
        savor::db::execution::ExecutionQueueConfig{}
            .availability_watch_interval,
        std::chrono::seconds(1));
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
        worker_config.worker_runtime_preflight =
        [&](std::size_t worker_id,
            const WorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>&) {
            if (worker_id == 1) {
                secondary_entered = true;
                release.wait();
            }
            return savor::runner::parallel::savordb::
                WorkerCoordinatorRuntimePreflightResult{
                    .process_ready = true,
                                        .runtime_contract = TestRuntimeContract(),
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
        TestProgramDescriptor("test-seed-probe")));
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
    coordinator.OpenCancellationAdmission();

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
        worker_config.worker_runtime_preflight =
        [&](std::size_t worker_id,
            const WorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>&) {
            if (worker_id == 1) {
                secondary_entered = true;
                release.wait();
            }
            return savor::runner::parallel::savordb::
                WorkerCoordinatorRuntimePreflightResult{
                    .process_ready = true,
                                        .runtime_contract = TestRuntimeContract(),
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
        TestProgramDescriptor("test-seed-probe")));
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
    coordinator.OpenCancellationAdmission();
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
        worker_config.worker_runtime_preflight =
        [](
            std::size_t,
            const WorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>&) {
            return savor::runner::parallel::savordb::
                WorkerCoordinatorRuntimePreflightResult{
                    .process_ready = true,
                                        .runtime_contract = TestRuntimeContract(),
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
        TestProgramDescriptor("test-seed-probe")));
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
    coordinator.OpenCancellationAdmission();
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
    EmptyGlobalClaimCampaignBacksOffOneFiveThenThirtySeconds) {
    TemporaryCoordinatorDirectory temporary;
    WorkerCoordinatorConfig worker_config{
        .desired_workers = 1,
        .controller_sleep_ms = 10000,
        .liveness_probe_interval_ms = 60000,
        .liveness_probe_failure_threshold = 1000,
    };
        worker_config.worker_runtime_preflight =
        [](
            std::size_t,
            const WorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>&) {
            return savor::runner::parallel::savordb::
                WorkerCoordinatorRuntimePreflightResult{
                    .process_ready = true,
                                        .runtime_contract = TestRuntimeContract(),
                };
        };
    WorkerCoordinator workers(std::move(worker_config));
    ASSERT_TRUE(workers.Start().started());

    TrackingExecutionDb execution_db;
    savor::db::execution::programdb::ProgramKindRegistry registry;
    ASSERT_TRUE(registry.Register(
        TestProgramDescriptor("test-seed-probe")));
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
    coordinator.OpenCancellationAdmission();
    ASSERT_TRUE(WaitUntil(
        [&]() { return execution_db.claim_calls.load() >= 1; }));
    auto telemetry = coordinator.SnapshotTelemetry();
    EXPECT_EQ(telemetry.claim_backoff_stage, 1u);
    EXPECT_EQ(telemetry.current_claim_backoff_ms, 1000u);
    const bool observed_second = WaitUntil(
        [&]() { return execution_db.claim_calls.load() >= 2; },
        std::chrono::seconds(2));
    if (!observed_second) {
        const auto stalled = coordinator.SnapshotTelemetry();
        ADD_FAILURE()
            << "claims=" << execution_db.claim_calls.load()
            << " ready_workers="
            << workers.SnapshotReadyWorkers().size()
            << " stage=" << stalled.claim_backoff_stage
            << " delay_ms=" << stalled.current_claim_backoff_ms
            << " wake_reason=" << stalled.last_scheduler_wake_reason;
        coordinator.Stop();
        workers.Stop();
        return;
    }
    telemetry = coordinator.SnapshotTelemetry();
    EXPECT_EQ(telemetry.claim_backoff_stage, 2u);
    EXPECT_EQ(telemetry.current_claim_backoff_ms, 5000u);
    ASSERT_TRUE(WaitUntil(
        [&]() { return execution_db.claim_calls.load() >= 3; },
        std::chrono::seconds(7)));
    telemetry = coordinator.SnapshotTelemetry();
    EXPECT_EQ(telemetry.claim_backoff_stage, 3u);
    EXPECT_EQ(telemetry.current_claim_backoff_ms, 30000u);

    coordinator.Stop();
    workers.Stop();
}

TEST(
    JobExecutionCoordinator,
    BlockedSingleReconstructorExposesBacklogWithoutClaimLeaseRenewal) {
    TemporaryCoordinatorDirectory temporary;
    WorkerCoordinatorConfig worker_config{
        .desired_workers = 2,
        .controller_sleep_ms = 250,
        .max_concurrent_worker_starts = 2,
    };
        worker_config.worker_runtime_preflight =
        [](
            std::size_t,
            const WorkerCoordinatorConfig&,
            const std::shared_ptr<savor::ProcessWorker>&) {
            return savor::runner::parallel::savordb::
                WorkerCoordinatorRuntimePreflightResult{
                    .process_ready = true,
                                        .runtime_contract = TestRuntimeContract(),
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
    auto descriptor =
        TestProgramDescriptor("blocked-reconstruction-test");
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
    coordinator.OpenCancellationAdmission();

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
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(execution_db.renew_calls.load(), 0);
    EXPECT_EQ(execution_db.maximum_renewal_batch_size.load(), 0);
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
    ASSERT_TRUE(registry.Register(
        TestProgramDescriptor("test-seed-probe")));
    WorkerCoordinatorConfig worker_config{.desired_workers = 0};
        WorkerCoordinator workers(std::move(worker_config));
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

TEST(
    JobExecutionCoordinator,
    StartupRejectsDescriptorWithMismatchedFullPhaseIdentity) {
    TemporaryCoordinatorDirectory temporary;
    TrackingExecutionDb execution_db;
    savor::db::execution::programdb::ProgramKindRegistry registry;
    auto descriptor = TestProgramDescriptor("mismatched-seed-probe");
    ASSERT_TRUE(descriptor.full_phase_identity.has_value());
    descriptor.full_phase_identity->canonical_sha256[0] =
        descriptor.full_phase_identity->canonical_sha256[0] == 'a'
        ? 'b'
        : 'a';
    ASSERT_TRUE(registry.Register(descriptor));
    WorkerCoordinatorConfig worker_config{.desired_workers = 0};
        WorkerCoordinator workers(std::move(worker_config));
    ASSERT_TRUE(workers.Start().started());
    savor::db::execution::WorkerResultBlobStore blob_store(
        temporary.root() / "object_store");
    JobExecutionCoordinator coordinator(
        &execution_db,
        &registry,
        &workers,
        &blob_store,
        TestConfig());
    std::string error;
    EXPECT_FALSE(coordinator.Start(&error));
    EXPECT_NE(error.find("immutable FullPhase"), std::string::npos)
        << error;
    EXPECT_EQ(execution_db.claim_calls.load(), 0);
    workers.Stop();
}

TEST(
    JobExecutionCoordinator,
    TerminalItemCancellationRaceRequiresExactTypedRejection) {
    using savor::runner::parallel::savordb::WorkerCommandDisposition;
    using savor::runner::parallel::savordb::WorkerCommandKind;
    using savor::runner::parallel::savordb::WorkerCommandResult;

    WorkerCommandResult result{
        .disposition = WorkerCommandDisposition::DefiniteRejected,
        .command_kind = WorkerCommandKind::CancelWorksetItem,
        .rejection_code =
            savor::wrms::RejectionCode::WorksetItemAlreadyTerminal,
        .diagnostic = "Workset item is already terminal",
    };
    EXPECT_TRUE(result.terminal_item_cancellation_race());

    result.rejection_code = savor::wrms::RejectionCode::DuplicateCancellation;
    EXPECT_FALSE(result.terminal_item_cancellation_race());
    auto failure = savor::runner::parallel::savordb::detail::
        DescribeCancellationDeliveryFailure(result);
    EXPECT_STREQ(
        failure.warning_message,
        "Worker cancellation was already requested");
    EXPECT_STREQ(
        failure.error_code,
        "WORKER_CANCELLATION_ALREADY_REQUESTED");

    result.rejection_code =
        savor::wrms::RejectionCode::WorksetItemAlreadyTerminal;
    result.command_kind = WorkerCommandKind::CancelWorkset;
    EXPECT_FALSE(result.terminal_item_cancellation_race());

    result.command_kind = WorkerCommandKind::CancelWorksetItem;
    result.disposition = WorkerCommandDisposition::AmbiguousAfterWrite;
    EXPECT_FALSE(result.terminal_item_cancellation_race());
    failure = savor::runner::parallel::savordb::detail::
        DescribeCancellationDeliveryFailure(result);
    EXPECT_STREQ(
        failure.warning_message,
        "Worker cancellation outcome was ambiguous after write");
    EXPECT_STREQ(
        failure.error_code,
        "WORKER_CANCELLATION_AMBIGUOUS_AFTER_WRITE");

    result.disposition = WorkerCommandDisposition::NotFound;
    failure = savor::runner::parallel::savordb::detail::
        DescribeCancellationDeliveryFailure(result);
    EXPECT_STREQ(
        failure.warning_message,
        "Worker cancellation route was not found");

    result.disposition = WorkerCommandDisposition::DefiniteRejected;
    result.rejection_code = savor::wrms::RejectionCode::WorksetNotFound;
    failure = savor::runner::parallel::savordb::detail::
        DescribeCancellationDeliveryFailure(result);
    EXPECT_STREQ(
        failure.warning_message,
        "Worker cancellation workset was not resident");
}

} // namespace
