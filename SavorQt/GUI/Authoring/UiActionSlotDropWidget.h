#pragma once

#include <QtCore/QtGlobal>
#include <QtWidgets/QFrame>

class QDragEnterEvent;
class QDragMoveEvent;
class QDragLeaveEvent;
class QDropEvent;
class QMimeData;

class UiActionSlotDropWidget final : public QFrame
{
    Q_OBJECT

public:
    explicit UiActionSlotDropWidget(int turnIndex, int actorSlot, QWidget* parent = nullptr);

signals:
    void presetDropped(int turnIndex, int actorSlot, qint64 presetId);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void setDropActive(bool active);
    bool canAccept(const QMimeData* mimeData, qint64* presetId = nullptr) const;

    int turnIndex_ = 0;
    int actorSlot_ = 0;
};
