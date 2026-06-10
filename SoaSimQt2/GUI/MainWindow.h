#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtWidgets/QMainWindow>

#include "GUI/Widgets/StatusBarWidget.h"
#include "GUI/Panes/CoordinatorPane/CoordinatorPane.h"
#include "GUI/Panes/CoordinatorPane/CoordinatorController.h"
#include "GUI/Panes/SettingsPane/SettingsPage.h"
#include "GUI/Panes/DtmEditorPane/DtmEditorPage.h"
#include "GUI/Panes/BattleRunSettingsPane/AuthoringLibraryDialog.h"
#include "GUI/Workspace/WorkspaceWidgets.h"

class CoordinatorController;
class CoordinatorPane;
class ExplorerRunsPage;
class JobsPage;
class QLabel;
class QDialog;
class QStackedWidget;
class QWidget;
class WorkflowGraphEditorWindow;
class WorkflowsPage;
namespace simcore::db { struct WorkflowGraphSnapshot; }

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
        ExplorerRuns,
        DtmEditor,
        Settings
    };

    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();
    void shutdownCoordinator();

signals:
    void coordinatorStateChanged(bool running, bool paused, int targetWorkers, int activeWorkers, const QString& validationMessage);

private slots:
    void handleWorkspaceChanged(int currentIndex);
    void handleCoordinatorSettingsNavigation(CoordinatorPane::SettingsFocusTarget target);
    void handleVisualReplayRequested(qint64 jobId);
    void syncStatusBar();
    void openSeedProbeSpecLibrary();
    void openTasSpecLibrary();
    void openBattleRunSpecLibrary();
    void openPredicateSpecLibrary();
    void openPredicateSetSpecLibrary();
    void openBattlePlanSpecLibrary();
    void openExplorerSettingsSpecLibrary();

private:
    void createWidgets();
    void createMenus();
    void openAuthoringLibraryLast();
    void openAuthoringLibrary(AuthoringLibraryKey key);
    StatusBarWidget* createStatusBarWidget();
    void emitCoordinatorStateChanged();
    void refreshWorkspaceBadges();
    void openFocusedTool(FocusedTool tool);
    void openWorkflowGraphEditor();
    void openWorkflowGraphEditor(const simcore::db::WorkflowGraphSnapshot& snapshot, bool duplicate);
    void openSettingsTool(SettingsPage::CoordinatorFocusTarget focusTarget = SettingsPage::CoordinatorFocusTarget::Section);
    QDialog* createFocusedDialog(const QString& key, const QString& title);
    void setWorkspaceIndex(int index);
    void ensureVisualReplayHost();

    QStackedWidget* workspaceStack_ = nullptr;
    soasimqt2::gui::WorkspaceSelectorBar* workspaceSelector_ = nullptr;
    CoordinatorController* coordinatorController_ = nullptr;
    CoordinatorPane* visualReplayHost_ = nullptr;
    StatusBarWidget* statusBarWidget_ = nullptr;
    QTimer statusBarRefreshTimer_;
    QDateTime lastCoordinatorRefresh_;
    QPointer<AuthoringLibraryDialog> authoringLibraryDialog_;
    QPointer<WorkflowGraphEditorWindow> workflowGraphEditor_;
    AuthoringLibraryKey lastAuthoringLibrary_ = AuthoringLibraryKey::Tas;
    QHash<QString, QPointer<QDialog>> focusedDialogs_;
};
