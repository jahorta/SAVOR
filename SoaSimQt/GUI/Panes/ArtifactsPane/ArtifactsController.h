#pragma once

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QDateTime>
#include <QtCore/QFutureWatcher>
#include <QtCore/QObject>
#include <QtCore/QString>

#include "DB/DBCore/ObjectStore.h"
#include "DB/Querying/DataService.h"
#include "DB/Querying/PagedQuery.h"

#include <optional>

class ArtifactsController final : public QObject
{
    Q_OBJECT

public:
    explicit ArtifactsController(QObject* parent = nullptr);

    struct ViewState {
        Page<simcore::db::ObjectRefLite> page;
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
    void importArtifact(const QString& sourcePath, const QString& filename, simcore::db::Compression compression = simcore::db::Compression::None);
    void materializeSelectedArtifact(const QString& outputPath);

signals:
    void stateChanged();

private:
    using ObjectPageResult = simcore::db::DbResult<Page<simcore::db::ObjectRefLite>>;
    using ObjectRowResult = simcore::db::DbResult<simcore::db::ObjectRefRow>;
    using VoidResult = simcore::db::DbResult<void>;

    void refreshRootsState();
    void kickPageFetch();
    void emitStateChanged();
    void loadSettings();
    void persistSettings() const;
    void syncFetchStateFromView();
    QString normalizedExtension(const QString& extension) const;
    const simcore::db::ObjectRefLite* selectedArtifact() const;

    ViewState state_;
    QString fetchSearch_;
    QString fetchExtension_;
    int fetchPageLimit_ = 100;
    std::optional<KeysetCursor> before_;
    std::optional<KeysetCursor> after_;
    bool initialLoadStarted_ = false;
    bool pendingPageFetch_ = false;
    QFutureWatcher<ObjectPageResult> pageWatcher_;
    QFutureWatcher<ObjectRowResult> importWatcher_;
    QFutureWatcher<VoidResult> materializeWatcher_;
};
