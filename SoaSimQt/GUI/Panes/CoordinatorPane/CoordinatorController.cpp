#include "CoordinatorController.h"

#include <QtCore/QSettings>
#include <QtCore/QSignalBlocker>
#include <QtCore/QStringList>
#include <QtCore/QVariant>
#include <QtConcurrent/QtConcurrentRun>

#include "DB/Querying/DataService.h"

#include <algorithm>

namespace {
constexpr auto kSettingsGroup = "Coordinator";
constexpr auto kIsoPathKey = "iso_path";
constexpr auto kDolphinBaseKey = "dolphin_base";
constexpr auto kTargetWorkersKey = "target_workers";
constexpr auto kEventRingKey = "event_ring";
constexpr auto kStartPausedKey = "start_paused";
constexpr auto kVisualRenderHandleKey = "visual_render_widget_handle";
}

CoordinatorController::CoordinatorController(QObject* parent)
    : QObject(parent)
{
    loadSettings();
    updateValidationMessage();
    updateSnapshotCache();
}

CoordinatorController::~CoordinatorController()
{
    const QSignalBlocker blocker(this);
    stopCoordinator();
}

bool CoordinatorController::isRunning() const
{
    return coordinator_ != nullptr;
}

bool CoordinatorController::isPaused() const
{
    return paused_;
}

int CoordinatorController::targetWorkers() const
{
    return targetWorkers_;
}

int CoordinatorController::activeWorkers() const
{
    return static_cast<int>(snapshotCache_.size());
}

int CoordinatorController::eventBufferCapacity() const
{
    return eventBufferCapacity_;
}

bool CoordinatorController::startPaused() const
{
    return startPaused_;
}

QString CoordinatorController::isoPath() const
{
    return isoPath_;
}

QString CoordinatorController::dolphinBaseDir() const
{
    return dolphinBaseDir_;
}

QString CoordinatorController::validationMessage() const
{
    return validationMessage_;
}

const std::vector<WorkerSnapshot>& CoordinatorController::snapshot() const
{
    return snapshotCache_;
}

const std::vector<WorkerSnapshot>& CoordinatorController::visualSnapshot() const
{
    return visualSnapshotCache_;
}

void CoordinatorController::startCoordinator()
{
    if (isRunning()) {
        return;
    }

    updateValidationMessage();
    if (!validationMessage_.isEmpty()) {
        emit stateChanged();
        return;
    }

    WorkerCoordinatorConfig cfg = buildConfig();
    coordinator_ = std::make_unique<simcore::WorkerCoordinator>(cfg);
    coordinator_->SetVisualRenderWidgetHandle(static_cast<uint64_t>(visualRenderWidgetHandle_));
    coordinator_->start();
    paused_ = cfg.start_to_paused;
    coordinator_->SetEventBufferCapacity(static_cast<size_t>(eventBufferCapacity_));

    updateSnapshotCache();
    emit stateChanged();
    emit snapshotChanged();
}

void CoordinatorController::stopCoordinator()
{
    if (!coordinator_) {
        return;
    }

    coordinator_->stop();
    coordinator_.reset();
    paused_ = false;
    snapshotCache_.clear();
    visualSnapshotCache_.clear();

    emit stateChanged();
    emit snapshotChanged();
}

void CoordinatorController::setPaused(bool paused)
{
    paused_ = paused;
    if (coordinator_) {
        coordinator_->set_paused(paused_);
        updateSnapshotCache();
        emit snapshotChanged();
    }
    emit stateChanged();
}

void CoordinatorController::togglePaused()
{
    setPaused(!paused_);
}

void CoordinatorController::setTargetWorkers(int targetWorkers)
{
    const int clampedValue = (std::max)(kMinTargetWorkers, targetWorkers);
    if (targetWorkers_ == clampedValue) {
        return;
    }

    targetWorkers_ = clampedValue;
    persistInt(kTargetWorkersKey, targetWorkers_);

    if (coordinator_) {
        coordinator_->set_target_workers(static_cast<size_t>(targetWorkers_));
    }

    emit stateChanged();
}

void CoordinatorController::setEventBufferCapacity(int capacity)
{
    const int clampedValue = (std::max)(kMinEventBufferCapacity, capacity);
    if (eventBufferCapacity_ == clampedValue) {
        return;
    }

    eventBufferCapacity_ = clampedValue;
    persistInt(kEventRingKey, eventBufferCapacity_);

    if (coordinator_) {
        coordinator_->SetEventBufferCapacity(static_cast<size_t>(eventBufferCapacity_));
    }

    emit stateChanged();
}

