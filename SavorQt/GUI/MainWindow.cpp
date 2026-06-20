#include "MainWindow.h"

#include "GUI/Panes/ArtifactsPane/ArtifactsPage.h"
#include "GUI/Panes/ArchivePane/ArchiveWorkbenchPage.h"
#include "GUI/Panes/BattleRunSettingsPane/BattleRunSettingsPage.h"
#include "GUI/Panes/JobBuilderPane/WorkflowGraphEditorWindow.h"
#include "GUI/Panes/JobBuilderPane/WorkflowLauncherPage.h"
#include "GUI/Panes/JobsPane/JobsPage.h"
#include "GUI/Panes/JobSetsPane/WorkflowsPage.h"
#include "GUI/Panes/SeedProbePane/SeedProbePage.h"
#include "GUI/Tabs/AnalysisTab.h"
#include "GUI/Tabs/RunningTab.h"
#include "GUI/Tabs/SetupTab.h"
#include "DB/SavorDbArtifactService.h"
#include "DB/SavorDbAuthoringService.h"
#include "DB/SavorDbExplorerRunService.h"
#include "DB/SavorDbJobService.h"
#include "DB/SavorDbWorkflowService.h"
#include "SavorDbRuntime.h"

#include <QtCore/QStringList>
#include <QtGui/QAction>
#include <QtGui/QCursor>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtWidgets/QDialog>
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

