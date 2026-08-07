#include "SeedProbeRealWorkerScenario.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Analysis/IAnalysisDb.h"
#include "Execution/ProgramDB/ProductionProgramKindRegistry.h"
#include "Execution/ProgramDB/ProgramKindRegistry.h"
#include "Execution/ProgramDB/SeedProbe/SeedProbeJobSpec.h"
#include "Phases/Programs/SeedProbe/SeedProbeModule.h"
#include "Phases/RNGSeedDeltaMap.h"
#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "Utils/Hash.h"

#include "Cli.h"
#include "DbSetup.h"
#include "DurableLogFile.h"
#include "MultiLineProgressRenderer.h"
#include "SplitCoordinatorRuntime.h"
#include "WorkerStartupBarrier.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace savor::e2e {

using savor::db::execution::programdb::ProgramResultProcessorTelemetry;
using savor::db::execution::programdb::WorkerResultBlobCleanupTelemetry;
using savor::db::execution::workflow::WorkflowCoordinatorTelemetry;
using savor::runner::parallel::savordb::JobExecutionCoordinatorTelemetry;
using savor::runner::parallel::savordb::JobExecutionCoordinatorWarning;
using savor::runner::parallel::savordb::
    JobExecutionWorkerLaneSnapshot;
using savor::runner::parallel::savordb::ReadyWorkerCompatibilitySnapshot;
using savor::runner::parallel::savordb::WorkerCoordinatorTelemetry;
using ::WorkerSnapshot;
using ::WorkerStateKind;

namespace {

constexpr std::uint32_t kWorkerStartupOperationTimeoutMs = 60'000;

enum class DurableLineSeverity {
    Info,
    Warning,
    Error,
};

struct DurableLine {
    DurableLineCategory category = DurableLineCategory::Debug;
    DurableLineSeverity severity = DurableLineSeverity::Info;
    std::string tag;
    std::string text;
};

bool AreSeedProbeWorkflowStepsTerminal(
    const savor::db::execution::workflow::WorkflowGraphSnapshot& graph) {
    using savor::db::execution::workflow::WorkflowStepState;
    return !graph.steps.empty()
        && std::all_of(
            graph.steps.begin(),
            graph.steps.end(),
            [](const auto& step) {
                return step.state == WorkflowStepState::Completed
                    || step.state == WorkflowStepState::Failed
                    || step.state == WorkflowStepState::Skipped;
            });
}

bool HasFailedSeedProbeWorkflowStep(
    const savor::db::execution::workflow::WorkflowGraphSnapshot& graph) {
    using savor::db::execution::workflow::WorkflowStepState;
    return std::any_of(
        graph.steps.begin(),
        graph.steps.end(),
        [](const auto& step) {
            return step.state == WorkflowStepState::Failed;
        });
}

const char* ToString(savor::db::execution::workflow::WorkflowInstanceState state) {
    using savor::db::execution::workflow::WorkflowInstanceState;
    switch (state) {
    case WorkflowInstanceState::Pending: return "PENDING";
    case WorkflowInstanceState::Running: return "RUNNING";
    case WorkflowInstanceState::Completed: return "COMPLETED";
    case WorkflowInstanceState::Failed: return "FAILED";
    case WorkflowInstanceState::Canceled: return "CANCELED";
    }
    return "UNKNOWN";
}

const char* ToString(savor::db::execution::workflow::WorkflowStepState state) {
    using savor::db::execution::workflow::WorkflowStepState;
    switch (state) {
    case WorkflowStepState::Waiting: return "WAITING";
    case WorkflowStepState::Ready: return "READY";
    case WorkflowStepState::Materialized: return "MATERIALIZED";
    case WorkflowStepState::Running: return "RUNNING";
    case WorkflowStepState::Completed: return "COMPLETED";
    case WorkflowStepState::Failed: return "FAILED";
    case WorkflowStepState::Skipped: return "SKIPPED";
    }
    return "UNKNOWN";
}

bool IsSeedProbeInteractiveStdout() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return ::isatty(fileno(stdout)) != 0;
#endif
}

std::string ExtractDurableTag(std::string_view line) {
    if (line.empty() || line.front() != '[') {
        return {};
    }
    const auto close = line.find(']');
    if (close == std::string_view::npos || close <= 1) {
        return {};
    }
    return std::string(line.substr(1, close - 1));
}

bool ContainsToken(std::string_view line, std::string_view token) {
    return line.find(token) != std::string_view::npos;
}

DurableLine ClassifyDurableLine(std::string line) {
    DurableLine durable{};
    durable.tag = ExtractDurableTag(line);
    durable.text = std::move(line);

    const auto& tag = durable.tag;
    const auto text = std::string_view(durable.text);
    if (tag == "seedprobe-result"
        || tag == "seedprobe-observation"
        || tag == "seedprobe-evidence") {
        durable.category = DurableLineCategory::Result;
    } else if (tag == "seedprobe-error"
        || tag == "seedprobe-step-failed"
        || ContainsToken(text, " status=failed")) {
        durable.category = DurableLineCategory::Failure;
        durable.severity = DurableLineSeverity::Error;
    } else if (tag == "seedprobe-warning") {
        durable.category = DurableLineCategory::Warning;
        durable.severity = DurableLineSeverity::Warning;
    } else if (tag == "seedprobe-step-materialized") {
        durable.category = DurableLineCategory::Workflow;
    } else if (tag == "seedprobe-split-coordinators") {
        durable.category = DurableLineCategory::Workflow;
    } else if (tag == "seedprobe-materialization"
        || tag == "seedprobe-materialization-counts") {
        durable.category = DurableLineCategory::Materialization;
    } else if (tag == "seedprobe-continuation") {
        durable.category = DurableLineCategory::Workflow;
    } else {
        durable.category = DurableLineCategory::Debug;
    }
    return durable;
}

bool ShouldDisplayDurableLine(const DurableLine& line, std::uint32_t mask) {
    if (line.severity == DurableLineSeverity::Error || line.severity == DurableLineSeverity::Warning) {
        return true;
    }
    return (mask & DurableLineBit(line.category)) != 0;
}

std::string FormatProgressDetails(
    std::int64_t job_set_id,
    const savor::db::ExecutionJobSetProgressDetails& row) {
    const std::int64_t total = row.total_jobs;
    const std::int64_t done = row.completed_jobs;
    const std::int64_t ok = row.succeeded_jobs;
    const std::int64_t fail = row.failed_jobs;
    const std::int64_t can = row.canceled_jobs;
    const std::int64_t remaining = std::max<std::int64_t>(0, total - ok - fail - can);

    std::ostringstream progress;
    progress << "job_set=" << job_set_id
             << " progress done=" << done << "/" << total;
    if (row.expected_total.has_value()) {
        progress << " expected_total=" << *row.expected_total;
    }
    progress << " ok=" << ok
             << " fail=" << fail
             << " can=" << can
             << " remaining=" << remaining;
    return progress.str();
}

std::vector<std::string> BuildNewFailedStepEventLines(
    const savor::db::execution::workflow::WorkflowGraphSnapshot& graph,
    std::unordered_set<std::int64_t>* emitted_failed_step_ids) {
    std::vector<std::string> lines;
    if (emitted_failed_step_ids == nullptr) {
        return lines;
    }

    for (const auto& step : graph.steps) {
        if (step.state != savor::db::execution::workflow::WorkflowStepState::Failed) {
            continue;
        }
        if (!emitted_failed_step_ids->insert(step.workflow_step_id).second) {
            continue;
        }

        std::ostringstream oss;
        oss << "[seedprobe-step-failed] key=" << step.step_key
            << " kind=" << step.step_kind
            << " workflow_step_id=" << step.workflow_step_id;
        if (step.job_set_id.has_value()) {
            oss << " job_set=" << *step.job_set_id;
        }
        if (step.blocked_reason.has_value() && !step.blocked_reason->empty()) {
            oss << " reason=" << *step.blocked_reason;
        }
        lines.push_back(oss.str());
    }
    return lines;
}

std::vector<std::string> BuildNewMaterializedStepEventLines(
    savor::db::IExecutionDb* execution_db,
    const savor::db::execution::workflow::WorkflowGraphSnapshot& graph,
    std::unordered_set<std::int64_t>* emitted_materialized_step_ids) {
    std::vector<std::string> lines;
    if (execution_db == nullptr || emitted_materialized_step_ids == nullptr) {
        return lines;
    }

    for (const auto& step : graph.steps) {
        if (!step.job_set_id.has_value()) {
            continue;
        }
        if (step.state != savor::db::execution::workflow::WorkflowStepState::Materialized
            && step.state != savor::db::execution::workflow::WorkflowStepState::Running
            && step.state != savor::db::execution::workflow::WorkflowStepState::Completed) {
            continue;
        }
        if (!emitted_materialized_step_ids->insert(step.workflow_step_id).second) {
            continue;
        }

        const auto job_set_id = *step.job_set_id;
        const auto details = execution_db->GetJobSetProgress(job_set_id);
        std::ostringstream oss;
        oss << "[seedprobe-step-materialized] key=" << step.step_key
            << " kind=" << step.step_kind
            << " workflow_step_id=" << step.workflow_step_id
            << " state=" << ToString(step.state)
            << " job_set=" << job_set_id;
        if (details.has_value()) {
            oss << " total=" << details->total_jobs
                << " done=" << details->completed_jobs
                << " ok=" << details->succeeded_jobs
                << " fail=" << details->failed_jobs
                << " can=" << details->canceled_jobs;
            if (details->expected_total.has_value()) {
                oss << " expected_total=" << *details->expected_total;
            }
        } else {
            oss << " progress=unavailable";
        }

        const auto child_rows = execution_db->GetChildJobSetProgress(job_set_id);
        if (!child_rows.empty()) {
            std::int64_t child_total = 0;
            std::int64_t child_expected = 0;
            for (const auto& child : child_rows) {
                child_total += child.total_jobs;
                child_expected += child.expected_total.value_or(0);
            }
            oss << " child_job_sets=" << child_rows.size()
                << " child_total=" << child_total
                << " child_expected_total=" << child_expected;
        }
        lines.push_back(oss.str());
    }
    return lines;
}

