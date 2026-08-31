#pragma once

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QDateTime>
#include <QtCore/QFutureWatcher>
#include <QtCore/QObject>
#include <QtCore/QString>

#include "DB/SavorDbArtifactService.h"
#include "GUI/Refresh/DatabaseProjectionController.h"

#include <optional>

class ArtifactsController final : public QObject
{
    Q_OBJECT

public:
    explicit ArtifactsController(QObject* parent = nullptr);

    struct ViewState {
        savor::db::UiReadPage<savor::db::UiArtifactSummary> page;
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
    using ObjectPageResult = savorqt::db::ServiceResult<savor::db::UiReadPage<savor::db::UiArtifactSummary>>;
    using ObjectRowResult = savorqt::db::ServiceResult<savor::db::UiArtifactSummary>;
    using VoidResult = savorqt::db::ServiceResult<void>;
    struct ObjectPageFetchRequest {
        savor::db::UiReadArtifactListQuery query{};
    };

    void refreshRootsState();
    void kickPageFetch();
    void emitStateChanged();
    void loadSettings();
    void persistSettings() const;
    void syncFetchStateFromView();
    QString normalizedExtension(const QString& extension) const;
    const savor::db::UiArtifactSummary* selectedArtifact() const;

    ViewState state_;
    QString fetchSearch_;
    QString fetchExtension_;
    int fetchPageLimit_ = 100;
    std::optional<savor::db::UiReadListCursor> before_;
    std::optional<savor::db::UiReadListCursor> after_;
    bool initialLoadStarted_ = false;
    savorqt::gui::DatabaseProjectionController<ObjectPageFetchRequest, ObjectPageResult>* pageRefreshPipeline_ = nullptr;
    QFutureWatcher<ObjectRowResult> importWatcher_;
    QFutureWatcher<VoidResult> materializeWatcher_;
};
