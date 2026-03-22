#pragma once

#include <QtCore/QTimer>
#include <QtWidgets/QWidget>

#include "DB/Scheduling/JobsRepo.h"
#include "DB/Scheduling/JobSetsRepo.h"

#include <future>
#include <optional>
#include <vector>
#include <array>
#include <string>
#include <unordered_map>

class ExplorerRunsGroupTableModel;
class ExplorerRunsGroupTableView;
class ExplorerRunsJobsTableModel;
class ExplorerRunsJobsTableView;
class QLabel;
class QCheckBox;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QStandardItemModel;
class QTreeView;
class QComboBox;
class QModelIndex;

class ExplorerRunsPage final : public QWidget
{
public:
    explicit ExplorerRunsPage(QWidget* parent = nullptr);
    ~ExplorerRunsPage() override;

private:
    enum class SortMetric {
        PredicatesPassed = 0,
        DeltaVI,
        FakeAttacks,
        JobId,
        RngSeed,
    };

    struct SortKey {
        SortMetric metric = SortMetric::PredicatesPassed;
        bool ascending = true;
    };

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
    };

    struct ViewState {
        std::future<std::vector<GroupRow>> groupsFuture;
        bool groupsInFlight = false;
        std::vector<GroupRow> groups;

        qint64 selectedRoot = -1;
        std::vector<qint64> selectedWaves;

        std::future<std::vector<JobViewRow>> jobsFuture;
        bool jobsInFlight = false;
        std::vector<JobViewRow> jobs;
        qint64 selectedJob = -1;

        std::future<std::optional<QString>> resultsFuture;
        std::future<QString> progressFuture;
        std::future<QString> blueprintFuture;
        bool detailsInFlight = false;
        QString resultsLog;
        QString progressLog;
        QString blueprintInfo;

        bool overrideMaxFakeAttacks = false;
        int maxFakeAttacksOverride = 0;

        bool autoRefresh = true;
        int refreshSeconds = 3;

        bool winnersOnly = true;
        bool showDuplicates = false;
        bool successOnly = true;
        std::array<SortKey, 3> sortKeys{{
            { SortMetric::PredicatesPassed, false },
            { SortMetric::DeltaVI, true },
            { SortMetric::FakeAttacks, true }
        }};
    };

    void createWidgets();
    void wireSignals();
    void refreshView();
    void syncControls();
    void refreshGroupModel();
    void refreshWaveTree();
    void refreshJobModel();
    void refreshDetailPanel();
    void setStatusMessage(const QString& text, bool error = false);

    void kickGroupsFetch();
    void kickJobsFetch();
    void kickDetailsFetch(qint64 jobId);
    void consumeFutures();

    std::vector<GroupRow> buildGroups() const;
    std::vector<JobViewRow> buildJobsForSelectedWaves() const;
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
    bool isDuplicateState(const QString& state) const;
    QString waveStatusIcon(bool hasWinner, bool hasSuccessOutcome) const;
    QString sortMetricLabel(SortMetric metric) const;
    int compareMetric(const JobViewRow& a, const JobViewRow& b, SortMetric metric) const;
    std::vector<JobViewRow> buildVisibleSortedJobs() const;
    const GroupRow* selectedGroup() const;
    bool selectedJobCanTrigger() const;
    void triggerNextWave();
    void restoreSelectedGroupRow();
    void restoreSelectedJobRow();
    void selectFirstWaveIfNeeded();
    std::vector<qint64> selectedWaveIdsFromTree() const;

    ViewState state_;
    QTimer refreshTimer_;

    QPushButton* refreshButton_ = nullptr;
    QCheckBox* autoRefreshCheck_ = nullptr;
    QSpinBox* refreshSecondsSpin_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
    QLabel* inlineMessageLabel_ = nullptr;

    ExplorerRunsGroupTableModel* groupsModel_ = nullptr;
    ExplorerRunsGroupTableView* groupsView_ = nullptr;
    QStandardItemModel* wavesModel_ = nullptr;
    QTreeView* wavesView_ = nullptr;

    QCheckBox* winnersOnlyCheck_ = nullptr;
    QCheckBox* showDuplicatesCheck_ = nullptr;
    QCheckBox* successOnlyCheck_ = nullptr;
    QComboBox* sortMetricBoxes_[3]{};
    QCheckBox* sortAscendingChecks_[3]{};
    QLabel* jobsSummaryLabel_ = nullptr;
    ExplorerRunsJobsTableModel* jobsModel_ = nullptr;
    ExplorerRunsJobsTableView* jobsView_ = nullptr;

    QLabel* triggerHintLabel_ = nullptr;
    QCheckBox* overrideFakeAttacksCheck_ = nullptr;
    QSpinBox* fakeAttacksSpin_ = nullptr;
    QPushButton* triggerButton_ = nullptr;
    QPlainTextEdit* blueprintText_ = nullptr;
    QPlainTextEdit* progressText_ = nullptr;
    QPlainTextEdit* resultsText_ = nullptr;
};