std::string FormatCoordinatorTelemetryLine(
    const SplitCoordinatorTelemetry& telemetry,
    size_t active_workers) {
    std::ostringstream oss;
    oss << "workers=" << active_workers
        << " claim_batches=" << telemetry.execution.claim_batches
        << " claim_batches_success="
        << telemetry.execution.successful_claim_batches
        << " claim_batches_empty="
        << telemetry.execution.empty_claim_batches
        << " worksets_claimed=" << telemetry.execution.worksets_claimed
        << " reconstructed="
        << telemetry.execution.worksets_reconstructed
        << " submitted=" << telemetry.execution.worksets_submitted
        << " submit_calls="
        << telemetry.execution.submission_calls_started
        << " submit_accepted="
        << telemetry.execution.submission_accepted
        << " submit_ambiguous="
        << telemetry.execution.submission_ambiguous_after_write
        << " submit_temporary="
        << telemetry.execution.submission_temporary_unavailable
        << " submit_stale="
        << telemetry.execution.submission_stale_generation
        << " submit_incompatible="
        << telemetry.execution.submission_incompatible
        << " submit_deterministic="
        << telemetry.execution.submission_deterministic_rejection
        << " submit_transport_canceled="
        << telemetry.execution
               .submission_transport_canceled_before_write
        << " terminals_observed="
        << telemetry.execution.worker_terminals_observed
        << " terminals_staged="
        << telemetry.execution.worker_terminals_staged
        << " terminals_discarded_after_authority_release="
        << telemetry.execution
               .worker_terminals_discarded_after_authority_release
        << " terminal_acks="
        << telemetry.execution.worker_terminal_acks
        << " terminal_stage_failures="
        << telemetry.execution.worker_terminal_staging_failures
        << " terminal_retries="
        << telemetry.execution.worker_terminal_retry_attempts
        << " worker_event_batches="
        << telemetry.execution.worker_event_batches
        << " worker_event_items="
        << telemetry.execution.worker_event_batch_items
        << " worker_event_batch_max="
        << telemetry.execution.worker_event_batch_max_size
        << " worker_event_batch_avg="
        << telemetry.execution.worker_event_batch_average_size
        << " worker_event_batch_q="
        << telemetry.execution.worker_event_batch_queue_depth
        << " worker_event_batch_q_hwm="
        << telemetry.execution.worker_event_batch_queue_high_water
        << " worker_event_batch_rollbacks="
        << telemetry.execution.worker_event_batch_rollbacks
        << " worker_event_full_flushes="
        << telemetry.execution.worker_event_full_flushes
        << " worker_event_deadline_flushes="
        << telemetry.execution.worker_event_deadline_flushes
        << " worker_event_barrier_flushes="
        << telemetry.execution.worker_event_barrier_flushes
        << " blob_ready="
        << (telemetry.execution.blob_store_ready ? 1 : 0)
        << " blob_readiness_failures="
        << telemetry.execution.blob_readiness_failures
        << " pending_terminals="
        << telemetry.execution.pending_worker_terminals
        << " reserved=" << telemetry.execution.reserved_slots
        << " recon_q=" << telemetry.execution.reconstruction_queue_depth
        << " recon_q_hwm="
        << telemetry.execution.reconstruction_queue_high_water
        << " recon_oldest_ms="
        << telemetry.execution.reconstruction_oldest_item_age_ms
        << " recon_active="
        << (telemetry.execution.reconstruction_active ? 1 : 0)
        << " recon_total_ms="
        << telemetry.execution.reconstruction_total_duration_ms
        << " recon_max_ms="
        << telemetry.execution.reconstruction_max_duration_ms
        << " waiting="
        << telemetry.execution.reconstructed_waiting_worksets
        << " submitting=" << telemetry.execution.submitting_worksets
        << " active=" << telemetry.execution.active_worksets
        << " draining_worksets="
        << telemetry.execution.draining_worksets
        << " persist_q="
        << telemetry.execution.persistence_queue_depth
        << " persist_q_hwm="
        << telemetry.execution.persistence_queue_high_water
        << " persist_oldest_ms="
        << telemetry.execution.persistence_oldest_event_age_ms
        << " persist_streams="
        << telemetry.execution.active_worker_streams
        << " control_q="
        << telemetry.execution.worker_control_queue_depth
        << " control_q_hwm="
        << telemetry.execution.worker_control_queue_high_water
        << " control_oldest_ms="
        << telemetry.execution.worker_control_oldest_command_age_ms
        << " control_attempted="
        << telemetry.execution.worker_control_commands_attempted
        << " control_applied="
        << telemetry.execution.worker_control_commands_applied
        << " control_failed="
        << telemetry.execution.worker_control_commands_failed
        << " control_abandoned="
        << telemetry.execution.worker_control_commands_abandoned
        << " pending_acks="
        << telemetry.execution.pending_acknowledgements
        << " pending_cancels="
        << telemetry.execution.pending_cancellations
        << " cancellation_holds_registered="
        << telemetry.execution.cancellation_precommit_holds_registered
        << " cancellation_holds_promoted="
        << telemetry.execution.cancellation_precommit_holds_promoted
        << " cancellation_holds_pending="
        << telemetry.execution.cancellation_precommit_holds_pending
        << " cancellations_indexed="
        << telemetry.execution.committed_cancellations_indexed
        << " cancellation_requested_canaries="
        << telemetry.execution.unresolved_requested_cancellation_canaries
        << " sidecar_suppressed_jobs="
        << telemetry.execution.waiting_jobs_suppressed_by_sidecar
        << " fully_canceled_worksets_avoided="
        << telemetry.execution.fully_canceled_worksets_avoided
        << " sidecar_items_submitted="
        << telemetry.execution.sidecar_items_submitted
        << " sidecar_receipts_accepted="
        << telemetry.execution.sidecar_submit_receipts_accepted
        << " sidecar_receipts_repeated="
        << telemetry.execution.sidecar_submit_receipts_repeated
        << " sidecar_receipts_mismatched="
        << telemetry.execution.sidecar_submit_receipts_mismatched
        << " post_fence_cancellations="
        << telemetry.execution.post_fence_cancellation_commands
        << " cancellation_mutation_batches="
        << telemetry.execution.cancellation_mutation_batches
        << " cancellation_mutation_items="
        << telemetry.execution.cancellation_mutation_batch_items
        << " cancellation_mutation_full_flushes="
        << telemetry.execution.cancellation_mutation_full_flushes
        << " cancellation_mutation_deadline_flushes="
        << telemetry.execution.cancellation_mutation_deadline_flushes
        << " cancellation_mutation_barrier_flushes="
        << telemetry.execution.cancellation_mutation_barrier_flushes
        << " cancellation_mutation_rollbacks="
        << telemetry.execution.cancellation_mutation_rollbacks
        << " cancellation_mutations_pending="
        << telemetry.execution.pending_cancellation_mutations
        << " pause_user="
        << (telemetry.execution.user_admission_paused ? 1 : 0)
        << " pause_invariant="
        << (telemetry.execution.invariant_admission_paused ? 1 : 0)
        << " pause_storage="
        << (telemetry.execution.storage_admission_paused ? 1 : 0)
        << " claims_storage_paused="
        << (telemetry.execution.claims_paused_for_terminal_staging
                ? 1
                : 0)
        << " startup_recovered_dispatches="
        << telemetry.execution.startup_recovered_dispatches
        << " startup_requeued_jobs="
        << telemetry.execution.startup_requeued_jobs
        << " startup_recovery_attempts_granted="
        << telemetry.execution.startup_recovery_attempts_granted
        << " active_residence_probes="
        << telemetry.execution.active_residence_probes
        << " active_residence_matches="
        << telemetry.execution.active_residence_matches
        << " active_residence_failures="
        << telemetry.execution.active_residence_failures
        << " active_lease_renewal_batches="
        << telemetry.execution.active_lease_renewal_batches
        << " active_lease_renewal_retries="
        << telemetry.execution.active_lease_renewal_retries
        << " draining_transitions="
        << telemetry.execution.draining_transitions
        << " scheduler_wakeups="
        << telemetry.execution.scheduler_wakeups
        << " ready_work_generation="
        << telemetry.execution.availability_generation
        << " ready_work_present="
        << (telemetry.execution.ready_worksets_present ? 1 : 0)
        << " results_present="
        << (telemetry.execution.execution_finished_results_present ? 1 : 0)
        << " ready_work_signal_wakeups="
        << telemetry.execution.availability_signal_wakeups
        << " claim_backoff_stage="
        << telemetry.execution.claim_backoff_stage
        << " claim_backoff_ms="
        << telemetry.execution.current_claim_backoff_ms
        << " reconciliation_claims="
        << telemetry.execution.reconciliation_claims
        << " scheduler_wake_reason="
        << telemetry.execution.last_scheduler_wake_reason
        << " results_claimed=" << telemetry.results.claims
        << " result_claim_batches=" << telemetry.results.claim_batches
        << " result_claim_batch_max="
        << telemetry.results.claim_batch_max_size
        << " result_claim_batch_avg="
        << telemetry.results.claim_batch_average_size
        << " results_finalized=" << telemetry.results.finalized
        << " result_finalization_batches="
        << telemetry.results.finalization_batches
        << " result_finalization_batch_max="
        << telemetry.results.finalization_batch_max_size
        << " result_finalization_batch_avg="
        << telemetry.results.finalization_batch_average_size
        << " result_finalization_full_flushes="
        << telemetry.results.finalization_full_flushes
        << " result_finalization_deadline_flushes="
        << telemetry.results.finalization_deadline_flushes
        << " result_finalization_barrier_flushes="
        << telemetry.results.finalization_barrier_flushes
        << " result_processing_canaries="
        << telemetry.results.processing_canaries
        << " cancellation_requested_canaries="
        << telemetry.execution.unresolved_requested_cancellation_canaries
        << " workflow_materialized="
        << telemetry.workflow.materialization_count
        << " workflow_advanced="
        << telemetry.workflow.targeted_terminal_advancement_count;
    return oss.str();
}

const char* AffinityStateName(
    savor::runner::parallel::savordb::
        WorkerSchedulerAffinityState state) {
    using State = savor::runner::parallel::savordb::
        WorkerSchedulerAffinityState;
    switch (state) {
    case State::Cold: return "COLD";
    case State::Projected: return "PROJECTED";
    case State::Actual: return "ACTUAL";
    }
    return "COLD";
}

