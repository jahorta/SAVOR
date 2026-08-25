#include "UiActionSlotDropWidget.h"

#include "BattlePlanDragDrop.h"

#include <QtCore/QMimeData>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDragLeaveEvent>
#include <QtGui/QDropEvent>
#include <QtWidgets/QStyle>

UiActionSlotDropWidget::UiActionSlotDropWidget(const int turnIndex, const int actorSlot, QWidget* parent)
    : QFrame(parent)
    , turnIndex_(turnIndex)
    , actorSlot_(actorSlot)
{
    setAcceptDrops(true);
    setProperty("dropActive", false);
}

void UiActionSlotDropWidget::dragEnterEvent(QDragEnterEvent* event)
{
    if (!event) {
        return;
    }

    if (canAccept(event->mimeData())) {
        setDropActive(true);
        event->acceptProposedAction();
        return;
    }

    QFrame::dragEnterEvent(event);
}

void UiActionSlotDropWidget::dragMoveEvent(QDragMoveEvent* event)
{
    if (!event) {
        return;
    }

    if (canAccept(event->mimeData())) {
        setDropActive(true);
        event->acceptProposedAction();
        return;
    }

    QFrame::dragMoveEvent(event);
}

void UiActionSlotDropWidget::dragLeaveEvent(QDragLeaveEvent* event)
{
    setDropActive(false);
    QFrame::dragLeaveEvent(event);
}

void UiActionSlotDropWidget::dropEvent(QDropEvent* event)
{
    if (!event) {
        return;
    }

    qint64 presetId = 0;
    if (canAccept(event->mimeData(), &presetId)) {
        setDropActive(false);
        emit presetDropped(turnIndex_, actorSlot_, presetId);
        event->acceptProposedAction();
        return;
    }

    setDropActive(false);
    QFrame::dropEvent(event);
}

void UiActionSlotDropWidget::setDropActive(const bool active)
{
    if (property("dropActive").toBool() == active) {
        return;
    }

    setProperty("dropActive", active);
    style()->unpolish(this);
    style()->polish(this);
    update();
}

bool UiActionSlotDropWidget::canAccept(const QMimeData* mimeData, qint64* presetId) const
{
    qint64 decodedPresetId = 0;
    if (!battlerunsettings::decodePresetId(mimeData, &decodedPresetId)) {
        return false;
    }

    if (presetId) {
        *presetId = decodedPresetId;
    }
    return true;
}
