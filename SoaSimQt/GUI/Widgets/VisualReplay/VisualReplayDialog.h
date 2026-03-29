#pragma once

#include <QtWidgets/QDialog>
#include <QtCore/QStringList>

class QWidget;
class QLabel;
class QTextEdit;
class QPushButton;

class VisualReplayDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit VisualReplayDialog(QWidget* parent = nullptr);
    ~VisualReplayDialog() override;

    quintptr renderWidgetHandle() const;
    void showRenderSurface();
    void showReplayDoneLabel();
    void resetLiveLog();
    void setReplayRuntimeStateText(const QString& text);
    void setReplayControlsEnabled(bool enabled);

public slots:
    void updateLiveLogLines(const QStringList& lines);
    void appendLiveLogLines(const QStringList& lines);
    void appendHostEventLine(const QString& eventName, const QString& argsJson);
    void setRenderSurfaceSize(int widthPx, int heightPx);

signals:
    void pauseRequested();
    void resumeRequested();
    void vmStepRequested();

private:
    QWidget* renderWidget_ = nullptr;
    QLabel* replayDoneLabel_ = nullptr;
    QLabel* replayStateLabel_ = nullptr;
    QTextEdit* liveLogView_ = nullptr;
    QPushButton* pauseButton_ = nullptr;
    QPushButton* stepVmButton_ = nullptr;
    QPushButton* resumeButton_ = nullptr;
};
