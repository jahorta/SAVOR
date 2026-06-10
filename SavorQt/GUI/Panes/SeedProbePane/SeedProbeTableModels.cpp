#include "SeedProbeTableModels.h"

#include "GUI/Refresh/RowUpdate.h"

SeedProbeListModel::SeedProbeListModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

void SeedProbeListModel::setRows(const QVector<Row>& rows)
{
    const auto equalRows = [](const Row& lhs, const Row& rhs) {
        return lhs.probeId == rhs.probeId
            && lhs.status == rhs.status
            && lhs.savestateId == rhs.savestateId
            && lhs.filename == rhs.filename;
    };
    if (savorqt::gui::RowsEqual(rows_, rows, equalRows)) {
        return;
    }

    beginResetModel();
    rows_ = rows;
    endResetModel();
}

const SeedProbeListModel::Row* SeedProbeListModel::rowAt(int row) const
{
    if (row < 0 || row >= rows_.size()) {
        return nullptr;
    }
    return &rows_[row];
}

int SeedProbeListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : rows_.size();
}

int SeedProbeListModel::columnCount(const QModelIndex& parent) const
{
    Q_UNUSED(parent);
    return 4;
}

QVariant SeedProbeListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) {
        return {};
    }

    const Row& row = rows_.at(index.row());
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case 0: return row.probeId;
        case 1: return row.status;
        case 2: return row.savestateId;
        case 3: return row.filename;
        default: return {};
        }
    }
    if (role == Qt::TextAlignmentRole && index.column() != 3) {
        return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
    }
    return {};
}

QVariant SeedProbeListModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QAbstractTableModel::headerData(section, orientation, role);
    }

    switch (section) {
    case 0: return QStringLiteral("Probe ID");
    case 1: return QStringLiteral("Status");
    case 2: return QStringLiteral("Savestate ID");
    case 3: return QStringLiteral("Filename");
    default: return {};
    }
}

SeedProbeUniqueTableModel::SeedProbeUniqueTableModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

void SeedProbeUniqueTableModel::setRows(const QVector<Row>& rows)
{
    const auto equalRows = [](const Row& lhs, const Row& rhs) {
        return lhs.input == rhs.input && lhs.seedHex == rhs.seedHex;
    };
    if (savorqt::gui::RowsEqual(rows_, rows, equalRows)) {
        return;
    }

    beginResetModel();
    rows_ = rows;
    endResetModel();
}

int SeedProbeUniqueTableModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : rows_.size();
}

int SeedProbeUniqueTableModel::columnCount(const QModelIndex& parent) const
{
    Q_UNUSED(parent);
    return 2;
}

QVariant SeedProbeUniqueTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) {
        return {};
    }

    const Row& row = rows_.at(index.row());
    if (role == Qt::DisplayRole) {
        return index.column() == 0 ? QVariant(row.input) : QVariant(row.seedHex);
    }
    if (role == Qt::TextAlignmentRole) {
        return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
    }
    return {};
}

QVariant SeedProbeUniqueTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QAbstractTableModel::headerData(section, orientation, role);
    }
    return section == 0 ? QVariant(QStringLiteral("Input")) : QVariant(QStringLiteral("Seed"));
}
