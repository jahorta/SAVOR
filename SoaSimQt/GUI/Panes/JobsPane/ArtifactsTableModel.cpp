#include "ArtifactsTableModel.h"

ArtifactsTableModel::ArtifactsTableModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

int ArtifactsTableModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : artifacts_.size();
}

int ArtifactsTableModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : 2;
}

QVariant ArtifactsTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal) {
        return QAbstractTableModel::headerData(section, orientation, role);
    }

    return section == 0 ? QStringLiteral("Role") : QStringLiteral("File");
}

QVariant ArtifactsTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || role != Qt::DisplayRole || index.row() >= artifacts_.size()) {
        return {};
    }

    if (index.column() == 0) {
        return index.row() == 0 ? QStringLiteral("primary") : QStringLiteral("support");
    }
    if (index.column() == 1) {
        return artifacts_.at(index.row());
    }
    return {};
}

void ArtifactsTableModel::setArtifacts(const QStringList& artifacts)
{
    beginResetModel();
    artifacts_ = artifacts;
    endResetModel();
}
