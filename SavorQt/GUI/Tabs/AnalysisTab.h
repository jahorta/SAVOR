#pragma once

#include "GUI/Workspace/WorkspaceWidgets.h"

#include <functional>

class QButtonGroup;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTableWidget;

class AnalysisTab final : public savorqt::gui::WorkspacePageShell
{
public:
    struct Actions {
        std::function<void()> openSeedProbe;
        std::function<void()> openExplorerRuns;
        std::function<void()> openArtifacts;
        std::function<void()> openWorkflows;
    };

    explicit AnalysisTab(Actions actions, QWidget* parent = nullptr);

private:
    void build();
    void refreshResults();
    void setCurrentPane(int index);

    Actions actions_;
    std::function<void()> requestResultsRefresh_;

    QButtonGroup* selectorGroup_ = nullptr;
    QStackedWidget* paneStack_ = nullptr;

    QLabel* outcomesValueLabel_ = nullptr;
    QLabel* workflowsValueLabel_ = nullptr;
    QLabel* artifactsValueLabel_ = nullptr;
    QLabel* problemsValueLabel_ = nullptr;
    QLabel* lastRefreshLabel_ = nullptr;

    QPushButton* openExplorerButton_ = nullptr;
    QPushButton* openWorkflowsButton_ = nullptr;
    QPushButton* openArtifactsButton_ = nullptr;

    QLabel* overviewSummaryLabel_ = nullptr;
    QLabel* outcomesSummaryLabel_ = nullptr;
    QLabel* workflowsSummaryLabel_ = nullptr;
    QLabel* futureSummaryLabel_ = nullptr;

    QTableWidget* overviewTable_ = nullptr;
    QTableWidget* outcomesTable_ = nullptr;
    QTableWidget* workflowsTable_ = nullptr;

    int currentPaneIndex_ = 0;
};
