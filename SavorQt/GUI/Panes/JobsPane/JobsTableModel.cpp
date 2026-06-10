#include "JobsTableModel.h"

#include <QtCore/QVariant>

#include <algorithm>

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
    case ProgramKindColumn: return QStringLiteral("program_kind");
    case StateColumn: return QStringLiteral("state");
    case AttemptsColumn: return QStringLiteral("attempts");
    case QueuedAtColumn: return QStringLiteral("queued_at");
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
        if (index.column() == JobIdColumn || index.column() == JobSetIdColumn || index.column() == AttemptsColumn) {
            return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        }
    }

    if (role != Qt::DisplayRole) {
        return {};
    }

    switch (index.column()) {
    case JobIdColumn: return row->jobId;
    case JobSetIdColumn: return row->jobSetId;
    case ProgramKindColumn: return row->programKind;
    case StateColumn: return row->state;
    case AttemptsColumn: return row->attempts;
    case QueuedAtColumn: return row->queuedAt;
    default: return {};
    }
}

void JobsTableModel::setRows(const std::vector<Row>& rows)
{
    int targetRow = 0;
    while (targetRow < static_cast<int>(rows.size())) {
        const qint64 targetJobId = rows[static_cast<size_t>(targetRow)].jobId;

        if (targetRow < static_cast<int>(rows_.size()) && rows_[static_cast<size_t>(targetRow)].jobId == targetJobId) {
            ++targetRow;
            continue;
        }

        auto existingIt = std::find_if(rows_.begin() + std::min(targetRow, static_cast<int>(rows_.size())), rows_.end(), [targetJobId](const Row& row) {
            return row.jobId == targetJobId;
        });

        if (existingIt != rows_.end()) {
            const int sourceRow = static_cast<int>(std::distance(rows_.begin(), existingIt));
            const int destinationChild = targetRow;
            beginMoveRows(QModelIndex(), sourceRow, sourceRow, QModelIndex(), destinationChild);
            Row moved = std::move(rows_[static_cast<size_t>(sourceRow)]);
            rows_.erase(rows_.begin() + sourceRow);
            rows_.insert(rows_.begin() + destinationChild, std::move(moved));
            endMoveRows();
            ++targetRow;
            continue;
        }

        beginInsertRows(QModelIndex(), targetRow, targetRow);
        rows_.insert(rows_.begin() + targetRow, rows[static_cast<size_t>(targetRow)]);
        endInsertRows();
        ++targetRow;
    }

    while (static_cast<int>(rows_.size()) > static_cast<int>(rows.size())) {
        const int staleRow = static_cast<int>(rows_.size()) - 1;
        beginRemoveRows(QModelIndex(), staleRow, staleRow);
        rows_.pop_back();
        endRemoveRows();
    }

    int changeStart = -1;
    for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
        const Row& incoming = rows[static_cast<size_t>(row)];
        if (rowsAffectDisplay(rows_[static_cast<size_t>(row)], incoming)) {
            rows_[static_cast<size_t>(row)] = incoming;
            if (changeStart < 0) {
                changeStart = row;
            }
            continue;
        }

        if (changeStart >= 0) {
            emit dataChanged(index(changeStart, 0), index(row - 1, ColumnCount - 1));
            changeStart = -1;
        }
    }

    if (changeStart >= 0) {
        emit dataChanged(index(changeStart, 0), index(static_cast<int>(rows.size()) - 1, ColumnCount - 1));
    }
}

const JobsTableModel::Row* JobsTableModel::rowAt(int row) const
{
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return nullptr;
    }
    return &rows_[row];
}

bool JobsTableModel::rowsAffectDisplay(const Row& lhs, const Row& rhs)
{
    return lhs.jobId != rhs.jobId
        || lhs.jobSetId != rhs.jobSetId
        || lhs.programKind != rhs.programKind
        || lhs.state != rhs.state
        || lhs.attempts != rhs.attempts
        || lhs.queuedAt != rhs.queuedAt;
}
