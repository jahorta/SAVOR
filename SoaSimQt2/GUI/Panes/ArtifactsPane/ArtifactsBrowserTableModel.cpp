#include "ArtifactsBrowserTableModel.h"

#include "GUI/Refresh/RowUpdate.h"

#include <QtCore/QDateTime>

namespace {
constexpr int kColumnCount = 6;
}

ArtifactsBrowserTableModel::ArtifactsBrowserTableModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

int ArtifactsBrowserTableModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int ArtifactsBrowserTableModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : kColumnCount;
}

QVariant ArtifactsBrowserTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal) {
        return QAbstractTableModel::headerData(section, orientation, role);
    }

    switch (section) {
    case 0: return QStringLiteral("ID");
    case 1: return QStringLiteral("Filename");
    case 2: return QStringLiteral("Kind");
    case 3: return QStringLiteral("Size");
    case 4: return QStringLiteral("SHA-256");
    case 5: return QStringLiteral("Created");
    default: return {};
    }
}

QVariant ArtifactsBrowserTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size())) {
        return {};
    }

    const simcore::db::UiArtifactSummary& artifact = rows_[index.row()].artifact;
    if (role == Qt::TextAlignmentRole && (index.column() == 0 || index.column() == 3 || index.column() == 5)) {
        return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
    }
    if (role == Qt::ToolTipRole && index.column() == 4) {
        return QString::fromStdString(artifact.sha256);
    }
    if (role != Qt::DisplayRole) {
        return {};
    }

    switch (index.column()) {
    case 0: return artifact.artifact_id;
    case 1: return QString::fromStdString(artifact.filename);
    case 2: return QString::fromStdString(artifact.artifact_kind);
    case 3: return formatSize(static_cast<qint64>(artifact.size_bytes));
    case 4: {
        const QString sha = QString::fromStdString(artifact.sha256);
        return sha.size() > 12 ? QStringLiteral("%1...").arg(sha.left(12)) : sha;
    }
    case 5: return formatCreatedAt(artifact.created_at_utc);
    default: return {};
    }
}

void ArtifactsBrowserTableModel::setRows(const std::vector<Row>& rows)
{
    const auto equalRows = [](const Row& lhs, const Row& rhs) {
        return lhs.artifact.artifact_id == rhs.artifact.artifact_id
            && lhs.artifact.filename == rhs.artifact.filename
            && lhs.artifact.artifact_kind == rhs.artifact.artifact_kind
            && lhs.artifact.size_bytes == rhs.artifact.size_bytes
            && lhs.artifact.sha256 == rhs.artifact.sha256
            && lhs.artifact.created_at_utc == rhs.artifact.created_at_utc;
    };
    if (soasimqt2::gui::RowsEqual(rows_, rows, equalRows)) {
        return;
    }

    beginResetModel();
    rows_ = rows;
    endResetModel();
}

const ArtifactsBrowserTableModel::Row* ArtifactsBrowserTableModel::rowAt(int row) const
{
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return nullptr;
    }
    return &rows_[row];
}

QString ArtifactsBrowserTableModel::formatSize(qint64 size)
{
    static const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    double value = static_cast<double>(size);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    return unit == 0
        ? QStringLiteral("%1 %2").arg(static_cast<qlonglong>(value)).arg(QString::fromLatin1(units[unit]))
        : QStringLiteral("%1 %2").arg(value, 0, 'f', 1).arg(QString::fromLatin1(units[unit]));
}

QString ArtifactsBrowserTableModel::formatCreatedAt(qint64 epochMilliseconds)
{
    return QDateTime::fromMSecsSinceEpoch(epochMilliseconds).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}
