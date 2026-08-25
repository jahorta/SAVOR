#pragma once

#include "GUI/Workspace/WorkspaceWidgets.h"

#include <functional>

class QButtonGroup;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTableWidget;
namespace savorqt::gui { class BattleRunsWidget; }

class AnalysisTab final : public savorqt::gui::WorkspacePageShell
{
public:
    struct Actions {
        std::function<void()> openSeedProbe;
        std::function<void()> openBattleRuns;
        std::function<void()> openArtifacts;
        std::function<void()> openWorkflows;
        std::function<void(qint64)> replayVisual;
        std::function<void(const QString&, const QString&, qint64)> openSetupLauncher;
    };

    explicit AnalysisTab(Actions actions, QWidget* parent = nullptr);
    void showBattleRunsPane();
    void setPageActive(bool active);

private:
    void build();
    void refreshResults();
    void setCurrentPane(int index);
    void showBattleJobDetails(qint64 jobId);
    void showBattleReplicationDetails(qint64 jobId);
    void showBattleUnavailable(qint64 jobId, const QString& title, const QString& message);

    Actions actions_;
    std::function<void()> requestResultsRefresh_;

    QButtonGroup* selectorGroup_ = nullptr;
    QStackedWidget* paneStack_ = nullptr;

    QLabel* outcomesValueLabel_ = nullptr;
    QLabel* workflowsValueLabel_ = nullptr;
    QLabel* artifactsValueLabel_ = nullptr;
    QLabel* problemsValueLabel_ = nullptr;
    QLabel* lastRefreshLabel_ = nullptr;

    QPushButton* openBattleRunsButton_ = nullptr;
    QPushButton* openWorkflowsButton_ = nullptr;
    QPushButton* openArtifactsButton_ = nullptr;

    QLabel* overviewSummaryLabel_ = nullptr;
    QLabel* outcomesSummaryLabel_ = nullptr;
    QLabel* workflowsSummaryLabel_ = nullptr;
    QLabel* futureSummaryLabel_ = nullptr;

    QTableWidget* overviewTable_ = nullptr;
    QTableWidget* workflowsTable_ = nullptr;
    QTableWidget* tasMoviesTable_ = nullptr;
    savorqt::gui::BattleRunsWidget* battleRunsWidget_ = nullptr;

    int currentPaneIndex_ = 0;
    bool pageActive_ = false;
};
