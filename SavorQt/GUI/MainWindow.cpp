#include "MainWindow.h"

#include <QtGui/QCloseEvent>

#include "GUI/Panes/ArtifactsPane/ArtifactsPage.h"
#include "GUI/Panes/ArchivePane/ArchiveWorkbenchPage.h"
#include "GUI/Panes/JobBuilderPane/WorkflowGraphEditorWindow.h"
#include "GUI/Panes/JobsPane/JobsPage.h"
#include "GUI/Panes/JobSetsPane/WorkflowsPage.h"
#include "GUI/Panes/SeedProbePane/SeedProbePage.h"
#include "GUI/Tabs/AnalysisTab.h"
#include "GUI/Tabs/RunningTab.h"
#include "GUI/Tabs/SetupTab.h"
#include "GUI/Tabs/TasRoutesTab.h"
#include "GUI/Panes/BattleRunsPane/VictoryResultsWidget.h"
#include "GUI/Panes/FirstBattleCoveragePane/FirstBattleCoverageWidget.h"
#include "GUI/Widgets/PersistentToolWindow.h"
#include "DB/SavorDbArtifactService.h"
#include "DB/SavorDbAuthoringService.h"
#include "DB/SavorDbExplorerRunService.h"
#include "DB/SavorDbJobService.h"
#include "DB/SavorDbWorkflowService.h"
#include "SavorDbRuntime.h"

#include <QtCore/QStringList>
#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QFutureWatcher>
#include <QtGui/QAction>
#include <QtGui/QCursor>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <optional>
#include <exception>

namespace {
constexpr int kTopBarHeight = 24;

QString focusedToolKey(MainWindow::FocusedTool tool)
{
    switch (tool) {
    case MainWindow::FocusedTool::Workflows: return QStringLiteral("workflows");
    case MainWindow::FocusedTool::Jobs: return QStringLiteral("jobs");
    case MainWindow::FocusedTool::Workers: return QStringLiteral("workers");
    case MainWindow::FocusedTool::Artifacts: return QStringLiteral("artifacts");
    case MainWindow::FocusedTool::SeedProbe: return QStringLiteral("seed_probe");
    case MainWindow::FocusedTool::BattleRuns: return QStringLiteral("battle_runs");
    case MainWindow::FocusedTool::ArchiveWorkbench: return QStringLiteral("archive_workbench");
    case MainWindow::FocusedTool::DtmEditor: return QStringLiteral("dtm_editor");
    case MainWindow::FocusedTool::Settings: return QStringLiteral("settings");
    }
    return QStringLiteral("unknown");
}

QString focusedToolTitle(MainWindow::FocusedTool tool)
{
    switch (tool) {
    case MainWindow::FocusedTool::Workflows: return QStringLiteral("Workflows");
    case MainWindow::FocusedTool::Jobs: return QStringLiteral("Jobs");
    case MainWindow::FocusedTool::Workers: return QStringLiteral("Workers");
    case MainWindow::FocusedTool::Artifacts: return QStringLiteral("Artifacts");
    case MainWindow::FocusedTool::SeedProbe: return QStringLiteral("Seed Probe");
    case MainWindow::FocusedTool::BattleRuns: return QStringLiteral("Battle Runs");
    case MainWindow::FocusedTool::ArchiveWorkbench: return QStringLiteral("Archive Workbench");
    case MainWindow::FocusedTool::DtmEditor: return QStringLiteral("DTM Editor");
    case MainWindow::FocusedTool::Settings: return QStringLiteral("Settings");
    }
    return QStringLiteral("Tool");
}
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    coordinatorPane_ = new CoordinatorPane(this);
    coordinatorController_ = coordinatorPane_->controller();
    createWidgets();
    createMenus();

    connect(this, &MainWindow::coordinatorStateChanged, statusBarWidget_, &StatusBarWidget::setCoordinatorState);
    connect(coordinatorController_, &CoordinatorController::stateChanged, this, &MainWindow::syncStatusBar);
    connect(coordinatorController_, &CoordinatorController::snapshotChanged, this, &MainWindow::syncStatusBar);
    connect(&statusBarRefreshTimer_, &QTimer::timeout, this, &MainWindow::syncStatusBar);
    statusBarRefreshTimer_.start(1000);
    syncStatusBar();

    setWindowTitle("Skies of Arcadia Simulator");

