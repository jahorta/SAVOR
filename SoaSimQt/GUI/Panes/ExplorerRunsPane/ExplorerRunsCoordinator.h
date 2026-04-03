#pragma once

#include <QtConcurrent/QtConcurrentRun>
#include "DB/Scheduling/JobsRepo.h"
#include "DB/Scheduling/JobSetsRepo.h"

#include <QtCore/QDateTime>
#include <QtCore/QFutureWatcher>
#include <QtCore/QObject>
#include <QtCore/QTimer>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class ExplorerRunsCoordinator final : public QObject
{
    Q_OBJECT

public:
    struct WaveMeta {
        qint64 rootGroupId = -1;
        quint32 waveTurn = 1;
        qint64 settingsId = -1;
        qint64 seedProbeId = -1;
        std::string settingsName;
        qint64 tasMovieId = -1;
    };

    struct JobResultSummary {
        quint32 fakeUsed = 0;
        quint32 viStart = 0;
        quint32 viEnd = 0;
        quint32 deltaVi = 0;
        quint32 rngSeed = 0;
        quint32 battleOutcome = 0;
        quint32 planMaterializeErr = 0;
        quint32 predPassed = 0;
        quint32 predTotal = 0;
        quint32 predAbortRun = 0;
        bool hasResults = false;
        bool successOutcome = false;
    };

    struct WaveRow {
        qint64 jobSetId = 0;
        qint64 createdAt = 0;
        quint32 waveTurn = 1;
        QString statusSummary;
        bool hasWinner = false;
        bool hasSuccessOutcome = false;
    };

    struct GroupRow {
        qint64 rootGroupId = 0;
        QString settingsLabel;
        qint64 createdAt = 0;
        int totalWaves = 0;
        QString statusSummary;
        QString resultsSummary;
        bool hasSuccessOutcome = false;
        std::vector<WaveRow> waves;
    };

    struct JobViewRow {
        qint64 jobId = 0;
        QString state;
        quint32 fakeUsed = 0;
        quint32 deltaVi = 0;
        quint32 viStart = 0;
        quint32 viEnd = 0;
        quint32 rngSeed = 0;
        quint32 battleOutcome = 0;
        quint32 planMaterializeErr = 0;
        quint32 predPassed = 0;
        quint32 predTotal = 0;
        quint32 predAbortRun = 0;
        bool hasResults = false;
        bool hasChildVictory = false;
    };

    struct DetailBundle {
        QString resultsLog;
        QString progressLog;
        QString blueprintInfo;
    };

    explicit ExplorerRunsCoordinator(QObject* parent = nullptr);

    const std::vector<GroupRow>& groups() const;
    const std::vector<JobViewRow>& jobs() const;
    const DetailBundle& details() const;

    bool groupsInFlight() const;
    bool jobsInFlight() const;
    bool detailsInFlight() const;
    bool autoRefreshEnabled() const;
    int refreshSeconds() const;
    QString describeBattlePlanForJob(qint64 jobId) const;

public slots:
    void requestGroupsRefresh();
    void requestJobsRefresh(const std::vector<qint64>& waveJobSetIds);
    void requestDetailsRefresh(qint64 jobId);
    void setAutoRefreshEnabled(bool enabled);
    void setRefreshSeconds(int seconds);
    void setChildVictoryOnly(bool enabled);
    void clearJobs();
    void clearDetails();

signals:
    void stateChanged();

private:
    void emitStateChanged();
    void startAutoRefreshTimer();
    void handleAutoRefreshTick();

    std::vector<GroupRow> buildGroups(bool childVictoryOnly) const;
    std::vector<JobViewRow> buildJobsForWaves(const std::vector<qint64>& waveJobSetIds) const;
    std::unordered_map<qint64, JobResultSummary> loadJobResultsMap(const std::vector<qint64>& ids) const;
    QString buildProgressLog(qint64 jobId) const;
    QString buildBlueprintInfo(qint64 jobId) const;
    std::optional<QString> fetchResultsIniText(qint64 jobId) const;

    WaveMeta parseWaveMeta(const std::optional<std::string>& text) const;
    qint64 resolveRoot(const simcore::db::JobSetRow& js) const;
    QString summarizeStates(const std::vector<simcore::db::JobRow>& jobs) const;
    bool isSuccessOutcome(quint32 battleOutcome) const;
    bool isWinnerState(const QString& state) const;

    std::vector<GroupRow> groups_;
    std::vector<JobViewRow> jobs_;
    DetailBundle details_;
    bool groupsInFlight_ = false;
    bool jobsInFlight_ = false;
    bool detailsInFlight_ = false;
    bool autoRefreshEnabled_ = true;
    int refreshSeconds_ = 3;
    bool childVictoryOnly_ = false;
    qint64 detailsRequestJobId_ = 0;
    QDateTime lastGroupsRefresh_;
    QFutureWatcher<std::vector<GroupRow>> groupsWatcher_;
    QFutureWatcher<std::vector<JobViewRow>> jobsWatcher_;
    QFutureWatcher<DetailBundle> detailsWatcher_;
    QTimer autoRefreshTimer_;
};