std::string FormatWorkerLaneLine(
    const JobExecutionWorkerLaneSnapshot& lane) {
    std::ostringstream out;
    out << "lane=w" << lane.worker_id
        << " gen=" << lane.process_generation
        << " reserved=" << lane.reservations
        << " reconstructing=" << lane.reconstructing
        << " waiting=" << lane.waiting_queue_depth
        << " submitting="
        << (lane.submitting_dispatch_attempt_id.has_value()
                ? std::to_string(
                    *lane.submitting_dispatch_attempt_id)
                : "none")
        << " active="
        << (lane.active_dispatch_attempt_id.has_value()
                ? std::to_string(*lane.active_dispatch_attempt_id)
                : "none")
        << " persist_q=" << lane.persistence_queue_depth
        << " control_q=" << lane.control_queue_depth
        << " acks=" << lane.pending_acknowledgements
        << " cancels=" << lane.pending_cancellations;
    out << " affinity=" << AffinityStateName(lane.affinity_state)
        << " projected_execution="
        << lane.projected_execution_affinity_key.value_or("none")
        << " actual_execution="
        << lane.actual_execution_affinity_key.value_or("none");
    return out.str();
}

std::string FormatExecutionDbQueueLine(
    const savor::db::execution::ExecutionQueueTelemetrySnapshot&
        snapshot) {
    const auto maximum_latency =
        [](const auto& operations, auto member) {
            std::uint64_t value = 0;
            for (const auto& operation : operations) {
                value = std::max(
                    value,
                    (operation.*member).max_ms);
            }
            return value;
        };
    std::ostringstream out;
    out << "execution_db_queue"
        << " write_depth=" << snapshot.write_depth
        << " read_depth=" << snapshot.read_depth
        << " write_hwm=" << snapshot.write_high_water_depth
        << " read_hwm=" << snapshot.read_high_water_depth
        << " write_oldest_ms="
        << snapshot.write_oldest_queued_age_ms
        << " read_oldest_ms="
        << snapshot.read_oldest_queued_age_ms
        << " write_wait_max_ms="
        << maximum_latency(
            snapshot.queued.write_lane.operations,
            &savor::db::core::DbOperationTelemetrySnapshot::
                queue_wait)
        << " write_exec_max_ms="
        << maximum_latency(
            snapshot.queued.write_lane.operations,
            &savor::db::core::DbOperationTelemetrySnapshot::
                execution)
        << " read_wait_max_ms="
        << maximum_latency(
            snapshot.queued.read_lane.operations,
            &savor::db::core::DbOperationTelemetrySnapshot::
                queue_wait)
        << " read_exec_max_ms="
        << maximum_latency(
            snapshot.queued.read_lane.operations,
            &savor::db::core::DbOperationTelemetrySnapshot::
                execution)
        << " ready_watcher_reads="
        << snapshot.availability_watcher_reads
        << " ready_signal_transitions="
        << snapshot.availability_signal_transitions
        << " ready_callback_wakes="
        << snapshot.availability_callback_wakes
        << " workset_waves=" << snapshot.workset_waves
        << " wave_worksets=" << snapshot.worksets_published_in_waves
        << " wave_jobs=" << snapshot.jobs_published_in_waves
        << " wave_ready_transitions="
        << snapshot.workset_wave_ready_transitions;
    return out.str();
}

std::string FormatWorkerRollupLine(const std::vector<WorkerSnapshot>& workers) {
    std::size_t running = 0;
    std::size_t idle = 0;
    std::size_t dead = 0;
    std::vector<std::string> assigned_job_ids;
    std::optional<std::string> last_error;

    for (const auto& worker : workers) {
        if (worker.job_id.has_value()) {
            ++running;
            assigned_job_ids.push_back(std::to_string(*worker.job_id));
        } else if (worker.state == WorkerStateKind::Idle || worker.state == WorkerStateKind::Paused) {
            ++idle;
        } else if (worker.state == WorkerStateKind::Dead || worker.state == WorkerStateKind::Stopping) {
            ++dead;
        } else {
            ++idle;
        }

        if (!worker.last_error.empty()) {
            std::ostringstream err;
            err << "w" << worker.worker_id << " pid=" << worker.pid << " err=" << worker.last_error;
            last_error = err.str();
        }
    }

    std::ostringstream oss;
    oss << "worker_rollup running=" << running << " idle=" << idle << " dead=" << dead;
    if (!assigned_job_ids.empty()) {
        oss << " jobs=";
        for (std::size_t i = 0; i < assigned_job_ids.size(); ++i) {
            if (i > 0) {
                oss << ",";
            }
            oss << assigned_job_ids[i];
        }
    } else {
        oss << " jobs=none";
    }
    if (last_error.has_value()) {
        oss << " last_error=" << *last_error;
    }
    return oss.str();
}

std::string FormatWorkflowStateLine(const savor::db::execution::workflow::WorkflowGraphSnapshot& graph) {
    std::array<std::size_t, 7> counts{};
    for (const auto& step : graph.steps) {
        const auto idx = static_cast<std::size_t>(step.state);
        if (idx < counts.size()) {
            ++counts[idx];
        }
    }

    const std::size_t completed = counts[static_cast<std::size_t>(savor::db::execution::workflow::WorkflowStepState::Completed)];
    std::ostringstream oss;
    oss << "workflow=" << ToString(graph.instance.state) << " steps=" << completed << "/" << graph.steps.size()
        << " [WAITING=" << counts[static_cast<std::size_t>(savor::db::execution::workflow::WorkflowStepState::Waiting)]
        << " READY=" << counts[static_cast<std::size_t>(savor::db::execution::workflow::WorkflowStepState::Ready)]
        << " MATERIALIZED=" << counts[static_cast<std::size_t>(savor::db::execution::workflow::WorkflowStepState::Materialized)]
        << " RUNNING=" << counts[static_cast<std::size_t>(savor::db::execution::workflow::WorkflowStepState::Running)]
        << " COMPLETED=" << counts[static_cast<std::size_t>(savor::db::execution::workflow::WorkflowStepState::Completed)]
        << " FAILED=" << counts[static_cast<std::size_t>(savor::db::execution::workflow::WorkflowStepState::Failed)]
        << " SKIPPED=" << counts[static_cast<std::size_t>(savor::db::execution::workflow::WorkflowStepState::Skipped)]
        << "]";
    return oss.str();
}

std::vector<std::string> FormatActiveJobSetLines(
    savor::db::IExecutionDb* execution_db,
    const savor::db::execution::workflow::WorkflowGraphSnapshot& graph) {
    (void)execution_db;
    using savor::db::execution::workflow::WorkflowStepState;
    const auto is_terminal = [](WorkflowStepState state) {
        return state == WorkflowStepState::Completed
            || state == WorkflowStepState::Failed
            || state == WorkflowStepState::Skipped;
    };
    const auto select_step = [&](WorkflowStepState target) -> const savor::db::execution::workflow::WorkflowStepRecord* {
        const auto it = std::find_if(graph.steps.begin(), graph.steps.end(), [&](const auto& step) {
            return step.state == target;
        });
        return it == graph.steps.end() ? nullptr : &(*it);
    };

    const auto* selected_step = select_step(WorkflowStepState::Running);
    if (selected_step == nullptr) selected_step = select_step(WorkflowStepState::Materialized);
    if (selected_step == nullptr) selected_step = select_step(WorkflowStepState::Ready);
    if (selected_step == nullptr) {
        const auto it = std::find_if(graph.steps.begin(), graph.steps.end(), [&](const auto& step) {
            return !is_terminal(step.state);
        });
        selected_step = (it == graph.steps.end()) ? nullptr : &(*it);
    }
    if (selected_step == nullptr) {
        return { "current_step=none (all steps terminal)" };
    }

    std::ostringstream step_label;
    step_label << "current_step key=" << selected_step->step_key
               << " kind=" << selected_step->step_kind
               << " workflow_step_id=" << selected_step->workflow_step_id
               << " state=" << ToString(selected_step->state);
    if (!selected_step->job_set_id.has_value()) {
        return { step_label.str(), "current step not materialized yet" };
    }

    const auto job_set_id = *selected_step->job_set_id;
    const auto details = execution_db != nullptr ? execution_db->GetJobSetProgress(job_set_id) : std::nullopt;
    if (!details.has_value()) {
        return { step_label.str(), "job_set progress unavailable" };
    }

    std::vector<std::string> lines{ step_label.str(), FormatProgressDetails(job_set_id, *details) };
    const auto child_rows = execution_db->GetChildJobSetProgress(job_set_id);
    if (!child_rows.empty()) {
        std::size_t active_children = 0;
        const savor::db::ExecutionChildJobSetProgressDetails* selected_child = nullptr;
        for (const auto& child : child_rows) {
            if (child.completed_jobs < child.total_jobs) {
                ++active_children;
                if (selected_child == nullptr) {
                    selected_child = &child;
                }
            }
        }

        std::ostringstream child_rollup;
        child_rollup << "child_job_sets=" << child_rows.size()
                     << " active=" << active_children;
        lines.push_back(child_rollup.str());

        if (selected_child != nullptr) {
            const auto child_remaining = std::max<std::int64_t>(0, selected_child->total_jobs - selected_child->completed_jobs);
            std::ostringstream child;
            child << "active_child_job_set=" << selected_child->job_set_id;
            child << " done=" << selected_child->completed_jobs << "/" << selected_child->total_jobs
                  << " ok=" << selected_child->succeeded_jobs
                  << " fail=" << selected_child->failed_jobs
                  << " can=" << selected_child->canceled_jobs
                  << " remaining=" << child_remaining;
            lines.push_back(child.str());
        }
    }
    return lines;
}

std::size_t CountActiveWorkers(const std::vector<WorkerSnapshot>& workers) {
    return std::count_if(workers.begin(), workers.end(), [](const WorkerSnapshot& worker) {
        return worker.state != WorkerStateKind::Dead && worker.state != WorkerStateKind::Stopping;
    });
}

std::vector<std::string> BuildProgressLines(
    savor::db::IExecutionDb* execution_db,
    const SplitCoordinatorTelemetry& telemetry,
    const std::vector<WorkerSnapshot>& worker_snapshot,
    const std::optional<savor::db::execution::workflow::WorkflowGraphSnapshot>& graph) {
    std::vector<std::string> lines;
    lines.push_back("");
    lines.push_back("Coordinator Telemetry");
    lines.push_back("");
    lines.push_back(FormatCoordinatorTelemetryLine(telemetry, worker_snapshot.size()));
    if (telemetry.execution_db_queue.has_value()) {
        lines.push_back(
            FormatExecutionDbQueueLine(
                *telemetry.execution_db_queue));
    }
    for (const auto& lane : telemetry.lanes) {
        lines.push_back(FormatWorkerLaneLine(lane));
    }
    if (CountActiveWorkers(worker_snapshot) > 1) {
        lines.push_back(FormatWorkerRollupLine(worker_snapshot));
    }
    if (!graph.has_value()) {
        lines.push_back("workflow=unavailable");
        lines.push_back("job_set=unavailable");
        return lines;
    }

    lines.push_back(FormatWorkflowStateLine(*graph));
    const auto job_lines = FormatActiveJobSetLines(execution_db, *graph);
    lines.insert(lines.end(), job_lines.begin(), job_lines.end());
    return lines;
}

