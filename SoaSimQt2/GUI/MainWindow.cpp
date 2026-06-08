#include "MainWindow.h"
#include "GUI/Panes/JobSetsPane/WorkflowsPage.h"
#include "GUI/Panes/JobsPane/JobsPage.h"
#include "GUI/Panes/SeedProbePane/SeedProbePage.h"
#include "GUI/Panes/ArtifactsPane/ArtifactsPage.h"
#include "GUI/Panes/JobBuilderPane/WorkflowLauncherPage.h"
#include "GUI/Panes/BattleRunSettingsPane/BattleRunSettingsPage.h"
#include "GUI/Panes/BattleRunSettingsPane/AuthoringLibraryDialog.h"
#include "GUI/Panes/ExplorerRunsPane/ExplorerRunsPage.h"

#include <QtCore/QStringList>

#include <iterator>
#include <QtGui/QCursor>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMenu>
#include <QtGui/QAction>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

namespace {
constexpr int kLeftNavWidth = 250;
constexpr int kTopBarHeight = 24;

struct PageMetadata {
    const char* title;
    const char* description;
};

constexpr PageMetadata kPageMetadata[] = {
    { "Workflows", "Workflow execution workspace grouped by instance, step, and job set drilldown." },
    { "Jobs", "Live Jobs workspace with backend filters, cursor paging, inspector tabs, auto-refresh, and job actions." },
    { "Workers", "Coordinator controls, persisted runtime settings, and live worker telemetry." },
    { "Workflow Launcher", "Workflow graph authoring and instancing with per-run external input bindings." },
    { "Battle Run Settings", "Qt-native battle run settings authoring with preset libraries, predicates, battle chain specs, context validation, estimates, and save/materialize actions." },
    { "Artifacts", "Artifact storage browser with search, paging, import, inspector metadata, and materialize/export actions." },
    { "Seed Probe", "Seed probe grid and unique probing results." },
    { "Explorer Runs", "Explorer run history and controls." },
    { "DTM Editor", "Poll-based DTM editing with deterministic annotation sidecar binding." },
    { "Settings", "Application-wide storage settings with shared DB relocation flow and room for future sections." }
};
} // namespace

MainWindow::MainWindow(QWidget *parent)
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
        const QSize availableSize = targetScreen->availableGeometry().size();
        defaultWindowSize = defaultWindowSize.boundedTo(availableSize);
    }

    resize(defaultWindowSize);
}

MainWindow::~MainWindow()
{
    shutdownCoordinator();
}

void MainWindow::shutdownCoordinator()
{
    statusBarRefreshTimer_.stop();
    if (coordinatorController_) {
        coordinatorController_->stopCoordinator();
        disconnect(coordinatorController_, nullptr, this, nullptr);
    }
}

void MainWindow::handleNavigationChanged(int currentRow)
{
    if (!contentStack_ || currentRow < 0 || currentRow >= contentStack_->count()) {
        return;
    }

    contentStack_->setCurrentIndex(currentRow);
    if (workflowsPage_) {
        workflowsPage_->setPageActive(currentRow == 0);
    }
    if (jobsPage_) {
        jobsPage_->setPageActive(currentRow == 1);
    }
    if (coordinatorPane_) {
        coordinatorPane_->setPageActive(currentRow == 2);
    }
    if (seedProbePage_) {
        seedProbePage_->setPageActive(currentRow == 6);
    }
    if (explorerRunsPage_) {
        explorerRunsPage_->setPageActive(currentRow == 7);
    }
    if (dtmEditorPage_) {
        dtmEditorPage_->setPageActive(currentRow == 8);
    }

    if (contentTitleLabel_ && contentDescriptionLabel_ && currentRow < static_cast<int>(std::size(kPageMetadata))) {
        contentTitleLabel_->setText(kPageMetadata[currentRow].title);
        contentDescriptionLabel_->setText(kPageMetadata[currentRow].description);
    }
}

void MainWindow::handleCoordinatorSettingsNavigation(CoordinatorPane::SettingsFocusTarget target)
{
    if (!navigationList_ || !settingsPage_) {
        return;
    }

        navigationList_->setCurrentRow(9);

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

    settingsPage_->focusCoordinatorSettings(focusTarget);
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
}