namespace {
constexpr int kTopBarHeight = 24;

QString focusedToolKey(MainWindow::FocusedTool tool)
{
    switch (tool) {
    case MainWindow::FocusedTool::Workflows: return QStringLiteral("workflows");
    case MainWindow::FocusedTool::Jobs: return QStringLiteral("jobs");
    case MainWindow::FocusedTool::Workers: return QStringLiteral("workers");
    case MainWindow::FocusedTool::WorkflowLauncher: return QStringLiteral("workflow_launcher");
    case MainWindow::FocusedTool::BattleRunSettings: return QStringLiteral("battle_run_settings");
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
    case MainWindow::FocusedTool::WorkflowLauncher: return QStringLiteral("Workflow Launcher");
    case MainWindow::FocusedTool::BattleRunSettings: return QStringLiteral("Battle Run Settings");
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
    coordinatorController_ = new CoordinatorController(this);
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
    ensureVisualReplayHost();
    if (visualReplayHost_) {
        visualReplayHost_->requestVisualReplay(jobId);
    }
}

void MainWindow::createMenus()
{
    auto* specsMenu = menuBar()->addMenu(QStringLiteral("Spec Authoring"));
    connect(specsMenu->addAction(QStringLiteral("Authoring Dialog")), &QAction::triggered, this, &MainWindow::openAuthoringLibraryLast);
    specsMenu->addSeparator();
    connect(specsMenu->addAction(QStringLiteral("Seed Probe Specs")), &QAction::triggered, this, &MainWindow::openSeedProbeSpecLibrary);
    connect(specsMenu->addAction(QStringLiteral("TAS Specs")), &QAction::triggered, this, &MainWindow::openTasSpecLibrary);
    connect(specsMenu->addAction(QStringLiteral("Battle Run Specs")), &QAction::triggered, this, &MainWindow::openBattleRunSpecLibrary);
    specsMenu->addSeparator();
    connect(specsMenu->addAction(QStringLiteral("Battle Explorer Settings")), &QAction::triggered, this, &MainWindow::openExplorerSettingsSpecLibrary);
    connect(specsMenu->addAction(QStringLiteral("Battle Plans")), &QAction::triggered, this, &MainWindow::openBattlePlanSpecLibrary);
    connect(specsMenu->addAction(QStringLiteral("Predicates")), &QAction::triggered, this, &MainWindow::openPredicateSpecLibrary);
    connect(specsMenu->addAction(QStringLiteral("Predicate Sets")), &QAction::triggered, this, &MainWindow::openPredicateSetSpecLibrary);

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
    connect(analysisMenu->addAction(QStringLiteral("Archive Workbench")), &QAction::triggered, this, [this]() {
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
    if (authoringLibraryDialog_) {
        authoringLibraryDialog_->show();
        authoringLibraryDialog_->selectLibrary(key);
        authoringLibraryDialog_->raise();
        authoringLibraryDialog_->activateWindow();
        return;
    }

    authoringLibraryDialog_ = new AuthoringLibraryDialog(this);
    connect(authoringLibraryDialog_.data(), &AuthoringLibraryDialog::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
    authoringLibraryDialog_->selectLibrary(key);
    authoringLibraryDialog_->show();
}

void MainWindow::openSeedProbeSpecLibrary() { openAuthoringLibrary(AuthoringLibraryKey::SeedProbe); }
void MainWindow::openTasSpecLibrary() { openAuthoringLibrary(AuthoringLibraryKey::Tas); }
void MainWindow::openBattleRunSpecLibrary() { openAuthoringLibrary(AuthoringLibraryKey::BattleRun); }
void MainWindow::openPredicateSpecLibrary() { openAuthoringLibrary(AuthoringLibraryKey::Predicate); }
void MainWindow::openPredicateSetSpecLibrary() { openAuthoringLibrary(AuthoringLibraryKey::PredicateSet); }
void MainWindow::openBattlePlanSpecLibrary() { openAuthoringLibrary(AuthoringLibraryKey::BattlePlan); }
void MainWindow::openExplorerSettingsSpecLibrary() { openAuthoringLibrary(AuthoringLibraryKey::ExplorerSettings); }

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
    workspaceStack_ = new QStackedWidget(root);
    workspaceStack_->setObjectName("workspaceStack");
    workspaceStack_->addWidget(new SetupTab(coordinatorController_, SetupTab::Actions{
        [this]() { openFocusedTool(FocusedTool::WorkflowLauncher); },
        [this]() { openAuthoringLibraryLast(); },
        [this]() { openWorkflowGraphEditor(); },
        [this](const savor::db::WorkflowGraphSnapshot& snapshot, bool duplicate) {
            openWorkflowGraphEditor(snapshot, duplicate);
        },
        [this]() { openSettingsTool(); },
        [this]() { openSettingsTool(SettingsPage::CoordinatorFocusTarget::IsoPath); },
        [this]() { openSettingsTool(SettingsPage::CoordinatorFocusTarget::DolphinBaseDir); },
        [this]() { openFocusedTool(FocusedTool::Artifacts); },
        [this]() { openFocusedTool(FocusedTool::DtmEditor); },
        [this]() { openFocusedTool(FocusedTool::BattleRunSettings); }
    }, root));
    workspaceStack_->addWidget(new RunningTab(coordinatorController_, RunningTab::Actions{
        [this]() { openFocusedTool(FocusedTool::Workflows); },
        [this]() { openFocusedTool(FocusedTool::Jobs); },
        [this]() { openFocusedTool(FocusedTool::Workers); },
        [this]() { openSettingsTool(SettingsPage::CoordinatorFocusTarget::Section); },
        [this]() { openSettingsTool(SettingsPage::CoordinatorFocusTarget::IsoPath); },
        [this]() { openSettingsTool(SettingsPage::CoordinatorFocusTarget::DolphinBaseDir); }
    }, root));
    analysisTab_ = new AnalysisTab(AnalysisTab::Actions{
        [this]() { openFocusedTool(FocusedTool::SeedProbe); },
        [this]() { showBattleRunsAnalysisPane(); },
        [this]() { openFocusedTool(FocusedTool::Artifacts); },
        [this]() { openFocusedTool(FocusedTool::Workflows); },
        [this](qint64 jobId) { handleVisualReplayRequested(jobId); }
    }, root);
    workspaceStack_->addWidget(analysisTab_);

    workspaceSelector_ = new savorqt::gui::WorkspaceSelectorBar(root);
    workspaceSelector_->setSelectionChangedCallback([this](int index) {
        handleWorkspaceChanged(index);
    });
    workspaceBadgeRefreshPipeline_ = new savorqt::gui::AsyncRefreshPipeline<int, QPair<int, int>>(this);
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
        return savorqt::gui::AsyncRefreshResult<QPair<int, int>>::Ok(qMakePair(activeJobs, battleGroups));
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

void MainWindow::openFocusedTool(FocusedTool tool)
{
    if (tool == FocusedTool::Settings) {
        openSettingsTool();
        return;
    }
    if (tool == FocusedTool::BattleRuns) {
        showBattleRunsAnalysisPane();
        return;
    }

    const QString key = focusedToolKey(tool);
    QDialog* dialog = createFocusedDialog(key, focusedToolTitle(tool));
    if (dialog == nullptr) {
        return;
    }

    QWidget* page = nullptr;
    switch (tool) {
    case FocusedTool::Workflows: {
        auto* workflows = new WorkflowsPage(dialog);
        workflows->setPageActive(true);
        connect(dialog, &QDialog::finished, workflows, [workflows]() { workflows->setPageActive(false); });
        connect(workflows, &WorkflowsPage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
        page = workflows;
        break;
    }
    case FocusedTool::Jobs: {
        auto* jobs = new JobsPage(dialog);
        jobs->setPageActive(true);
        connect(dialog, &QDialog::finished, jobs, [jobs]() { jobs->setPageActive(false); });
        connect(jobs, &JobsPage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
        connect(jobs, &JobsPage::visualReplayRequested, this, &MainWindow::handleVisualReplayRequested);
        page = jobs;
        break;
    }
    case FocusedTool::Workers: {
        auto* workers = new CoordinatorPane(coordinatorController_, dialog);
        workers->setPageActive(true);
        connect(dialog, &QDialog::finished, workers, [workers]() { workers->setPageActive(false); });
        connect(workers, &CoordinatorPane::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
        connect(workers, &CoordinatorPane::settingsNavigationRequested, this, &MainWindow::handleCoordinatorSettingsNavigation);
        page = workers;
        break;
    }
    case FocusedTool::WorkflowLauncher: {
        auto* launcher = new WorkflowLauncherPage(dialog);
        connect(launcher, &WorkflowLauncherPage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
        page = launcher;
        break;
    }
    case FocusedTool::BattleRunSettings: {
        auto* battle = new BattleRunSettingsPage(dialog);
        connect(battle, &BattleRunSettingsPage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
        page = battle;
        break;
    }
    case FocusedTool::Artifacts: {
        auto* artifacts = new ArtifactsPage(dialog);
        connect(artifacts, &ArtifactsPage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
        page = artifacts;
        break;
    }
    case FocusedTool::SeedProbe: {
        auto* seedProbe = new SeedProbePage(dialog);
        seedProbe->setPageActive(true);
        connect(dialog, &QDialog::finished, seedProbe, [seedProbe]() { seedProbe->setPageActive(false); });
        connect(seedProbe, &SeedProbePage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
        page = seedProbe;
        break;
    }
    case FocusedTool::BattleRuns:
        break;
    case FocusedTool::ArchiveWorkbench: {
        auto* archive = new ArchiveWorkbenchPage(dialog);
        archive->setPageActive(true);
        connect(dialog, &QDialog::finished, archive, [archive]() { archive->setPageActive(false); });
        connect(archive, &ArchiveWorkbenchPage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
        page = archive;
        break;
    }
    case FocusedTool::DtmEditor: {
        auto* dtm = new DtmEditorPage(dialog);
        dtm->setPageActive(true);
        connect(dialog, &QDialog::finished, dtm, [dtm]() { dtm->setPageActive(false); });
        connect(dtm, &DtmEditorPage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
        page = dtm;
        break;
    }
    case FocusedTool::Settings:
        break;
    }

    if (page != nullptr) {
        dialog->layout()->addWidget(page);
    }
    dialog->show();
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
    if (QDialog* existing = focusedDialogs_.value(key); existing != nullptr) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        if (auto* settings = existing->findChild<SettingsPage*>()) {
            settings->focusCoordinatorSettings(focusTarget);
        }
        return;
    }

    QDialog* dialog = createFocusedDialog(key, focusedToolTitle(FocusedTool::Settings));
    if (dialog == nullptr) {
        return;
    }
    auto* settings = new SettingsPage(coordinatorController_, dialog);
    settings->setObjectName("focusedSettingsPage");
    connect(settings, &SettingsPage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
    dialog->layout()->addWidget(settings);
    settings->focusCoordinatorSettings(focusTarget);
    dialog->show();
}

QDialog* MainWindow::createFocusedDialog(const QString& key, const QString& title)
{
    if (QDialog* existing = focusedDialogs_.value(key); existing != nullptr) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return nullptr;
    }

    auto* dialog = new QDialog(this);
    dialog->setObjectName("focusedToolDialog");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(title);
    dialog->resize(1240, 780);
    auto* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(10);
    focusedDialogs_.insert(key, dialog);
    connect(dialog, &QObject::destroyed, this, [this, key]() {
        focusedDialogs_.remove(key);
    });
    return dialog;
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
}

void MainWindow::ensureVisualReplayHost()
{
    if (visualReplayHost_ != nullptr) {
        return;
    }
    visualReplayHost_ = new CoordinatorPane(coordinatorController_, this);
    visualReplayHost_->hide();
    connect(visualReplayHost_, &CoordinatorPane::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
    connect(visualReplayHost_, &CoordinatorPane::settingsNavigationRequested, this, &MainWindow::handleCoordinatorSettingsNavigation);
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
        coordinatorController_->isRunning(),
        coordinatorController_->isPaused(),
        coordinatorController_->targetWorkers(),
        coordinatorController_->activeWorkers(),
        coordinatorController_->validationMessage());
}
