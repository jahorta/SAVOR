#include "ExplorerRunsJobsTableModel.h"

#include <QtCore/QVariant>

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
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}

const ExplorerRunsJobRow* ExplorerRunsJobsTableModel::rowAt(int row) const
{
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return nullptr;
    }
    return &rows_[static_cast<size_t>(row)];
}
