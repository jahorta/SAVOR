#include "ActionPresetDragTableModel.h"

#include "BattlePlanDragDrop.h"

#include <QtCore/QMimeData>

ActionPresetDragTableModel::ActionPresetDragTableModel(QObject* parent)
    : QStandardItemModel(parent)
{
}

Qt::DropActions ActionPresetDragTableModel::supportedDragActions() const
{
    return Qt::CopyAction;
}

QStringList ActionPresetDragTableModel::mimeTypes() const
{
    return QStringList{ battlerunsettings::presetMimeType() };
}

QMimeData* ActionPresetDragTableModel::mimeData(const QModelIndexList& indexes) const
{
    if (indexes.isEmpty()) {
        return nullptr;
    }

    QModelIndex sourceIndex;
    for (const QModelIndex& index : indexes) {
        if (index.isValid()) {
            sourceIndex = index.siblingAtColumn(0);
            break;
        }
    }
    if (!sourceIndex.isValid()) {
        return nullptr;
    }

    const QVariant presetIdData = data(sourceIndex, battlerunsettings::kPresetIdRole);
    const qint64 presetId = presetIdData.toLongLong();
    if (presetId <= 0) {
        return nullptr;
    }

    auto* mimeData = new QMimeData();
    mimeData->setData(battlerunsettings::presetMimeType(), battlerunsettings::encodePresetId(presetId));
    return mimeData;
}
