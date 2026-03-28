#pragma once

#include <QtWidgets/QWidget>

#include "ExplorerRunsCoordinator.h"

#include <array>
#include <vector>

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
class QPoint;
struct ExplorerRunsJobRow;

class ExplorerRunsPage final : public QWidget
{
    Q_OBJECT

public:
    explicit ExplorerRunsPage(QWidget* parent = nullptr);
    ~ExplorerRunsPage() override;

signals:
    void visualReplayRequested(qint64 jobId);

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

    struct ViewState {
        qint64 selectedRoot = -1;
        std::vector<qint64> selectedWaves;
        qint64 selectedJob = -1;
        bool overrideMaxFakeAttacks = false;
        int maxFakeAttacksOverride = 0;
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
    void syncControls();
    void refreshView();
    void refreshGroupModel();
    void refreshWaveTree();
    void refreshJobModel();
    void refreshDetailPanel();
    void setStatusMessage(const QString& text, bool error = false);
    void handleCoordinatorStateChanged();

    QString waveStatusIcon(bool hasWinner, bool hasSuccessOutcome) const;
    QString sortMetricLabel(SortMetric metric) const;
    int compareMetric(const ExplorerRunsCoordinator::JobViewRow& a, const ExplorerRunsCoordinator::JobViewRow& b, SortMetric metric) const;
    std::vector<ExplorerRunsCoordinator::JobViewRow> buildVisibleSortedJobs() const;
    const ExplorerRunsCoordinator::GroupRow* selectedGroup() const;
    bool isSuccessOutcome(quint32 battleOutcome) const;
    bool isWinnerState(const QString& state) const;
    bool isDuplicateState(const QString& state) const;
    bool selectedJobCanTrigger() const;
    void triggerNextWave();
    void showJobsContextMenu(const QPoint& pos);
    void openTurnInputsDialogForJob(const ExplorerRunsJobRow& row);
    void restoreSelectedGroupRow();
    void restoreSelectedJobRow();
    void selectFirstWaveIfNeeded();
    std::vector<qint64> selectedWaveIdsFromTree() const;
    void loadFilterSettings();
    void persistFilterSettings() const;

    ViewState state_;
    ExplorerRunsCoordinator* coordinator_ = nullptr;

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
