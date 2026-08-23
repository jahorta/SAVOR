#include "WorkerTableModel.h"

#include "DB/ProgramKindNameResolver.h"
#include "GUI/Refresh/RowUpdate.h"

#include <QtCore/QString>
#include <QtCore/QVariant>

#include <algorithm>
#include <string>

namespace {
const char* workerStateLabel(WorkerStateKind state)
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

const char* workerStartupPhaseLabel(WorkerStartupPhase phase)
{
    switch (phase) {
    case WorkerStartupPhase::PendingFilesystem: return "Pending filesystem";
    case WorkerStartupPhase::PreparingFilesystem: return "Preparing filesystem";
    case WorkerStartupPhase::WaitingToOpen: return "Waiting to open";
    case WorkerStartupPhase::Launching: return "Launching";
    case WorkerStartupPhase::OpeningSession: return "Opening session";
    case WorkerStartupPhase::Failed: return "Startup failed";
    case WorkerStartupPhase::None:
    default: return "";
    }
}

QString formatOptionalInt64(const std::optional<int64_t>& value, const QString& fallback = QStringLiteral("--"))
{
    return value.has_value() ? QString::number(*value) : fallback;
}

QString workerStatusText(const WorkerSnapshot& row)
{
    if (row.startup_phase != WorkerStartupPhase::None
        && row.startup_phase != WorkerStartupPhase::Failed) {
        return QString::fromUtf8(
            workerStartupPhaseLabel(row.startup_phase));
    }
    const bool hasProgress = !row.last_progress.empty();
    const bool hasError = !row.last_error.empty();
    if (hasProgress && (!hasError || row.last_progress_mono_ns >= row.last_error_mono_ns)) {
        return QString::fromStdString(row.last_progress);
    }
    if (hasError) {
        return QString::fromStdString(row.last_error);
    }
    return QStringLiteral("--");
}
}

WorkerTableModel::WorkerTableModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

int WorkerTableModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }

    return static_cast<int>(snapshots_.size());
}

int WorkerTableModel::columnCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }

    return Count;
}

QVariant WorkerTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(snapshots_.size())) {
        return {};
    }

    if (role != Qt::DisplayRole) {
        return {};
    }

    const WorkerSnapshot& row = snapshots_[static_cast<size_t>(index.row())];
    switch (index.column()) {
    case Id: return QString::number(row.worker_id);
    case Pid: return QString::number(row.pid);
    case State: return QString::fromUtf8(workerStateLabel(row.state));
    case Job: return formatOptionalInt64(row.job_id, QStringLiteral("0"));
    case Kind: return QString::fromStdString(savorqt::db::ResolveProgramKindName(row.program_kind));
    case LastHeartbeat: return QString::number(row.last_heartbeat_mono_ns);
    case Status: return workerStatusText(row);
    default: return {};
    }
}

QVariant WorkerTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole) {
        return {};
    }

    if (orientation == Qt::Vertical) {
        return {};
    }

    switch (section) {
    case Id: return QStringLiteral("ID");
    case Pid: return QStringLiteral("PID");
    case State: return QStringLiteral("State");
    case Job: return QStringLiteral("Job");
    case Kind: return QStringLiteral("Kind");
    case LastHeartbeat: return QStringLiteral("Last HB");
    case Status: return QStringLiteral("Status");
    default: return {};
    }
}

void WorkerTableModel::setSnapshots(std::vector<WorkerSnapshot> snapshots)
{
    const auto equalRows = [](const WorkerSnapshot& lhs, const WorkerSnapshot& rhs) {
        return !WorkerTableModel::rowsAffectDisplay(lhs, rhs);
    };
    if (savorqt::gui::RowsEqual(snapshots_, snapshots, equalRows)) {
        return;
    }

    int targetRow = 0;
    while (targetRow < static_cast<int>(snapshots.size())) {
        const auto targetWorkerId = snapshots[static_cast<size_t>(targetRow)].worker_id;

        if (targetRow < static_cast<int>(snapshots_.size())
            && snapshots_[static_cast<size_t>(targetRow)].worker_id == targetWorkerId) {
            ++targetRow;
            continue;
        }

        auto existingIt = std::find_if(
            snapshots_.begin() + std::min(targetRow, static_cast<int>(snapshots_.size())),
            snapshots_.end(),
            [targetWorkerId](const WorkerSnapshot& row) {
                return row.worker_id == targetWorkerId;
            });

        if (existingIt != snapshots_.end()) {
            const int sourceRow = static_cast<int>(std::distance(snapshots_.begin(), existingIt));
            beginMoveRows(QModelIndex(), sourceRow, sourceRow, QModelIndex(), targetRow);
            WorkerSnapshot moved = std::move(snapshots_[static_cast<size_t>(sourceRow)]);
            snapshots_.erase(snapshots_.begin() + sourceRow);
            snapshots_.insert(snapshots_.begin() + targetRow, std::move(moved));
            endMoveRows();
            ++targetRow;
            continue;
        }

        beginInsertRows(QModelIndex(), targetRow, targetRow);
        snapshots_.insert(snapshots_.begin() + targetRow, snapshots[static_cast<size_t>(targetRow)]);
        endInsertRows();
        ++targetRow;
    }

    while (static_cast<int>(snapshots_.size()) > static_cast<int>(snapshots.size())) {
        const int staleRow = static_cast<int>(snapshots_.size()) - 1;
        beginRemoveRows(QModelIndex(), staleRow, staleRow);
        snapshots_.pop_back();
        endRemoveRows();
    }

    int changeStart = -1;
    for (int row = 0; row < static_cast<int>(snapshots.size()); ++row) {
        const WorkerSnapshot& incoming = snapshots[static_cast<size_t>(row)];
        if (rowsAffectDisplay(snapshots_[static_cast<size_t>(row)], incoming)) {
            snapshots_[static_cast<size_t>(row)] = incoming;
            if (changeStart < 0) {
                changeStart = row;
            }
            continue;
        }

        if (changeStart >= 0) {
            emit dataChanged(index(changeStart, 0), index(row - 1, Count - 1));
            changeStart = -1;
        }
    }

    if (changeStart >= 0) {
        emit dataChanged(index(changeStart, 0), index(static_cast<int>(snapshots.size()) - 1, Count - 1));
    }
}

std::optional<WorkerSnapshot> WorkerTableModel::snapshotAt(int row) const
{
    if (row < 0 || row >= static_cast<int>(snapshots_.size()))
        return std::nullopt;
    return snapshots_[static_cast<std::size_t>(row)];
}

bool WorkerTableModel::rowsAffectDisplay(const WorkerSnapshot& lhs, const WorkerSnapshot& rhs)
{
    return lhs.worker_id != rhs.worker_id
        || lhs.pid != rhs.pid
        || lhs.process_generation != rhs.process_generation
        || lhs.log_path != rhs.log_path
        || lhs.startup_phase != rhs.startup_phase
        || lhs.state != rhs.state
        || lhs.job_id != rhs.job_id
        || lhs.program_kind != rhs.program_kind
        || lhs.last_heartbeat_mono_ns != rhs.last_heartbeat_mono_ns
        || lhs.last_progress != rhs.last_progress
        || lhs.last_progress_mono_ns != rhs.last_progress_mono_ns
        || lhs.last_error != rhs.last_error
        || lhs.last_error_mono_ns != rhs.last_error_mono_ns;
}
