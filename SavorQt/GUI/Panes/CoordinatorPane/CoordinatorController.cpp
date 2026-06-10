#include "CoordinatorController.h"

#include "SavorDbRuntime.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtCore/QStringList>

#include "Runner/IPC/Wire.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <utility>

namespace {
constexpr auto kSettingsGroup = "Coordinator";
constexpr auto kIsoPathKey = "iso_path";
constexpr auto kDolphinBaseKey = "dolphin_base";
constexpr auto kTargetWorkersKey = "target_workers";
constexpr auto kEventRingKey = "event_ring";
constexpr auto kStartPausedKey = "start_paused";
constexpr auto kAutoRestartFailedJobsKey = "auto_restart_failed_jobs";
constexpr auto kVisualWorkerPoolKey = "visual_worker_pool";

bool fileExists(const QString& path)
{
    return QFileInfo(path).isFile();
}

bool dirExists(const QString& path)
{
    return QFileInfo(path).isDir();
}
} // namespace

CoordinatorController::CoordinatorController(QObject* parent)
    : QObject(parent)
{
    loadSettings();
    updateValidationMessage();
    updateSnapshotCache();
}

CoordinatorController::~CoordinatorController()
{
    if (coordinator_) {
        coordinator_->Stop();
        coordinator_.reset();
    }
}

bool CoordinatorController::isRunning() const { return coordinator_ != nullptr; }
bool CoordinatorController::isPaused() const { return coordinator_ ? coordinator_->IsPaused() : paused_; }
int CoordinatorController::targetWorkers() const { return targetWorkers_; }
int CoordinatorController::activeWorkers() const { return coordinator_ ? static_cast<int>(coordinator_->ActiveWorkerCount()) : 0; }
int CoordinatorController::eventBufferCapacity() const { return eventBufferCapacity_; }
bool CoordinatorController::startPaused() const { return startPaused_; }
bool CoordinatorController::restartFailedJobsAutomatically() const { return restartFailedJobsAutomatically_; }
bool CoordinatorController::visualWorkerPoolEnabled() const { return visualWorkerPoolEnabled_; }
QString CoordinatorController::isoPath() const { return isoPath_; }
QString CoordinatorController::dolphinBaseDir() const { return dolphinBaseDir_; }
QString CoordinatorController::validationMessage() const { return validationMessage_; }
const std::vector<WorkerSnapshot>& CoordinatorController::snapshot() const { return snapshotCache_; }
const std::vector<WorkerSnapshot>& CoordinatorController::visualSnapshot() const { return visualSnapshotCache_; }
QStringList CoordinatorController::takeVisualLiveLogLineUpdates()
{
    if (!coordinator_) {
        return {};
    }
    QStringList lines;
    for (const auto& line : coordinator_->TakeVisualDebugLogLines()) {
        lines.append(QString::fromStdString(line));
    }
    return lines;
}

QString CoordinatorController::visualReplayRuntimeStateText() const
{
    if (!coordinator_) {
        return visualReplayLastError_.isEmpty()
            ? QStringLiteral("Visual replay idle. Start the coordinator before launching debug replay.")
            : visualReplayLastError_;
    }
    const auto snapshot = coordinator_->SnapshotVisualDebugReplay();
    if (!snapshot.active) {
        return visualReplayLastError_.isEmpty()
            ? QStringLiteral("Visual replay idle.")
            : visualReplayLastError_;
    }
    const QString stateText = visualReplayStateToText(snapshot.state);
    const QString detail = QString::fromStdString(snapshot.detail);
    return detail.isEmpty() ? stateText : QStringLiteral("%1: %2").arg(stateText, detail);
}

bool CoordinatorController::visualReplayControlsEnabled() const
{
    return coordinator_ ? coordinator_->SnapshotVisualDebugReplay().controls_enabled : false;
}

