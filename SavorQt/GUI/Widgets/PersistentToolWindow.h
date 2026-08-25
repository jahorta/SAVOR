#pragma once

#include <QtWidgets/QWidget>

class QCloseEvent;

class PersistentToolWindow : public QWidget
{
    Q_OBJECT

public:
    explicit PersistentToolWindow(QWidget* parent = nullptr);

signals:
    void aboutToClose();

protected:
    void closeEvent(QCloseEvent* event) override;
};
