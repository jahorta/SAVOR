#include "ArtifactsTableModel.h"

#include <algorithm>

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
    int targetRow = 0;
    while (targetRow < static_cast<int>(artifacts.size())) {
        const qint64 targetArtifactId = artifacts[static_cast<size_t>(targetRow)].artifact_id;

        if (targetRow < static_cast<int>(artifacts_.size()) && artifacts_[static_cast<size_t>(targetRow)].artifact_id == targetArtifactId) {
            ++targetRow;
            continue;
        }

        auto existingIt = std::find_if(artifacts_.begin() + std::min(targetRow, static_cast<int>(artifacts_.size())), artifacts_.end(), [targetArtifactId](const simcore::db::ArtifactRefLite& artifact) {
            return artifact.artifact_id == targetArtifactId;
        });

        if (existingIt != artifacts_.end()) {
            const int sourceRow = static_cast<int>(std::distance(artifacts_.begin(), existingIt));
            const int destinationChild = targetRow;
            beginMoveRows(QModelIndex(), sourceRow, sourceRow, QModelIndex(), destinationChild);
            auto moved = std::move(artifacts_[static_cast<size_t>(sourceRow)]);
            artifacts_.erase(artifacts_.begin() + sourceRow);
            artifacts_.insert(artifacts_.begin() + destinationChild, std::move(moved));
            endMoveRows();
            ++targetRow;
            continue;
        }

        beginInsertRows(QModelIndex(), targetRow, targetRow);
        artifacts_.insert(artifacts_.begin() + targetRow, artifacts[static_cast<size_t>(targetRow)]);
        endInsertRows();
        ++targetRow;
    }

    while (static_cast<int>(artifacts_.size()) > static_cast<int>(artifacts.size())) {
        const int staleRow = static_cast<int>(artifacts_.size()) - 1;
        beginRemoveRows(QModelIndex(), staleRow, staleRow);
        artifacts_.pop_back();
        endRemoveRows();
    }

    int changeStart = -1;
    for (int row = 0; row < static_cast<int>(artifacts.size()); ++row) {
        const auto& incoming = artifacts[static_cast<size_t>(row)];
        if (artifactAffectsDisplay(artifacts_[static_cast<size_t>(row)], incoming)) {
            artifacts_[static_cast<size_t>(row)] = incoming;
            if (changeStart < 0) {
                changeStart = row;
            }
            continue;
        }

        if (changeStart >= 0) {
            emit dataChanged(index(changeStart, 0), index(row - 1, columnCount() - 1));
            changeStart = -1;
        }
    }

    if (changeStart >= 0) {
        emit dataChanged(index(changeStart, 0), index(static_cast<int>(artifacts.size()) - 1, columnCount() - 1));
    }
}

bool ArtifactsTableModel::artifactAffectsDisplay(const simcore::db::ArtifactRefLite& lhs, const simcore::db::ArtifactRefLite& rhs)
{
    return lhs.artifact_id != rhs.artifact_id
        || lhs.role != rhs.role
        || lhs.filename != rhs.filename
        || lhs.size_bytes != rhs.size_bytes;
}