void CoordinatorController::startCoordinator()
{
    if (coordinator_) {
        return;
    }

    updateValidationMessage();
    if (!validationMessage_.isEmpty()) {
        emit stateChanged();
        return;
    }

    auto& runtime = savorqt::SavorDbRuntime::instance();
    auto* programRegistry = runtime.programKindRegistry();
    if (programRegistry == nullptr) {
        validationMessage_ = QStringLiteral("Workflow program registry is unavailable.");
        emit stateChanged();
        return;
    }

    auto* executionDb = runtime.executionDb();
    auto* stateDb = runtime.stateDb();

    try {
        auto coordinator = std::make_unique<savor::runner::parallel::savordb::DBWorkflowWorkerCoordinator>(
            executionDb,
            buildWorkerConfig(),
            savor::runner::parallel::savordb::CoordinatorIntegrationConfig{},
            programRegistry,
            savor::runner::parallel::savordb::DBWorkflowWorkerCoordinator::ReadyStepPersistFn{},
            nullptr,
            stateDb);
        if (visualWorkerPoolEnabled_) {
            for (int workerIndex = 0; workerIndex < targetWorkers_; ++workerIndex) {
                const auto it = visualWorkerSurfaces_.find(workerIndex);
                if (it == visualWorkerSurfaces_.end() || it->second.renderWidgetHandle == 0) {
                    validationMessage_ = QStringLiteral("Visual worker dashboard surfaces are not ready.");
                    emit stateChanged();
                    return;
                }
                coordinator->SetWorkerVisualSurface(
                    static_cast<size_t>(workerIndex),
                    static_cast<uint64_t>(it->second.renderWidgetHandle),
                    it->second.hostEventsPipeName.toStdString());
            }
        }
        coordinator->Start();
        coordinator->SetPaused(startPaused_);
        paused_ = startPaused_;
        coordinator_ = std::move(coordinator);
    } catch (const std::exception& ex) {
        validationMessage_ = QStringLiteral("Coordinator startup failed: %1").arg(QString::fromUtf8(ex.what()));
        coordinator_.reset();
        emit stateChanged();
        return;
    } catch (...) {
        validationMessage_ = QStringLiteral("Coordinator startup failed with an unknown exception.");
        coordinator_.reset();
        emit stateChanged();
        return;
    }

    updateSnapshotCache();
    emit stateChanged();
    emit snapshotChanged();
}

void CoordinatorController::stopCoordinator()
{
    if (!coordinator_) {
        return;
    }

    coordinator_->Stop();
    coordinator_.reset();
    paused_ = false;
    updateSnapshotCache();
    emit stateChanged();
    emit snapshotChanged();
}

void CoordinatorController::setPaused(bool paused)
{
    paused_ = paused;
    if (coordinator_) {
        coordinator_->SetPaused(paused_);
        updateSnapshotCache();
        emit snapshotChanged();
    }
    emit stateChanged();
}

void CoordinatorController::togglePaused() { setPaused(!isPaused()); }