enum class SeedProbeInfrastructureHealth {
    Clean = 0,
    Degraded,
    Failed,
};

const char* InfrastructureHealthName(
    SeedProbeInfrastructureHealth health) {
    switch (health) {
    case SeedProbeInfrastructureHealth::Clean: return "CLEAN";
    case SeedProbeInfrastructureHealth::Degraded: return "DEGRADED";
    case SeedProbeInfrastructureHealth::Failed: return "FAILED";
    }
    return "FAILED";
}

bool ValidateSplitCoordinatorExecution(
    savor::db::IExecutionDb* execution_db,
    const std::optional<
        savor::db::execution::workflow::WorkflowGraphSnapshot>& graph,
    const SplitCoordinatorTelemetry& telemetry,
    const std::vector<ReadyWorkerCompatibilitySnapshot>& ready_workers,
    const savor::runtime::ProgramModuleIdentity&
        expected_seed_probe_module,
    SeedProbeInfrastructureHealth* health_out,
    std::vector<std::string>* health_issues_out,
    std::string* error_out) {
    std::vector<std::string> failures;
    std::vector<std::string> degradations;
    const auto require = [&](bool condition, std::string message) {
        if (!condition) {
            failures.push_back(std::move(message));
        }
    };
    const auto require_clean = [&](bool condition, std::string message) {
        if (!condition) {
            degradations.push_back(std::move(message));
        }
    };

    require(graph.has_value(), "workflow graph is unavailable");
    if (graph.has_value()) {
        using savor::db::execution::workflow::WorkflowInstanceState;
        require(
            graph->instance.state == WorkflowInstanceState::Completed,
            "workflow instance did not reach COMPLETED");
        require(
            graph->steps.size() == 1,
            "SeedProbe E2E must contain exactly one workflow step");
        for (const auto& step : graph->steps) {
            require(
                step.step_kind == "seedprobe.run",
                "workflow contains a non-SeedProbe step kind: "
                    + step.step_kind);
            require(
                step.step_key != "Neutral"
                    && step.step_key != "Grid"
                    && step.step_key != "Unique",
                "legacy SeedProbe workflow step was materialized: "
                    + step.step_key);
        }
        if (execution_db != nullptr
            && execution_db->WorkflowQueryService() != nullptr) {
            const auto outputs =
                execution_db->WorkflowQueryService()->ListStepOutputs(
                    graph->instance.workflow_instance_id);
            const auto accepted =
                std::find_if(
                    outputs.begin(),
                    outputs.end(),
                    [](const auto& output) {
                        return output.output_key
                                == "seed_probe_run"
                            && output.data_kind
                                == "analysis.seed_probe_run"
                            && output.ref_kind == "sp_probe_run"
                            && output.ref_id > 0;
                    });
            require(
                accepted != outputs.end(),
                "SeedProbe workflow did not publish seed_probe_run");
            require(
                std::none_of(
                    outputs.begin(),
                    outputs.end(),
                    [](const auto& output) {
                        return output.data_kind
                                == "state.savestate_id"
                            || output.output_key.find("savestate")
                                != std::string::npos;
                    }),
                "SeedProbe workflow published a forbidden savestate output");
        }

        if (graph->steps.size() == 1
            && graph->steps.front().job_set_id.has_value()
            && execution_db != nullptr) {
            const auto root_job_set_id =
                *graph->steps.front().job_set_id;
            const auto root =
                execution_db->GetJobSetProgress(root_job_set_id);
            const auto survey_jobs =
                execution_db->ListJobsInJobSet(root_job_set_id);
            require(
                root.has_value(),
                "SeedProbe Survey root job-set progress is unavailable");
            if (root.has_value()) {
                require(
                    survey_jobs.size() > 1,
                    "SeedProbe Survey root did not contain multiple scalar "
                    "requests");
                require(
                    root->completed_jobs == root->total_jobs,
                    "SeedProbe job-set hierarchy did not become "
                    "business-final");
                require(
                    root->expected_total.has_value()
                        && *root->expected_total
                            == root->total_jobs,
                    "SeedProbe recursive expected_total does not match "
                    "the complete job-set hierarchy");
            }

            std::vector<savor::db::ExecutionChildJobSetProgressDetails>
                children;
            std::vector<std::int64_t> all_job_set_ids{
                root_job_set_id,
            };
            std::deque<std::int64_t> job_sets_to_visit{
                root_job_set_id,
            };
            while (!job_sets_to_visit.empty()) {
                const auto parent = job_sets_to_visit.front();
                job_sets_to_visit.pop_front();
                auto direct_children =
                    execution_db->GetChildJobSetProgress(parent);
                for (const auto& child : direct_children) {
                    job_sets_to_visit.push_back(child.job_set_id);
                    all_job_set_ids.push_back(child.job_set_id);
                }
                children.insert(
                    children.end(),
                    std::make_move_iterator(direct_children.begin()),
                    std::make_move_iterator(direct_children.end()));
            }
            require(
                !children.empty(),
                "SeedProbe materializer did not add an internal child "
                "job set");
            const bool saw_confirm =
                std::any_of(
                    children.begin(),
                    children.end(),
                    [](const auto& child) {
                        return child.purpose == "SEEDPROBE_CONFIRM";
                    });
            require(
                saw_confirm,
                "SeedProbe materializer did not publish a Confirm child "
                "job set");
            for (const auto& child : children) {
                require(
                    child.completed_jobs == child.total_jobs,
                    "SeedProbe child job set "
                        + std::to_string(child.job_set_id)
                        + " did not become business-final");
            }

            std::size_t jobs_with_worker_terminals = 0;
            for (const auto job_set_id : all_job_set_ids) {
                const auto jobs =
                    execution_db->ListJobsInJobSet(job_set_id);
                for (const auto& listed : jobs) {
                    const auto job = execution_db->GetJob(listed.job_id);
                    require(
                        job.has_value(),
                        "execution job disappeared from job set "
                            + std::to_string(job_set_id));
                    if (!job.has_value()) {
                        continue;
                    }
                    const bool business_final =
                        job->state != "PENDING_WORKSET"
                        && job->state != "QUEUED"
                        && job->state != "CLAIMED"
                        && job->state != "RUNNING"
                        && job->state != "EXECUTION_FINISHED";
                    require(
                        business_final,
                        "execution job "
                            + std::to_string(job->job_id)
                            + " is not business-final; state="
                            + job->state);
                    require(
                        job->state != "EXECUTION_FINISHED",
                        "execution job "
                            + std::to_string(job->job_id)
                            + " remained at EXECUTION_FINISHED");
                    require(
                        job->result_processing_failures == 0,
                        "execution job "
                            + std::to_string(job->job_id)
                            + " recorded a result-processing failure");
                    if (job->worker_terminal_status.has_value()) {
                        ++jobs_with_worker_terminals;
                        require(
                            job->result_processing_state
                                == std::optional<std::string>(
                                    "PROCESSED"),
                            "worker terminal for execution job "
                                + std::to_string(job->job_id)
                                + " was not processed");
                    }
                }
            }
            require_clean(
                jobs_with_worker_terminals
                    == telemetry.execution.worker_terminals_staged,
                "durable worker-terminal job count differs from "
                "JobExecutionCoordinator telemetry");
        } else {
            failures.push_back(
                "SeedProbe workflow step has no root job set");
        }
    }

    require_clean(
        telemetry.workflow.materialization_count > 0,
        "WorkflowCoordinatorService did not materialize the SeedProbe step");
    require_clean(
        telemetry.workflow.targeted_terminal_notification_count > 0,
        "WorkflowCoordinatorService received no result-finalization "
        "notifications");
    require_clean(
        telemetry.workflow.targeted_terminal_advancement_count > 0,
        "WorkflowCoordinatorService did not advance from a targeted "
        "terminal notification");
    require_clean(
        telemetry.workflow.workflow_completed_count > 0,
        "WorkflowCoordinatorService did not complete the workflow");
    require_clean(
        telemetry.workflow.materialization_failure_count == 0
            && telemetry.workflow.workflow_failed_count == 0,
        "WorkflowCoordinatorService reported a materialization or workflow "
        "failure");

    require_clean(
        telemetry.execution.worksets_claimed > 0,
        "JobExecutionCoordinator claimed no published worksets");
    require_clean(
        telemetry.execution.worksets_claimed
            == telemetry.execution.worksets_reconstructed
            && telemetry.execution.worksets_reconstructed
                == telemetry.execution.worksets_submitted
                    + telemetry.execution.fully_canceled_worksets_avoided,
        "JobExecutionCoordinator claim/reconstruct/submit-or-suppress counts differ");
    const auto execution_submission_outcomes =
        telemetry.execution.submission_accepted
        + telemetry.execution.submission_temporary_unavailable
        + telemetry.execution.submission_stale_generation
        + telemetry.execution.submission_incompatible
        + telemetry.execution.submission_deterministic_rejection
        + telemetry.execution.submission_ambiguous_after_write
        + telemetry.execution
              .submission_transport_canceled_before_write;
    require_clean(
        telemetry.execution.submission_calls_started
            == execution_submission_outcomes,
        "JobExecutionCoordinator submission outcomes do not reconcile");
    require_clean(
        telemetry.execution.worksets_durably_dispatched
            == telemetry.execution.worksets_submitted,
        "JobExecutionCoordinator durable dispatch accounting differs");
    require_clean(
        telemetry.execution.reconstruction_invariant_failures == 0,
        "JobExecutionCoordinator reported a reconstruction invariant "
        "failure");
    require_clean(
        telemetry.execution.cancellation_precommit_holds_pending == 0
            && telemetry.execution
                   .unresolved_requested_cancellation_canaries == 0
            && telemetry.execution.pending_cancellation_mutations == 0,
        "JobExecutionCoordinator retained unresolved cancellation state");
    require_clean(
        telemetry.execution.sidecar_submit_receipts_mismatched == 0
            && telemetry.execution.cancellation_mutation_rollbacks == 0,
        "JobExecutionCoordinator reported sidecar or cancellation-persistence anomalies");
    require_clean(
        telemetry.execution.worker_terminals_staged > 0,
        "JobExecutionCoordinator staged no worker terminals");
    require_clean(
        telemetry.execution.worker_terminal_acks
            == telemetry.execution.worker_terminals_staged
                + telemetry.execution
                      .worker_terminals_discarded_after_authority_release,
        "JobExecutionCoordinator terminal acknowledgements do not reconcile");
    require_clean(
        telemetry.execution.worker_terminals_observed
            == telemetry.execution.worker_terminals_staged
                + telemetry.execution
                      .worker_terminals_discarded_after_authority_release,
        "JobExecutionCoordinator observed terminal accounting does not reconcile");
    require_clean(
        telemetry.execution
                .worker_terminals_discarded_after_authority_release
            == 0,
        "JobExecutionCoordinator discarded terminal facts after durable authority release");
    require_clean(
        telemetry.execution
                .worker_terminal_ack_abandoned_generation_loss
            == 0,
        "JobExecutionCoordinator abandoned staged terminal acknowledgements after generation loss");
    require_clean(
        telemetry.execution.worker_terminal_staging_failures == 0
            && telemetry.execution.worker_terminal_retry_attempts == 0,
        "JobExecutionCoordinator required worker-terminal storage recovery");
    require_clean(
        telemetry.execution.active_residence_probes
                == telemetry.execution.active_residence_matches
            && telemetry.execution.active_residence_failures == 0,
        "JobExecutionCoordinator active workset residence evidence did not reconcile");
    require_clean(
        telemetry.execution.active_lease_renewal_retries == 0,
        "JobExecutionCoordinator retried an active workset lease renewal");
    require_clean(
        telemetry.execution.draining_transitions
            == telemetry.execution.worksets_submitted,
        "JobExecutionCoordinator terminal workset-state transitions do not reconcile");
    require_clean(
        telemetry.execution.blob_store_ready
            && telemetry.execution.blob_readiness_failures == 0,
        "JobExecutionCoordinator result blob store was not continuously ready");
    require_clean(
        telemetry.execution.pending_worker_terminals == 0
            && telemetry.execution.draining_worksets == 0
            && !telemetry.execution
                    .claims_paused_for_terminal_staging,
        "JobExecutionCoordinator ended with storage backpressure or "
        "authority-loss draining");
    require_clean(
        !telemetry.execution.invariant_paused
            && telemetry.execution.last_error.empty(),
        "JobExecutionCoordinator ended paused or with an error: "
            + telemetry.execution.last_error);
    require_clean(
        telemetry.execution.jobs_started
            > telemetry.execution.worksets_submitted,
        "SeedProbe E2E did not exercise a multi-item workset");

    require_clean(
        telemetry.worker.submit_accepted > 0,
        "WorkerCoordinator accepted no worksets");
    require_clean(
        telemetry.worker.submit_rejected == 0
            && telemetry.worker.worker_losses == 0,
        "WorkerCoordinator rejected a workset or lost a worker");
    const auto worker_submission_outcomes =
        telemetry.worker.submit_accepted
        + telemetry.worker.submit_ambiguous
        + telemetry.worker.submit_temporary_unavailable
        + telemetry.worker.submit_stale_generation
        + telemetry.worker.submit_incompatible
        + telemetry.worker.submit_deterministic_rejection
        + telemetry.worker.submit_transport_canceled_before_write;
    require_clean(
        telemetry.worker.submit_attempts == worker_submission_outcomes,
        "WorkerCoordinator submission outcomes do not reconcile");
    require_clean(
        telemetry.worker.liveness_probe_failures == 0
            && telemetry.worker.liveness_quarantines == 0,
        "WorkerCoordinator observed a control-plane liveness failure");
    require_clean(
        telemetry.worker.terminal_envelopes
            == telemetry.execution.worker_terminals_observed,
        "WorkerCoordinator terminal count differs from observed terminal "
        "count");
    require_clean(
        telemetry.worker.terminal_ack_accepted
            == telemetry.execution.worker_terminal_acks,
        "WorkerCoordinator terminal acknowledgement count differs from "
        "JobExecutionCoordinator");

    require_clean(
        telemetry.results.claims
                == telemetry.execution.worker_terminals_staged
            && telemetry.results.finalized
                == telemetry.execution.worker_terminals_staged
            && telemetry.results.execution_retries == 0,
        "ProgramResultProcessor did not finalize every staged worker "
        "terminal");
    require_clean(
        telemetry.results.processing_failures == 0
            && telemetry.results.descriptor_unavailable == 0
            && telemetry.results.startup_recovery_canaries == 0
            && telemetry.results.last_error.empty(),
        "ProgramResultProcessor reported a processing failure: "
            + telemetry.results.last_error);
    require_clean(
        telemetry.cleanup.failed == 0
            && telemetry.cleanup.orphan_failed == 0
            && telemetry.cleanup.last_error.empty(),
        "WorkerResultBlobCleanupService reported a failure: "
            + telemetry.cleanup.last_error);

    require_clean(
        !ready_workers.empty(),
        "WorkerCoordinator had no ready worker at scenario completion");
    for (const auto& worker : ready_workers) {
        const auto module =
            std::find_if(
                worker.runtime_manifest.modules.begin(),
                worker.runtime_manifest.modules.end(),
                [&](const auto& candidate) {
                    return candidate.module
                            == expected_seed_probe_module
                        && std::find(
                               candidate.entrypoints.begin(),
                               candidate.entrypoints.end(),
                               savor::runtime::seedprobe::Entrypoint)
                            != candidate.entrypoints.end();
                });
        require_clean(
            module != worker.runtime_manifest.modules.end(),
            "ready worker "
                + std::to_string(worker.worker_id)
                + " did not advertise the exact SeedProbe module");
    }

    if (health_issues_out != nullptr) {
        *health_issues_out = degradations;
    }
    if (failures.empty()) {
        if (health_out != nullptr) {
            *health_out = degradations.empty()
                ? SeedProbeInfrastructureHealth::Clean
                : SeedProbeInfrastructureHealth::Degraded;
        }
        if (error_out != nullptr) {
            error_out->clear();
        }
        return true;
    }

    if (health_out != nullptr) {
        *health_out = SeedProbeInfrastructureHealth::Failed;
    }

    std::ostringstream error;
    error << "split coordinator E2E assertions failed:";
    for (const auto& failure : failures) {
        error << "\n  - " << failure;
    }
    if (error_out != nullptr) {
        *error_out = error.str();
    }
    return false;
}

