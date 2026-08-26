#include "CoordinatorController.h"

#include "SavorDbRuntime.h"
#include "Phases/Programs/SeedProbe/SeedProbeModule.h"
#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtCore/QStringList>

#include <algorithm>
#include <atomic>
#include <exception>
#include <filesystem>
#include <mutex>
#include <optional>
#include <utility>

struct CoordinatorStartupSharedState {
    std::mutex mutex;
    std::atomic_bool cancel_requested{false};
    std::uint64_t generation = 0;
    bool initially_paused = true;
    std::unique_ptr<
        savor::runner::parallel::savordb::CoordinatorRuntime>
        runtime;
};

struct CoordinatorShutdownSharedState {
    std::mutex mutex;
    std::uint64_t generation = 0;
    std::unique_ptr<
        savor::runner::parallel::savordb::CoordinatorRuntime>
        runtime;
};

namespace {
constexpr auto kSettingsGroup = "Coordinator";
constexpr auto kIsoPathKey = "iso_path";
constexpr auto kDolphinBaseKey = "dolphin_base";
constexpr auto kTargetWorkersKey = "target_workers";
constexpr auto kStartPausedKey = "start_paused";
constexpr auto kVisualWorkerPoolKey = "visual_worker_pool";

bool fileExists(const QString& path)
{
    return QFileInfo(path).isFile();
}

bool dirExists(const QString& path)
{
    return QFileInfo(path).isDir();
}

std::optional<std::string> sha256File(
    const QString& path,
    QString* errorOut,
    const std::atomic_bool* cancelRequested)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorOut != nullptr) {
            *errorOut = file.errorString();
        }
        return std::nullopt;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    constexpr qint64 kChunkBytes = 4 * 1024 * 1024;
    while (!file.atEnd()) {
        if (cancelRequested != nullptr
            && cancelRequested->load(std::memory_order_acquire)) {
            if (errorOut != nullptr) {
                errorOut->clear();
            }
            return std::nullopt;
        }
        const QByteArray chunk = file.read(kChunkBytes);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
            if (errorOut != nullptr) {
                *errorOut = file.errorString();
            }
            return std::nullopt;
        }
        hash.addData(chunk);
    }

    if (errorOut != nullptr) {
        errorOut->clear();
    }
    return hash.result().toHex().toStdString();
}
} // namespace

CoordinatorController::CoordinatorController(QObject* parent)
    : QObject(parent)
{
    connect(
        &startup_watcher_,
        &QFutureWatcher<CoordinatorStartupResult>::finished,
        this,
        &CoordinatorController::handleStartupFinished);
    connect(
        &startup_cleanup_watcher_,
        &QFutureWatcher<void>::finished,
        this,
        &CoordinatorController::handleStartupCleanupFinished);
    connect(
        &shutdown_watcher_,
        &QFutureWatcher<CoordinatorShutdownResult>::finished,
        this,
        &CoordinatorController::handleShutdownFinished);
    loadSettings();
    updateValidationMessage();
    updateSnapshotCache();
}

CoordinatorController::~CoordinatorController()
{
    stopCoordinator();
    waitForShutdown();
}

CoordinatorLifecycleState CoordinatorController::lifecycleState() const
{
    return lifecycle_state_;
}

bool CoordinatorController::isStopped() const
{
    return lifecycle_state_ == CoordinatorLifecycleState::Stopped;
}

bool CoordinatorController::isTransitioning() const
{
    return lifecycle_state_ == CoordinatorLifecycleState::Starting
        || lifecycle_state_ == CoordinatorLifecycleState::Stopping;
}

