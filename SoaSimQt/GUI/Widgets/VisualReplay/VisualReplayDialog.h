#pragma once

#include <QtWidgets/QDialog>
#include <QtCore/QStringList>

class QWidget;
class QLabel;
class QListView;
class QCheckBox;
class QComboBox;
class QToolButton;
class QPushButton;
class VisualReplayCoordinator;
class LiveLogListModel;
class LiveLogFilterController;

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
    void startLiveLogStreaming();
    void stopLiveLogStreaming();
    void startHostEventsListener();
    void stopHostEventsListener();
    QString hostEventsPipeName() const;
    VisualReplayCoordinator* visualReplayCoordinator() const;

public slots:
    void updateLiveLogLines(const QStringList& lines);
    void appendLiveLogLines(const QStringList& lines);
    void appendHostEventLine(const QString& eventName, const QString& argsJson);
    void setRenderSurfaceSize(int widthPx, int heightPx);

signals:
    void visualLiveLogLinesRequested();
    void pauseRequested();
    void resumeRequested();
    void vmStepRequested();

private:
    void refreshSourceMenu();

    QWidget* renderWidget_ = nullptr;
    QLabel* replayDoneLabel_ = nullptr;
    QLabel* replayStateLabel_ = nullptr;
    QListView* liveLogView_ = nullptr;
    QComboBox* levelFilterCombo_ = nullptr;
    QToolButton* sourceFilterButton_ = nullptr;
    QCheckBox* showFileCheck_ = nullptr;
    QPushButton* pauseButton_ = nullptr;
    QPushButton* stepVmButton_ = nullptr;
    QPushButton* resumeButton_ = nullptr;
    VisualReplayCoordinator* visualReplayCoordinator_ = nullptr;
    LiveLogListModel* liveLogModel_ = nullptr;
    LiveLogFilterController* liveLogController_ = nullptr;
};
