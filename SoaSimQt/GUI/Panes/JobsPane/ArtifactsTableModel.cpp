#include "ArtifactsTableModel.h"

ArtifactsTableModel::ArtifactsTableModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

int ArtifactsTableModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : static_cast<int>(artifacts_.size()); }
int ArtifactsTableModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : 4; }

QVariant ArtifactsTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal) return QAbstractTableModel::headerData(section, orientation, role);
    switch (section) {
    case 0: return QStringLiteral("Role");
    case 1: return QStringLiteral("Artifact ID");
    case 2: return QStringLiteral("Size");
    case 3: return QStringLiteral("File");
    default: return {};
    }
}

QVariant ArtifactsTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(artifacts_.size())) return {};
    const auto& artifact = artifacts_[index.row()];
    if (role == Qt::TextAlignmentRole && (index.column() == 1 || index.column() == 2)) return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
    if (role != Qt::DisplayRole) return {};
    switch (index.column()) {
    case 0: return QString::fromStdString(artifact.role);
    case 1: return artifact.artifact_id;
    case 2: return QString::number(static_cast<qulonglong>(artifact.size_bytes));
    case 3: return QString::fromStdString(artifact.filename);
    default: return {};
    }
}

void ArtifactsTableModel::setArtifacts(const std::vector<simcore::db::ArtifactRefLite>& artifacts)
{
    beginResetModel();
    artifacts_ = artifacts;
    endResetModel();
}