bool CoordinatorController::isRunning() const
{
    return lifecycle_state_ == CoordinatorLifecycleState::Running
        && coordinator_runtime_ != nullptr
        && coordinator_runtime_->IsStarted();
}
bool CoordinatorController::isPaused() const
{
    return coordinator_runtime_
        ? coordinator_runtime_->IsExecutionPaused()
        : paused_;
}
int CoordinatorController::targetWorkers() const { return targetWorkers_; }
int CoordinatorController::activeWorkers() const
{
    return coordinator_runtime_
        ? static_cast<int>(coordinator_runtime_->SnapshotWorkers().size())
        : static_cast<int>(snapshotCache_.size());
}
bool CoordinatorController::startPaused() const { return startPaused_; }
bool CoordinatorController::visualWorkerPoolEnabled() const { return visualWorkerPoolEnabled_; }
QString CoordinatorController::isoPath() const { return isoPath_; }
QString CoordinatorController::dolphinBaseDir() const { return dolphinBaseDir_; }
QString CoordinatorController::validationMessage() const { return validationMessage_; }
QString CoordinatorController::resultStagingCleanupError() const { return resultStagingCleanupError_; }
const std::vector<WorkerSnapshot>& CoordinatorController::snapshot() const { return snapshotCache_; }
std::vector<WorkerSnapshot> CoordinatorController::freshSnapshot() const
{
    return coordinator_runtime_
        ? coordinator_runtime_->SnapshotWorkers()
        : snapshotCache_;
}
const std::vector<WorkerSnapshot>& CoordinatorController::visualSnapshot() const { return visualSnapshotCache_; }
const std::vector<
    savor::runner::parallel::savordb::JobExecutionCoordinatorWarning>&
CoordinatorController::warningSnapshot() const
{
    return warningSnapshotCache_;
}
QStringList CoordinatorController::takeVisualLiveLogLineUpdates()
{
    return {};
}

QString CoordinatorController::visualReplayRuntimeStateText() const
{
    return visualReplayLastError_.isEmpty()
        ? QStringLiteral(
              "Visual replay is unavailable during the coordinator "
              "architecture cutover.")
        : visualReplayLastError_;
}

bool CoordinatorController::visualReplayControlsEnabled() const
{
    return false;
}

