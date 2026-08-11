#include "CoordinatorController.h"

#include "SavorDbRuntime.h"
#include "Phases/Programs/SeedProbe/SeedProbeModule.h"
#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtCore/QStringList>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <optional>
#include <utility>

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
    QString* errorOut)
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
    loadSettings();
    updateValidationMessage();
    updateSnapshotCache();
}

CoordinatorController::~CoordinatorController()
{
    stopCoordinatorServices();
}

bool CoordinatorController::isRunning() const
{
    return worker_coordinator_ != nullptr
        && job_execution_coordinator_ != nullptr
        && worker_coordinator_->IsStarted()
        && job_execution_coordinator_->IsRunning();
}
bool CoordinatorController::isPaused() const
{
    return job_execution_coordinator_
        ? job_execution_coordinator_->IsPaused()
        : paused_;
}
int CoordinatorController::targetWorkers() const { return targetWorkers_; }
int CoordinatorController::activeWorkers() const
{
    return worker_coordinator_
        ? static_cast<int>(worker_coordinator_->SnapshotWorkers().size())
        : 0;
}
bool CoordinatorController::startPaused() const { return startPaused_; }
bool CoordinatorController::visualWorkerPoolEnabled() const { return visualWorkerPoolEnabled_; }
QString CoordinatorController::isoPath() const { return isoPath_; }
QString CoordinatorController::dolphinBaseDir() const { return dolphinBaseDir_; }
QString CoordinatorController::validationMessage() const { return validationMessage_; }
const std::vector<WorkerSnapshot>& CoordinatorController::snapshot() const { return snapshotCache_; }
std::vector<WorkerSnapshot> CoordinatorController::freshSnapshot() const
{
    return worker_coordinator_
        ? worker_coordinator_->SnapshotWorkers()
        : std::vector<WorkerSnapshot>{};
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
    if (worker_coordinator_ || job_execution_coordinator_) {
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
    auto* resultBlobStore = runtime.workerResultBlobStore();
    if (executionDb == nullptr || resultBlobStore == nullptr
        || !runtime.workflowCoordinatorRunning()
        || !runtime.programResultProcessorRunning()) {
        validationMessage_ = QStringLiteral(
            "Execution scheduling services are unavailable.");
        emit stateChanged();
        return;
    }

    QString isoHashError;
    const auto isoSha256 =
        sha256File(isoPath_.trimmed(), &isoHashError);
    if (!isoSha256.has_value()) {
        validationMessage_ =
            QStringLiteral("Failed to hash the configured ISO: %1")
                .arg(isoHashError);
        emit stateChanged();
        return;
    }

    try {
        auto workerCoordinator = std::make_unique<
            savor::runner::parallel::savordb::WorkerCoordinator>(
                buildWorkerConfig());
        if (visualWorkerPoolEnabled_) {
            for (int workerIndex = 0; workerIndex < targetWorkers_; ++workerIndex) {
                const auto it = visualWorkerSurfaces_.find(workerIndex);
                if (it == visualWorkerSurfaces_.end() || it->second.renderWidgetHandle == 0) {
                    validationMessage_ = QStringLiteral("Visual worker dashboard surfaces are not ready.");
                    emit stateChanged();
                    return;
                }
                workerCoordinator->SetWorkerVisualSurface(
                    static_cast<size_t>(workerIndex),
                    static_cast<uint64_t>(it->second.renderWidgetHandle),
                    it->second.hostEventsPipeName.toStdString());
            }
        }
        const auto startResult = workerCoordinator->Start();
        if (!startResult) {
            validationMessage_ =
                QStringLiteral("Worker coordinator startup failed: %1")
                    .arg(QString::fromStdString(
                        startResult.diagnostic));
            workerCoordinator->Stop();
            emit stateChanged();
            return;
        }
        workerCoordinator->SetPaused(startPaused_);

        savor::runner::parallel::savordb::
            JobExecutionCoordinatorConfig executionConfig{};
        executionConfig.state_compatibility = {
            .game_id = std::string(
                savor::runtime::program::capabilities::
                    kSupportedGameId),
            .iso_sha256 = *isoSha256,
            .emulator_build = "dolphin-2506a",
            .runtime_revision = "worker-runtime-slice4",
        };

        auto jobExecutionCoordinator = std::make_unique<
            savor::runner::parallel::savordb::JobExecutionCoordinator>(
                executionDb,
                programRegistry,
                workerCoordinator.get(),
                resultBlobStore,
                std::move(executionConfig));
        jobExecutionCoordinator->SetPaused(startPaused_);
        std::string execution_start_error;
        if (!jobExecutionCoordinator->Start(
                &execution_start_error)) {
            validationMessage_ =
                QStringLiteral(
                    "Job execution coordinator startup failed: %1")
                    .arg(QString::fromStdString(
                        execution_start_error));
            jobExecutionCoordinator->Stop();
            workerCoordinator->Stop();
            emit stateChanged();
            return;
        }

        paused_ = startPaused_;
        worker_coordinator_ = std::move(workerCoordinator);
        job_execution_coordinator_ =
            std::move(jobExecutionCoordinator);
    } catch (const std::exception& ex) {
        validationMessage_ = QStringLiteral("Coordinator startup failed: %1").arg(QString::fromUtf8(ex.what()));
        stopCoordinatorServices();
        emit stateChanged();
        return;
    } catch (...) {
        validationMessage_ = QStringLiteral("Coordinator startup failed with an unknown exception.");
        stopCoordinatorServices();
        emit stateChanged();
        return;
    }

    updateSnapshotCache();
    emit stateChanged();
    emit snapshotChanged();
}

void CoordinatorController::stopCoordinator()
{
    if (!worker_coordinator_ && !job_execution_coordinator_) {
        return;
    }

    stopCoordinatorServices();
    paused_ = false;
    updateSnapshotCache();
    emit stateChanged();
    emit snapshotChanged();
}

void CoordinatorController::setPaused(bool paused)
{
    paused_ = paused;
    if (worker_coordinator_) {
        worker_coordinator_->SetPaused(paused_);
    }
    if (job_execution_coordinator_) {
        job_execution_coordinator_->SetPaused(paused_);
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
    if (worker_coordinator_) {
        worker_coordinator_->SetDesiredWorkerCount(
            static_cast<size_t>(targetWorkers_));
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

void CoordinatorController::setVisualWorkerPoolEnabled(bool enabled)
{
    if (visualWorkerPoolEnabled_ == enabled) {
        return;
    }
    if (worker_coordinator_ || job_execution_coordinator_) {
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
    if (worker_coordinator_) {
        worker_coordinator_->SetWorkerVisualSurface(
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
    if (!worker_coordinator_) {
        snapshotCache_.clear();
        visualSnapshotCache_.clear();
        warningSnapshotCache_.clear();
        return;
    }

    snapshotCache_ = worker_coordinator_->SnapshotWorkers();
    visualSnapshotCache_.clear();
    warningSnapshotCache_ = job_execution_coordinator_
        ? job_execution_coordinator_->SnapshotWarnings()
        : std::vector<
              savor::runner::parallel::savordb::
                  JobExecutionCoordinatorWarning>{};
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
    return cfg;
}

void CoordinatorController::stopCoordinatorServices()
{
    std::string shutdownError;
    if (job_execution_coordinator_) {
        job_execution_coordinator_->Quiesce();
        std::string releaseError;
        if (!job_execution_coordinator_->ReleaseBufferedClaims(
                &releaseError)
            && !releaseError.empty()) {
            shutdownError = std::move(releaseError);
        }
    }

    if (worker_coordinator_) {
        worker_coordinator_->Stop();
    }

    if (job_execution_coordinator_) {
        std::string recoveryError;
        if (!job_execution_coordinator_->RecoverAfterWorkersStopped(
                &recoveryError)
            && shutdownError.empty() && !recoveryError.empty()) {
            shutdownError = std::move(recoveryError);
        }
        job_execution_coordinator_->Stop();
        job_execution_coordinator_.reset();
    }
    worker_coordinator_.reset();

    if (!shutdownError.empty()) {
        validationMessage_ =
            QStringLiteral("Coordinator shutdown warning: %1")
                .arg(QString::fromStdString(shutdownError));
    }
}

void CoordinatorController::applyVisualWorkerSurfaces()
{
    if (!worker_coordinator_) {
        return;
    }
    for (const auto& [workerIndex, surface] : visualWorkerSurfaces_) {
        if (workerIndex < 0 || surface.renderWidgetHandle == 0) {
            continue;
        }
        worker_coordinator_->SetWorkerVisualSurface(
            static_cast<size_t>(workerIndex),
            static_cast<uint64_t>(surface.renderWidgetHandle),
            surface.hostEventsPipeName.toStdString());
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
