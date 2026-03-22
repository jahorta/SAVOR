#include "WorkerTableModel.h"

#include "CoordinatorUiCommon.h"

#include <QtCore/QString>
#include <QtCore/QVariant>

namespace {
QString formatOptionalInt64(const std::optional<int64_t>& value, const QString& fallback = QStringLiteral("--"))
{
    return value.has_value() ? QString::number(*value) : fallback;
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
    case State: return QString::fromUtf8(soasimqt::ui::WorkerStateLabel(row.state));
    case Job: return formatOptionalInt64(row.job_id, QStringLiteral("0"));
    case Kind: return QString::fromStdString(soasimqt::ui::WorkerProgramKindLabel(row.program_kind));
    case Lease: return formatOptionalInt64(row.lease_expires_at);
    case Attempts: return QStringLiteral("%1 / %2").arg(row.attempts).arg(row.max_attempts);
    case LastHeartbeat: return QString::number(row.last_heartbeat_mono_ns);
    case DbOk: return QString::number(row.last_successful_db_call_mono_ns);
    case Error: return QString::fromStdString(row.last_error);
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
    case Lease: return QStringLiteral("Lease");
    case Attempts: return QStringLiteral("Attempts");
    case LastHeartbeat: return QStringLiteral("Last HB");
    case DbOk: return QStringLiteral("DB OK");
    case Error: return QStringLiteral("Err");
    default: return {};
    }
}

void WorkerTableModel::setSnapshots(std::vector<WorkerSnapshot> snapshots)
{
    beginResetModel();
    snapshots_ = std::move(snapshots);
    endResetModel();
}
