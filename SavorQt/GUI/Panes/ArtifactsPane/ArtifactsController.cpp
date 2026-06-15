#include "ArtifactsController.h"

#include <QtCore/QFileInfo>
#include <QtCore/QSettings>

#include <algorithm>
#include <exception>
#include <utility>

using savorqt::db::ArtifactImportRequest;
using savorqt::db::SavorDbArtifactService;

namespace {
constexpr auto kSettingsGroup = "ArtifactsPane";
constexpr auto kSearchKey = "search";
constexpr auto kExtensionKey = "extension";
constexpr auto kPageLimitKey = "page_limit";

QString describeException(const char* prefix)
{
    try {
        throw;
    } catch (const std::exception& ex) {
        return QStringLiteral("%1: %2").arg(QString::fromUtf8(prefix), QString::fromUtf8(ex.what()));
    } catch (...) {
        return QStringLiteral("%1: unknown exception").arg(QString::fromUtf8(prefix));
    }
}

template <typename AsyncCall>
auto runAsync(AsyncCall&& asyncCall)
{
    return QtConcurrent::run([call = std::forward<AsyncCall>(asyncCall)]() mutable {
        return call();
    });
}
}

ArtifactsController::ArtifactsController(QObject* parent)
    : QObject(parent)
{
    loadSettings();
    syncFetchStateFromView();

    pageRefreshPipeline_ = new savorqt::gui::AsyncRefreshPipeline<ObjectPageFetchRequest, ObjectPageResult>(this);
    pageRefreshPipeline_->setAutoRefreshEnabled(false);
    pageRefreshPipeline_->setRequestBuilder([this](savorqt::gui::RefreshReason) -> std::optional<ObjectPageFetchRequest> {
        refreshRootsState();
        if (!state_.rootsReady) {
            state_.page = {};
            state_.selectedArtifactId = 0;
            emitStateChanged();
            return std::nullopt;
        }

        state_.loading = true;
        state_.errorMessage.clear();
        savor::db::UiReadArtifactListQuery query{};
        query.before = before_;
        query.after = after_;
        query.limit = fetchPageLimit_;
        query.search = fetchSearch_.toStdString();
        query.extension = fetchExtension_.toStdString();
        emitStateChanged();
        return ObjectPageFetchRequest{ query };
    });
    pageRefreshPipeline_->setLoadAndPrepare([](ObjectPageFetchRequest request) {
        return savorqt::gui::AsyncRefreshResult<ObjectPageResult>::Ok(
            SavorDbArtifactService::ListArtifacts(request.query));
    });
    pageRefreshPipeline_->setApply([this](const ObjectPageResult& result, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        state_.loading = false;
        refreshRootsState();
        if (!state_.rootsReady) {
            state_.page = {};
            state_.selectedArtifactId = 0;
            emitStateChanged();
            return;
        }
        if (result.ok) {
            state_.page = result.value;
            state_.lastRefresh = QDateTime::currentDateTime();
            state_.errorMessage.clear();
            if (!state_.page.items.empty()) {
                bool foundSelection = false;
                for (const auto& item : state_.page.items) {
                    if (item.artifact_id == state_.selectedArtifactId) {
                        foundSelection = true;
                        break;
                    }
                }
                if (!foundSelection) {
                    state_.selectedArtifactId = state_.page.items.front().artifact_id;
                }
            } else {
                state_.selectedArtifactId = 0;
            }
        } else {
            state_.page = {};
            state_.selectedArtifactId = 0;
            state_.errorMessage = QStringLiteral("Artifacts failed: %1").arg(QString::fromStdString(result.error.message));
        }
        emitStateChanged();
    });
    pageRefreshPipeline_->setApplyError([this](const QString& error, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        state_.loading = false;
        state_.page = {};
        state_.selectedArtifactId = 0;
        state_.errorMessage = error;
        emitStateChanged();
    });

    connect(&importWatcher_, &QFutureWatcher<ObjectRowResult>::finished, this, [this]() {
        state_.importBusy = false;
        try {
            const auto result = importWatcher_.result();
            if (result.ok) {
                state_.infoMessage = QStringLiteral("Imported artifact %1.").arg(result.value.artifact_id);
                state_.errorMessage.clear();
                before_.reset();
                after_.reset();
                kickPageFetch();
                return;
            }
            state_.errorMessage = QStringLiteral("Import failed: %1").arg(QString::fromStdString(result.error.message));
        } catch (...) {
            state_.errorMessage = describeException("Import failed");
        }
        emitStateChanged();
    });

    connect(&materializeWatcher_, &QFutureWatcher<VoidResult>::finished, this, [this]() {
        state_.exportBusy = false;
        try {
            const auto result = materializeWatcher_.result();
            if (result.ok) {
                state_.infoMessage = QStringLiteral("Materialized artifact to %1.").arg(state_.selectedFilePath);
                state_.errorMessage.clear();
            } else {
                state_.errorMessage = QStringLiteral("Materialize failed: %1").arg(QString::fromStdString(result.error.message));
            }
        } catch (...) {
            state_.errorMessage = describeException("Materialize failed");
        }
        emitStateChanged();
    });
}

const ArtifactsController::ViewState& ArtifactsController::viewState() const
{
    return state_;
}

void ArtifactsController::loadInitial()
{
    if (initialLoadStarted_) {
        return;
    }
    initialLoadStarted_ = true;
    refreshRootsState();
    if (state_.rootsReady) {
        kickPageFetch();
    }
    emitStateChanged();
}

