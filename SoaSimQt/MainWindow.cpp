#include "MainWindow.h"
#include "GUI/StyleSheet.h"
#include "JobsPage.h"

#include <QtCore/QStringList>
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
constexpr int kStatusBarMinHeight = 34;

QFrame* createPanelFrame(const QString& title, const QString& body)
{
    QFrame* panel = new QFrame();
    panel->setObjectName("placeholderPanel");
    panel->setFrameShape(QFrame::StyledPanel);

    QVBoxLayout* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(8);

    QLabel* titleLabel = new QLabel(title, panel);
    titleLabel->setObjectName("panelTitle");

    QLabel* bodyLabel = new QLabel(body, panel);
    bodyLabel->setObjectName("panelBody");
    bodyLabel->setWordWrap(true);

    layout->addWidget(titleLabel);
    layout->addWidget(bodyLabel);
    layout->addStretch();

    return panel;
}

QLabel* createBadge(const QString& text, const QString& objectName)
{
    QLabel* badge = new QLabel(text);
    badge->setObjectName(objectName);
    badge->setAlignment(Qt::AlignCenter);
    badge->setMargin(6);
    return badge;
}
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    createWidgets();
}

MainWindow::~MainWindow()
{}

void MainWindow::handleNavigationChanged(int currentRow)
{
    if (!contentStack_ || currentRow < 0 || currentRow >= contentStack_->count()) {
        return;
    }

    contentStack_->setCurrentIndex(currentRow);
}

void MainWindow::createWidgets()
{
    setWindowTitle("SoaSimQt");
    resize(1440, 900);

    QWidget* root = new QWidget(this);
    root->setObjectName("mainRoot");
    setCentralWidget(root);

    QVBoxLayout* rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    QWidget* topBar = createTopBar();
    QWidget* navigationPane = createNavigationPane();
    QWidget* contentPane = createContentPane();
    QWidget* statusBarWidget = createStatusBarWidget();

    QWidget* body = new QWidget(root);
    body->setObjectName("bodyRegion");
    QHBoxLayout* bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    bodyLayout->addWidget(navigationPane);
    bodyLayout->addWidget(contentPane, 1);

    rootLayout->addWidget(topBar);
    rootLayout->addWidget(body, 1);
    rootLayout->addWidget(statusBarWidget);

    setStyleSheet(SoaSimQt::GUI::kMainWindowStyleSheet);
}

QWidget* MainWindow::createTopBar()
{
    QWidget* topBar = new QWidget(this);
    topBar->setObjectName("topBar");
    topBar->setFixedHeight(kTopBarHeight);

    QHBoxLayout* layout = new QHBoxLayout(topBar);
    layout->setContentsMargins(0, 0, 0, 0);

    QLabel* title = new QLabel("SoaSimQt Mockup", topBar);
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
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(12);

    contentStack_ = new QStackedWidget(contentPane);
    contentStack_->addWidget(createPlaceholderPage("Job Sets", "Mockup page for job set management and filters."));
    contentStack_->addWidget(new JobsPage(contentPane));
    contentStack_->addWidget(createPlaceholderPage("Workers", "Mockup page for coordinator and worker activity."));
    contentStack_->addWidget(createPlaceholderPage("Job Builder", "Mockup page for constructing new simulation runs."));
    contentStack_->addWidget(createPlaceholderPage("Battle Run Settings", "Mockup page for tuning battle run configuration."));
    contentStack_->addWidget(createPlaceholderPage("Artifacts", "Mockup page for artifact browsing and import/export flows."));
    contentStack_->addWidget(createPlaceholderPage("Seed Probe", "Mockup page for seed probing tools and diagnostics."));
    contentStack_->addWidget(createPlaceholderPage("Explorer Runs", "Mockup page for explorer run history and controls."));
    contentStack_->addWidget(createPlaceholderPage("Settings", "Mockup page for application-wide settings and environment setup."));

    layout->addWidget(contentStack_, 1);

    if (navigationList_) {
        navigationList_->setCurrentRow(1);
    }

    return contentPane;
}

QWidget* MainWindow::createStatusBarWidget()
{
    QFrame* statusBarWidget = new QFrame(this);
    statusBarWidget->setObjectName("statusBarWidget");
    statusBarWidget->setMinimumHeight(kStatusBarMinHeight);

    QHBoxLayout* layout = new QHBoxLayout(statusBarWidget);
    layout->setContentsMargins(12, 6, 12, 6);
    layout->setSpacing(10);

    layout->addWidget(createBadge("Connected", "badgeConnected"));

    QLabel* envLabel = new QLabel("env: placeholder", statusBarWidget);
    envLabel->setObjectName("statusText");
    layout->addWidget(envLabel);

    QLabel* refreshLabel = new QLabel("Last refresh: --", statusBarWidget);
    refreshLabel->setObjectName("statusText");
    layout->addWidget(refreshLabel);

    layout->addWidget(createBadge("Coordinator: 0", "badgeCoordinator"));
    layout->addStretch();
    layout->addWidget(createBadge("Mock toast", "badgeToastInfo"));

    return statusBarWidget;
}

QWidget* MainWindow::createPlaceholderPage(const QString& title, const QString& description)
{
    QWidget* page = new QWidget(this);

    QVBoxLayout* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    QLabel* titleLabel = new QLabel(title, page);
    titleLabel->setObjectName("pageTitle");

    QLabel* descriptionLabel = new QLabel(description, page);
    descriptionLabel->setObjectName("pageDescription");
    descriptionLabel->setWordWrap(true);

    QHBoxLayout* topRow = new QHBoxLayout();
    topRow->setSpacing(12);
    topRow->addWidget(createPanelFrame("Primary Workspace", "Large placeholder region for the main page-specific content."), 2);
    topRow->addWidget(createPanelFrame("Inspector", "Secondary placeholder panel for details, forms, or actions."), 1);

    QHBoxLayout* bottomRow = new QHBoxLayout();
    bottomRow->setSpacing(12);
    bottomRow->addWidget(createPanelFrame("Lower Panel A", "Reserved for tables, logs, or summary widgets."), 1);
    bottomRow->addWidget(createPanelFrame("Lower Panel B", "Reserved for charts, previews, or secondary controls."), 1);

    layout->addWidget(titleLabel);
    layout->addWidget(descriptionLabel);
    layout->addLayout(topRow, 2);
    layout->addLayout(bottomRow, 1);

    return page;
}
