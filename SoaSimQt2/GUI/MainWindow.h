#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QTimer>
#include <QtWidgets/QMainWindow>
#include "GUI/Widgets/StatusBarWidget.h"
#include "GUI/Panes/CoordinatorPane/CoordinatorPane.h"
#include "GUI/Panes/CoordinatorPane/CoordinatorController.h"
#include "GUI/Panes/SettingsPane/SettingsPage.h"
#include "GUI/Panes/DtmEditorPane/DtmEditorPage.h"

class CoordinatorController;
class CoordinatorPane;
class WorkflowsPage;
class JobsPage;
class SeedProbePage;
class ExplorerRunsPage;
class QLabel;

class QListWidget;
class QStackedWidget;
class QWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

signals:
    void coordinatorStateChanged(bool running, bool paused, int targetWorkers, int activeWorkers, const QString& validationMessage);

private slots:
    void handleNavigationChanged(int currentRow);
    void handleCoordinatorSettingsNavigation(CoordinatorPane::SettingsFocusTarget target);
    void syncStatusBar();

private:
    void createWidgets();
    QWidget* createTopBar();
    QWidget* createNavigationPane();
    QWidget* createContentPane();
    StatusBarWidget* createStatusBarWidget();
    void emitCoordinatorStateChanged();

    QListWidget* navigationList_ = nullptr;
    QStackedWidget* contentStack_ = nullptr;
    QLabel* contentTitleLabel_ = nullptr;
    QLabel* contentDescriptionLabel_ = nullptr;
    CoordinatorController* coordinatorController_ = nullptr;
    CoordinatorPane* coordinatorPane_ = nullptr;
    WorkflowsPage* workflowsPage_ = nullptr;
    JobsPage* jobsPage_ = nullptr;
    SeedProbePage* seedProbePage_ = nullptr;
    ExplorerRunsPage* explorerRunsPage_ = nullptr;
    SettingsPage* settingsPage_ = nullptr;
    DtmEditorPage* dtmEditorPage_ = nullptr;
    StatusBarWidget* statusBarWidget_ = nullptr;
    QTimer statusBarRefreshTimer_;
    QDateTime lastCoordinatorRefresh_;
};
