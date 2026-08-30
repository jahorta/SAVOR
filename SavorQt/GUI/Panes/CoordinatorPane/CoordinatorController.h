#pragma once

#include <QtCore/QFutureWatcher>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QTimer>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "Execution/CoordinatorRuntime.h"

enum class CoordinatorLifecycleState {
    Stopped,
    Starting,
    Running,
    Stopping,
};

enum class CoordinatorStartupDisposition {
    Started,
    Failed,
    Canceled,
};

struct CoordinatorStartupResult {
    std::uint64_t generation = 0;
    CoordinatorStartupDisposition disposition =
        CoordinatorStartupDisposition::Failed;
    QString error;
};

struct CoordinatorStartupSharedState;
struct CoordinatorShutdownSharedState;

struct CoordinatorShutdownResult {
    std::uint64_t generation = 0;
    QString warning;
};

class CoordinatorController : public QObject
{
    Q_OBJECT

public:
    explicit CoordinatorController(QObject* parent = nullptr);
    ~CoordinatorController() override;

    CoordinatorLifecycleState lifecycleState() const;
    bool isStopped() const;
    bool isTransitioning() const;
    bool isRunning() const;
    bool isPaused() const;
    int targetWorkers() const;
    int activeWorkers() const;
    bool startPaused() const;
    bool visualWorkerPoolEnabled() const;
    QString isoPath() const;
    QString dolphinBaseDir() const;
    QString validationMessage() const;
    QString resultStagingCleanupError() const;
    const std::vector<WorkerSnapshot>& snapshot() const;
    std::vector<WorkerSnapshot> freshSnapshot() const;
    const std::vector<
        savor::runner::parallel::savordb::JobExecutionCoordinatorWarning>&
        warningSnapshot() const;
    QStringList takeVisualLiveLogLineUpdates();
    QString visualReplayRuntimeStateText() const;
    bool visualReplayControlsEnabled() const;
    void waitForShutdown();

public slots:
    void startCoordinator();
    void stopCoordinator();
    void setPaused(bool paused);
    void togglePaused();
    void setTargetWorkers(int targetWorkers);
    void setStartPaused(bool startPaused);
    void setVisualWorkerPoolEnabled(bool enabled);
    void setVisualWorkerSurface(
        int workerIndex,
        quintptr hwnd,
        quint64 surfaceGeneration,
        quint32 ownerProcessId,
        const QString& hostEventsPipeName);
    void invalidateVisualWorkerSurface(
        int workerIndex,
        quint64 surfaceGeneration);
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
    static constexpr int kMaxTargetWorkers = static_cast<int>(
        savor::runner::parallel::savordb::kMaximumWorkerCount);

    void loadSettings();
    void persistString(const char* key, const QString& value);
    void persistInt(const char* key, int value);
    void updateValidationMessage();
    void updateSnapshotCache();
    void handleStartupFinished();
    void handleStartupCleanupFinished();
    void handleShutdownFinished();
    void startStartupCleanup();
    void startRuntimeShutdown();
    savor::runner::parallel::savordb::WorkerCoordinatorConfig buildWorkerConfig() const;
    void stopCoordinatorServices();
    QString workerExePath() const;
    QString workerRootPath() const;

    struct VisualWorkerSurface {
        quintptr renderWidgetHandle = 0;
        quint64 surfaceGeneration = 0;
        quint32 ownerProcessId = 0;
        QString hostEventsPipeName;
    };

    std::unique_ptr<
        savor::runner::parallel::savordb::CoordinatorRuntime>
        coordinator_runtime_;
    std::shared_ptr<CoordinatorStartupSharedState> startup_state_;
    QFutureWatcher<CoordinatorStartupResult> startup_watcher_;
    QFutureWatcher<void> startup_cleanup_watcher_;
    std::shared_ptr<CoordinatorShutdownSharedState> shutdown_state_;
    QFutureWatcher<CoordinatorShutdownResult> shutdown_watcher_;
    QTimer snapshot_refresh_timer_;
    CoordinatorLifecycleState lifecycle_state_ =
        CoordinatorLifecycleState::Stopped;
    std::uint64_t startup_generation_ = 0;
    std::uint64_t shutdown_generation_ = 0;
    std::vector<WorkerSnapshot> snapshotCache_;
    std::vector<
        savor::runner::parallel::savordb::JobExecutionCoordinatorWarning>
        warningSnapshotCache_;
    int targetWorkers_ = 1;
    bool paused_ = false;
    bool startPaused_ = true;
    bool visualWorkerPoolEnabled_ = false;
    std::unordered_map<int, VisualWorkerSurface> visualWorkerSurfaces_;
    quintptr visualRenderWidgetHandle_ = 0;
    QString visualHostEventsPipeName_;
    QString visualReplayLastError_;
    QString isoPath_;
    QString dolphinBaseDir_;
    QString validationMessage_;
    QString resultStagingCleanupError_;
};
