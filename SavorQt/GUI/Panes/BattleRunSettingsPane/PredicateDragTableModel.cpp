#include "PredicateDragTableModel.h"

#include "BattleRunSettingsDragDrop.h"

#include <QtCore/QMimeData>

PredicateDragTableModel::PredicateDragTableModel(QObject* parent)
    : QStandardItemModel(parent)
{
}

Qt::DropActions PredicateDragTableModel::supportedDragActions() const
{
    return Qt::CopyAction;
}

QStringList PredicateDragTableModel::mimeTypes() const
{
    return QStringList{ battlerunsettings::predicateMimeType() };
}

QMimeData* PredicateDragTableModel::mimeData(const QModelIndexList& indexes) const
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

    const QVariant predicateIdData = data(sourceIndex, battlerunsettings::kPredicateIdRole);
    const qint64 predicateId = predicateIdData.toLongLong();
    if (predicateId <= 0) {
        return nullptr;
    }

    auto* mimeData = new QMimeData();
    mimeData->setData(battlerunsettings::predicateMimeType(), battlerunsettings::encodePredicateId(predicateId));
    return mimeData;
}
