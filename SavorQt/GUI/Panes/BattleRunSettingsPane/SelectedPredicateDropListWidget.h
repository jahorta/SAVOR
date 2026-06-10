#pragma once

#include <QtCore/QtGlobal>
#include <QtWidgets/QListWidget>

class QDragEnterEvent;
class QDragMoveEvent;
class QDragLeaveEvent;
class QDropEvent;
class QMimeData;
class QPoint;

class SelectedPredicateDropListWidget final : public QListWidget
{
    Q_OBJECT

public:
    explicit SelectedPredicateDropListWidget(QWidget* parent = nullptr);

signals:
    void predicateDropped(qint64 predicateId, int insertRow);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void setDropActive(bool active);
    bool canAccept(const QMimeData* mimeData, qint64* predicateId = nullptr) const;
    int insertionRowAt(const QPoint& pos) const;
};
