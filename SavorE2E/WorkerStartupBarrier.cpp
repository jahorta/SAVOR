#include "WorkerStartupBarrier.h"

#include <algorithm>
#include <sstream>
#include <thread>

#include "Execution/IExecutionDb.h"
#include "Execution/Workflow/WorkflowOrchestration.h"

namespace savor::e2e {
namespace {

void Emit(
    const StartupBarrierEventSink& sink,
    const std::string& line) {
    if (sink) {
        sink(line);
    }
}

} // namespace

std::string FormatFleetStartupSnapshot(
    const FleetStartupSnapshot& snapshot) {
    std::ostringstream out;
    out << "desired=" << snapshot.desired
        << " ready=" << snapshot.ready
        << " starting=" << snapshot.starting
        << " retry_pending=" << snapshot.retry_pending
        << " exhausted=" << snapshot.exhausted;
    for (const auto& slot : snapshot.worker_slots) {
        out << " slot=" << slot.worker_id
            << ":attempts=" << slot.attempt_count
            << "/" << slot.maximum_attempts
            << ",state=";
        if (slot.ready) {
            out << "ready";
        } else if (slot.starting) {
            out << "starting";
        } else if (slot.exhausted) {
            out << "exhausted";
        } else {
            out << "retry_pending";
        }
        if (!slot.terminal_diagnostic.empty()) {
            out << ",diagnostic=\"" << slot.terminal_diagnostic << '"';
        }
    }
    return out.str();
}

void ArmInitialWorkerPoolBarrier(
    bool enabled,
    const ClaimingPauseControl& set_claiming_paused,
    const StartupBarrierEventSink& event_sink) {
    if (enabled && set_claiming_paused) {
        set_claiming_paused(true);
    }
    Emit(
        event_sink,
        std::string("[worker-startup-barrier-config] enabled=")
            + (enabled ? "1" : "0"));
}

InitialWorkerPoolBarrierResult WaitForInitialWorkerPool(
    bool enabled,
    std::chrono::milliseconds poll_interval,
    const FleetStartupSnapshotProvider& snapshot_provider,
    const ClaimingPauseControl& set_claiming_paused,
    const StartupBarrierEventSink& event_sink) {
    InitialWorkerPoolBarrierResult result{};
    if (!enabled) {
        result.satisfied = true;
        if (snapshot_provider) {
            result.snapshot = snapshot_provider();
        }
        return result;
    }
    if (!snapshot_provider || !set_claiming_paused) {
        result.diagnostic =
            "initial worker-pool barrier callbacks are incomplete";
        Emit(
            event_sink,
            "[worker-startup-barrier-failed] "
            + result.diagnostic);
        return result;
    }

    const auto cadence =
        std::max(poll_interval, std::chrono::milliseconds(1));
    bool have_previous = false;
    FleetStartupSnapshot previous{};
    for (;;) {
        result.snapshot = snapshot_provider();
        if (!have_previous || result.snapshot != previous) {
            Emit(
                event_sink,
                "[worker-startup-barrier-progress] "
                + FormatFleetStartupSnapshot(result.snapshot));
            previous = result.snapshot;
            have_previous = true;
        }
        if (result.snapshot.full_pool_ready()) {
            set_claiming_paused(false);
            result.satisfied = true;
            Emit(
                event_sink,
                "[worker-startup-barrier-complete] "
                + FormatFleetStartupSnapshot(result.snapshot));
            return result;
        }
        if (result.snapshot.full_pool_impossible()) {
            result.diagnostic =
                "required initial worker pool is unavailable: "
                + FormatFleetStartupSnapshot(result.snapshot);
            Emit(
                event_sink,
                "[worker-startup-barrier-failed] "
                + result.diagnostic);
            return result;
        }
        std::this_thread::sleep_for(cadence);
    }
}

bool TerminalFailInitialWorkerPoolWorkflows(
    savor::db::IExecutionDb* execution_db,
    std::int64_t workflow_instance_id,
    const FleetStartupSnapshot& snapshot,
    std::string* error_out) {
    if (error_out != nullptr) {
        error_out->clear();
    }
    if (execution_db == nullptr
        || execution_db->WorkflowCommandService() == nullptr) {
        if (error_out != nullptr) {
            *error_out =
                "execution DB workflow command service is unavailable";
        }
        return false;
    }

    const auto diagnostic =
        "initial worker pool unavailable: "
        + FormatFleetStartupSnapshot(snapshot);
    if (workflow_instance_id <= 0) {
        if (error_out != nullptr) {
            *error_out = "workflow instance id is invalid";
        }
        return false;
    }
    return execution_db->WorkflowCommandService()
        ->TerminalFailWorkflowInstance(
            {
                .workflow_instance_id = workflow_instance_id,
                .failure_code =
                    "INITIAL_WORKER_POOL_UNAVAILABLE",
                .failure_message = diagnostic,
                .requested_by =
                    "SavorE2E.initial-worker-pool-barrier",
            },
            error_out);
}

} // namespace savor::e2e
