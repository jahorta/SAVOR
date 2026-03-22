#include "ExplorerRunsGroupTableModel.h"

#include <QtCore/QVariant>

ExplorerRunsGroupTableModel::ExplorerRunsGroupTableModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

int ExplorerRunsGroupTableModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int ExplorerRunsGroupTableModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : Count;
}

QVariant ExplorerRunsGroupTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) {
        return {};
    }

    const ExplorerRunsGroupRow& row = rows_[static_cast<size_t>(index.row())];
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case RootGroupId: return QString::number(row.rootGroupId);
        case ExplorerSettings: return row.settingsLabel;
        case Results: return row.resultsSummary;
        case TotalWaves: return row.totalWaves;
        case StatusSummary: return row.statusSummary;
        default: return {};
        }
    }

    if (role == Qt::TextAlignmentRole && index.column() == TotalWaves) {
        return Qt::AlignCenter;
    }

    return {};
}

QVariant ExplorerRunsGroupTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }

    switch (section) {
    case RootGroupId: return QStringLiteral("Root Group ID");
    case ExplorerSettings: return QStringLiteral("Explorer Settings");
    case Results: return QStringLiteral("Results");
    case TotalWaves: return QStringLiteral("Total Waves");
    case StatusSummary: return QStringLiteral("Status Summary");
    default: return {};
    }
}

void ExplorerRunsGroupTableModel::setRows(std::vector<ExplorerRunsGroupRow> rows)
{
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
}

const ExplorerRunsGroupRow* ExplorerRunsGroupTableModel::rowAt(int row) const
{
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return nullptr;
    }
    return &rows_[static_cast<size_t>(row)];
}
