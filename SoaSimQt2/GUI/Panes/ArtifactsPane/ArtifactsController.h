#pragma once

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QDateTime>
#include <QtCore/QFutureWatcher>
#include <QtCore/QObject>
#include <QtCore/QString>

#include "DB/SimCoreDbArtifactService.h"

#include <optional>

class ArtifactsController final : public QObject
{
    Q_OBJECT

public:
    explicit ArtifactsController(QObject* parent = nullptr);

    struct ViewState {
        simcore::db::UiReadPage<simcore::db::UiArtifactSummary> page;
        QString search;
        QString extension;
        QString errorMessage;
        QString infoMessage;
        QString selectedFilePath;
        bool rootsReady = false;
        bool loading = false;
        bool importBusy = false;
        bool exportBusy = false;
        int pageLimit = 100;
        qint64 selectedArtifactId = 0;
        QDateTime lastRefresh;
    };

    const ViewState& viewState() const;

    void loadInitial();
    void applyFilters(const QString& search, const QString& extension, int pageLimit);
    void resetFilters();
    void requestRefresh();
    void requestNextPage();
    void requestPreviousPage();
    void selectArtifact(qint64 artifactId);
    void importArtifact(const QString& sourcePath, const QString& filename, const QString& artifactKind);
    void materializeSelectedArtifact(const QString& outputPath);

signals:
    void stateChanged();

private:
    using ObjectPageResult = soasimqt2::db::ServiceResult<simcore::db::UiReadPage<simcore::db::UiArtifactSummary>>;
    using ObjectRowResult = soasimqt2::db::ServiceResult<simcore::db::UiArtifactSummary>;
    using VoidResult = soasimqt2::db::ServiceResult<void>;

    void refreshRootsState();
    void kickPageFetch();
    void emitStateChanged();
    void loadSettings();
    void persistSettings() const;
    void syncFetchStateFromView();
    QString normalizedExtension(const QString& extension) const;
    const simcore::db::UiArtifactSummary* selectedArtifact() const;

    ViewState state_;
    QString fetchSearch_;
    QString fetchExtension_;
    int fetchPageLimit_ = 100;
    std::optional<simcore::db::UiReadListCursor> before_;
    std::optional<simcore::db::UiReadListCursor> after_;
    bool initialLoadStarted_ = false;
    bool pendingPageFetch_ = false;
    QFutureWatcher<ObjectPageResult> pageWatcher_;
    QFutureWatcher<ObjectRowResult> importWatcher_;
    QFutureWatcher<VoidResult> materializeWatcher_;
};
