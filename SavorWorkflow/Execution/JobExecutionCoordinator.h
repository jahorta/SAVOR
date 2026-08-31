#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "Execution/IExecutionDb.h"
#include "Execution/ProgramDB/ProgramKindRegistry.h"
#include "Execution/WorkerResultBlobStore.h"
#include "WorkerCoordinator.h"

namespace savor::runner::parallel::savordb {

namespace detail {

enum class DispatchRetirementAuthority : std::uint8_t {
    None = 0,
    DrainingPending,
    DrainingPersisted,
    ReleasedDraining,
};

struct DispatchRetirementFacts {
    DispatchRetirementAuthority authority =
        DispatchRetirementAuthority::None;
    bool summary_observed = false;
    std::size_t executable_items = 0;
    std::size_t staged_items = 0;
    std::size_t acknowledged_items = 0;
    bool sidecar_persisted = false;
};

[[nodiscard]] bool DispatchReadyToRetire(
    const DispatchRetirementFacts& facts) noexcept;

struct CancellationDeliveryFailureDescription {
    const char* warning_message = nullptr;
    const char* error_code = nullptr;
};

[[nodiscard]] CancellationDeliveryFailureDescription
DescribeCancellationDeliveryFailure(
    const WorkerCommandResult& result) noexcept;

} // namespace detail

struct JobExecutionCoordinatorConfig {
    std::chrono::milliseconds poll_interval{20};
    std::chrono::milliseconds workset_lease_duration{180000};
    std::chrono::milliseconds workset_lease_renewal_point{90000};
    std::chrono::milliseconds workset_lease_retry_interval{5000};
    std::chrono::milliseconds terminal_retry_interval{250};
    std::chrono::milliseconds terminal_retry_max_interval{30000};
    std::chrono::milliseconds blob_readiness_retry_interval{1000};
    std::chrono::milliseconds blob_readiness_retry_max_interval{30000};
    // Global preparation capacity per currently ready worker. Prepared
    // worksets are not owned by a worker until submission begins.
    std::size_t prepared_worksets_per_ready_worker = 2;
    std::size_t persistence_io_threads = 4;
    std::size_t max_pending_evidence_per_dispatch = 4096;
    std::size_t cancellation_batch_size = 32;
    std::chrono::milliseconds cancellation_mutation_delay{2};
    std::uint32_t maximum_items_per_workset =
        savor::runtime::WorkerWorksetLimits{}.maximum_items_per_workset;
    std::uint64_t maximum_encoded_workset_bytes =
        savor::runtime::WorkerWorksetLimits{}.maximum_encoded_workset_bytes;
    savor::runtime::ArtifactCompatibilityToken state_compatibility;
};

struct JobExecutionCoordinatorWarning {
    std::uint64_t sequence = 0;
    std::int64_t worker_id = 0;
    std::int64_t job_id = 0;
    std::int64_t observed_mono_ns = 0;
    std::string message;
    std::string detail;
};

enum class JobExecutionWorkerDispatchState : std::uint8_t {
    Unavailable = 0,
    Ready,
    Submitting,
    Active,
    Draining,
};

enum class JobExecutionDispatchPersistenceState : std::uint8_t {
    Drained = 0,
    Ready,
    InFlight,
    RetryPending,
    GloballyBlocked,
};

struct JobExecutionWorkerDispatchSnapshot {
    std::size_t worker_id = 0;
    std::uint64_t process_generation = 0;
    JobExecutionWorkerDispatchState state =
        JobExecutionWorkerDispatchState::Unavailable;
    std::optional<std::int64_t> submitting_dispatch_attempt_id;
    std::optional<std::int64_t> active_dispatch_attempt_id;
    std::size_t persistence_queue_depth = 0;
    JobExecutionDispatchPersistenceState persistence_state =
        JobExecutionDispatchPersistenceState::Drained;
    std::uint32_t persistence_retry_attempt = 0;
    std::string persistence_diagnostic;
    std::size_t mailbox_depth = 0;
    std::size_t pending_acknowledgements = 0;
    std::size_t pending_cancellations = 0;
    std::optional<std::string> actual_execution_affinity_key;
};

struct JobExecutionCoordinatorTelemetry {
    std::uint64_t claim_batches = 0;
    std::uint64_t successful_claim_batches = 0;
    std::uint64_t empty_claim_batches = 0;
    std::uint64_t worksets_claimed = 0;
    std::uint64_t worksets_reconstructed = 0;
    std::uint64_t worksets_submitted = 0;
    std::uint64_t submission_calls_started = 0;
    std::uint64_t submission_accepted = 0;
    std::uint64_t submission_temporary_unavailable = 0;
    std::uint64_t submission_stale_generation = 0;
    std::uint64_t submission_deterministic_rejection = 0;
    std::uint64_t submission_ambiguous_after_write = 0;
    std::uint64_t submission_transport_canceled_before_write = 0;
    std::uint64_t worksets_durably_dispatched = 0;
    std::uint64_t reconstruction_invariant_failures = 0;
    std::uint64_t jobs_started = 0;
    std::uint64_t worker_terminals_observed = 0;
    std::uint64_t worker_terminals_staged = 0;
    std::uint64_t
        worker_terminals_discarded_after_authority_release = 0;
    std::uint64_t worker_terminal_acks = 0;
    std::uint64_t worker_terminal_ack_abandoned_generation_loss = 0;
    std::uint64_t worker_terminal_staging_failures = 0;
    std::uint64_t worker_terminal_retry_attempts = 0;
    std::uint64_t dispatch_persistence_attempts = 0;
    std::uint64_t dispatch_persistence_events = 0;
    std::uint64_t dispatch_persistence_failures = 0;
    std::uint64_t dispatch_persistence_retries = 0;
    std::size_t persistence_streams_ready = 0;
    std::size_t persistence_streams_in_flight = 0;
    std::size_t persistence_streams_retrying = 0;
    std::uint64_t blob_readiness_failures = 0;
    std::uint64_t cancellations_delivered = 0;
    std::uint64_t cancellation_precommit_holds_registered = 0;
    std::uint64_t cancellation_precommit_holds_promoted = 0;
    std::size_t cancellation_precommit_holds_pending = 0;
    std::uint64_t committed_cancellations_indexed = 0;
    std::size_t unresolved_requested_cancellation_canaries = 0;
    std::uint64_t waiting_jobs_suppressed_by_sidecar = 0;
    std::uint64_t fully_suppressed_worksets_avoided = 0;
    std::uint64_t sidecar_items_submitted = 0;
    std::uint64_t sidecar_submit_receipts_accepted = 0;
    std::uint64_t sidecar_submit_receipts_repeated = 0;
    std::uint64_t sidecar_submit_receipts_mismatched = 0;
    std::uint64_t post_fence_cancellation_commands = 0;
    std::uint64_t cancellation_mutation_batches = 0;
    std::uint64_t cancellation_mutation_batch_items = 0;
    double cancellation_mutation_batch_average_size = 0.0;
    std::uint64_t cancellation_mutation_full_flushes = 0;
    std::uint64_t cancellation_mutation_deadline_flushes = 0;
    std::uint64_t cancellation_mutation_barrier_flushes = 0;
    std::uint64_t cancellation_mutation_rollbacks = 0;
    std::uint64_t cancellation_mutation_retries = 0;
    std::uint64_t cancellation_mutation_max_size = 0;
    std::uint64_t cancellation_mutation_max_collection_age_ms = 0;
    std::size_t pending_cancellation_mutations = 0;
    std::size_t cancellation_mutation_queue_high_water = 0;
    std::uint64_t worker_losses = 0;
    std::uint64_t startup_recovered_dispatches = 0;
    std::uint64_t startup_interrupted_jobs = 0;
    std::uint64_t active_residence_probes = 0;
    std::uint64_t active_residence_matches = 0;
    std::uint64_t active_residence_failures = 0;
    std::uint64_t active_lease_renewal_batches = 0;
    std::uint64_t active_lease_renewal_retries = 0;
    std::uint64_t draining_transitions = 0;
    std::uint64_t scheduler_wakeups = 0;
    std::uint64_t availability_generation = 0;
    bool ready_worksets_present = false;
    bool execution_finished_results_present = false;
    std::uint64_t availability_signal_wakeups = 0;
    std::uint32_t claim_backoff_stage = 0;
    std::uint64_t current_claim_backoff_ms = 0;
    std::uint64_t reconciliation_claims = 0;
    std::string last_scheduler_wake_reason;
    std::size_t prepared_worksets = 0;
    std::size_t reconstruction_queue_depth = 0;
    std::size_t reconstruction_queue_high_water = 0;
    std::uint64_t reconstruction_oldest_item_age_ms = 0;
    bool reconstruction_active = false;
    std::uint64_t reconstruction_total_duration_ms = 0;
    std::uint64_t reconstruction_max_duration_ms = 0;
    std::size_t global_prepared_worksets = 0;
    std::size_t submitting_worksets = 0;
    std::size_t active_worksets = 0;
    std::size_t draining_worksets = 0;
    std::size_t persistence_queue_depth = 0;
    std::size_t persistence_queue_high_water = 0;
    std::uint64_t persistence_oldest_event_age_ms = 0;
    std::size_t active_worker_streams = 0;
    std::size_t mailbox_command_depth = 0;
    std::size_t worker_control_queue_high_water = 0;
    std::uint64_t worker_control_oldest_command_age_ms = 0;
    std::size_t pending_acknowledgements = 0;
    std::size_t pending_cancellations = 0;
    std::uint64_t worker_control_commands_queued = 0;
    std::uint64_t worker_control_commands_attempted = 0;
    std::uint64_t worker_control_commands_applied = 0;
    std::uint64_t worker_control_commands_failed = 0;
    std::uint64_t worker_control_commands_abandoned = 0;
    std::uint64_t cancellation_deliveries_already_applied = 0;
    std::uint64_t cancellation_delivery_total_latency_ms = 0;
    std::uint64_t cancellation_delivery_max_latency_ms = 0;
    std::map<std::string, std::uint64_t>
        cancellation_resolution_counts;
    std::map<std::string, std::uint64_t>
        dispatch_release_reason_counts;
    std::map<std::string, std::uint64_t>
        dispatch_release_phase_counts;
    bool blob_store_ready = false;
    bool cancellation_admission_open = false;
    bool user_admission_paused = false;
    bool invariant_admission_paused = false;
    bool global_storage_unavailable = false;