bool ValidateSeedProbeAcceptedEvidence(
    savor::db::IExecutionDb* execution_db,
    savor::db::IAnalysisDb* analysis_db,
    const std::optional<
        savor::db::execution::workflow::WorkflowGraphSnapshot>& graph,
    std::string* error_out) {
    if (execution_db == nullptr || analysis_db == nullptr
        || !graph.has_value()
        || graph->steps.size() != 1
        || !graph->steps.front().input_ref_id.has_value()
        || *graph->steps.front().input_ref_id <= 0) {
        if (error_out) {
            *error_out =
                "completed SeedProbe step does not identify its probe run";
        }
        return false;
    }

    const auto probe_run_id = *graph->steps.front().input_ref_id;
    const auto run = analysis_db->GetSeedProbeRun(probe_run_id);
    if (!run.has_value()) {
        if (error_out) {
            *error_out =
                "completed SeedProbe run is missing from Analysis DB";
        }
        return false;
    }
    if (run->status != savor::db::SeedProbeRunStatus::Completed) {
        if (error_out) {
            *error_out =
                "managed SeedProbe E2E did not confirm every discovered "
                "delta";
        }
        return false;
    }
    if (run->accepted_input_set_id <= 0) {
        if (error_out) {
            *error_out =
                "completed SeedProbe run has no accepted input set";
        }
        return false;
    }

    const auto accepted_frames =
        analysis_db->ListAnalysisInputSetFrames(
            run->accepted_input_set_id);
    const auto results =
        analysis_db->ListSeedProbeResults(probe_run_id);
    if (accepted_frames.empty()) {
        if (error_out) {
            *error_out =
                "completed SeedProbe accepted input set is empty";
        }
        return false;
    }

    std::vector<std::string> failures;
    const auto& first_frame = accepted_frames.front();
    if (first_frame.ordinal != 0
        || first_frame.main_x != 128
        || first_frame.main_y != 128
        || first_frame.cstick_x != 128
        || first_frame.cstick_y != 128
        || first_frame.trigger_x != 0
        || first_frame.trigger_y != 0) {
        failures.push_back(
            "accepted input set does not begin with the Neutral frame");
    }
    std::unordered_set<std::int64_t> accepted_frame_ids;
    std::vector<const savor::db::SeedProbeResultRow*>
        accepted_representatives;
    accepted_representatives.reserve(accepted_frames.size());
    for (std::size_t accepted_index = 0;
         accepted_index < accepted_frames.size();
         ++accepted_index) {
        const auto& accepted = accepted_frames[accepted_index];
        if (accepted.ordinal
            != static_cast<int>(accepted_index)) {
            failures.push_back(
                "accepted input set ordinals are not contiguous");
        }
        if (!accepted_frame_ids.insert(accepted.input_frame_id).second) {
            failures.push_back(
                "accepted input frame is duplicated: "
                + std::to_string(accepted.input_frame_id));
            continue;
        }

        std::vector<const savor::db::SeedProbeResultRow*>
            representatives;
        for (const auto& result : results) {
            if (result.input_frame_id == accepted.input_frame_id
                && result.evidence_state
                    == savor::db::SeedProbeEvidenceState::Confirmed) {
                representatives.push_back(&result);
            }
        }
        if (representatives.size() != 1) {
            failures.push_back(
                "accepted input frame "
                + std::to_string(accepted.input_frame_id)
                + " does not have exactly one confirmed representative");
            continue;
        }

        const auto& representative = *representatives.front();
        accepted_representatives.push_back(
            representatives.front());
        if (representative.confirmation_of_probe_result_id.has_value()) {
            failures.push_back(
                "accepted representative is itself a confirmation "
                "observation");
        }
        std::vector<const savor::db::SeedProbeResultRow*>
            confirmations;
        for (const auto& result : results) {
            if (result.confirmation_of_probe_result_id
                == representative.probe_result_id) {
                confirmations.push_back(&result);
            }
        }
        if (confirmations.size() != 1) {
            failures.push_back(
                "confirmed representative "
                + std::to_string(representative.probe_result_id)
                + " does not have exactly one confirmation observation");
            continue;
        }

        const auto& confirmation = *confirmations.front();
        if (confirmation.evidence_state
            != savor::db::SeedProbeEvidenceState::Observed) {
            failures.push_back(
                "confirmation fact is not retained as an OBSERVED "
                "observation");
        }
        if (confirmation.input_frame_id
                != representative.input_frame_id
            || confirmation.seed_value != representative.seed_value) {
            failures.push_back(
                "confirmation observation disagrees with representative "
                + std::to_string(representative.probe_result_id));
        }
        if (confirmation.source_job_id
                == representative.source_job_id
            || (confirmation.origin_worker_id
                    == representative.origin_worker_id
                && confirmation.origin_process_generation
                    == representative.origin_process_generation
                && confirmation.origin_workset_epoch
                    == representative.origin_workset_epoch)
            || confirmation.origin_process_generation == 0
            || representative.origin_process_generation == 0
            || confirmation.origin_workset_epoch == 0
            || representative.origin_workset_epoch == 0) {
            failures.push_back(
                "confirmation did not use a distinct job and scoped WorksetEpoch "
                "for representative "
                + std::to_string(representative.probe_result_id));
        }
        if (confirmation.terminal_sha256.empty()
            || representative.terminal_sha256.empty()) {
            failures.push_back(
                "accepted evidence is missing durable terminal hashes");
        }
    }

    const auto confirmed_count =
        std::count_if(
            results.begin(),
            results.end(),
            [](const auto& result) {
                return result.evidence_state
                    == savor::db::SeedProbeEvidenceState::Confirmed;
            });
    if (confirmed_count != accepted_frames.size()) {
        failures.push_back(
            "confirmed representative count differs from accepted frame "
            "count");
    }

    if (accepted_representatives.size()
        == accepted_frames.size()) {
        const auto neutral_seed =
            accepted_representatives.front()->seed_value;
        std::vector<std::int32_t> accepted_deltas;
        std::set<std::int32_t> accepted_delta_set;
        bool accepted_incidental_search_delta = false;
        bool observed_search_result = false;
        accepted_deltas.reserve(
            accepted_representatives.size());
        for (const auto* representative :
             accepted_representatives) {
            const auto delta = savor::wrapped_seed_delta(
                representative->seed_value,
                neutral_seed);
            accepted_deltas.push_back(delta);
            accepted_delta_set.insert(delta);

            const auto source_job =
                execution_db->GetJob(
                    representative->source_job_id);
            const auto source_spec = source_job.has_value()
                ? savor::db::execution::programdb::seedprobe::
                      DecodeSeedProbeJobSpec(
                          source_job->input_ini)
                : std::nullopt;
            if (!source_spec.has_value()
                || (source_spec->stage
                        != savor::db::execution::programdb::
                            seedprobe::SeedProbeJobStage::Survey
                    && source_spec->stage
                        != savor::db::execution::programdb::
                            seedprobe::SeedProbeJobStage::Search)) {
                failures.push_back(
                    "accepted representative has invalid Survey/Search "
                    "source metadata");
                continue;
            }
            if (source_spec->stage
                    == savor::db::execution::programdb::
                        seedprobe::SeedProbeJobStage::Search
                && source_spec->desired_delta.has_value()
                && *source_spec->desired_delta != delta) {
                accepted_incidental_search_delta = true;
            }
        }
        if (accepted_deltas.empty()
            || accepted_deltas.front() != 0) {
            failures.push_back(
                "accepted delta order does not begin with zero");
        }
        for (std::size_t index = 2;
             index < accepted_deltas.size();
             ++index) {
            if (accepted_deltas[index - 1]
                >= accepted_deltas[index]) {
                failures.push_back(
                    "non-neutral accepted deltas are not in strict "
                    "signed order");
                break;
            }
        }
        if (accepted_delta_set.size()
            != accepted_deltas.size()) {
            failures.push_back(
                "accepted input set contains more than one frame for a "
                "delta");
        }

        std::set<std::int32_t> discovered_deltas;
        for (const auto& result : results) {
            if (result.confirmation_of_probe_result_id.has_value()) {
                continue;
            }
            const auto source_job =
                execution_db->GetJob(result.source_job_id);
            const auto source_spec = source_job.has_value()
                ? savor::db::execution::programdb::seedprobe::
                      DecodeSeedProbeJobSpec(
                          source_job->input_ini)
                : std::nullopt;
            if (!source_spec.has_value()
                || (source_spec->stage
                        != savor::db::execution::programdb::
                            seedprobe::SeedProbeJobStage::Survey
                    && source_spec->stage
                        != savor::db::execution::programdb::
                            seedprobe::SeedProbeJobStage::Search)) {
                failures.push_back(
                    "factual result has invalid Survey/Search source "
                    "metadata");
                continue;
            }
            observed_search_result =
                observed_search_result
                || source_spec->stage
                    == savor::db::execution::programdb::
                        seedprobe::SeedProbeJobStage::Search;
            discovered_deltas.insert(
                savor::wrapped_seed_delta(
                    result.seed_value,
                    neutral_seed));
        }
        if (accepted_delta_set != discovered_deltas) {
            failures.push_back(
                "accepted delta set differs from the complete factual "
                "Survey/Search discovery set");
        }
        if (observed_search_result
            && !accepted_incidental_search_delta) {
            failures.push_back(
                "no accepted Search representative was incidental to "
                "its job's desired delta");
        }
    }

    if (failures.empty()) {
        if (error_out) {
            error_out->clear();
        }
        return true;
    }
    std::ostringstream error;
    error << "SeedProbe evidence assertions failed:";
    for (const auto& failure : failures) {
        error << "\n  - " << failure;
    }
    if (error_out) {
        *error_out = error.str();
    }
    return false;
}

} // namespace

