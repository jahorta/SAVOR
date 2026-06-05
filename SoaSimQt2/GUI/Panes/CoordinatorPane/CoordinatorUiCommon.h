#pragma once

#include <optional>
#include <string>

#include <QtCore/QString>

#include "Runner/IPC/Wire.h"
#include "Runner/Parallel/WorkerTelemetry.h"

namespace soasimqt::ui {

inline const char* WorkerStateLabel(WorkerStateKind state)
{
    switch (state) {
    case WorkerStateKind::Spawning: return "Spawning";
    case WorkerStateKind::Idle: return "Idle";
    case WorkerStateKind::Leasing: return "Leasing";
    case WorkerStateKind::Running: return "Running";
    case WorkerStateKind::Renewing: return "Renewing";
    case WorkerStateKind::Paused: return "Paused";
    case WorkerStateKind::Draining: return "Draining";
    case WorkerStateKind::Exiting: return "Exiting";
    case WorkerStateKind::Stopping: return "Stopping";
    case WorkerStateKind::Dead: return "Dead";
    default: return "Unknown";
    }
}

inline std::string WorkerProgramKindLabel(const std::optional<int>& kind)
{
    if (!kind.has_value()) return "(none)";

    switch (*kind) {
    case simcore::PK_None: return "None";
    case simcore::PK_SeedProbe: return "SeedProbe";
    case simcore::PK_TasMovie: return "TasMovie";
    case simcore::PK_TasInputStreamDetector: return "TasInputStreamDetector";
    case simcore::PK_BattleTurnRunner: return "BattleTurnRunner";
    case simcore::PK_BattleContextProbe: return "BattleContextProbe";
    case simcore::PK_BattleSingleTurnRunner: return "BattleSingleTurnRunner";
    default: return "kind " + std::to_string(*kind);
    }
}

inline QString VisualReplayRuntimeStateText(const void*)
{
    return QStringLiteral("Disabled during SimCoreDB UIRead cutover");
}

} // namespace soasimqt::ui
