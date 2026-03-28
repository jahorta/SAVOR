#pragma once

#include <QtWidgets/QDialog>
#include <QtCore/QStringList>

class QWidget;
class QLabel;
class QTextEdit;
class QTimer;

class VisualWorkerDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit VisualWorkerDialog(QWidget* parent = nullptr);
    ~VisualWorkerDialog() override;

    quintptr renderWidgetHandle() const;
    void showRenderSurface();
    void showReplayDoneLabel();
    void startLogPolling();
    void stopLogPolling();

public slots:
    void updateLiveLogLines(const QStringList& lines);

signals:
    void pauseRequested();
    void resumeRequested();
    void vmStepRequested();
    void logPollRequested();

private:
    QWidget* renderWidget_ = nullptr;
    QLabel* replayDoneLabel_ = nullptr;
    QTextEdit* liveLogView_ = nullptr;
    QTimer* logPollTimer_ = nullptr;
};
