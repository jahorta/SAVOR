#pragma once

#include <QtCore/QStringList>
#include <QtGui/QStandardItemModel>

class QMimeData;

class PredicateDragTableModel final : public QStandardItemModel
{
public:
    explicit PredicateDragTableModel(QObject* parent = nullptr);

    Qt::DropActions supportedDragActions() const override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
};
