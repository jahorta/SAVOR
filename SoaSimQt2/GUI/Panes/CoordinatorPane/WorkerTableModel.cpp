#include "WorkerTableModel.h"

#include "DB/ProgramKindNameResolver.h"

#include <QtCore/QString>
#include <QtCore/QVariant>

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

QString formatOptionalInt64(const std::optional<int64_t>& value, const QString& fallback = QStringLiteral("--"))
{
    return value.has_value() ? QString::number(*value) : fallback;
}

QString workerStatusText(const WorkerSnapshot& row)
{
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
    case Kind: return QString::fromStdString(soasimqt2::db::ResolveProgramKindName(row.program_kind));
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
    beginResetModel();
    snapshots_ = std::move(snapshots);
    endResetModel();
}
