#include "JobsTableModel.h"

#include <QtCore/QVariant>

JobsTableModel::JobsTableModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

int JobsTableModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int JobsTableModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant JobsTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal) {
        return QAbstractTableModel::headerData(section, orientation, role);
    }

    switch (section) {
    case JobIdColumn: return QStringLiteral("id");
    case JobSetIdColumn: return QStringLiteral("job_set_id");
    case SaveStateIdColumn: return QStringLiteral("savestate_id");
    case ProgramKindColumn: return QStringLiteral("program_kind");
    case StateColumn: return QStringLiteral("state");
    case AttemptsColumn: return QStringLiteral("attempts");
    case QueuedAtColumn: return QStringLiteral("queued_at");
    case ProgressColumn: return QStringLiteral("progress");
    default: return {};
    }
}

QVariant JobsTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) {
        return {};
    }

    const Row* row = rowAt(index.row());
    if (!row) {
        return {};
    }

    if (role == Qt::TextAlignmentRole) {
        if (index.column() == JobIdColumn || index.column() == JobSetIdColumn || index.column() == SaveStateIdColumn || index.column() == AttemptsColumn) {
            return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        }
    }

    if (role != Qt::DisplayRole) {
        return {};
    }

    switch (index.column()) {
    case JobIdColumn: return row->jobId;
    case JobSetIdColumn: return row->jobSetId;
    case SaveStateIdColumn: return row->savestateId.has_value() ? QVariant::fromValue(*row->savestateId) : QVariant(QStringLiteral("Null"));
    case ProgramKindColumn: return row->programKind;
    case StateColumn: return row->state;
    case AttemptsColumn: return row->attempts;
    case QueuedAtColumn: return row->queuedAt;
    case ProgressColumn: return row->progress;
    default: return {};
    }
}

void JobsTableModel::setRows(const std::vector<Row>& rows)
{
    beginResetModel();
    rows_ = rows;
    endResetModel();
}

const JobsTableModel::Row* JobsTableModel::rowAt(int row) const
{
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return nullptr;
    }
    return &rows_[row];
}