    constexpr int kDefaultWindowWidth = 1440;
    constexpr int kDefaultWindowHeight = 900;
    QScreen* targetScreen = screen();
    if (!targetScreen) {
        targetScreen = QGuiApplication::screenAt(QCursor::pos());
    }
    if (!targetScreen) {
        targetScreen = QGuiApplication::primaryScreen();
    }

    QSize defaultWindowSize(kDefaultWindowWidth, kDefaultWindowHeight);
    if (targetScreen) {
        defaultWindowSize = defaultWindowSize.boundedTo(targetScreen->availableGeometry().size());
    }
    resize(defaultWindowSize);
}

MainWindow::~MainWindow()
{
    shutdownCoordinator();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    QMainWindow::closeEvent(event);
    if (!event->isAccepted()) {
        return;
    }
    if (coordinatorPane_ != nullptr) {
        coordinatorPane_->closeVisualWorkersForApplicationClose();
    }
    shutdownCoordinator();
}

void MainWindow::shutdownCoordinator()
{
    if (shutdownCoordinatorStarted_) {
        return;
    }
    shutdownCoordinatorStarted_ = true;
    statusBarRefreshTimer_.stop();
    if (coordinatorController_) {
        coordinatorController_->stopCoordinator();
        disconnect(coordinatorController_, nullptr, this, nullptr);
    }
}

void MainWindow::handleWorkspaceChanged(int currentIndex)
{
    setWorkspaceIndex(currentIndex);
}

void MainWindow::handleCoordinatorSettingsNavigation(CoordinatorPane::SettingsFocusTarget target)
{
    setWorkspaceIndex(0);
    SettingsPage::CoordinatorFocusTarget focusTarget = SettingsPage::CoordinatorFocusTarget::Section;
    switch (target) {
    case CoordinatorPane::SettingsFocusTarget::IsoPath:
        focusTarget = SettingsPage::CoordinatorFocusTarget::IsoPath;
        break;
    case CoordinatorPane::SettingsFocusTarget::DolphinBaseDir:
        focusTarget = SettingsPage::CoordinatorFocusTarget::DolphinBaseDir;
        break;
    case CoordinatorPane::SettingsFocusTarget::CoordinatorSection:
        focusTarget = SettingsPage::CoordinatorFocusTarget::Section;
        break;
    }
    openSettingsTool(focusTarget);
}

void MainWindow::handleVisualReplayRequested(qint64 jobId)
{
    if (coordinatorPane_) {
        coordinatorPane_->requestVisualReplay(jobId);
    }
}

void MainWindow::createMenus()
{
    auto* specsMenu = menuBar()->addMenu(QStringLiteral("Spec Authoring"));
    connect(specsMenu->addAction(QStringLiteral("Authoring Libraries")), &QAction::triggered, this, &MainWindow::openAuthoringLibraryLast);
    specsMenu->addSeparator();
    connect(specsMenu->addAction(QStringLiteral("Seed Probe Specs")), &QAction::triggered, this, &MainWindow::openSeedProbeSpecLibrary);
    connect(specsMenu->addAction(QStringLiteral("Predicates")), &QAction::triggered, this, &MainWindow::openPredicateLibrary);
    connect(specsMenu->addAction(QStringLiteral("Predicate Groups")), &QAction::triggered, this, &MainWindow::openPredicateGroupLibrary);
    specsMenu->addSeparator();
    connect(specsMenu->addAction(QStringLiteral("Battle Plans")), &QAction::triggered, this, &MainWindow::openBattlePlanSpecLibrary);

    auto* analysisMenu = menuBar()->addMenu(QStringLiteral("Analysis"));
    connect(analysisMenu->addAction(QStringLiteral("Seed Probe Results")), &QAction::triggered, this, [this]() {
        openFocusedTool(FocusedTool::SeedProbe);
    });
    connect(analysisMenu->addAction(QStringLiteral("Battle Runs")), &QAction::triggered, this, [this]() {
        showBattleRunsAnalysisPane();
    });
    connect(analysisMenu->addAction(QStringLiteral("Artifacts")), &QAction::triggered, this, [this]() {
        openFocusedTool(FocusedTool::Artifacts);
    });
    connect(analysisMenu->addAction(QStringLiteral("Workflow Provenance")), &QAction::triggered, this, [this]() {
        openFocusedTool(FocusedTool::Workflows);
    });

    auto* toolsMenu = menuBar()->addMenu(QStringLiteral("Tools"));
    connect(toolsMenu->addAction(QStringLiteral("Archive Workbench")), &QAction::triggered, this, [this]() {
        openFocusedTool(FocusedTool::ArchiveWorkbench);
    });
}

