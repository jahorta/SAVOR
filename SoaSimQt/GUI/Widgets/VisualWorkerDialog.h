#pragma once

#include <QtWidgets/QDialog>

class QWidget;
class QLabel;

class VisualWorkerDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit VisualWorkerDialog(QWidget* parent = nullptr);
    ~VisualWorkerDialog() override;

    quintptr renderWidgetHandle() const;
    void showRenderSurface();
    void showReplayDoneLabel();

private:
    QWidget* renderWidget_ = nullptr;
    QLabel* replayDoneLabel_ = nullptr;
};
