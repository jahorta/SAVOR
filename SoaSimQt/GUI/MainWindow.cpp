#include "MainWindow.h"
#include "GUI/StyleSheet.h"
#include "GUI/Panes/JobSetsPane/JobSetsPage.h"
#include "GUI/Panes/JobsPane/JobsPage.h"
#include "GUI/Panes/SeedProbePane/SeedProbePage.h"
#include "GUI/Panes/ArtifactsPane/ArtifactsPage.h"
#include "GUI/Panes/JobBuilderPane/JobBuilderPage.h"
#include "GUI/Panes/BattleRunSettingsPane/BattleRunSettingsPage.h"
#include "GUI/Panes/ExplorerRunsPane/ExplorerRunsPage.h"

#include <QtCore/QStringList>

#include <iterator>
#include <QtGui/QCursor>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
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
    { "Job Sets", "Live Job Sets workspace with filtering, paging, expansion state, right-click actions, and progress visuals." },
    { "Jobs", "Live Jobs workspace with backend filters, cursor paging, inspector tabs, auto-refresh, and job actions." },
    { "Workers", "Coordinator controls, persisted runtime settings, and live worker telemetry." },
    { "Job Builder", "Qt-native phase builder for SeedProbe, TasMovie, and Explorer/BattleTurnRunner job set creation." },
    { "Battle Run Settings", "Qt-native battle run settings authoring with preset libraries, predicates, templates, context validation, estimates, and save/materialize actions." },
    { "Artifacts", "Object-store artifact browser with search, paging, import, inspector metadata, and materialize/export actions." },
    { "Seed Probe", "Seed probe grid and unique probing results." },
    { "Explorer Runs", "Explorer run history and controls." },
    { "Settings", "Application-wide storage settings with shared DB relocation flow and room for future sections." }
};
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    
    coordinatorController_ = new CoordinatorController(this);
    createWidgets();

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
        setMaximumSize(availableSize);
    }

    resize(defaultWindowSize);
}

MainWindow::~MainWindow()
{
    statusBarRefreshTimer_.stop();
    if (coordinatorController_) {
        disconnect(coordinatorController_, nullptr, this, nullptr);
    }
}

void MainWindow::handleNavigationChanged(int currentRow)
{
    if (!contentStack_ || currentRow < 0 || currentRow >= contentStack_->count()) {
        return;
    }

    contentStack_->setCurrentIndex(currentRow);

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

    navigationList_->setCurrentRow(8);

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

    QWidget* topBar = createTopBar();
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

    rootLayout->addWidget(topBar);
    rootLayout->addWidget(body, 1);
    rootLayout->addWidget(statusBarWidget_);

    setStyleSheet(SoaSimQt::GUI::kMainWindowStyleSheet);
}

QWidget* MainWindow::createTopBar()
{
    QWidget* topBar = new QWidget(this);
    topBar->setObjectName("topBar");
    topBar->setFixedHeight(kTopBarHeight);

    QHBoxLayout* layout = new QHBoxLayout(topBar);
    layout->setContentsMargins(0, 0, 0, 0);

    QLabel* title = new QLabel("SoaSimQt", topBar);
    title->setObjectName("topBarTitle");
    layout->addWidget(title);
    layout->addStretch();

    return topBar;
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
        "Job Sets",
        "Jobs",
        "Workers",
        "Job Builder",
        "Battle Run Settings",
        "Artifacts",
        "Seed Probe",
        "Explorer Runs",
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
    auto* jobSetsPage = new JobSetsPage(contentPane);
    connect(jobSetsPage, &JobSetsPage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
    contentStack_->addWidget(jobSetsPage);
    auto* jobsPage = new JobsPage(contentPane);
    connect(jobsPage, &JobsPage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
    contentStack_->addWidget(jobsPage);
    coordinatorPane_ = new CoordinatorPane(coordinatorController_, contentStack_);
    contentStack_->addWidget(coordinatorPane_);
    connect(coordinatorPane_, &CoordinatorPane::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
    connect(coordinatorPane_, &CoordinatorPane::settingsNavigationRequested, this, &MainWindow::handleCoordinatorSettingsNavigation);
    connect(jobsPage, &JobsPage::visualReplayRequested, coordinatorPane_, &CoordinatorPane::requestVisualReplay);
    auto* jobBuilderPage = new JobBuilderPage(contentStack_);
    connect(jobBuilderPage, &JobBuilderPage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
    contentStack_->addWidget(jobBuilderPage);
    auto* battleRunSettingsPage = new BattleRunSettingsPage(contentStack_);
    connect(battleRunSettingsPage, &BattleRunSettingsPage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
    contentStack_->addWidget(battleRunSettingsPage);
    auto* artifactsPage = new ArtifactsPage(contentPane);
    connect(artifactsPage, &ArtifactsPage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
    contentStack_->addWidget(artifactsPage);
    auto* seedProbePage = new SeedProbePage(contentStack_);
    connect(seedProbePage, &SeedProbePage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
    contentStack_->addWidget(seedProbePage);
    auto* explorerRunsPage = new ExplorerRunsPage(contentStack_);
    connect(explorerRunsPage, &ExplorerRunsPage::statusToastRequested, statusBarWidget_, qOverload<StatusToast>(&StatusBarWidget::postToast));
    connect(explorerRunsPage, &ExplorerRunsPage::visualReplayRequested, coordinatorPane_, &CoordinatorPane::requestVisualReplay);
    contentStack_->addWidget(explorerRunsPage);
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