void CoordinatorController::startCoordinator()
{
    if (!isStopped() || coordinator_runtime_ || startup_state_
        || shutdown_state_ || shutdown_watcher_.isRunning()) {
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
    auto* authoringDb = runtime.authoringDb();
    if (executionDb == nullptr || authoringDb == nullptr) {
        validationMessage_ = QStringLiteral(
            "Coordinator database services are unavailable.");
        emit stateChanged();
        return;
    }

    try {
        std::vector<savor::runner::parallel::savordb::
            CoordinatorWorkerVisualSurface> visualSurfaces;
        if (visualWorkerPoolEnabled_) {
            visualSurfaces.reserve(
                static_cast<std::size_t>(targetWorkers_));
            for (int workerIndex = 0; workerIndex < targetWorkers_; ++workerIndex) {
                const auto it = visualWorkerSurfaces_.find(workerIndex);
                if (it == visualWorkerSurfaces_.end() || it->second.renderWidgetHandle == 0) {
                    validationMessage_ = QStringLiteral("Visual worker dashboard surfaces are not ready.");
                    emit stateChanged();
                    return;
                }
                visualSurfaces.push_back({
                    .worker_id = static_cast<std::size_t>(workerIndex),
                    .render_widget_handle = static_cast<std::uint64_t>(
                        it->second.renderWidgetHandle),
                    .host_events_pipe_name =
                        it->second.hostEventsPipeName.toStdString(),
                });
            }
        }
        auto startupState =
            std::make_shared<CoordinatorStartupSharedState>();
        startupState->generation = ++startup_generation_;
        startupState->initially_paused = startPaused_;
        startup_state_ = startupState;

        auto workerConfig = buildWorkerConfig();
        const QString isoPath = isoPath_.trimmed();
        const auto objectStoreRoot = runtime.root() / "object_store";
        const bool initiallyPaused = startPaused_;
        const std::uint64_t generation = startupState->generation;

        lifecycle_state_ = CoordinatorLifecycleState::Starting;
        validationMessage_.clear();
        updateSnapshotCache();
        emit stateChanged();
        emit snapshotChanged();

        startup_watcher_.setFuture(QtConcurrent::run(
            [startupState,
             generation,
             isoPath,
             executionDb,
             authoringDb,
             programRegistry,
             workerConfig = std::move(workerConfig),
             visualSurfaces = std::move(visualSurfaces),
             objectStoreRoot,
             initiallyPaused]() mutable -> CoordinatorStartupResult {
                const auto canceled = [&startupState]() {
                    return startupState->cancel_requested.load(
                        std::memory_order_acquire);
                };
                if (canceled()) {
                    return {generation, CoordinatorStartupDisposition::Canceled, {}};
                }

                QString isoHashError;
                const auto isoSha256 = sha256File(
                    isoPath,
                    &isoHashError,
                    &startupState->cancel_requested);
                if (!isoSha256.has_value()) {
                    if (canceled()) {
                        return {generation, CoordinatorStartupDisposition::Canceled, {}};
                    }
                    return {
                        generation,
                        CoordinatorStartupDisposition::Failed,
                        QStringLiteral("Failed to hash the configured ISO: %1")
                            .arg(isoHashError),
                    };
                }
                if (canceled()) {
                    return {generation, CoordinatorStartupDisposition::Canceled, {}};
                }

                try {
                    auto coordinatorRuntime = std::make_unique<
                        savor::runner::parallel::savordb::CoordinatorRuntime>();
                    savor::runner::parallel::savordb::CoordinatorRuntimeConfig
                        coordinatorConfig{
                            .worker = std::move(workerConfig),
                            .state_compatibility = {
                                .game_id = std::string(
                                    savor::runtime::program::capabilities::
                                        kSupportedGameId),
                                .iso_sha256 = *isoSha256,
                                .emulator_build = "dolphin-2506a",
                                .runtime_revision = "worker-runtime-slice4",
                            },
                            .initially_paused = initiallyPaused,
                            .object_store_root = objectStoreRoot,
                            .event_line_callback = [](const std::string&) {},
                            .visual_surfaces = std::move(visualSurfaces),
                        };
                    std::string startupError;
                    if (!coordinatorRuntime->Start(
                            executionDb,
                            authoringDb,
                            programRegistry,
                            std::move(coordinatorConfig),
                            &startupError)) {
                        return {
                            generation,
                            CoordinatorStartupDisposition::Failed,
                            QStringLiteral("Coordinator startup failed: %1")
                                .arg(QString::fromStdString(startupError)),
                        };
                    }
                    if (canceled()) {
                        (void)coordinatorRuntime->Stop(nullptr);
                        return {generation, CoordinatorStartupDisposition::Canceled, {}};
                    }

                    {
                        std::lock_guard lock(startupState->mutex);
                        if (!startupState->cancel_requested.load(
                                std::memory_order_acquire)) {
                            startupState->runtime =
                                std::move(coordinatorRuntime);
                        }
                    }
                    if (coordinatorRuntime) {
                        (void)coordinatorRuntime->Stop(nullptr);
                        return {generation, CoordinatorStartupDisposition::Canceled, {}};
                    }
                    return {generation, CoordinatorStartupDisposition::Started, {}};
                } catch (const std::exception& ex) {
                    return {
                        generation,
                        CoordinatorStartupDisposition::Failed,
                        QStringLiteral("Coordinator startup failed: %1")
                            .arg(QString::fromUtf8(ex.what())),
                    };
                } catch (...) {
                    return {
                        generation,
                        CoordinatorStartupDisposition::Failed,
                        QStringLiteral("Coordinator startup failed with an unknown exception."),
                    };
                }
            }));
    } catch (const std::exception& ex) {
        validationMessage_ = QStringLiteral("Coordinator startup failed: %1").arg(QString::fromUtf8(ex.what()));
        startup_state_.reset();
        lifecycle_state_ = CoordinatorLifecycleState::Stopped;
        emit stateChanged();
        return;
    } catch (...) {
        validationMessage_ = QStringLiteral("Coordinator startup failed with an unknown exception.");
        startup_state_.reset();
        lifecycle_state_ = CoordinatorLifecycleState::Stopped;
        emit stateChanged();
        return;
    }
}

void CoordinatorController::stopCoordinator()
{
    if (lifecycle_state_ == CoordinatorLifecycleState::Starting
        && startup_state_) {
        startup_state_->cancel_requested.store(
            true,
            std::memory_order_release);
        lifecycle_state_ = CoordinatorLifecycleState::Stopping;
        emit stateChanged();
        return;
    }
    if (lifecycle_state_ == CoordinatorLifecycleState::Stopping) {
        return;
    }
    if (!coordinator_runtime_) {
        lifecycle_state_ = CoordinatorLifecycleState::Stopped;
        return;
    }

    paused_ = coordinator_runtime_->IsExecutionPaused();
    updateSnapshotCache();
    auto shutdownState =
        std::make_shared<CoordinatorShutdownSharedState>();
    shutdownState->generation = ++shutdown_generation_;
    shutdownState->runtime = std::move(coordinator_runtime_);
    shutdown_state_ = shutdownState;
    lifecycle_state_ = CoordinatorLifecycleState::Stopping;
    emit stateChanged();
    emit snapshotChanged();
    startRuntimeShutdown();
}

void CoordinatorController::setPaused(bool paused)
{
    if (!isRunning()) {
        return;
    }
    paused_ = paused;
    if (coordinator_runtime_) {
        coordinator_runtime_->SetExecutionPaused(paused_);
        updateSnapshotCache();
        emit snapshotChanged();
    }
    emit stateChanged();
}

void CoordinatorController::togglePaused() { setPaused(!isPaused()); }

void CoordinatorController::setTargetWorkers(int targetWorkers)
{
    if (isTransitioning()) {
        return;
    }
    const int clampedValue = (std::min)(kMaxTargetWorkers, (std::max)(kMinTargetWorkers, targetWorkers));
    if (targetWorkers_ == clampedValue) {
        return;
    }

    targetWorkers_ = clampedValue;
    persistInt(kTargetWorkersKey, targetWorkers_);
    if (coordinator_runtime_) {
        coordinator_runtime_->SetDesiredWorkerCount(
            static_cast<size_t>(targetWorkers_));
    }
    emit stateChanged();
}

void CoordinatorController::setStartPaused(bool startPaused)
{
    if (!isStopped()) {
        return;
    }
    if (startPaused_ == startPaused) {
        return;
    }
    startPaused_ = startPaused;
    persistInt(kStartPausedKey, startPaused_ ? 1 : 0);
    emit stateChanged();
}

void CoordinatorController::setVisualWorkerPoolEnabled(bool enabled)
{
    if (visualWorkerPoolEnabled_ == enabled) {
        return;
    }
    if (!isStopped()) {
        return;
    }
    visualWorkerPoolEnabled_ = enabled;
    persistInt(kVisualWorkerPoolKey, visualWorkerPoolEnabled_ ? 1 : 0);
    emit stateChanged();
}

void CoordinatorController::setVisualWorkerSurface(int workerIndex, quintptr hwnd, const QString& hostEventsPipeName)
{
    if (!isStopped() || workerIndex < 0) {
        return;
    }
    visualWorkerSurfaces_[workerIndex] = VisualWorkerSurface{
        .renderWidgetHandle = hwnd,
        .hostEventsPipeName = hostEventsPipeName,
    };
}

void CoordinatorController::clearVisualWorkerSurfaces()
{
    if (!isStopped()) {
        return;
    }
    visualWorkerSurfaces_.clear();
}

void CoordinatorController::setIsoPath(const QString& isoPath)
{
    if (!isStopped() || isoPath_ == isoPath) {
        return;
    }
    isoPath_ = isoPath;
    persistString(kIsoPathKey, isoPath_);
    updateValidationMessage();
    emit stateChanged();
}

void CoordinatorController::setDolphinBaseDir(const QString& dolphinBaseDir)
{
    if (!isStopped() || dolphinBaseDir_ == dolphinBaseDir) {
        return;
    }
    dolphinBaseDir_ = dolphinBaseDir;
    persistString(kDolphinBaseKey, dolphinBaseDir_);
    updateValidationMessage();
    emit stateChanged();
}

void CoordinatorController::setVisualRenderWidgetHandle(quintptr hwnd)
{
    if (!isStopped()) {
        return;
    }
    visualRenderWidgetHandle_ = hwnd;
}

void CoordinatorController::setVisualHostEventsPipeName(const QString& pipeName)
{
    if (!isStopped()) {
        return;
    }
    visualHostEventsPipeName_ = pipeName;
}

void CoordinatorController::requestVisualReplay(qint64 jobId)
{
    if (jobId <= 0) {
        visualReplayLastError_ = QStringLiteral("Visual replay requires a valid job id.");
    } else {
        visualReplayLastError_ = QStringLiteral(
            "Visual replay is not part of the new worker/job execution "
            "coordinator boundary.");
    }
    emit stateChanged();
}

void CoordinatorController::pauseVisualReplayEmulation()
{
}

void CoordinatorController::stepVisualReplayVm()
{
}

void CoordinatorController::resumeVisualReplayEmulation()
{
}

void CoordinatorController::stopVisualReplay()
{
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
    startPaused_ = settings.value(kStartPausedKey, startPaused_ ? 1 : 0).toInt() != 0;
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
    if (!coordinator_runtime_) {
        if (lifecycle_state_ == CoordinatorLifecycleState::Stopping
            && shutdown_state_) {
            return;
        }
        snapshotCache_.clear();
        visualSnapshotCache_.clear();
        warningSnapshotCache_.clear();
        resultStagingCleanupError_.clear();
        return;
    }

    snapshotCache_ = coordinator_runtime_->SnapshotWorkers();
    visualSnapshotCache_.clear();
    warningSnapshotCache_ =
        coordinator_runtime_->SnapshotExecutionWarnings();
    const auto telemetry = coordinator_runtime_->SnapshotTelemetry();
    resultStagingCleanupError_ = QString::fromStdString(
        telemetry.result_staging_cleanup.last_blocked_error);
}

void CoordinatorController::handleStartupFinished()
{
    CoordinatorStartupResult result;
    try {
        result = startup_watcher_.result();
    } catch (const std::exception& ex) {
        result = {
            startup_generation_,
            CoordinatorStartupDisposition::Failed,
            QStringLiteral("Coordinator startup failed: %1")
                .arg(QString::fromUtf8(ex.what())),
        };
    } catch (...) {
        result = {
            startup_generation_,
            CoordinatorStartupDisposition::Failed,
            QStringLiteral("Coordinator startup failed with an unknown exception."),
        };
    }

    const bool currentGeneration = startup_state_
        && result.generation == startup_generation_
        && result.generation == startup_state_->generation;
    const bool canceled = !currentGeneration
        || startup_state_->cancel_requested.load(std::memory_order_acquire)
        || lifecycle_state_ == CoordinatorLifecycleState::Stopping
        || result.disposition == CoordinatorStartupDisposition::Canceled;

    if (result.disposition == CoordinatorStartupDisposition::Started
        && !canceled) {
        std::unique_ptr<
            savor::runner::parallel::savordb::CoordinatorRuntime>
            startedRuntime;
        {
            std::lock_guard lock(startup_state_->mutex);
            startedRuntime = std::move(startup_state_->runtime);
        }
        if (startedRuntime) {
            paused_ = startup_state_->initially_paused;
            coordinator_runtime_ = std::move(startedRuntime);
            startup_state_.reset();
            lifecycle_state_ = CoordinatorLifecycleState::Running;
            updateSnapshotCache();
            emit stateChanged();
            emit snapshotChanged();
            return;
        }
        result.disposition = CoordinatorStartupDisposition::Failed;
        result.error = QStringLiteral(
            "Coordinator startup completed without publishing its runtime.");
    }

    if (startup_state_) {
        bool hasRuntime = false;
        {
            std::lock_guard lock(startup_state_->mutex);
            hasRuntime = startup_state_->runtime != nullptr;
        }
        if (hasRuntime) {
            lifecycle_state_ = CoordinatorLifecycleState::Stopping;
            startStartupCleanup();
            emit stateChanged();
            return;
        }
    }

    startup_state_.reset();
    paused_ = false;
    lifecycle_state_ = CoordinatorLifecycleState::Stopped;
    if (!canceled && !result.error.isEmpty()) {
        validationMessage_ = result.error;
    }
    updateSnapshotCache();
    emit stateChanged();
    emit snapshotChanged();
}

void CoordinatorController::startStartupCleanup()
{
    const auto state = startup_state_;
    if (!state || startup_cleanup_watcher_.isRunning()) {
        return;
    }
    startup_cleanup_watcher_.setFuture(QtConcurrent::run([state]() {
        std::unique_ptr<
            savor::runner::parallel::savordb::CoordinatorRuntime>
            runtime;
        {
            std::lock_guard lock(state->mutex);
            runtime = std::move(state->runtime);
        }
        if (runtime) {
            (void)runtime->Stop(nullptr);
        }
    }));
}

void CoordinatorController::handleStartupCleanupFinished()
{
    startup_state_.reset();
    paused_ = false;
    lifecycle_state_ = CoordinatorLifecycleState::Stopped;
    updateSnapshotCache();
    emit stateChanged();
    emit snapshotChanged();
}

void CoordinatorController::startRuntimeShutdown()
{
    const auto state = shutdown_state_;
    if (!state || shutdown_watcher_.isRunning()) {
        return;
    }
    shutdown_watcher_.setFuture(QtConcurrent::run(
        [state]() -> CoordinatorShutdownResult {
            std::unique_ptr<
                savor::runner::parallel::savordb::CoordinatorRuntime>
                runtime;
            {
                std::lock_guard lock(state->mutex);
                runtime = std::move(state->runtime);
            }
            std::string shutdownError;
            try {
                if (runtime) {
                    (void)runtime->Stop(&shutdownError);
                }
            } catch (const std::exception& ex) {
                shutdownError = ex.what();
            } catch (...) {
                shutdownError =
                    "coordinator shutdown failed with an unknown exception";
            }
            runtime.reset();
            return {
                state->generation,
                QString::fromStdString(shutdownError),
            };
        }));
}

void CoordinatorController::handleShutdownFinished()
{
    CoordinatorShutdownResult result;
    try {
        result = shutdown_watcher_.result();
    } catch (const std::exception& ex) {
        result = {
            shutdown_generation_,
            QString::fromUtf8(ex.what()),
        };
    } catch (...) {
        result = {
            shutdown_generation_,
            QStringLiteral(
                "coordinator shutdown failed with an unknown exception"),
        };
    }

    if (!shutdown_state_
        || result.generation != shutdown_generation_
        || result.generation != shutdown_state_->generation) {
        return;
    }

    shutdown_state_.reset();
    paused_ = false;
    lifecycle_state_ = CoordinatorLifecycleState::Stopped;
    if (!result.warning.isEmpty()) {
        validationMessage_ =
            QStringLiteral("Coordinator shutdown warning: %1")
                .arg(result.warning);
    }
    updateSnapshotCache();
    emit stateChanged();
    emit snapshotChanged();
}

void CoordinatorController::waitForShutdown()
{
    if (startup_state_) {
        startup_state_->cancel_requested.store(
            true,
            std::memory_order_release);
    }
    if (startup_watcher_.isRunning()) {
        startup_watcher_.waitForFinished();
    }
    if (startup_cleanup_watcher_.isRunning()) {
        startup_cleanup_watcher_.waitForFinished();
    }
    if (shutdown_watcher_.isRunning()) {
        shutdown_watcher_.waitForFinished();
    }
    if (startup_state_) {
        std::unique_ptr<
            savor::runner::parallel::savordb::CoordinatorRuntime>
            runtime;
        {
            std::lock_guard lock(startup_state_->mutex);
            runtime = std::move(startup_state_->runtime);
        }
        if (runtime) {
            (void)runtime->Stop(nullptr);
        }
        startup_state_.reset();
    }
    if (shutdown_state_) {
        std::unique_ptr<
            savor::runner::parallel::savordb::CoordinatorRuntime>
            runtime;
        {
            std::lock_guard lock(shutdown_state_->mutex);
            runtime = std::move(shutdown_state_->runtime);
        }
        if (runtime) {
            (void)runtime->Stop(nullptr);
        }
        shutdown_state_.reset();
    }
    stopCoordinatorServices();
    paused_ = false;
    lifecycle_state_ = CoordinatorLifecycleState::Stopped;
}

savor::runner::parallel::savordb::WorkerCoordinatorConfig
CoordinatorController::buildWorkerConfig() const
{
    savor::runner::parallel::savordb::WorkerCoordinatorConfig cfg{};
    cfg.desired_workers = static_cast<size_t>((std::max)(kMinTargetWorkers, targetWorkers_));
    cfg.worker_exe_path = workerExePath().toStdString();
    cfg.iso_path = isoPath_.trimmed().toStdString();
    cfg.dolphin_base_dir = dolphinBaseDir_.trimmed().toStdString();
    cfg.worker_dir_root = workerRootPath().toStdString();
    cfg.worker_mode = visualWorkerPoolEnabled_
        ? savor::runtime::WorkerMode::Visual
        : savor::runtime::WorkerMode::Headless;
    cfg.breakpoint_diagnostics =
        QCoreApplication::arguments().contains(
            QStringLiteral("--breakpoint-diagnostics"));
    return cfg;
}

void CoordinatorController::stopCoordinatorServices()
{
    std::string shutdownError;
    if (coordinator_runtime_) {
        (void)coordinator_runtime_->Stop(&shutdownError);
        coordinator_runtime_.reset();
    }

    if (!shutdownError.empty()) {
        validationMessage_ =
            QStringLiteral("Coordinator shutdown warning: %1")
                .arg(QString::fromStdString(shutdownError));
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
