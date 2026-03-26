#pragma once

#include <QtWidgets/QStyledItemDelegate>

class JobSetsProgressDelegate final : public QStyledItemDelegate
{
public:
    explicit JobSetsProgressDelegate(QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};