void CoordinatorController::setStartPaused(bool startPaused)
{
    if (startPaused_ == startPaused) {
        return;
    }

    startPaused_ = startPaused;
    persistInt(kStartPausedKey, startPaused_ ? 1 : 0);
    emit stateChanged();
}

void CoordinatorController::setIsoPath(const QString& isoPath)
{
    if (isoPath_ == isoPath) {
        return;
    }

    isoPath_ = isoPath;
    persistString(kIsoPathKey, isoPath_);
    updateValidationMessage();
    emit stateChanged();
}

void CoordinatorController::setDolphinBaseDir(const QString& dolphinBaseDir)
{
    if (dolphinBaseDir_ == dolphinBaseDir) {
        return;
    }

    dolphinBaseDir_ = dolphinBaseDir;
    persistString(kDolphinBaseKey, dolphinBaseDir_);
    updateValidationMessage();
    emit stateChanged();
}

void CoordinatorController::setVisualRenderWidgetHandle(quintptr hwnd)
{
    if (visualRenderWidgetHandle_ == hwnd) {
        return;
    }
    visualRenderWidgetHandle_ = hwnd;
    if (coordinator_) {
        coordinator_->SetVisualRenderWidgetHandle(static_cast<uint64_t>(visualRenderWidgetHandle_));
    }
    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    settings.setValue(kVisualRenderHandleKey, QVariant::fromValue(static_cast<qulonglong>(visualRenderWidgetHandle_)));
    settings.endGroup();
}

void CoordinatorController::requestVisualReplay(qint64 jobId)
{
    if (jobId <= 0) {
        return;
    }
    QtConcurrent::run([jobId]() {
        (void)simcore::db::DataService::ReplayJobVisuallyAsync(jobId).get();
    });
}

void CoordinatorController::pauseVisualReplayEmulation()
{
    if (!coordinator_) return;
    (void)coordinator_->PauseVisualReplayEmulation();
}

void CoordinatorController::stepVisualReplayVm()
{
    if (!coordinator_) return;
    (void)coordinator_->StepVisualReplayVm();
}

void CoordinatorController::resumeVisualReplayEmulation()
{
    if (!coordinator_) return;
    (void)coordinator_->ResumeVisualReplayEmulation();
}

void CoordinatorController::refreshSnapshot()
{
    updateSnapshotCache();
    emit stateChanged();
    emit snapshotChanged();
}

void CoordinatorController::loadSettings()
{
    QSettings settings;
    settings.beginGroup(kSettingsGroup);

    isoPath_ = settings.value(kIsoPathKey).toString();
    dolphinBaseDir_ = settings.value(kDolphinBaseKey).toString();
    targetWorkers_ = (std::max)(kMinTargetWorkers, settings.value(kTargetWorkersKey, targetWorkers_).toInt());
    eventBufferCapacity_ = (std::max)(kMinEventBufferCapacity, settings.value(kEventRingKey, eventBufferCapacity_).toInt());
    startPaused_ = settings.value(kStartPausedKey, startPaused_ ? 1 : 0).toInt() != 0;
    visualRenderWidgetHandle_ = static_cast<quintptr>(settings.value(kVisualRenderHandleKey, 0).toULongLong());

    settings.endGroup();
}

void CoordinatorController::persistString(const char* key, const QString& value)
{
    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    settings.setValue(key, value);
    settings.endGroup();
}

void CoordinatorController::persistInt(const char* key, int value)
{
    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    settings.setValue(key, value);
    settings.endGroup();
}

void CoordinatorController::updateValidationMessage()
{
    QStringList issues;
    if (isoPath_.trimmed().isEmpty()) {
        issues.append("ISO path is required.");
    }
    if (dolphinBaseDir_.trimmed().isEmpty()) {
        issues.append("Dolphin base directory is required.");
    }

    validationMessage_ = issues.join(' ');
}

void CoordinatorController::updateSnapshotCache()
{
    if (!coordinator_) {
        snapshotCache_.clear();
        visualSnapshotCache_.clear();
        return;
    }

    snapshotCache_ = coordinator_->GetClusterSnapshot();
    visualSnapshotCache_.clear();
    if (const auto visual = coordinator_->GetVisualWorkerSnapshot(); visual.has_value()) {
        visualSnapshotCache_.push_back(*visual);
    }
}

WorkerCoordinatorConfig CoordinatorController::buildConfig() const
{
    WorkerCoordinatorConfig cfg{};
    cfg.max_concurrent_processes = static_cast<size_t>((std::max)(64, targetWorkers_));
    cfg.desired_workers = static_cast<size_t>(targetWorkers_);
    cfg.iso_path = isoPath_.toStdString();
    cfg.dolphin_base_dir = dolphinBaseDir_.toStdString();
    cfg.start_to_paused = startPaused_;
    return cfg;
}
