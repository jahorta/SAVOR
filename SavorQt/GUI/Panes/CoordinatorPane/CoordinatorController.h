#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <memory>
#include <unordered_map>
#include <vector>

#include "Runner/Parallel/PRTypes.h"
#include "Execution/DBWorkflowWorkerCoordinator.h"
#include "Execution/Workflow/WorkflowCoordinatorService.h"

class CoordinatorController : public QObject
{
    Q_OBJECT

public:
    explicit CoordinatorController(QObject* parent = nullptr);
    ~CoordinatorController() override;

    bool isRunning() const;
    bool isPaused() const;
    int targetWorkers() const;
    int activeWorkers() const;
    int eventBufferCapacity() const;
    bool startPaused() const;
    bool restartFailedJobsAutomatically() const;
    bool visualWorkerPoolEnabled() const;
    QString isoPath() const;
    QString dolphinBaseDir() const;
    QString validationMessage() const;
    const std::vector<WorkerSnapshot>& snapshot() const;
    std::vector<WorkerSnapshot> freshSnapshot() const;
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
    void setVisualWorkerPoolEnabled(bool enabled);
    void setVisualWorkerSurface(int workerIndex, quintptr hwnd, const QString& hostEventsPipeName);
    void clearVisualWorkerSurfaces();
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
    static constexpr int kMinTargetWorkers = 1;
    static constexpr int kMaxTargetWorkers = 9999;
    static constexpr int kMinEventBufferCapacity = 8;

    void loadSettings();
    void persistString(const char* key, const QString& value);
    void persistInt(const char* key, int value);
    void updateValidationMessage();
    void updateSnapshotCache();
    savor::runner::parallel::savordb::DBWorkflowWorkerCoordinatorConfig buildWorkerConfig() const;
    QString visualReplayStateToText(savor::runner::parallel::savordb::VisualReplayRuntimeState state) const;
    void applyVisualWorkerSurfaces();
    QString workerExePath() const;
    QString workerRootPath() const;

    struct VisualWorkerSurface {
        quintptr renderWidgetHandle = 0;
        QString hostEventsPipeName;
    };

    std::unique_ptr<savor::runner::parallel::savordb::DBWorkflowWorkerCoordinator> coordinator_;
    std::vector<WorkerSnapshot> snapshotCache_;
    std::vector<WorkerSnapshot> visualSnapshotCache_;
    savor::PRStatus statusSnapshot_{};
    savor::db::execution::workflow::WorkflowCoordinatorTelemetry telemetrySnapshot_{};
    int targetWorkers_ = 1;
    int eventBufferCapacity_ = 64;
    bool paused_ = false;
    bool startPaused_ = true;
    bool restartFailedJobsAutomatically_ = true;
    bool visualWorkerPoolEnabled_ = false;
    std::unordered_map<int, VisualWorkerSurface> visualWorkerSurfaces_;
    quintptr visualRenderWidgetHandle_ = 0;
    QString visualHostEventsPipeName_;
    QString visualReplayLastError_;
    QString isoPath_;
    QString dolphinBaseDir_;
    QString validationMessage_;
};
