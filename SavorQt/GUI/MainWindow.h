#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QPair>
#include <QtCore/QTimer>
#include <QtWidgets/QMainWindow>

#include <cstdint>

#include "GUI/Widgets/StatusBarWidget.h"
#include "GUI/Panes/CoordinatorPane/CoordinatorPane.h"
#include "GUI/Panes/CoordinatorPane/CoordinatorController.h"
#include "GUI/Panes/SettingsPane/SettingsPage.h"
#include "GUI/Panes/DtmEditorPane/DtmEditorPage.h"
#include "GUI/Panes/BattleRunSettingsPane/AuthoringLibraryDialog.h"
#include "GUI/Refresh/AsyncRefreshPipeline.h"
#include "GUI/Workspace/WorkspaceWidgets.h"

class CoordinatorController;
class CoordinatorPane;
class SetupTab;
class RunningTab;
class AnalysisTab;
class JobsPage;
class QLabel;
class QDialog;
class QStackedWidget;
class QWidget;
class WorkflowGraphEditorWindow;
class WorkflowsPage;
namespace savor::db { struct WorkflowGraphSnapshot; }

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    enum class FocusedTool {
        Workflows,
        Jobs,
        Workers,
        WorkflowLauncher,
        BattleRunSettings,
        Artifacts,
        SeedProbe,
        BattleRuns,
        ArchiveWorkbench,
        DtmEditor,
        Settings
    };

    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();
    void shutdownCoordinator();
    void waitForCoordinatorShutdown();

signals:
    void coordinatorStateChanged(CoordinatorLifecycleState state, bool paused, int targetWorkers, int activeWorkers, const QString& validationMessage);

private slots:
    void handleWorkspaceChanged(int currentIndex);
    void handleCoordinatorSettingsNavigation(CoordinatorPane::SettingsFocusTarget target);
    void handleVisualReplayRequested(qint64 jobId);
    void syncStatusBar();
    void openSeedProbeSpecLibrary();
    void openBattlePlanSpecLibrary();
    void openPredicateLibrary();
    void openPredicateGroupLibrary();

private:
    void createWidgets();
    void createMenus();
    void openAuthoringLibraryLast();
    void openAuthoringLibrary(AuthoringLibraryKey key);
    StatusBarWidget* createStatusBarWidget();
    void emitCoordinatorStateChanged();
    void refreshWorkspaceBadges();
    void openFocusedTool(FocusedTool tool);
    void openWorkflow(std::int64_t workflowInstanceId);
    void openJob(std::int64_t jobId);
    void retryWorkflowJobs(std::int64_t workflowInstanceId);
    void showBattleRunsAnalysisPane();
    void openWorkflowGraphEditor();
    void openWorkflowGraphEditor(const savor::db::WorkflowGraphSnapshot& snapshot, bool duplicate);
    void openWorkflowLauncherPreselected(const QString& unitKind,const QString& inputKey,qint64 refId);
    void openSettingsTool(SettingsPage::CoordinatorFocusTarget focusTarget = SettingsPage::CoordinatorFocusTarget::Section);
    QDialog* createFocusedDialog(const QString& key, const QString& title);
    void setWorkspaceIndex(int index);
    void ensureVisualReplayHost();

    QStackedWidget* workspaceStack_ = nullptr;
    savorqt::gui::WorkspaceSelectorBar* workspaceSelector_ = nullptr;
	SetupTab* setupTab_ = nullptr;
	RunningTab* runningTab_ = nullptr;
    AnalysisTab* analysisTab_ = nullptr;
    CoordinatorController* coordinatorController_ = nullptr;
    CoordinatorPane* visualReplayHost_ = nullptr;
    StatusBarWidget* statusBarWidget_ = nullptr;
    QTimer statusBarRefreshTimer_;
    savorqt::gui::AsyncRefreshPipeline<int, QPair<int, int>>* workspaceBadgeRefreshPipeline_ = nullptr;
    QDateTime lastCoordinatorRefresh_;
    QPointer<AuthoringLibraryDialog> authoringLibraryDialog_;
    QPointer<WorkflowGraphEditorWindow> workflowGraphEditor_;
    AuthoringLibraryKey lastAuthoringLibrary_ = AuthoringLibraryKey::SeedProbe;
    QHash<QString, QPointer<QDialog>> focusedDialogs_;
    QString lastCoordinatorWarningToastSignature_;
    bool shutdownCoordinatorStarted_ = false;
    bool workflowRetryInFlight_ = false;
};