void ArtifactsController::applyFilters(const QString& search, const QString& extension, int pageLimit)
{
    state_.search = search.trimmed();
    state_.extension = normalizedExtension(extension);
    state_.pageLimit = pageLimit;
    before_.reset();
    after_.reset();
    state_.errorMessage.clear();
    state_.infoMessage.clear();
    syncFetchStateFromView();
    persistSettings();
    kickPageFetch();
}

void ArtifactsController::resetFilters()
{
    state_.search.clear();
    state_.extension.clear();
    state_.pageLimit = 100;
    before_.reset();
    after_.reset();
    state_.errorMessage.clear();
    state_.infoMessage.clear();
    syncFetchStateFromView();
    persistSettings();
    kickPageFetch();
    emitStateChanged();
}

void ArtifactsController::requestRefresh()
{
    before_.reset();
    after_.reset();
    kickPageFetch();
}

void ArtifactsController::requestNextPage()
{
    if (state_.page.next.has_value()) {
        after_ = state_.page.next;
        before_.reset();
        kickPageFetch();
    }
}

void ArtifactsController::requestPreviousPage()
{
    if (state_.page.prev.has_value()) {
        before_ = state_.page.prev;
        after_.reset();
        kickPageFetch();
    }
}

void ArtifactsController::selectArtifact(qint64 artifactId)
{
    const qint64 normalizedArtifactId = artifactId > 0 ? artifactId : 0;
    if (state_.selectedArtifactId == normalizedArtifactId) {
        return;
    }

    state_.selectedArtifactId = normalizedArtifactId;
    emitStateChanged();
}

void ArtifactsController::importArtifact(const QString& sourcePath, const QString& filename, const QString& artifactKind)
{
    if (state_.importBusy || sourcePath.isEmpty() || filename.trimmed().isEmpty()) {
        return;
    }

    refreshRootsState();
    if (!state_.rootsReady) {
        state_.errorMessage = QStringLiteral("Artifact storage roots are unset. Configure storage before importing artifacts.");
        emitStateChanged();
        return;
    }

    state_.importBusy = true;
    state_.selectedFilePath = sourcePath;
    state_.errorMessage.clear();
    state_.infoMessage = QStringLiteral("Importing artifact...");
    importWatcher_.setFuture(runAsync([source = sourcePath.toStdString(),
                                        outputName = filename.trimmed().toStdString(),
                                        kind = artifactKind.trimmed().toStdString()]() {
        return SavorDbArtifactService::ImportArtifact(ArtifactImportRequest{
            .source_path = source,
            .filename = outputName,
            .artifact_kind = kind,
            .compression_kind = 0,
        });
    }));
    emitStateChanged();
}

void ArtifactsController::materializeSelectedArtifact(const QString& outputPath)
{
    const auto* artifact = selectedArtifact();
    if (!artifact || state_.exportBusy || outputPath.isEmpty()) {
        return;
    }

    refreshRootsState();
    if (!state_.rootsReady) {
        state_.errorMessage = QStringLiteral("Artifact storage roots are unset. Configure storage before exporting artifacts.");
        emitStateChanged();
        return;
    }

    state_.exportBusy = true;
    state_.selectedFilePath = outputPath;
    state_.errorMessage.clear();
    state_.infoMessage = QStringLiteral("Materializing artifact...");
    materializeWatcher_.setFuture(runAsync([artifactId = artifact->artifact_id, path = outputPath.toStdString()]() {
        return SavorDbArtifactService::MaterializeArtifactToPath(artifactId, path);
    }));
    emitStateChanged();
}

void ArtifactsController::refreshRootsState()
{
    state_.rootsReady = SavorDbArtifactService::StorageReady();
    if (!state_.rootsReady && state_.errorMessage.isEmpty()) {
        state_.infoMessage = QStringLiteral("Artifact storage roots are unset. Artifact view/actions are disabled until storage is configured.");
    }
}

void ArtifactsController::kickPageFetch()
{
    if (pageRefreshPipeline_ != nullptr) {
        pageRefreshPipeline_->requestRefresh(savorqt::gui::RefreshReason::Manual);
    }
}

void ArtifactsController::emitStateChanged()
{
    emit stateChanged();
}

void ArtifactsController::loadSettings()
{
    QSettings settings;
    settings.beginGroup(kSettingsGroup);

    state_.search = settings.value(kSearchKey, QString()).toString().trimmed();
    state_.extension = normalizedExtension(settings.value(kExtensionKey, QString()).toString());
    state_.pageLimit = (std::max)(1, settings.value(kPageLimitKey, state_.pageLimit).toInt());

    settings.endGroup();
}

void ArtifactsController::persistSettings() const
{
    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    settings.setValue(kSearchKey, state_.search);
    settings.setValue(kExtensionKey, state_.extension);
    settings.setValue(kPageLimitKey, state_.pageLimit);
    settings.endGroup();
}

void ArtifactsController::syncFetchStateFromView()
{
    fetchSearch_ = state_.search;
    fetchExtension_ = state_.extension;
    fetchPageLimit_ = state_.pageLimit;
}

QString ArtifactsController::normalizedExtension(const QString& extension) const
{
    const QString trimmed = extension.trimmed();
    if (trimmed.isEmpty() || trimmed.startsWith('.')) {
        return trimmed;
    }
    return QStringLiteral(".") + trimmed;
}

const savor::db::UiArtifactSummary* ArtifactsController::selectedArtifact() const
{
    for (const auto& artifact : state_.page.items) {
        if (artifact.artifact_id == state_.selectedArtifactId) {
            return &artifact;
        }
    }
    return nullptr;
}
