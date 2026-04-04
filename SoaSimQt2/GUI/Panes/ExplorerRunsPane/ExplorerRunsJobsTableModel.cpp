#include "ExplorerRunsJobsTableModel.h"

#include <QtCore/QVariant>

#include <algorithm>

ExplorerRunsJobsTableModel::ExplorerRunsJobsTableModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

int ExplorerRunsJobsTableModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int ExplorerRunsJobsTableModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : Count;
}

QVariant ExplorerRunsJobsTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) {
        return {};
    }

    const ExplorerRunsJobRow& row = rows_[static_cast<size_t>(index.row())];
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case JobId: return QString::number(row.jobId);
        case JobState: return row.state;
        case Outcome: return row.outcome;
        case Predicates: return row.predicates;
        case DeltaVi: return row.deltaVi;
        case FakeAttacks: return row.fakeAttacks;
        case RngSeed: return row.rngSeed;
        default: return {};
        }
    }

    return {};
}

QVariant ExplorerRunsJobsTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }

    switch (section) {
    case JobId: return QStringLiteral("Job ID");
    case JobState: return QStringLiteral("Job State");
    case Outcome: return QStringLiteral("Outcome");
    case Predicates: return QStringLiteral("Predicates");
    case DeltaVi: return QStringLiteral("Delta VI");
    case FakeAttacks: return QStringLiteral("Fake Attacks");
    case RngSeed: return QStringLiteral("RNG Seed");
    default: return {};
    }
}

void ExplorerRunsJobsTableModel::setRows(std::vector<ExplorerRunsJobRow> rows)
{
    int common = 0;
    while (common < static_cast<int>(rows_.size())
        && common < static_cast<int>(rows.size())
        && rows_[static_cast<size_t>(common)].jobId == rows[static_cast<size_t>(common)].jobId) {
        ++common;
    }

    if (common < static_cast<int>(rows_.size())) {
        beginRemoveRows(QModelIndex(), common, static_cast<int>(rows_.size()) - 1);
        rows_.erase(rows_.begin() + common, rows_.end());
        endRemoveRows();
    }

    if (common < static_cast<int>(rows.size())) {
        beginInsertRows(QModelIndex(), common, static_cast<int>(rows.size()) - 1);
        rows_.insert(rows_.end(), rows.begin() + common, rows.end());
        endInsertRows();
    }

    const int compareCount = std::min(static_cast<int>(rows_.size()), common);
    for (int row = 0; row < compareCount; ++row) {
        const ExplorerRunsJobRow& incoming = rows[static_cast<size_t>(row)];
        if (!(rows_[static_cast<size_t>(row)] == incoming)) {
            rows_[static_cast<size_t>(row)] = incoming;
            const QModelIndex left = index(row, 0);
            const QModelIndex right = index(row, Count - 1);
            emit dataChanged(left, right);
        }
    }
}

const ExplorerRunsJobRow* ExplorerRunsJobsTableModel::rowAt(int row) const
{
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return nullptr;
    }
    return &rows_[static_cast<size_t>(row)];
}