    std::string last_error;
};

class JobExecutionCoordinator {
public:
    JobExecutionCoordinator(
        savor::db::IExecutionDb* execution_db,
        const savor::db::execution::programdb::ProgramKindRegistry*
            program_kind_registry,
        WorkerCoordinator* worker_coordinator,
        savor::db::execution::WorkerResultBlobStore* blob_store,
        JobExecutionCoordinatorConfig config = {});
    ~JobExecutionCoordinator();

    JobExecutionCoordinator(const JobExecutionCoordinator&) = delete;
    JobExecutionCoordinator& operator=(const JobExecutionCoordinator&) =
        delete;

    bool Start(std::string* error_out = nullptr);
    void Quiesce();
    bool ReleaseBufferedClaims(std::string* error_out = nullptr);
    bool RecoverAfterWorkersStopped(std::string* error_out = nullptr);
    void Stop();

    void SetPaused(bool paused);
    [[nodiscard]] bool IsPaused() const noexcept;
    [[nodiscard]] bool IsRunning() const noexcept;
    bool ClearInvariantPause();
    void RegisterCancellationCommitPending(
        std::uint64_t hold_id,
        const std::vector<savor::db::ExecutionCancellationRequestSpec>&
            cancellations);
    void RegisterCommittedCancellations(
        std::uint64_t hold_id,
        const std::vector<savor::db::CommittedJobCancellation>&
            cancellations);
    void OpenCancellationAdmission();
    [[nodiscard]] JobExecutionCoordinatorTelemetry SnapshotTelemetry() const;
    [[nodiscard]] std::vector<JobExecutionWorkerDispatchSnapshot>
        SnapshotWorkerDispatches() const;
    [[nodiscard]] std::vector<JobExecutionCoordinatorWarning>
        SnapshotWarnings() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace savor::runner::parallel::savordb