void MainWindow::openAuthoringLibraryLast() {
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

void MainWindow::openSeedProbeSpecLibrary()
{
    openAuthoringLibrary(AuthoringLibraryKey::SeedProbe);
}

void MainWindow::openTasSpecLibrary()
{
    openAuthoringLibrary(AuthoringLibraryKey::Tas);
}

void MainWindow::openBattleRunSpecLibrary()
{
    openAuthoringLibrary(AuthoringLibraryKey::BattleRun);
}

void MainWindow::openPredicateSpecLibrary()
{
    openAuthoringLibrary(AuthoringLibraryKey::Predicate);
}

void MainWindow::openPredicateSetSpecLibrary()
{
    openAuthoringLibrary(AuthoringLibraryKey::PredicateSet);
}

void MainWindow::openBattlePlanSpecLibrary()
{
    openAuthoringLibrary(AuthoringLibraryKey::BattlePlan);
}

void MainWindow::openExplorerSettingsSpecLibrary()
{
    openAuthoringLibrary(AuthoringLibraryKey::ExplorerSettings);
}

void MainWindow::syncStatusBar()
{
    if (!statusBarWidget_) {
        return;
    }

    if (coordinatorController_ && coordinatorController_->isRunning()) {
        lastCoordinatorRefresh_ = QDateTime::currentDateTime();
    }

    const StatusBarSnapshot snapshot = StatusBarWidget::buildSnapshot(coordinatorController_, lastCoordinatorRefresh_);
    statusBarWidget_->setSnapshot(snapshot);
    emitCoordinatorStateChanged();
}

void MainWindow::createWidgets()
{

    QWidget* root = new QWidget(this);
    root->setObjectName("mainRoot");
    setCentralWidget(root);

    QVBoxLayout* rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    QWidget* navigationPane = createNavigationPane();
    statusBarWidget_ = createStatusBarWidget();
    QWidget* contentPane = createContentPane();

    QWidget* body = new QWidget(root);
    body->setObjectName("bodyRegion");
    QHBoxLayout* bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    bodyLayout->addWidget(navigationPane);
    bodyLayout->addWidget(contentPane, 1);

    rootLayout->addWidget(body, 1);
    rootLayout->addWidget(statusBarWidget_);

}

QWidget* MainWindow::createNavigationPane()
{
    QFrame* navFrame = new QFrame(this);
    navFrame->setObjectName("navigationPane");
    navFrame->setFixedWidth(kLeftNavWidth);

    QVBoxLayout* layout = new QVBoxLayout(navFrame);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    QLabel* header = new QLabel("NAVIGATION", navFrame);
    header->setObjectName("navHeader");
    layout->addWidget(header);

    navigationList_ = new QListWidget(navFrame);
    navigationList_->setObjectName("navigationList");
    navigationList_->addItems(QStringList{
        "Workflows",
        "Jobs",
        "Workers",
        "Workflow Launcher",
        "Battle Run Settings",
        "Artifacts",
        "Seed Probe",
        "Explorer Runs",
        "DTM Editor",
        "Settings"
    });

    connect(navigationList_, &QListWidget::currentRowChanged, this, &MainWindow::handleNavigationChanged);

    layout->addWidget(navigationList_, 1);
    return navFrame;
}

QWidget* MainWindow::createContentPane()
{
    QWidget* contentPane = new QWidget(this);
    contentPane->setObjectName("contentPane");

    QVBoxLayout* layout = new QVBoxLayout(contentPane);
    layout->setContentsMargins(16, 8, 16, 8);
    layout->setSpacing(10);

    QHBoxLayout* topLayout = new QHBoxLayout(contentPane);
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing(10);

    contentTitleLabel_ = new QLabel(contentPane);
    contentTitleLabel_->setObjectName("pageTitle");

    contentDescriptionLabel_ = new QLabel(contentPane);
    contentDescriptionLabel_->setObjectName("pageDescription");
    contentDescriptionLabel_->setWordWrap(true);

    topLayout->addWidget(contentTitleLabel_, 0);
    topLayout->addWidget(contentDescriptionLabel_, 1);

    layout->addLayout(topLayout);

    contentStack_ = new QStackedWidget(contentPane);
    workflowsPage_ = new WorkflowsPage(contentPane);
    connect(workflowsPage_, &WorkflowsPage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
    contentStack_->addWidget(workflowsPage_);
    jobsPage_ = new JobsPage(contentPane);
    connect(jobsPage_, &JobsPage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
    contentStack_->addWidget(jobsPage_);
    coordinatorPane_ = new CoordinatorPane(coordinatorController_, contentStack_);
    contentStack_->addWidget(coordinatorPane_);
    connect(coordinatorPane_, &CoordinatorPane::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
    connect(coordinatorPane_, &CoordinatorPane::settingsNavigationRequested, this, &MainWindow::handleCoordinatorSettingsNavigation);
    connect(jobsPage_, &JobsPage::visualReplayRequested, coordinatorPane_, &CoordinatorPane::requestVisualReplay);
    auto* workflowLauncherPage = new WorkflowLauncherPage(contentStack_);
    connect(workflowLauncherPage, &WorkflowLauncherPage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
    contentStack_->addWidget(workflowLauncherPage);
    auto* battleRunSettingsPage = new BattleRunSettingsPage(contentStack_);
    connect(battleRunSettingsPage, &BattleRunSettingsPage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
    contentStack_->addWidget(battleRunSettingsPage);
    auto* artifactsPage = new ArtifactsPage(contentPane);
    connect(artifactsPage, &ArtifactsPage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
    contentStack_->addWidget(artifactsPage);
    seedProbePage_ = new SeedProbePage(contentStack_);
    connect(seedProbePage_, &SeedProbePage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
    contentStack_->addWidget(seedProbePage_);
    explorerRunsPage_ = new ExplorerRunsPage(contentStack_);
    connect(explorerRunsPage_, &ExplorerRunsPage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
    connect(explorerRunsPage_, &ExplorerRunsPage::visualReplayRequested, coordinatorPane_, &CoordinatorPane::requestVisualReplay);
    contentStack_->addWidget(explorerRunsPage_);
    dtmEditorPage_ = new DtmEditorPage(contentStack_);
    connect(dtmEditorPage_, &DtmEditorPage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
    contentStack_->addWidget(dtmEditorPage_);
    settingsPage_ = new SettingsPage(coordinatorController_, contentStack_);
    connect(settingsPage_, &SettingsPage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
    contentStack_->addWidget(settingsPage_);

    layout->addWidget(contentStack_, 1);

    if (navigationList_) {
        navigationList_->setCurrentRow(0);
    } else if constexpr (std::size(kPageMetadata) > 0) {
        contentTitleLabel_->setText(kPageMetadata[0].title);
        contentDescriptionLabel_->setText(kPageMetadata[0].description);
    }

    return contentPane;
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
