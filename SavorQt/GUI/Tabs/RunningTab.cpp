#include "GUI/Tabs/RunningTab.h"

#include "DB/SavorDbJobService.h"
#include "DB/SavorDbWorkflowService.h"
#include "GUI/Panes/CoordinatorPane/CoordinatorController.h"

#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <optional>

namespace {

QFrame* createMetricTile(const QString& label, const QString& value, const QString& caption, QWidget* parent)
{
    auto* tile = new QFrame(parent);
    tile->setObjectName("workspaceMetricTile");
    auto* layout = new QVBoxLayout(tile);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(4);

    auto* labelText = new QLabel(label, tile);
    labelText->setObjectName("sectionDescription");
    auto* valueText = new QLabel(value, tile);
    valueText->setObjectName("pageTitle");
    auto* captionText = new QLabel(caption, tile);
    captionText->setObjectName("sectionDescription");
    captionText->setWordWrap(true);

    layout->addWidget(labelText);
    layout->addWidget(valueText);
    layout->addWidget(captionText);
    return tile;
}

QFrame* createInfoPanel(const QString& title, const QString& body, QWidget* parent)
{
    auto* panel = new QFrame(parent);
    panel->setObjectName("workspaceInfoPanel");
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(8);

    auto* titleLabel = new QLabel(title, panel);
    titleLabel->setObjectName("panelTitle");
    auto* bodyLabel = new QLabel(body, panel);
    bodyLabel->setObjectName("sectionDescription");
    bodyLabel->setWordWrap(true);

    layout->addWidget(titleLabel);
    layout->addWidget(bodyLabel);
    layout->addStretch();
    return panel;
}

QFrame* createToolCard(
    savorqt::gui::WorkspacePageShell* shell,
    const savorqt::gui::UiEntityRef& entity,
    const QString& title,
    const QString& body,
    const QString& detailsTitle,
    const QString& detailsBody,
    const QString& openText,
    std::function<void()> openAction)
{
    auto* card = new QFrame(shell);
    card->setObjectName("workspaceToolCard");
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(8);

    auto* titleLabel = new QLabel(title, card);
    titleLabel->setObjectName("panelTitle");
    auto* bodyLabel = new QLabel(body, card);
    bodyLabel->setObjectName("sectionDescription");
    bodyLabel->setWordWrap(true);
    auto* detailsButton = new QPushButton(QStringLiteral("Details"), card);
    detailsButton->setObjectName("jobsSecondaryButton");
    auto* openButton = new QPushButton(openText, card);
    openButton->setObjectName("jobsPrimaryButton");

    layout->addWidget(titleLabel);
    layout->addWidget(bodyLabel);
    layout->addStretch();
    auto* buttonRow = new QHBoxLayout();
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->setSpacing(8);
    buttonRow->addWidget(detailsButton);
    buttonRow->addWidget(openButton);
    layout->addLayout(buttonRow);

    QObject::connect(detailsButton, &QPushButton::clicked, card, [shell, entity, detailsTitle, detailsBody, openText, openAction]() {
        shell->setContext(entity, detailsTitle, detailsBody, QVector<std::pair<QString, std::function<void()>>>{
            { openText, openAction }
        }, savorqt::gui::ContextDrawerMode::Expanded);
    });
    QObject::connect(openButton, &QPushButton::clicked, card, [openAction]() {
        if (openAction) {
            openAction();
        }
    });
    return card;
}

} // namespace

RunningTab::RunningTab(CoordinatorController* coordinatorController, Actions actions, QWidget* parent)
    : WorkspacePageShell(
        QStringLiteral("running"),
        QStringLiteral("Running"),
        QStringLiteral("Supervise active workflow groups, queues, workers, failures, and operational interventions."),
        parent)
    , coordinatorController_(coordinatorController)
    , actions_(std::move(actions))
{
    build();
}

