#pragma once

#include <QtWidgets/QDialog>

class QWidget;

class VisualWorkerDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit VisualWorkerDialog(QWidget* parent = nullptr);
    ~VisualWorkerDialog() override;

    quintptr renderWidgetHandle() const;

private:
    QWidget* renderWidget_ = nullptr;
};
