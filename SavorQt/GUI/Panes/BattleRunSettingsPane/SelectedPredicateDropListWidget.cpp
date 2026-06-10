#include "SelectedPredicateDropListWidget.h"

#include "BattleRunSettingsDragDrop.h"

#include <QtCore/QMimeData>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDragLeaveEvent>
#include <QtGui/QDropEvent>
#include <QtWidgets/QStyle>

SelectedPredicateDropListWidget::SelectedPredicateDropListWidget(QWidget* parent)
    : QListWidget(parent)
{
    setAcceptDrops(true);
    setProperty("dropActive", false);
}

void SelectedPredicateDropListWidget::dragEnterEvent(QDragEnterEvent* event)
{
    if (!event) {
        return;
    }

    if (canAccept(event->mimeData())) {
        setDropActive(true);
        event->acceptProposedAction();
        return;
    }

    QListWidget::dragEnterEvent(event);
}

void SelectedPredicateDropListWidget::dragMoveEvent(QDragMoveEvent* event)
{
    if (!event) {
        return;
    }

    if (canAccept(event->mimeData())) {
        setDropActive(true);
        event->acceptProposedAction();
        return;
    }

    QListWidget::dragMoveEvent(event);
}

void SelectedPredicateDropListWidget::dragLeaveEvent(QDragLeaveEvent* event)
{
    setDropActive(false);
    QListWidget::dragLeaveEvent(event);
}

void SelectedPredicateDropListWidget::dropEvent(QDropEvent* event)
{
    if (!event) {
        return;
    }

    qint64 predicateId = 0;
    if (canAccept(event->mimeData(), &predicateId)) {
        setDropActive(false);
        emit predicateDropped(predicateId, insertionRowAt(event->position().toPoint()));
        event->acceptProposedAction();
        return;
    }

    setDropActive(false);
    QListWidget::dropEvent(event);
}

void SelectedPredicateDropListWidget::setDropActive(const bool active)
{
    if (property("dropActive").toBool() == active) {
        return;
    }

    setProperty("dropActive", active);
    style()->unpolish(this);
    style()->polish(this);
    viewport()->update();
    update();
}

bool SelectedPredicateDropListWidget::canAccept(const QMimeData* mimeData, qint64* predicateId) const
{
    qint64 decodedPredicateId = 0;
    if (!battlerunsettings::decodePredicateId(mimeData, &decodedPredicateId)) {
        return false;
    }

    if (predicateId) {
        *predicateId = decodedPredicateId;
    }
    return true;
}

int SelectedPredicateDropListWidget::insertionRowAt(const QPoint& pos) const
{
    const QModelIndex hit = indexAt(pos);
    if (!hit.isValid()) {
        return count();
    }

    const QRect rect = visualRect(hit);
    if (pos.y() > rect.center().y()) {
        return hit.row() + 1;
    }
    return hit.row();
}
