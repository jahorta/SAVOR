#include "PersistentToolWindow.h"

#include <QtGui/QCloseEvent>

PersistentToolWindow::PersistentToolWindow(QWidget* parent)
    : QWidget(parent, Qt::Window)
{
}

void PersistentToolWindow::closeEvent(QCloseEvent* event)
{
    emit aboutToClose();
    QWidget::closeEvent(event);
}
