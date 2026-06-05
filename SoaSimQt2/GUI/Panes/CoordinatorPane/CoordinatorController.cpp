#include "CoordinatorController.h"

CoordinatorController::CoordinatorController(QObject* parent)
    : QObject(parent)
{
    updateValidationMessage();
}

bool CoordinatorController::isRunning() const { return false; }
bool CoordinatorController::isPaused() const { return paused_; }
int CoordinatorController::targetWorkers() const { return targetWorkers_; }
int CoordinatorController::activeWorkers() const { return 0; }
int CoordinatorController::eventBufferCapacity() const { return eventBufferCapacity_; }
bool CoordinatorController::startPaused() const { return startPaused_; }
bool CoordinatorController::restartFailedJobsAutomatically() const { return restartFailedJobsAutomatically_; }
QString CoordinatorController::isoPath() const { return isoPath_; }
QString CoordinatorController::dolphinBaseDir() const { return dolphinBaseDir_; }
QString CoordinatorController::validationMessage() const { return validationMessage_; }
const std::vector<WorkerSnapshot>& CoordinatorController::snapshot() const { return emptySnapshots_; }
const std::vector<WorkerSnapshot>& CoordinatorController::visualSnapshot() const { return emptySnapshots_; }
QStringList CoordinatorController::takeVisualLiveLogLineUpdates() { return {}; }
QString CoordinatorController::visualReplayRuntimeStateText() const { return QStringLiteral("Disabled during SimCoreDB UIRead cutover"); }
bool CoordinatorController::visualReplayControlsEnabled() const { return false; }

void CoordinatorController::startCoordinator() { updateValidationMessage(); emit stateChanged(); }
void CoordinatorController::stopCoordinator() { emit stateChanged(); }
void CoordinatorController::setPaused(bool paused) { paused_ = paused; emit stateChanged(); }
void CoordinatorController::togglePaused() { setPaused(!paused_); }
void CoordinatorController::setTargetWorkers(int targetWorkers) { targetWorkers_ = targetWorkers; emit stateChanged(); }
void CoordinatorController::setEventBufferCapacity(int capacity) { eventBufferCapacity_ = capacity; emit stateChanged(); }
void CoordinatorController::setStartPaused(bool startPaused) { startPaused_ = startPaused; emit stateChanged(); }
void CoordinatorController::setRestartFailedJobsAutomatically(bool enabled) { restartFailedJobsAutomatically_ = enabled; emit stateChanged(); }
void CoordinatorController::setIsoPath(const QString& isoPath) { isoPath_ = isoPath; updateValidationMessage(); emit stateChanged(); }
void CoordinatorController::setDolphinBaseDir(const QString& dolphinBaseDir) { dolphinBaseDir_ = dolphinBaseDir; updateValidationMessage(); emit stateChanged(); }
void CoordinatorController::setVisualRenderWidgetHandle(quintptr) {}
void CoordinatorController::setVisualHostEventsPipeName(const QString&) {}
void CoordinatorController::requestVisualReplay(qint64) { emit stateChanged(); }
void CoordinatorController::pauseVisualReplayEmulation() {}
void CoordinatorController::stepVisualReplayVm() {}
void CoordinatorController::resumeVisualReplayEmulation() {}
void CoordinatorController::stopVisualReplay() {}
void CoordinatorController::handleVisualLiveLogLinesRequested() { emit visualLiveLogLinesReady({}); }
void CoordinatorController::refreshSnapshot() { emit snapshotChanged(); }

void CoordinatorController::updateValidationMessage()
{
    validationMessage_ = QStringLiteral("Worker coordinator is disabled during SimCoreDB UIRead cutover.");
}