void MainWindow::openAuthoringLibraryLast()
{
    openAuthoringLibrary(lastAuthoringLibrary_);
}

void MainWindow::openAuthoringLibrary(AuthoringLibraryKey key)
{
    lastAuthoringLibrary_ = key;
    if (authoringLibraryWindow_) {
        authoringLibraryWindow_->show();
        authoringLibraryWindow_->selectLibrary(key);
        authoringLibraryWindow_->raise();
        authoringLibraryWindow_->activateWindow();
        return;
    }

    authoringLibraryWindow_ = new AuthoringLibraryWindow(this);
    connect(authoringLibraryWindow_.data(), &AuthoringLibraryWindow::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
    authoringLibraryWindow_->selectLibrary(key);
    authoringLibraryWindow_->show();
}

void MainWindow::openSeedProbeSpecLibrary() { openAuthoringLibrary(AuthoringLibraryKey::SeedProbe); }
void MainWindow::openBattlePlanSpecLibrary() { openAuthoringLibrary(AuthoringLibraryKey::BattlePlan); }
void MainWindow::openPredicateLibrary() { openAuthoringLibrary(AuthoringLibraryKey::Predicates); }
void MainWindow::openPredicateGroupLibrary() { openAuthoringLibrary(AuthoringLibraryKey::PredicateGroups); }

void MainWindow::syncStatusBar()
{
    if (!statusBarWidget_) {
        return;
    }
    if (coordinatorController_ && coordinatorController_->isRunning()) {
        lastCoordinatorRefresh_ = QDateTime::currentDateTime();
    }
    statusBarWidget_->setSnapshot(StatusBarWidget::buildSnapshot(coordinatorController_, lastCoordinatorRefresh_));
    if (coordinatorController_ != nullptr && !coordinatorController_->warningSnapshot().empty()) {
        const auto& warning = coordinatorController_->warningSnapshot().back();
        const QString signature = QStringLiteral("%1|%2|%3|%4")
            .arg(static_cast<qulonglong>(warning.sequence))
            .arg(warning.worker_id)
            .arg(warning.job_id)
            .arg(QString::fromStdString(warning.message));
        if (signature != lastCoordinatorWarningToastSignature_) {
            lastCoordinatorWarningToastSignature_ = signature;
            statusBarWidget_->postToast(StatusToast{
                StatusToast::Severity::Warn,
                QString::fromStdString(warning.message),
                QString::fromStdString(warning.detail),
                1,
                QDateTime::currentDateTimeUtc(),
                7000
            });
        }
    } else {
        lastCoordinatorWarningToastSignature_.clear();
    }
    emitCoordinatorStateChanged();
}

void MainWindow::createWidgets()
{
    QWidget* root = new QWidget(this);
    root->setObjectName("mainRoot");
    root->setStyleSheet(QStringLiteral(
        "QFrame#workspaceHeroPanel {"
        "  border: 1px solid #33443f;"
        "  border-radius: 14px;"
        "  background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #17211f, stop:1 #0f1719);"
        "  color: #eef7f2;"
        "}"
        "QFrame#workspaceMetricTile {"
        "  border: 1px solid #3a4d49;"
        "  border-radius: 12px;"
        "  background: #1b2826;"
        "  color: #eef7f2;"
        "}"
        "QFrame#workspaceInfoPanel {"
        "  border: 1px solid #5a4e34;"
        "  border-radius: 12px;"
        "  background: #251f13;"
        "  color: #fff2cf;"
        "}"
        "QFrame#workspaceToolCard {"
        "  border: 1px solid #35413f;"
        "  border-radius: 12px;"
        "  background: #151d20;"
        "  color: #edf5f1;"
        "}"
        "QFrame#workspaceToolCard:hover {"
        "  border-color: #5f9c8d;"
        "  background: #1a2729;"
        "}"
        "QFrame#workspaceContextDrawer {"
        "  border: 1px solid #33443f;"
        "  border-radius: 12px;"
        "  background: #121b1e;"
        "  color: #edf5f1;"
        "}"
        "QFrame#workspaceHeroPanel QLabel,"
        "QFrame#workspaceMetricTile QLabel,"
        "QFrame#workspaceToolCard QLabel,"
        "QFrame#workspaceContextDrawer QLabel {"
        "  color: #edf5f1;"
        "}"
        "QFrame#workspaceInfoPanel QLabel {"
        "  color: #fff2cf;"
        "}"));
    setCentralWidget(root);

    auto* rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    statusBarWidget_ = createStatusBarWidget();
    connect(
        coordinatorPane_,
        &CoordinatorPane::statusToastRequested,
        statusBarWidget_,
        qOverload<StatusToast>(&StatusBarWidget::postToast));
    connect(
        coordinatorPane_,
        &CoordinatorPane::settingsNavigationRequested,
        this,
        &MainWindow::handleCoordinatorSettingsNavigation);
    workspaceStack_ = new QStackedWidget(root);
    workspaceStack_->setObjectName("workspaceStack");
	setupTab_ = new SetupTab(coordinatorController_, root);
	connect(setupTab_, &SetupTab::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
    connect(setupTab_, &SetupTab::openAuthoringRequested, this, &MainWindow::openAuthoringLibraryLast);
    connect(setupTab_, &SetupTab::openGraphEditorRequested, this, [this]() {this->openWorkflowGraphEditor(); });
    connect(setupTab_, &SetupTab::openGraphEditorSnapshotRequested, this, [this](const savor::db::WorkflowGraphSnapshot snapshot, bool duplicate) {this->openWorkflowGraphEditor(snapshot, duplicate); });
    connect(setupTab_, &SetupTab::openSettingsRequested, this, [this]() { this->openSettingsTool(); });
    connect(setupTab_, &SetupTab::openIsoSettingsRequested, this, [this]() {this->openSettingsTool(SettingsPage::CoordinatorFocusTarget::IsoPath); });
    connect(setupTab_, &SetupTab::openDolphinSettingsRequested, this, [this]() {this->openSettingsTool(SettingsPage::CoordinatorFocusTarget::DolphinBaseDir); });
    connect(setupTab_, &SetupTab::openArtifactsRequested, this, [this]() {this->openFocusedTool(FocusedTool::Artifacts); });
    connect(setupTab_, &SetupTab::openDtmEditorRequested, this, [this]() {this->openFocusedTool(FocusedTool::DtmEditor); });
	workspaceStack_->addWidget(setupTab_);
	runningTab_ = new RunningTab(coordinatorController_, RunningTab::Actions{
		[this]() { openFocusedTool(FocusedTool::Workflows); },
		[this](qint64 workflowId) { openWorkflow(workflowId); },
		[this](qint64 workflowId) { retryWorkflowJobs(workflowId); },
		[this](qint64 expansionId) { openFirstBattleCoverage(expansionId); },
		[this]() { openFocusedTool(FocusedTool::Jobs); },
		[this]() { openFocusedTool(FocusedTool::Workers); },
		[this]() { openSettingsTool(SettingsPage::CoordinatorFocusTarget::Section); },
		[this]() { openSettingsTool(SettingsPage::CoordinatorFocusTarget::IsoPath); },
		[this]() { openSettingsTool(SettingsPage::CoordinatorFocusTarget::DolphinBaseDir); },
		[this]() { coordinatorPane_->startCoordinator(); },
		[this]() { coordinatorPane_->stopCoordinator(); },
		[this]() { coordinatorPane_->togglePaused(); },
		[this](int targetWorkers) { coordinatorPane_->setTargetWorkers(targetWorkers); },
		[this](bool enabled) { coordinatorPane_->setVisualWorkerPoolEnabled(enabled); }
		}, root);
	workspaceStack_->addWidget(runningTab_);
    analysisTab_ = new AnalysisTab(AnalysisTab::Actions{
        [this]() { openFocusedTool(FocusedTool::SeedProbe); },
        [this]() { showBattleRunsAnalysisPane(); },
        [this]() { openFocusedTool(FocusedTool::Artifacts); },
        [this]() { openFocusedTool(FocusedTool::Workflows); },
        [this](qint64 jobId) { handleVisualReplayRequested(jobId); },
        [this](qint64 turnJobId) { recordBattleVictory(turnJobId); },
        [this](const QString& unitKind,const QString& inputKey,qint64 refId){showSetupLauncherPreselected(unitKind,inputKey,refId);}
    }, root);
    workspaceStack_->addWidget(analysisTab_);
    tasRoutesTab_ = new TasRoutesTab(TasRoutesTab::Actions{
        [this](qint64 routeNodeId) { openVictoryResults(routeNodeId); },
        [this](qint64 workflowId) { openWorkflow(workflowId); },
        [this]() { openFirstBattleCoverage(); }
    }, root);
    workspaceStack_->addWidget(tasRoutesTab_);

    workspaceSelector_ = new savorqt::gui::WorkspaceSelectorBar(root);
    workspaceSelector_->setSelectionChangedCallback([this](int index) {
        handleWorkspaceChanged(index);
    });
    workspaceBadgeRefreshPipeline_ = new savorqt::gui::DatabaseProjectionController<int, QPair<int, int>>(this);
    workspaceBadgeRefreshPipeline_->setAutoRefreshEnabled(false);
    workspaceBadgeRefreshPipeline_->setRequestBuilder([](savorqt::gui::RefreshReason) {
        return 0;
    });
    workspaceBadgeRefreshPipeline_->setLoadAndPrepare([](int) {
        savor::db::UiReadJobListQuery query{};
        const auto jobs = savorqt::db::SavorDbJobService::FetchJobsPage(query, std::nullopt, std::nullopt, 50);
        int activeJobs = 0;
        if (jobs.ok) {
            for (const auto& job : jobs.value.items) {
                const QString state = QString::fromStdString(job.state);
                if (state == QStringLiteral("RUNNING") || state == QStringLiteral("CLAIMED") || state == QStringLiteral("QUEUED")) {
                    ++activeJobs;
                }
            }
        }

        savorqt::db::BattleRunGroupQuery battleQuery{};
        battleQuery.limit = 10;
        const auto battleRuns = savorqt::db::SavorDbExplorerRunService::ListBattleGroups(battleQuery);
        const int battleGroups = battleRuns.ok ? static_cast<int>(battleRuns.value.groups.size()) : 0;
        return savorqt::gui::ProjectionLoadResult<QPair<int, int>>::Ok(qMakePair(activeJobs, battleGroups));
    });
    workspaceBadgeRefreshPipeline_->setApply([this](const QPair<int, int>& counts, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        if (workspaceSelector_ == nullptr) {
            return;
        }
        workspaceSelector_->setBadge(1, counts.first > 0 ? QString::number(counts.first) : QString());
        workspaceSelector_->setBadge(2, counts.second > 0 ? QString::number(counts.second) : QString());
    });
    workspaceBadgeRefreshPipeline_->setActive(true);

    rootLayout->addWidget(workspaceStack_, 1);
    rootLayout->addWidget(workspaceSelector_);
    rootLayout->addWidget(statusBarWidget_);
    refreshWorkspaceBadges();
    setWorkspaceIndex(0);
}

void MainWindow::refreshWorkspaceBadges()
{
    if (workspaceSelector_ == nullptr) {
        return;
    }

    const bool setupWarning = !savorqt::SavorDbRuntime::instance().isRunning()
        || !savorqt::db::SavorDbArtifactService::StorageReady()
        || (coordinatorController_ != nullptr && !coordinatorController_->validationMessage().isEmpty());
    workspaceSelector_->setBadge(0, setupWarning ? QStringLiteral("!") : QString());

    if (workspaceBadgeRefreshPipeline_ != nullptr) {
        workspaceBadgeRefreshPipeline_->requestRefresh(savorqt::gui::RefreshReason::Manual);
    }
}

void MainWindow::showBattleRunsAnalysisPane()
{
    setWorkspaceIndex(2);
    if (analysisTab_ != nullptr) {
        analysisTab_->showBattleRunsPane();
    }
}

void MainWindow::waitForCoordinatorShutdown()
{
    if (coordinatorController_) {
        coordinatorController_->waitForShutdown();
    }
}

void MainWindow::showSetupLauncherPreselected(const QString& unitKind,const QString& inputKey,qint64 refId)
{
    if (setupTab_ == nullptr || workspaceStack_ == nullptr) {
        return;
    }
    setWorkspaceIndex(workspaceStack_->indexOf(setupTab_));
    setupTab_->showWorkflowLauncher(unitKind, inputKey, refId);
}

void MainWindow::openFocusedTool(FocusedTool tool)
{
    if (tool == FocusedTool::Workers) {
        coordinatorPane_->show();
        coordinatorPane_->raise();
        coordinatorPane_->activateWindow();
        return;
    }
    if (tool == FocusedTool::Settings) {
        openSettingsTool();
        return;
    }
    if (tool == FocusedTool::BattleRuns) {
        showBattleRunsAnalysisPane();
        return;
    }

    const QString key = focusedToolKey(tool);
    PersistentToolWindow* window = createFocusedWindow(key, focusedToolTitle(tool));
    if (window == nullptr) {
        return;
    }

    QWidget* page = nullptr;
    switch (tool) {
    case FocusedTool::Workflows: {
        auto* workflows = new WorkflowsPage(window);
        workflows->setPageActive(true);
        connect(window, &PersistentToolWindow::aboutToClose, workflows, [workflows]() { workflows->setPageActive(false); });
        connect(workflows, &WorkflowsPage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
        connect(workflows, &WorkflowsPage::retryFailedJobsRequested, this, &MainWindow::retryWorkflowJobs);
        connect(workflows, &WorkflowsPage::openJobRequested, this, &MainWindow::openJob);
        page = workflows;
        break;
    }
    case FocusedTool::Jobs: {
        auto* jobs = new JobsPage(window);
        jobs->setPageActive(true);
        connect(window, &PersistentToolWindow::aboutToClose, jobs, [jobs]() { jobs->setPageActive(false); });
        connect(jobs, &JobsPage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
        connect(jobs, &JobsPage::visualReplayRequested, this, &MainWindow::handleVisualReplayRequested);
        page = jobs;
        break;
    }
    case FocusedTool::Workers: {
        return;
    }
    case FocusedTool::Artifacts: {
        auto* artifacts = new ArtifactsPage(window);
        connect(artifacts, &ArtifactsPage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
        page = artifacts;
        break;
    }
    case FocusedTool::SeedProbe: {
        auto* seedProbe = new SeedProbePage(window);
        seedProbe->setPageActive(true);
        connect(window, &PersistentToolWindow::aboutToClose, seedProbe, [seedProbe]() { seedProbe->setPageActive(false); });
        connect(seedProbe, &SeedProbePage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
        page = seedProbe;
        break;
    }
    case FocusedTool::BattleRuns:
        break;
    case FocusedTool::ArchiveWorkbench: {
        auto* archive = new ArchiveWorkbenchPage(window);
        archive->setPageActive(true);
        connect(window, &PersistentToolWindow::aboutToClose, archive, [archive]() { archive->setPageActive(false); });
        connect(archive, &ArchiveWorkbenchPage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
        page = archive;
        break;
    }
    case FocusedTool::DtmEditor: {
        auto* dtm = new DtmEditorPage(window);
        dtm->setPageActive(true);
        connect(window, &PersistentToolWindow::aboutToClose, dtm, [dtm]() { dtm->setPageActive(false); });
        connect(dtm, &DtmEditorPage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
        page = dtm;
        break;
    }
    case FocusedTool::Settings:
        break;
    }

    if (page != nullptr) {
        window->layout()->addWidget(page);
    }
    window->show();
}

void MainWindow::openWorkflow(std::int64_t workflowInstanceId)
{
    const QString key = focusedToolKey(FocusedTool::Workflows);
    if (focusedWindows_.value(key) == nullptr) {
        openFocusedTool(FocusedTool::Workflows);
    }
    if (PersistentToolWindow* window = focusedWindows_.value(key); window != nullptr) {
        if (auto* workflows = window->findChild<WorkflowsPage*>()) {
            workflows->showWorkflow(workflowInstanceId);
        }
        window->show();
        window->raise();
        window->activateWindow();
    }
}

void MainWindow::openJob(std::int64_t jobId)
{
    const QString key = focusedToolKey(FocusedTool::Jobs);
    if (focusedWindows_.value(key) == nullptr) {
        openFocusedTool(FocusedTool::Jobs);
    }
    if (PersistentToolWindow* window = focusedWindows_.value(key); window != nullptr) {
        if (auto* jobs = window->findChild<JobsPage*>()) {
            jobs->showJob(jobId);
        }
        window->show();
        window->raise();
        window->activateWindow();
    }
}

void MainWindow::openVictoryResults(std::int64_t routeNodeId)
{
    const QString key = QStringLiteral("victory_results");
    if (focusedWindows_.value(key) == nullptr) {
        PersistentToolWindow* window = createFocusedWindow(key, QStringLiteral("Victory Results"));
        if (window != nullptr) {
            auto* page = new VictoryResultsWidget(VictoryResultsWidget::Actions{
                [this](qint64 turnJobId) { recordBattleVictory(turnJobId); },
                [this](qint64 workflowId) { openWorkflow(workflowId); },
                [this](qint64 jobId) { openJob(jobId); },
                [this]() { openFocusedTool(FocusedTool::Artifacts); }
            }, window);
            window->layout()->addWidget(page);
        }
    }
    if (PersistentToolWindow* window = focusedWindows_.value(key); window != nullptr) {
        for (QWidget* child : window->findChildren<QWidget*>()) {
            if (auto* page = dynamic_cast<VictoryResultsWidget*>(child)) {
                page->showRoute(routeNodeId);
                break;
            }
        }
        window->show(); window->raise(); window->activateWindow();
    }
}

void MainWindow::openFirstBattleCoverage(std::int64_t workflowExpansionId)
{
    const QString key = QStringLiteral("first_battle_coverage");
    if (focusedWindows_.value(key) == nullptr) {
        PersistentToolWindow* window = createFocusedWindow(
            key, QStringLiteral("First Battle Coverage"));
        if (window != nullptr) {
            auto* page = new FirstBattleCoverageWidget({
                [this](qint64 workflowId) { openWorkflow(workflowId); }
            }, window);
            page->setPageActive(true);
            connect(window, &PersistentToolWindow::aboutToClose, page,
                [page]() { page->setPageActive(false); });
            connect(page, &FirstBattleCoverageWidget::statusToastRequested,
                statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
            window->layout()->addWidget(page);
        }
    }
    if (PersistentToolWindow* window = focusedWindows_.value(key); window != nullptr) {
        if (auto* page = window->findChild<FirstBattleCoverageWidget*>()) {
            if (workflowExpansionId > 0) page->showExpansion(workflowExpansionId);
            else page->setPageActive(true);
        }
        window->show();
        window->raise();
        window->activateWindow();
    }
}

void MainWindow::retryWorkflowJobs(std::int64_t workflowInstanceId)
{
    if (workflowRetryInFlight_ || workflowInstanceId <= 0) {
        return;
    }
    workflowRetryInFlight_ = true;
    const QString workflowsKey = focusedToolKey(FocusedTool::Workflows);
    if (PersistentToolWindow* window = focusedWindows_.value(workflowsKey); window != nullptr) {
        if (auto* workflows = window->findChild<WorkflowsPage*>()) {
            workflows->setWorkflowRetryInFlight(true);
        }
    }

    using RetryResult = savorqt::db::ServiceResult<savor::db::WorksetJobReorganizationReceipt>;
    auto* watcher = new QFutureWatcher<RetryResult>(this);
    connect(watcher, &QFutureWatcher<RetryResult>::finished, this, [this, watcher, workflowInstanceId, workflowsKey]() {
        workflowRetryInFlight_ = false;
        QString message;
        StatusToast::Severity severity = StatusToast::Severity::Error;
        try {
            const RetryResult result = watcher->result();
            if (result.ok) {
                message = QStringLiteral("Requeued %1 failed or interrupted job(s) into %2 workset(s).")
                    .arg(result.value.requeued_job_count)
                    .arg(result.value.created_workset_count);
                severity = StatusToast::Severity::Success;
            } else {
                message = QStringLiteral("Retry failed jobs failed: %1")
                    .arg(QString::fromStdString(result.error.message));
            }
        } catch (const std::exception& exception) {
            message = QStringLiteral("Retry failed jobs failed: %1")
                .arg(QString::fromUtf8(exception.what()));
        } catch (...) {
            message = QStringLiteral("Retry failed jobs failed: unknown exception");
        }
        watcher->deleteLater();
        if (statusBarWidget_ != nullptr) {
            statusBarWidget_->postToast(StatusToast{ severity, message });
        }
        if (runningTab_ != nullptr) {
            runningTab_->requestRefresh();
        }
        if (PersistentToolWindow* window = focusedWindows_.value(workflowsKey); window != nullptr) {
            if (auto* workflows = window->findChild<WorkflowsPage*>()) {
                workflows->setWorkflowRetryInFlight(false);
                workflows->refreshAfterRetry(workflowInstanceId);
            }
        }
    });
    watcher->setFuture(QtConcurrent::run([workflowInstanceId]() {
        return savorqt::db::SavorDbWorkflowService::RetryFailedJobs(workflowInstanceId);
    }));
}

void MainWindow::recordBattleVictory(std::int64_t turnJobId)
{
    if (battleVictoryRecordingInFlight_ || turnJobId <= 0) return;
    battleVictoryRecordingInFlight_ = true;
    auto* watcher = new QFutureWatcher<
        savorqt::db::ServiceResult<savorqt::db::RecordBattleVictoryResult>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher]() {
        const auto result = watcher->result();
        watcher->deleteLater();
        battleVictoryRecordingInFlight_ = false;
        if (!result.ok) {
            statusBarWidget_->postToast(StatusToast{
                StatusToast::Severity::Error,
                QStringLiteral("Record Victory failed: %1")
                    .arg(QString::fromStdString(result.error.message))});
            return;
        }
        statusBarWidget_->postToast(StatusToast{
            StatusToast::Severity::Info,
            result.value.focused_existing_completion
                ? QStringLiteral("Focused the existing Victory completion workflow.")
                : QStringLiteral("Battle recording workflow started.")});
        if (analysisTab_) analysisTab_->requestBattleRunsRefresh();
        openWorkflow(result.value.workflow_instance_id);
    });
    watcher->setFuture(QtConcurrent::run([turnJobId]() {
        return savorqt::db::SavorDbWorkflowService::RecordBattleVictory(
            turnJobId);
    }));
}

void MainWindow::openWorkflowGraphEditor()
{
    if (workflowGraphEditor_) {
        workflowGraphEditor_->show();
        workflowGraphEditor_->raise();
        workflowGraphEditor_->activateWindow();
        return;
    }

    auto* editor = new WorkflowGraphEditorWindow(nullptr);
    workflowGraphEditor_ = editor;
    editor->setStatusCallback([this](const QString& text, StatusToast::Severity severity) {
        if (statusBarWidget_ != nullptr) {
            statusBarWidget_->postToast(StatusToast{ severity, text });
        }
    });
    editor->setSavedCallback([this]() { refreshWorkspaceBadges(); });
    connect(editor, &QObject::destroyed, this, [this]() { workflowGraphEditor_.clear(); });
    editor->show();
}

void MainWindow::openWorkflowGraphEditor(const savor::db::WorkflowGraphSnapshot& snapshot, bool duplicate)
{
    openWorkflowGraphEditor();
    if (workflowGraphEditor_) {
        workflowGraphEditor_->loadSnapshot(snapshot, duplicate);
        workflowGraphEditor_->show();
        workflowGraphEditor_->raise();
        workflowGraphEditor_->activateWindow();
    }
}

void MainWindow::openSettingsTool(SettingsPage::CoordinatorFocusTarget focusTarget)
{
    const QString key = focusedToolKey(FocusedTool::Settings);
    if (PersistentToolWindow* existing = focusedWindows_.value(key); existing != nullptr) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        if (auto* settings = existing->findChild<SettingsPage*>()) {
            settings->focusCoordinatorSettings(focusTarget);
        }
        return;
    }

    PersistentToolWindow* window = createFocusedWindow(key, focusedToolTitle(FocusedTool::Settings));
    if (window == nullptr) {
        return;
    }
    auto* settings = new SettingsPage(coordinatorController_, window);
    settings->setObjectName("focusedSettingsPage");
    connect(settings, &SettingsPage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
    window->layout()->addWidget(settings);
    settings->focusCoordinatorSettings(focusTarget);
    window->show();
}

PersistentToolWindow* MainWindow::createFocusedWindow(const QString& key, const QString& title)
{
    if (PersistentToolWindow* existing = focusedWindows_.value(key); existing != nullptr) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return nullptr;
    }

    auto* window = new PersistentToolWindow(this);
    window->setObjectName("focusedToolDialog");
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setWindowTitle(title);
    window->resize(1240, 780);
    auto* layout = new QVBoxLayout(window);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(10);
    focusedWindows_.insert(key, window);
    connect(window, &QObject::destroyed, this, [this, key]() {
        focusedWindows_.remove(key);
    });
    return window;
}

void MainWindow::setWorkspaceIndex(int index)
{
    if (workspaceStack_ == nullptr || index < 0 || index >= workspaceStack_->count()) {
        return;
    }
    workspaceStack_->setCurrentIndex(index);
    if (workspaceSelector_ != nullptr) {
        workspaceSelector_->setCurrentIndex(index);
    }
    if (analysisTab_ != nullptr) {
        analysisTab_->setPageActive(index == 2);
    }
    if (tasRoutesTab_ != nullptr) {
        tasRoutesTab_->setPageActive(index == 3);
    }
}

StatusBarWidget* MainWindow::createStatusBarWidget()
{
    return new StatusBarWidget(this);
}

void MainWindow::emitCoordinatorStateChanged()
{
    if (!coordinatorController_) {
        return;
    }
    emit coordinatorStateChanged(
        coordinatorController_->lifecycleState(),
        coordinatorController_->isPaused(),
        coordinatorController_->targetWorkers(),
        coordinatorController_->activeWorkers(),
        coordinatorController_->validationMessage());
}
