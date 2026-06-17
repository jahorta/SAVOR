#pragma once

#include "DB/SavorDbExplorerRunService.h"
#include "GUI/Widgets/StatusBarWidget.h"

#include <QtCore/QDateTime>
#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTreeWidget;

namespace savorqt::gui {

template <typename Request, typename Data>
class AsyncRefreshPipeline;

class BattleRunsWidget final : public QWidget
{
public:
    struct Actions {
        std::function<void(std::int64_t)> showJobDetails;
        std::function<void(std::int64_t)> showBattlePlan;
        std::function<void(std::int64_t)> showReplicationDetails;
        std::function<void(std::int64_t)> viewTurnInputs;
        std::function<void(std::int64_t)> replayVisual;
        std::function<void(StatusToast)> statusToast;
    };

    explicit BattleRunsWidget(Actions actions, QWidget* parent = nullptr);

    void setPageActive(bool active);
    void requestRefresh();

    // Public only so the cpp-local refresh helpers can prepare table rows without
    // exposing QObject state to background refresh work.
    struct RefreshRequest {
        std::optional<db::BattleRunCursor> before;
        std::optional<db::BattleRunCursor> after;
        std::int64_t selectedBattleSetId = 0;
        std::int64_t selectedJobId = 0;
        std::vector<std::int64_t> selectedWaveIds;
        int limit = 50;
        bool childVictoryOnly = false;
        bool winnersOnly = false;
        bool showDuplicates = true;
        bool successOnly = false;
        int primarySort = 0;
        int secondarySort = 0;
    };

    struct GroupRow {
        std::int64_t battleSetId = 0;
        QString group;
        QString name;
        QString results;
        QString waves;
        QString status;
    };

    struct WaveRow {
        std::int64_t waveId = 0;
        std::optional<std::int64_t> parentWaveId;
        int turnIndex = 0;
        QString label;
        QString jobs;
        QString status;
        bool hasWinner = false;
        bool hasFailure = false;
        bool selected = false;
    };

    struct JobRow {
        std::int64_t jobId = 0;
        std::optional<std::int64_t> execJobId;
        std::int64_t waveId = 0;
        QString state;
        QString outcome;
        QString predicates;
        QString deltaVi;
        QString fakeAttacks;
        QString rngSeed;
        bool winner = false;
        bool success = false;
        int predPassed = -1;
        int predTotal = -1;
        std::int64_t deltaViSort = 0;
        int fakeAttackSort = 0;
        std::int64_t rngSeedSort = 0;
    };

    struct WaveTurnRow {
        int turnIndex = 0;
        QString label;
        std::vector<WaveRow> waves;
    };

    struct RefreshData {
        db::BattleRunGroupPage groupPage;
        std::vector<GroupRow> groups;
        std::vector<WaveRow> waves;
        std::vector<JobRow> jobs;
        std::int64_t selectedBattleSetId = 0;
        std::vector<std::int64_t> selectedWaveIds;
        std::int64_t selectedJobId = 0;
        QString summary;
        QString message;
        QDateTime refreshedAt;
    };

private:
    void build();
    void wireSignals();
    void applyRefresh(const RefreshData& data);
    void applyError(const QString& error);
    void refreshGroups(const std::vector<GroupRow>& rows);
    void refreshWaves(const std::vector<WaveRow>& rows);
    void refreshJobs(const std::vector<JobRow>& rows);
    void refreshControlState();
    void selectCurrentGroup(std::int64_t battleSetId);
    void requestSelectedWavesRefresh();
    void showJobContextMenu(const QPoint& position);
    std::int64_t selectedJobId() const;

    Actions actions_;

    QPushButton* refreshButton_ = nullptr;
    QPushButton* prevButton_ = nullptr;
    QPushButton* nextButton_ = nullptr;
    QCheckBox* autoRefreshCheck_ = nullptr;
    QSpinBox* refreshSecondsSpin_ = nullptr;
    QSpinBox* pageSizeSpin_ = nullptr;
    QCheckBox* childVictoryOnlyCheck_ = nullptr;
    QCheckBox* winnersOnlyCheck_ = nullptr;
    QCheckBox* showDuplicatesCheck_ = nullptr;
    QCheckBox* successOnlyCheck_ = nullptr;
    QComboBox* primarySortCombo_ = nullptr;
    QComboBox* secondarySortCombo_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
    QLabel* lastRefreshLabel_ = nullptr;
    QLabel* inlineMessageLabel_ = nullptr;
    QTableWidget* groupsTable_ = nullptr;
    QTreeWidget* waveTree_ = nullptr;
    QTableWidget* jobsTable_ = nullptr;

    AsyncRefreshPipeline<RefreshRequest, RefreshData>* refreshPipeline_ = nullptr;

    std::vector<GroupRow> currentGroups_;
    std::vector<WaveTurnRow> currentWaveTurns_;
    std::vector<JobRow> currentJobs_;
    std::int64_t selectedBattleSetId_ = 0;
    std::vector<std::int64_t> selectedWaveIds_;
    std::int64_t selectedJobId_ = 0;
    std::optional<db::BattleRunCursor> before_;
    std::optional<db::BattleRunCursor> after_;
    std::optional<db::BattleRunCursor> next_;
    std::optional<db::BattleRunCursor> prev_;
    QDateTime lastRefresh_;
    bool refreshingSelection_ = false;
    bool pageActive_ = false;
};

} // namespace savorqt::gui
