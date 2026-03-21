#include "ArtifactsController.h"

#include "DB/DBCore/ObjectStore.h"

#include <QtCore/QFileInfo>

#include <exception>
#include <utility>

using simcore::db::Compression;
using simcore::db::DataService;
using simcore::db::ObjectStore;

namespace {
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
    connect(&pageWatcher_, &QFutureWatcher<ObjectPageResult>::finished, this, [this]() {
        state_.loading = false;
        try {
            refreshRootsState();
            if (!state_.rootsReady) {
                state_.page = {};
                state_.selectedArtifactId = 0;
                emitStateChanged();
                return;
            }

            const auto result = pageWatcher_.result();
            if (result.ok) {
                state_.page = result.value;
                state_.lastRefresh = QDateTime::currentDateTime();
                state_.errorMessage.clear();
                if (!state_.page.items.empty()) {
                    bool foundSelection = false;
                    for (const auto& item : state_.page.items) {
                        if (item.id == state_.selectedArtifactId) {
                            foundSelection = true;
                            break;
                        }
                    }
                    if (!foundSelection) {
                        state_.selectedArtifactId = state_.page.items.front().id;
                    }
                } else {
                    state_.selectedArtifactId = 0;
                }
            } else {
                state_.page = {};
                state_.selectedArtifactId = 0;
                state_.errorMessage = QStringLiteral("Artifacts failed: %1").arg(QString::fromStdString(result.error.message));
            }
        } catch (...) {
            state_.page = {};
            state_.selectedArtifactId = 0;
            state_.errorMessage = describeException("Artifacts failed");
        }
        emitStateChanged();
    });

    connect(&importWatcher_, &QFutureWatcher<ObjectRowResult>::finished, this, [this]() {
        state_.importBusy = false;
        try {
            const auto result = importWatcher_.result();
            if (result.ok) {
                state_.infoMessage = QStringLiteral("Imported artifact %1.").arg(result.value.id);
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
    if (artifactId <= 0) {
        state_.selectedArtifactId = 0;
    } else {
        state_.selectedArtifactId = artifactId;
    }
    emitStateChanged();
}

void ArtifactsController::importArtifact(const QString& sourcePath, const QString& filename)
{
    if (state_.importBusy || sourcePath.isEmpty() || filename.trimmed().isEmpty()) {
        return;
    }

    refreshRootsState();
    if (!state_.rootsReady) {
        state_.errorMessage = QStringLiteral("ObjectStore roots are unset. Configure storage before importing artifacts.");
        emitStateChanged();
        return;
    }

    state_.importBusy = true;
    state_.selectedFilePath = sourcePath;
    state_.errorMessage.clear();
    state_.infoMessage = QStringLiteral("Importing artifact…");
    importWatcher_.setFuture(runAsync([source = sourcePath.toStdString(), outputName = filename.trimmed().toStdString()]() {
        return ObjectStore::FinalizeFromFileAsync(source, Compression::None, outputName).get();
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
        state_.errorMessage = QStringLiteral("ObjectStore roots are unset. Configure storage before exporting artifacts.");
        emitStateChanged();
        return;
    }

    state_.exportBusy = true;
    state_.selectedFilePath = outputPath;
    state_.errorMessage.clear();
    state_.infoMessage = QStringLiteral("Materializing artifact…");
    materializeWatcher_.setFuture(runAsync([artifactId = artifact->id, path = outputPath.toStdString()]() {
        return ObjectStore::MaterializeToPathAsync(artifactId, path).get();
    }));
    emitStateChanged();
}

void ArtifactsController::refreshRootsState()
{
    state_.rootsReady = ObjectStore::Ready();
    if (!state_.rootsReady && state_.errorMessage.isEmpty()) {
        state_.infoMessage = QStringLiteral("ObjectStore roots are unset. Artifacts view/actions are disabled until storage is configured.");
    }
}

void ArtifactsController::kickPageFetch()
{
    refreshRootsState();
    if (!state_.rootsReady || state_.loading) {
        if (!state_.rootsReady) {
            state_.page = {};
            state_.selectedArtifactId = 0;
        }
        return;
    }

    state_.loading = true;
    state_.errorMessage.clear();
    PagedQuery<> query;
    query.before = before_;
    query.after = after_;
    query.limit = state_.pageLimit;

    const QString search = state_.search;
    const QString extension = state_.extension;
    pageWatcher_.setFuture(runAsync([query, search, extension]() {
        return DataService::FetchObjectRefsPage(query, search.toStdString(), extension.toStdString()).get();
    }));
    emitStateChanged();
}

void ArtifactsController::emitStateChanged()
{
    emit stateChanged();
}

QString ArtifactsController::normalizedExtension(const QString& extension) const
{
    const QString trimmed = extension.trimmed();
    if (trimmed.isEmpty() || trimmed.startsWith('.')) {
        return trimmed;
    }
    return QStringLiteral(".") + trimmed;
}

const simcore::db::ObjectRefLite* ArtifactsController::selectedArtifact() const
{
    for (const auto& artifact : state_.page.items) {
        if (artifact.id == state_.selectedArtifactId) {
            return &artifact;
        }
    }
    return nullptr;
}