void CoordinatorController::setTargetWorkers(int targetWorkers)
{
    const int clampedValue = (std::min)(kMaxTargetWorkers, (std::max)(kMinTargetWorkers, targetWorkers));
    if (targetWorkers_ == clampedValue) {
        return;
    }

    targetWorkers_ = clampedValue;
    persistInt(kTargetWorkersKey, targetWorkers_);
    if (coordinator_) {
        coordinator_->SetDesiredWorkerCount(static_cast<size_t>(targetWorkers_));
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

void CoordinatorController::setRestartFailedJobsAutomatically(bool enabled)
{
    if (restartFailedJobsAutomatically_ == enabled) {
        return;
    }
    restartFailedJobsAutomatically_ = enabled;
    persistInt(kAutoRestartFailedJobsKey, restartFailedJobsAutomatically_ ? 1 : 0);
    emit stateChanged();
}

void CoordinatorController::setVisualWorkerPoolEnabled(bool enabled)
{
    if (visualWorkerPoolEnabled_ == enabled) {
        return;
    }
    if (coordinator_) {
        return;
    }
    visualWorkerPoolEnabled_ = enabled;
    persistInt(kVisualWorkerPoolKey, visualWorkerPoolEnabled_ ? 1 : 0);
    emit stateChanged();
}

void CoordinatorController::setVisualWorkerSurface(int workerIndex, quintptr hwnd, const QString& hostEventsPipeName)
{
    if (workerIndex < 0) {
        return;
    }
    visualWorkerSurfaces_[workerIndex] = VisualWorkerSurface{
        .renderWidgetHandle = hwnd,
        .hostEventsPipeName = hostEventsPipeName,
    };
    if (coordinator_) {
        coordinator_->SetWorkerVisualSurface(
            static_cast<size_t>(workerIndex),
            static_cast<uint64_t>(hwnd),
            hostEventsPipeName.toStdString());
    }
}

void CoordinatorController::clearVisualWorkerSurfaces()
{
    visualWorkerSurfaces_.clear();
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
    visualRenderWidgetHandle_ = hwnd;
}

void CoordinatorController::setVisualHostEventsPipeName(const QString& pipeName)
{
    visualHostEventsPipeName_ = pipeName;
}

void CoordinatorController::requestVisualReplay(qint64 jobId)
{
    visualReplayLastError_.clear();
    if (jobId <= 0) {
        visualReplayLastError_ = QStringLiteral("Visual replay requires a valid job id.");
        emit stateChanged();
        return;
    }
    if (!coordinator_) {
        visualReplayLastError_ = QStringLiteral("Start the DB workflow coordinator before launching visual debug replay.");
        emit stateChanged();
        return;
    }

    std::string error;
    if (!coordinator_->StartVisualDebugReplay(
        static_cast<std::int64_t>(jobId),
        static_cast<uint64_t>(visualRenderWidgetHandle_),
        visualHostEventsPipeName_.toStdString(),
        &error)) {
        visualReplayLastError_ = QStringLiteral("Visual replay failed to start: %1").arg(QString::fromStdString(error));
    }
    emit stateChanged();
}

void CoordinatorController::pauseVisualReplayEmulation()
{
    if (coordinator_) {
        (void)coordinator_->PauseVisualDebugReplayEmulation();
    }
}

void CoordinatorController::stepVisualReplayVm()
{
    if (coordinator_) {
        (void)coordinator_->StepVisualDebugReplayVm();
    }
}

void CoordinatorController::resumeVisualReplayEmulation()
{
    if (coordinator_) {
        (void)coordinator_->ResumeVisualDebugReplayEmulation();
    }
}

void CoordinatorController::stopVisualReplay()
{
    if (coordinator_) {
        (void)coordinator_->StopVisualDebugReplay();
    }
    visualReplayLastError_.clear();
    emit stateChanged();
}

void CoordinatorController::handleVisualLiveLogLinesRequested()
{
    const QStringList lines = takeVisualLiveLogLineUpdates();
    if (!lines.isEmpty()) {
        emit visualLiveLogLinesReady(lines);
    }
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
    targetWorkers_ = (std::min)(kMaxTargetWorkers, (std::max)(kMinTargetWorkers, settings.value(kTargetWorkersKey, targetWorkers_).toInt()));
    eventBufferCapacity_ = (std::max)(kMinEventBufferCapacity, settings.value(kEventRingKey, eventBufferCapacity_).toInt());
    startPaused_ = settings.value(kStartPausedKey, startPaused_ ? 1 : 0).toInt() != 0;
    restartFailedJobsAutomatically_ = settings.value(kAutoRestartFailedJobsKey, restartFailedJobsAutomatically_ ? 1 : 0).toInt() != 0;
    visualWorkerPoolEnabled_ = settings.value(kVisualWorkerPoolKey, visualWorkerPoolEnabled_ ? 1 : 0).toInt() != 0;
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
    const QString trimmedIso = isoPath_.trimmed();
    const QString trimmedDolphin = dolphinBaseDir_.trimmed();

    if (!savorqt::SavorDbRuntime::instance().isRunning()) {
        issues.append(QStringLiteral("SavorDb runtime is not running."));
    } else if (savorqt::SavorDbRuntime::instance().executionDb() == nullptr
        || savorqt::SavorDbRuntime::instance().stateDb() == nullptr
        || savorqt::SavorDbRuntime::instance().analysisDb() == nullptr
        || savorqt::SavorDbRuntime::instance().authoringDb() == nullptr) {
        issues.append(QStringLiteral("SavorDb services are incomplete."));
    }

    if (trimmedIso.isEmpty()) {
        issues.append(QStringLiteral("ISO path is required."));
    } else if (!fileExists(trimmedIso)) {
        issues.append(QStringLiteral("ISO path does not point to a file."));
    }

    if (trimmedDolphin.isEmpty()) {
        issues.append(QStringLiteral("Dolphin base directory is required."));
    } else if (!dirExists(trimmedDolphin)) {
        issues.append(QStringLiteral("Dolphin base directory does not exist."));
    } else {
        const QDir dolphinDir(trimmedDolphin);
        if (!fileExists(dolphinDir.filePath(QStringLiteral("portable.txt")))) {
            issues.append(QStringLiteral("Dolphin base directory must contain portable.txt."));
        }
        if (!fileExists(dolphinDir.filePath(QStringLiteral("Sys/GC/dsp_coef.bin")))) {
            issues.append(QStringLiteral("Dolphin base directory is missing Sys/GC/dsp_coef.bin."));
        }
    }

    if (!fileExists(workerExePath())) {
        issues.append(QStringLiteral("SavorWorker.exe was not found next to SavorQt."));
    }

    validationMessage_ = issues.join(' ');
}

void CoordinatorController::updateSnapshotCache()
{
    if (!coordinator_) {
        snapshotCache_.clear();
        visualSnapshotCache_.clear();
        statusSnapshot_ = {};
        telemetrySnapshot_ = savorqt::SavorDbRuntime::instance().workflowCoordinatorTelemetry();
        return;
    }

    snapshotCache_ = coordinator_->SnapshotWorkers();
    visualSnapshotCache_.clear();
    statusSnapshot_ = coordinator_->SnapshotStatus();
    telemetrySnapshot_ = savorqt::SavorDbRuntime::instance().workflowCoordinatorTelemetry();
}

savor::runner::parallel::savordb::DBWorkflowWorkerCoordinatorConfig CoordinatorController::buildWorkerConfig() const
{
    savor::runner::parallel::savordb::DBWorkflowWorkerCoordinatorConfig cfg{};
    cfg.desired_workers = static_cast<size_t>((std::max)(kMinTargetWorkers, targetWorkers_));
    cfg.worker_exe_path = workerExePath().toStdString();
    cfg.iso_path = isoPath_.trimmed().toStdString();
    cfg.dolphin_base_dir = dolphinBaseDir_.trimmed().toStdString();
    cfg.worker_dir_root = workerRootPath().toStdString();
    cfg.visual_workers = visualWorkerPoolEnabled_;
    cfg.auto_resume_visual_workers = visualWorkerPoolEnabled_;
    return cfg;
}

void CoordinatorController::applyVisualWorkerSurfaces()
{
    if (!coordinator_) {
        return;
    }
    for (const auto& [workerIndex, surface] : visualWorkerSurfaces_) {
        if (workerIndex < 0 || surface.renderWidgetHandle == 0) {
            continue;
        }
        coordinator_->SetWorkerVisualSurface(
            static_cast<size_t>(workerIndex),
            static_cast<uint64_t>(surface.renderWidgetHandle),
            surface.hostEventsPipeName.toStdString());
    }
}

QString CoordinatorController::visualReplayStateToText(
    savor::runner::parallel::savordb::VisualReplayRuntimeState state) const
{
    using savor::runner::parallel::savordb::VisualReplayRuntimeState;
    switch (state) {
    case VisualReplayRuntimeState::Idle: return QStringLiteral("Idle");
    case VisualReplayRuntimeState::QueuedStartup: return QStringLiteral("Queued startup");
    case VisualReplayRuntimeState::LaunchingWorker: return QStringLiteral("Launching worker");
    case VisualReplayRuntimeState::AttachReady: return QStringLiteral("Attach ready");
    case VisualReplayRuntimeState::Active: return QStringLiteral("Active");
    case VisualReplayRuntimeState::Stopping: return QStringLiteral("Stopping");
    case VisualReplayRuntimeState::Finished: return QStringLiteral("Finished");
    case VisualReplayRuntimeState::Failed: return QStringLiteral("Failed");
    default: return QStringLiteral("Unknown");
    }
}

QString CoordinatorController::workerExePath() const
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("SavorWorker.exe"));
}

QString CoordinatorController::workerRootPath() const
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral(".workflow-workers"));
}
