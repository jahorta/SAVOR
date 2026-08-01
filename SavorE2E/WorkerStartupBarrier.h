#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

#include "Execution/FleetStartupSnapshot.h"

namespace savor::db {
struct IExecutionDb;
}

namespace savor::e2e {

using FleetStartupSnapshot =
    savor::runner::parallel::savordb::FleetStartupSnapshot;
using FleetStartupSnapshotProvider =
    std::function<FleetStartupSnapshot()>;
using ClaimingPauseControl = std::function<void(bool)>;
using StartupBarrierEventSink =
    std::function<void(const std::string&)>;

struct InitialWorkerPoolBarrierResult {
    bool satisfied = false;
    FleetStartupSnapshot snapshot;
    std::string diagnostic;
};

void ArmInitialWorkerPoolBarrier(
    bool enabled,
    const ClaimingPauseControl& set_claiming_paused,
    const StartupBarrierEventSink& event_sink = {});

InitialWorkerPoolBarrierResult WaitForInitialWorkerPool(
    bool enabled,
    std::chrono::milliseconds poll_interval,
    const FleetStartupSnapshotProvider& snapshot_provider,
    const ClaimingPauseControl& set_claiming_paused,
    const StartupBarrierEventSink& event_sink = {});

std::string FormatFleetStartupSnapshot(
    const FleetStartupSnapshot& snapshot);

bool TerminalFailInitialWorkerPoolWorkflows(
    savor::db::IExecutionDb* execution_db,
    std::int64_t workflow_instance_id,
    const FleetStartupSnapshot& snapshot,
    std::string* error_out = nullptr);

} // namespace savor::e2e