void RunningTab::build()
{
    savorqt::db::WorkflowListRequest workflowRequest{};
    workflowRequest.limit = 50;
    const auto workflows = savorqt::db::SavorDbWorkflowService::ListWorkflowInstances(workflowRequest);
    int activeWorkflows = 0;
    int failedWorkflows = 0;
    int terminalWorkflows = 0;
    if (workflows.ok) {
        for (const auto& workflow : workflows.value.items) {
            const QString state = QString::fromStdString(workflow.state);
            if (state == QStringLiteral("FAILED") || workflow.failed_step_count > 0) {
                ++failedWorkflows;
            } else if (state == QStringLiteral("COMPLETED") || state == QStringLiteral("SKIPPED")) {
                ++terminalWorkflows;
            } else {
                ++activeWorkflows;
            }
        }
    }

    savor::db::UiReadJobListQuery jobQuery{};
    const auto jobs = savorqt::db::SavorDbJobService::FetchJobsPage(jobQuery, std::nullopt, std::nullopt, 50);
    int runningJobs = 0;
    int queuedJobs = 0;
    int failedJobs = 0;
    if (jobs.ok) {
        for (const auto& job : jobs.value.items) {
            const QString state = QString::fromStdString(job.state);
            if (state == QStringLiteral("RUNNING") || state == QStringLiteral("CLAIMED")) {
                ++runningJobs;
            } else if (state == QStringLiteral("QUEUED")) {
                ++queuedJobs;
            } else if (state == QStringLiteral("FAILED")) {
                ++failedJobs;
            }
        }
    }

    auto* hero = new QFrame(this);
    hero->setObjectName("workspaceHeroPanel");
    auto* heroLayout = new QHBoxLayout(hero);
    heroLayout->setContentsMargins(14, 14, 14, 14);
    heroLayout->setSpacing(12);
    auto* heroText = new QFrame(hero);
    auto* heroTextLayout = new QVBoxLayout(heroText);
    heroTextLayout->setContentsMargins(0, 0, 0, 0);
    heroTextLayout->setSpacing(6);
    auto* heroTitle = new QLabel(failedWorkflows > 0 || failedJobs > 0
        ? QStringLiteral("Operations need attention")
        : activeWorkflows > 0 || runningJobs > 0 || queuedJobs > 0
            ? QStringLiteral("Workflow groups are in motion")
            : QStringLiteral("No active run pressure"),
        heroText);
    heroTitle->setObjectName("panelTitle");
    auto* heroBody = new QLabel(QStringLiteral("Use this workspace as mission control: workflow activation lanes, job pressure, worker health, and failure triage all start here."), heroText);
    heroBody->setObjectName("sectionDescription");
    heroBody->setWordWrap(true);
    heroTextLayout->addWidget(heroTitle);
    heroTextLayout->addWidget(heroBody);
    heroTextLayout->addStretch();
    heroLayout->addWidget(heroText, 2);

    auto* heroMetrics = new QGridLayout();
    heroMetrics->setContentsMargins(0, 0, 0, 0);
    heroMetrics->setSpacing(8);
    heroMetrics->addWidget(createMetricTile(QStringLiteral("Active workflows"), workflows.ok ? QString::number(activeWorkflows) : QStringLiteral("--"), QStringLiteral("Recent non-terminal"), hero), 0, 0);
    heroMetrics->addWidget(createMetricTile(QStringLiteral("Queue"), jobs.ok ? QString::number(queuedJobs) : QStringLiteral("--"), QStringLiteral("Queued jobs"), hero), 0, 1);
    heroMetrics->addWidget(createMetricTile(QStringLiteral("Running"), jobs.ok ? QString::number(runningJobs) : QStringLiteral("--"), QStringLiteral("Claimed/running jobs"), hero), 1, 0);
    heroMetrics->addWidget(createMetricTile(QStringLiteral("Workers"), coordinatorController_ ? QStringLiteral("%1/%2").arg(coordinatorController_->activeWorkers()).arg(coordinatorController_->targetWorkers()) : QStringLiteral("--"), QStringLiteral("Active/target"), hero), 1, 1);
    heroLayout->addLayout(heroMetrics, 3);
    canvasLayout()->addWidget(hero);

    auto* opsGrid = new QFrame(this);
    opsGrid->setObjectName("workspaceCardGrid");
    auto* opsLayout = new QGridLayout(opsGrid);
    opsLayout->setContentsMargins(0, 0, 0, 0);
    opsLayout->setSpacing(10);
    opsLayout->addWidget(createInfoPanel(
        QStringLiteral("Attention stack"),
        QStringLiteral("Failed workflows: %1\nFailed jobs: %2\nCoordinator: %3")
            .arg(failedWorkflows)
            .arg(failedJobs)
            .arg(coordinatorController_ && !coordinatorController_->validationMessage().isEmpty()
                ? coordinatorController_->validationMessage()
                : QStringLiteral("no blocking warning")),
        opsGrid), 0, 0);
    opsLayout->addWidget(createInfoPanel(
        QStringLiteral("Workflow pressure"),
        QStringLiteral("Recent workflows: %1\nActive/waiting: %2\nRecently terminal: %3\nRecent jobs sampled: %4")
            .arg(workflows.ok ? QString::number(static_cast<int>(workflows.value.items.size())) : QStringLiteral("unavailable"))
            .arg(activeWorkflows)
            .arg(terminalWorkflows)
            .arg(jobs.ok ? QString::number(static_cast<int>(jobs.value.items.size())) : QStringLiteral("unavailable")),
        opsGrid), 0, 1);
    canvasLayout()->addWidget(opsGrid);

    auto* gridHost = new QFrame(this);
    gridHost->setObjectName("workspaceCardGrid");
    auto* grid = new QGridLayout(gridHost);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(10);

    grid->addWidget(createToolCard(
        this,
        { QStringLiteral("running"), QStringLiteral("workflow"), 0, QStringLiteral("board") },
        QStringLiteral("Workflow board"),
        QStringLiteral("Activation-backed workflow hierarchy with current, future, and past groups."),
        QStringLiteral("Workflow board"),
        QStringLiteral("This is the primary running surface. Use it to inspect current/future/past activations and workflow alerts."),
        QStringLiteral("Open Workflows"),
        actions_.openWorkflows), 0, 0);
    grid->addWidget(createToolCard(
        this,
        { QStringLiteral("running"), QStringLiteral("job"), 0, QStringLiteral("queue") },
        QStringLiteral("Job queue"),
        QStringLiteral("Job sets, retries, artifacts, visual replay, and operational job interventions."),
        QStringLiteral("Job queue"),
        QStringLiteral("Use this for requeue, restart, cancel, input INI, artifacts, and visual replay actions."),
        QStringLiteral("Open Jobs"),
        actions_.openJobs), 0, 1);
    grid->addWidget(createToolCard(
        this,
        { QStringLiteral("running"), QStringLiteral("worker"), 0, QStringLiteral("workers") },
        QStringLiteral("Workers"),
        QStringLiteral("Coordinator controls, target workers, live telemetry, and visual worker dashboard."),
        QStringLiteral("Workers"),
        QStringLiteral("Start, pause, stop, and inspect worker telemetry. Configuration warnings link back to Setup settings."),
        QStringLiteral("Open Workers"),
        actions_.openWorkers), 1, 0);
    grid->addWidget(createToolCard(
        this,
        { QStringLiteral("running"), QStringLiteral("attention"), 0, QStringLiteral("attention") },
        QStringLiteral("Failure triage"),
        QStringLiteral("Open the workflow board when failures appear in the attention stack above."),
        QStringLiteral("Failure triage"),
        QStringLiteral("Workflow activation state is the first triage level. Drill from activation to step, then job set/job details as needed."),
        QStringLiteral("Open Workflows"),
        actions_.openWorkflows), 1, 1);

    canvasLayout()->addWidget(gridHost);
    canvasLayout()->addStretch();
}
