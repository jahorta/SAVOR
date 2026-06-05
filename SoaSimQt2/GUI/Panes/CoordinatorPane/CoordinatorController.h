#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <vector>

struct WorkerSnapshot {};

class CoordinatorController : public QObject
{
    Q_OBJECT

public:
    explicit CoordinatorController(QObject* parent = nullptr);
    ~CoordinatorController() override = default;

    bool isRunning() const;
    bool isPaused() const;
    int targetWorkers() const;
    int activeWorkers() const;
    int eventBufferCapacity() const;
    bool startPaused() const;
    bool restartFailedJobsAutomatically() const;
    QString isoPath() const;
    QString dolphinBaseDir() const;
    QString validationMessage() const;
    const std::vector<WorkerSnapshot>& snapshot() const;
    const std::vector<WorkerSnapshot>& visualSnapshot() const;
    QStringList takeVisualLiveLogLineUpdates();
    QString visualReplayRuntimeStateText() const;
    bool visualReplayControlsEnabled() const;

public slots:
    void startCoordinator();
    void stopCoordinator();
    void setPaused(bool paused);
    void togglePaused();
    void setTargetWorkers(int targetWorkers);
    void setEventBufferCapacity(int capacity);
    void setStartPaused(bool startPaused);
    void setRestartFailedJobsAutomatically(bool enabled);
    void setIsoPath(const QString& isoPath);
    void setDolphinBaseDir(const QString& dolphinBaseDir);
    void setVisualRenderWidgetHandle(quintptr hwnd);
    void setVisualHostEventsPipeName(const QString& pipeName);
    void requestVisualReplay(qint64 jobId);
    void pauseVisualReplayEmulation();
    void stepVisualReplayVm();
    void resumeVisualReplayEmulation();
    void stopVisualReplay();
    void handleVisualLiveLogLinesRequested();
    void refreshSnapshot();

signals:
    void stateChanged();
    void snapshotChanged();
    void visualLiveLogLinesReady(const QStringList& lines);

private:
    void updateValidationMessage();

    std::vector<WorkerSnapshot> emptySnapshots_;
    int targetWorkers_ = 1;
    int eventBufferCapacity_ = 64;
    bool paused_ = true;
    bool startPaused_ = true;
    bool restartFailedJobsAutomatically_ = true;
    QString isoPath_;
    QString dolphinBaseDir_;
    QString validationMessage_;
};