bool RunSeedProbeRealWorkerSmokeImpl(
    const CliOptions& options,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out) {
    if (db_service == nullptr) {
        if (error_out) *error_out = "db service is required";
        return false;
    }
    if (!db_service->IsRunning()) {
        if (error_out) *error_out = "db service must be started in main before running scenarios";
        return false;
    }

    const auto worker_exe = ResolveWorkerExePath(argv0);
    if (!std::filesystem::exists(worker_exe)) {
        if (error_out) *error_out = "SavorWorker.exe was not found next to SavorE2E: " + worker_exe.string();
        return false;
    }
    std::string err;

    std::int64_t savestate_id = 0;
    if (!SeedStateSavestate(db_service->StateDb(), options.savestate_file, &savestate_id, &err)) {
        if (error_out) *error_out = "failed seeding StateDB savestate: " + err;
        return false;
    }

    std::int64_t seed_probe_spec_id = 0;
    if (!SeedAuthoringSpec(db_service->AuthoringDb(), options, &seed_probe_spec_id, &err)) {
        if (error_out) *error_out = "failed seeding AuthoringDB seedprobe spec: " + err;
        return false;
    }

    auto* execution_db = db_service->ExecutionDb();
    if (execution_db == nullptr) {
        if (error_out) *error_out = "DBService execution db unavailable";
        return false;
    }

    std::int64_t workflow_instance_id = 0;
    if (!SeedWorkflowGraphExecution(
            db_service->AuthoringDb(),
            execution_db,
            savestate_id,
            seed_probe_spec_id,
            options,
            &workflow_instance_id,
            &err)) {
        if (error_out) *error_out = "failed seeding workflow graph execution rows: " + err;
        return false;
    }

    const auto scenario_workspace_root = options.workspace_root.value_or(
        std::filesystem::temp_directory_path() / "savor-e2e-default");
    auto registry_config =
        savor::db::execution::programdb::MakeProductionProgramKindRegistryConfig(
            scenario_workspace_root / "workflow-runtime");
    savor::db::execution::programdb::ProgramKindRegistry program_kind_registry;
    if (!savor::db::execution::programdb::BuildProductionProgramKindRegistry(
            savor::db::execution::programdb::ProductionProgramKindRegistryDependencies{
                .execution_db = execution_db,
                .state_db = db_service->StateDb(),
                .analysis_db = db_service->AnalysisDb(),
                .authoring_db = db_service->AuthoringDb(),
            },
            std::move(registry_config),
            &program_kind_registry,
            &err)) {
        if (error_out) *error_out = "failed building production program registry: " + err;
        return false;
    }

    DurableLogFile durable_log;
    if (!durable_log.Open(options, options.scenario, error_out)) {
        return false;
    }
    std::cout << "[durable-log] path=" << durable_log.path().string() << '\n';

    std::mutex event_lines_mtx;
    std::deque<DurableLine> pending_event_lines;
    auto enqueue_event_line = [&](std::string line) {
        durable_log.AppendLine(line);
        auto durable = ClassifyDurableLine(std::move(line));
        if (!ShouldDisplayDurableLine(durable, options.durable_line_mask)) {
            return;
        }
        std::lock_guard<std::mutex> lock(event_lines_mtx);
        pending_event_lines.push_back(std::move(durable));
    };
    auto drain_event_lines = [&]() {
        std::vector<DurableLine> lines;
        std::lock_guard<std::mutex> lock(event_lines_mtx);
        while (!pending_event_lines.empty()) {
            lines.push_back(std::move(pending_event_lines.front()));
            pending_event_lines.pop_front();
        }
        return lines;
    };
    auto append_event_lines = [&](std::vector<DurableLine>* dest, std::vector<std::string> raw_lines) {
        if (dest == nullptr) {
            return;
        }
        for (auto& line : raw_lines) {
            durable_log.AppendLine(line);
            auto durable = ClassifyDurableLine(std::move(line));
            if (ShouldDisplayDurableLine(durable, options.durable_line_mask)) {
                dest->push_back(std::move(durable));
            }
        }
    };

    const auto seed_probe_phase =
        savor::runtime::seedprobe::SeedProbeFullPhaseDefinitionV2();
    if (!seed_probe_phase || !seed_probe_phase->identity()) {
        if (error_out) {
            *error_out =
                "production SeedProbe Full Phase definition is unavailable";
        }
        return false;
    }
    const auto expected_seed_probe_module =
        seed_probe_phase->runtime_contract().module;
    std::string iso_sha256;
    try {
        iso_sha256 =
            hash::sha256_of_file(options.iso_path.string());
    } catch (const std::exception& exception) {
        if (error_out) {
            *error_out =
                "failed hashing SeedProbe E2E ISO: "
                + std::string(exception.what());
        }
        return false;
    }
    if (iso_sha256.size() != 64) {
        if (error_out) {
            *error_out =
                "SeedProbe E2E ISO hash is not a complete SHA-256";
        }
        return false;
    }
    savor::runtime::ArtifactCompatibilityToken
        state_compatibility{
            .game_id = std::string(
                savor::runtime::program::capabilities::
                    kSupportedGameId),
            .iso_sha256 = std::move(iso_sha256),
            .emulator_build = "dolphin-2506a",
            .runtime_revision = "worker-runtime-slice4",
        };

    savor::runner::parallel::savordb::WorkerCoordinatorConfig
        worker_config{
            .desired_workers =
                static_cast<std::size_t>(options.worker_count),
            .controller_sleep_ms =
                static_cast<std::uint32_t>(
                    std::max<std::int64_t>(1, options.poll_ms)),
            .worker_start_timeout_ms =
                kWorkerStartupOperationTimeoutMs,
            .worker_exe_path = worker_exe.string(),
            .iso_path = options.iso_path.string(),
            .dolphin_base_dir = options.dolphin_base_dir.string(),
            .worker_dir_root =
                options.worker_dir_root
                    .value_or(
                        std::filesystem::temp_directory_path()
                        / "savor-e2e-workers")
                    .string(),
            .worker_binary_runtime_root =
                (scenario_workspace_root / "worker-runtime").string(),
            .visual_workers = options.visual_worker,
            .auto_resume_visual_workers = false,
            .runtime_artifact_root =
                (scenario_workspace_root / "runtime-artifacts").string(),
            .enabled_program_kinds =
                program_kind_registry.RegisteredProgramKinds(),
        };

    SplitCoordinatorRuntime coordinators;
    ArmInitialWorkerPoolBarrier(
        options.wait_for_workers_ready,
        [&](bool paused) {
            coordinators.SetExecutionPaused(paused);
        },
        [&](const std::string& line) {
            enqueue_event_line(line);
        });
    if (!coordinators.Start(
            execution_db,
            db_service->AuthoringDb(),
            &program_kind_registry,
            std::move(worker_config),
            scenario_workspace_root / "object_store",
            std::chrono::milliseconds(
                std::max<std::int64_t>(1, options.poll_ms)),
            std::move(state_compatibility),
            [&](const std::string& line) {
                enqueue_event_line(line);
            },
            &err)) {
        if (options.wait_for_workers_ready) {
            std::string terminal_error;
            if (!TerminalFailInitialWorkerPoolWorkflows(
                execution_db,
                    workflow_instance_id,
                    coordinators.SnapshotFleetStartup(),
                    &terminal_error)
                && !terminal_error.empty()) {
                err += "; workflow terminal failure: "
                    + terminal_error;
            }
        }
        if (error_out) {
            *error_out =
                "split SeedProbe coordinator startup failed: " + err;
        }
        return false;
    }

    const auto startup_barrier = WaitForInitialWorkerPool(
        options.wait_for_workers_ready,
        std::chrono::milliseconds(
            std::max<std::int64_t>(1, options.poll_ms)),
        [&]() {
            return coordinators.SnapshotFleetStartup();
        },
        [&](bool paused) {
            coordinators.SetExecutionPaused(paused);
        },
        [&](const std::string& line) {
            enqueue_event_line(line);
        });
    if (!startup_barrier.satisfied) {
        std::string stop_error;
        (void)coordinators.Stop(&stop_error);
        std::string terminal_error;
        (void)TerminalFailInitialWorkerPoolWorkflows(
            execution_db,
            workflow_instance_id,
            startup_barrier.snapshot,
            &terminal_error);
        if (error_out) {
            *error_out = startup_barrier.diagnostic;
            if (!stop_error.empty()) {
                *error_out += "; shutdown: " + stop_error;
            }
            if (!terminal_error.empty()) {
                *error_out +=
                    "; workflow terminal failure: "
                    + terminal_error;
            }
        }
        return false;
    }

    for (const auto& worker : coordinators.SnapshotReadyWorkers()) {
        std::ostringstream ready;
        ready
            << "[seedprobe-ready-worker]"
            << " worker_id=" << worker.worker_id
            << " accepting_workset="
            << (worker.accepting_workset ? 1 : 0)
            << " capabilities=" << worker.capabilities
            << " available_item_credits="
            << worker.available_item_credits
            << " max_items="
            << worker.runtime_manifest.limits.maximum_items_per_workset
            << " max_bytes="
            << worker.runtime_manifest.limits
                   .maximum_encoded_workset_bytes
            << " runtime_profile="
            << worker.runtime_manifest.runtime_profile_sha256
            << " modules=" << worker.runtime_manifest.modules.size();
        for (const auto& module : worker.runtime_manifest.modules) {
            ready
                << " module=" << module.module.canonical_id
                << "@" << module.module.revision
                << "#" << module.module.canonical_hash
                << "[";
            for (std::size_t i = 0; i < module.entrypoints.size(); ++i) {
                if (i > 0) {
                    ready << ",";
                }
                ready << module.entrypoints[i];
            }
            ready << "]";
        }
        enqueue_event_line(ready.str());
    }

    const bool interactive_stdout = IsSeedProbeInteractiveStdout();
    MultiLineProgressRenderer progress_renderer;
    const auto interactive_refresh_cadence = std::chrono::milliseconds(100);
    std::size_t poll_count = 0;
    std::size_t ticks_since_snapshot = 0;
    bool reached_completed = false;
    bool saw_terminal_failure = false;
    std::string coordinator_failure;
    std::unordered_set<std::int64_t> emitted_failed_step_ids;
    std::unordered_set<std::int64_t> emitted_materialized_step_ids;
    std::optional<FleetStartupSnapshot> last_fleet_startup_snapshot;
    bool progressive_startup_proven = false;
    std::uint64_t last_execution_warning_sequence = 0;
    std::vector<std::string> latest_lines;
    while (true) {
        ++poll_count;
        ++ticks_since_snapshot;

        const auto telemetry = coordinators.SnapshotTelemetry();
        const auto worker_snapshot = coordinators.SnapshotWorkers();
        const auto fleet_startup =
            coordinators.SnapshotFleetStartup();
        for (const auto& warning :
             coordinators.SnapshotExecutionWarnings()) {
            if (warning.sequence <= last_execution_warning_sequence) {
                continue;
            }
            last_execution_warning_sequence = warning.sequence;
            std::ostringstream line;
            line
                << "[seedprobe-job-execution-warning] sequence="
                << warning.sequence
                << " worker_id=" << warning.worker_id
                << " job_id=" << warning.job_id
                << " observed_mono_ns=" << warning.observed_mono_ns
                << " message=" << warning.message;
            if (!warning.detail.empty()) {
                line << " detail=" << warning.detail;
            }
            enqueue_event_line(line.str());
        }
        if (!last_fleet_startup_snapshot.has_value()
            || fleet_startup != *last_fleet_startup_snapshot) {
            enqueue_event_line(
                "[seedprobe-fleet-startup] "
                + FormatFleetStartupSnapshot(fleet_startup));
            last_fleet_startup_snapshot = fleet_startup;
        }
        if (!options.wait_for_workers_ready
            && !progressive_startup_proven
            && telemetry.execution.worksets_claimed > 0
            && !fleet_startup.full_pool_ready()) {
            progressive_startup_proven = true;
            enqueue_event_line(
                "[seedprobe-progressive-startup-proven] "
                "worksets_claimed="
                + std::to_string(
                    telemetry.execution.worksets_claimed)
                + " "
                + FormatFleetStartupSnapshot(fleet_startup));
        }
        const auto graph = execution_db->WorkflowQueryService()->GetWorkflowGraph(workflow_instance_id);
        latest_lines = BuildProgressLines(execution_db, telemetry, worker_snapshot, graph);
        std::vector<DurableLine> event_lines = drain_event_lines();
        if (graph.has_value()) {
            auto materialized_step_lines = BuildNewMaterializedStepEventLines(
                execution_db,
                *graph,
                &emitted_materialized_step_ids);
            append_event_lines(&event_lines, std::move(materialized_step_lines));
            auto failed_step_lines = BuildNewFailedStepEventLines(*graph, &emitted_failed_step_ids);
            append_event_lines(&event_lines, std::move(failed_step_lines));
        }
        if (interactive_stdout) {
            progress_renderer.SetLines(latest_lines);
            std::vector<std::string> display_event_lines;
            display_event_lines.reserve(event_lines.size());
            for (const auto& line : event_lines) {
                display_event_lines.push_back(line.text);
            }
            progress_renderer.WriteEventLines(std::cout, display_event_lines);
            progress_renderer.RenderIfDue(std::cout, std::chrono::steady_clock::now(), interactive_refresh_cadence);
        } else {
            for (const auto& line : event_lines) {
                std::cout << line.text << '\n';
            }
        }
        if (!interactive_stdout && (ticks_since_snapshot >= 10 || poll_count == 1)) {
            ticks_since_snapshot = 0;
            std::ostringstream snapshot;
            snapshot << "[seedprobe] ";
            for (std::size_t i = 0; i < latest_lines.size(); ++i) {
                if (i > 0) {
                    snapshot << " | ";
                }
                snapshot << latest_lines[i];
            }
            durable_log.AppendLine(snapshot.str());
            std::cout << snapshot.str() << '\n';
        }

        if (telemetry.execution.invariant_paused) {
            saw_terminal_failure = true;
            coordinator_failure =
                telemetry.execution.last_error.empty()
                ? "JobExecutionCoordinator entered an invariant pause"
                : telemetry.execution.last_error;
            enqueue_event_line(
                "[seedprobe-infrastructure-failed] "
                "reason=job-execution-invariant-pause error="
                + coordinator_failure);
            break;
        }
        if (graph.has_value()) {
            using savor::db::execution::workflow::WorkflowInstanceState;
            if (graph->instance.state == WorkflowInstanceState::Completed) {
                reached_completed = true;
                break;
            }
            if (graph->instance.state == WorkflowInstanceState::Failed
                || graph->instance.state == WorkflowInstanceState::Canceled) {
                saw_terminal_failure = true;
                break;
            }
            if (AreSeedProbeWorkflowStepsTerminal(*graph)) {
                if (HasFailedSeedProbeWorkflowStep(*graph)) {
                    saw_terminal_failure = true;
                    break;
                }
            }
        }
        const auto worker_start =
            coordinators.SnapshotWorkerStartResult();
        if (worker_start.status
            == savor::runner::parallel::savordb::
                WorkerCoordinatorStartStatus::StartupExhausted) {
            saw_terminal_failure = true;
            coordinator_failure =
                worker_start.diagnostic.empty()
                ? "all worker startup attempts were exhausted"
                : worker_start.diagnostic;
            enqueue_event_line(
                "[seedprobe-worker-startup-failed] "
                + coordinator_failure);
            break;
        }
        if (telemetry.workflow.ready_scan_count == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(options.poll_ms));
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(options.poll_ms));
    }

    const auto final_graph =
        execution_db->WorkflowQueryService()->GetWorkflowGraph(
            workflow_instance_id);
    const auto final_telemetry = coordinators.SnapshotTelemetry();
    const auto final_worker_snapshot =
        coordinators.SnapshotWorkers();
    const auto final_ready_workers =
        coordinators.SnapshotReadyWorkers();
    {
        std::ostringstream split;
        split
            << "[seedprobe-split-coordinators]"
            << " ready_workers=" << final_ready_workers.size()
            << " worksets_claimed="
            << final_telemetry.execution.worksets_claimed
            << " worksets_reconstructed="
            << final_telemetry.execution.worksets_reconstructed
            << " worksets_submitted="
            << final_telemetry.execution.worksets_submitted
            << " jobs_started="
            << final_telemetry.execution.jobs_started
            << " terminals_staged="
            << final_telemetry.execution.worker_terminals_staged
            << " terminals_discarded_after_authority_release="
            << final_telemetry.execution
                   .worker_terminals_discarded_after_authority_release
            << " terminal_acks="
            << final_telemetry.execution.worker_terminal_acks
            << " terminal_stage_failures="
            << final_telemetry.execution
                   .worker_terminal_staging_failures
            << " terminal_retries="
            << final_telemetry.execution
                   .worker_terminal_retry_attempts
            << " worker_event_batches="
            << final_telemetry.execution.worker_event_batches
            << " worker_event_items="
            << final_telemetry.execution.worker_event_batch_items
            << " worker_event_batch_max="
            << final_telemetry.execution.worker_event_batch_max_size
            << " blob_ready="
            << (final_telemetry.execution.blob_store_ready ? 1 : 0)
            << " blob_readiness_failures="
            << final_telemetry.execution.blob_readiness_failures
            << " pending_terminals="
            << final_telemetry.execution.pending_worker_terminals
            << " draining_worksets="
            << final_telemetry.execution.draining_worksets
            << " claims_storage_paused="
            << (final_telemetry.execution
                        .claims_paused_for_terminal_staging
                    ? 1
                    : 0)
            << " startup_recovered_dispatches="
            << final_telemetry.execution.startup_recovered_dispatches
            << " startup_requeued_jobs="
            << final_telemetry.execution.startup_requeued_jobs
            << " startup_recovery_attempts_granted="
            << final_telemetry.execution.startup_recovery_attempts_granted
            << " active_residence_probes="
            << final_telemetry.execution.active_residence_probes
            << " active_residence_matches="
            << final_telemetry.execution.active_residence_matches
            << " active_residence_failures="
            << final_telemetry.execution.active_residence_failures
            << " active_lease_renewal_batches="
            << final_telemetry.execution.active_lease_renewal_batches
            << " active_lease_renewal_retries="
            << final_telemetry.execution.active_lease_renewal_retries
            << " draining_transitions="
            << final_telemetry.execution.draining_transitions
            << " worker_submissions="
            << final_telemetry.worker.submit_accepted
            << " liveness_attempts="
            << final_telemetry.worker.liveness_probe_attempts
            << " liveness_failures="
            << final_telemetry.worker.liveness_probe_failures
            << " liveness_quarantines="
            << final_telemetry.worker.liveness_quarantines
            << " result_claims="
            << final_telemetry.results.claims
            << " result_claim_batches="
            << final_telemetry.results.claim_batches
            << " result_finalizations="
            << final_telemetry.results.finalized
            << " result_finalization_batches="
            << final_telemetry.results.finalization_batches
            << " cancellation_holds_registered="
            << final_telemetry.execution
                   .cancellation_precommit_holds_registered
            << " cancellation_holds_promoted="
            << final_telemetry.execution
                   .cancellation_precommit_holds_promoted
            << " sidecar_suppressed_jobs="
            << final_telemetry.execution
                   .waiting_jobs_suppressed_by_sidecar
            << " fully_canceled_worksets_avoided="
            << final_telemetry.execution
                   .fully_canceled_worksets_avoided
            << " cancellation_mutation_batches="
            << final_telemetry.execution.cancellation_mutation_batches
            << " workflow_materializations="
            << final_telemetry.workflow.materialization_count
            << " workflow_advancements="
            << final_telemetry.workflow
                   .targeted_terminal_advancement_count;
        enqueue_event_line(split.str());
    }
    if (final_graph.has_value()) {
        latest_lines = BuildProgressLines(
            execution_db,
            final_telemetry,
            final_worker_snapshot,
            final_graph);
    }

    std::string shutdown_error;
    const bool clean_shutdown = coordinators.Stop(&shutdown_error);
    const auto final_event_lines = drain_event_lines();
    if (interactive_stdout) {
        std::vector<std::string> display_event_lines;
        display_event_lines.reserve(final_event_lines.size());
        for (const auto& line : final_event_lines) {
            display_event_lines.push_back(line.text);
        }
        progress_renderer.WriteEventLines(std::cout, display_event_lines);
    } else {
        for (const auto& line : final_event_lines) {
            std::cout << line.text << '\n';
        }
    }

    if (poll_count == 0) {
        if (error_out) {
            *error_out = "SeedProbe coordinator polling loop did not execute";
        }
        return false;
    }

    if (latest_lines.empty()) {
        latest_lines.push_back("workflow=unavailable");
    }
    if (final_graph.has_value()) {
        auto final_failed_step_lines = BuildNewFailedStepEventLines(*final_graph, &emitted_failed_step_ids);
        std::vector<DurableLine> final_failed_durable_lines;
        append_event_lines(&final_failed_durable_lines, std::move(final_failed_step_lines));
        if (interactive_stdout) {
            progress_renderer.SetLines(latest_lines);
            std::vector<std::string> display_event_lines;
            display_event_lines.reserve(final_failed_durable_lines.size());
            for (const auto& line : final_failed_durable_lines) {
                display_event_lines.push_back(line.text);
            }
            progress_renderer.WriteEventLines(std::cout, display_event_lines);
        } else {
            for (const auto& line : final_failed_durable_lines) {
                std::cout << line.text << '\n';
            }
        }
    }
    if (interactive_stdout) {
        progress_renderer.SetLines(latest_lines);
        progress_renderer.Render(std::cout);
    }

    std::string final_status = "success";
    if (saw_terminal_failure) {
        final_status = "failure";
    } else if (!final_graph.has_value()
        || final_graph->instance.state != savor::db::execution::workflow::WorkflowInstanceState::Completed) {
        final_status = "incomplete";
    }

    std::cout << "[seedprobe-final] status=" << final_status << '\n';
    durable_log.AppendLine(
        "[seedprobe-final] status=" + final_status);
    for (const auto& line : latest_lines) {
        std::cout << "  " << line << '\n';
    }

    const auto emit_infrastructure_health =
        [&](SeedProbeInfrastructureHealth health,
            const std::vector<std::string>& issues) {
            const std::string summary =
                "[seedprobe-infrastructure-health] health="
                + std::string(InfrastructureHealthName(health))
                + " issues=" + std::to_string(issues.size());
            durable_log.AppendLine(summary);
            std::cout << summary << '\n';
            for (const auto& issue : issues) {
                const std::string line =
                    "  [seedprobe-infrastructure-issue] " + issue;
                durable_log.AppendLine(line);
                std::cout << line << '\n';
            }
        };

    if (!clean_shutdown) {
        const std::string issue =
            "split coordinator shutdown failed: " + shutdown_error;
        emit_infrastructure_health(
            SeedProbeInfrastructureHealth::Failed,
            {issue});
        if (error_out) {
            *error_out = issue;
        }
        return false;
    }
    if (!final_graph.has_value()
        || final_graph->instance.state != savor::db::execution::workflow::WorkflowInstanceState::Completed) {
        const std::string issue = !coordinator_failure.empty()
            ? "coordinator infrastructure failed: " + coordinator_failure
            : saw_terminal_failure
                ? "workflow did not complete successfully"
                : "workflow stopped before reaching COMPLETED state";
        emit_infrastructure_health(
            SeedProbeInfrastructureHealth::Failed,
            {issue});
        if (error_out) {
            *error_out = issue;
        }
        return false;
    }

    SeedProbeInfrastructureHealth infrastructure_health =
        SeedProbeInfrastructureHealth::Failed;
    std::vector<std::string> infrastructure_issues;
    std::string coordinator_validation_error;
    const bool coordinator_valid = ValidateSplitCoordinatorExecution(
            execution_db,
            final_graph,
            final_telemetry,
            final_ready_workers,
            expected_seed_probe_module,
            &infrastructure_health,
            &infrastructure_issues,
            &coordinator_validation_error);
    std::string evidence_validation_error;
    const bool evidence_valid = coordinator_valid
        && ValidateSeedProbeAcceptedEvidence(
            execution_db,
            db_service->AnalysisDb(),
            final_graph,
            &evidence_validation_error);
    if (!coordinator_valid && !coordinator_validation_error.empty()) {
        infrastructure_issues.push_back(
            coordinator_validation_error);
    }
    if (!evidence_valid) {
        infrastructure_health = SeedProbeInfrastructureHealth::Failed;
        if (!evidence_validation_error.empty()) {
            infrastructure_issues.push_back(
                evidence_validation_error);
        }
    }
    emit_infrastructure_health(
        infrastructure_health,
        infrastructure_issues);
    if (!coordinator_valid || !evidence_valid) {
        if (error_out != nullptr) {
            *error_out = !coordinator_validation_error.empty()
                ? coordinator_validation_error
                : evidence_validation_error;
        }
        return false;
    }

    return true;
}

bool RunSeedProbeRealWorkerSmoke(
    const CliOptions& options,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out) {
    return RunSeedProbeRealWorkerSmokeImpl(options, argv0, db_service, error_out);
}

bool RunSeedProbeWorkflowGraphRealWorkerSmoke(
    const CliOptions& options,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out) {
    return RunSeedProbeRealWorkerSmokeImpl(options, argv0, db_service, error_out);
}

} // namespace savor::e2e
