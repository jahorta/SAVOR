#include "GUI/Tabs/AnalysisTab.h"

#include "DB/SavorDbArtifactService.h"
#include "DB/SavorDbExplorerRunService.h"
#include "DB/SavorDbWorkflowService.h"
#include "GUI/Panes/BattleRunsPane/BattleRunsWidget.h"
#include "GUI/Refresh/AsyncRefreshPipeline.h"
#include "GUI/Refresh/RowUpdate.h"
#include "SavorDbRuntime.h"

#include <QtCore/QDateTime>
#include <QtCore/QTimeZone>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTableWidgetItem>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace {

enum PaneIndex {
    OverviewPane = 0,
    BattleRunsPane = 1,
    WorkflowPane = 2,
    FuturePane = 3,
};

struct AnalysisSnapshot {
    bool uiReadAvailable = false;
    bool battleRunsOk = false;
    bool artifactsOk = false;
    bool workflowsOk = false;

    std::string battleRunsError;
    std::string artifactsError;
    std::string workflowsError;

    std::vector<savor::db::UiBattleGroupSummary> battleGroups;
    std::vector<savor::db::UiArtifactSummary> artifacts;
    std::vector<savor::db::UiWorkflowInstanceSummary> workflows;
    std::vector<savor::db::UiSeedProbeRunSummary> seedProbes;

    int completedBattleGroups = 0;
    int battleGroupsWithIssues = 0;
    int completedSeedProbes = 0;
    int failedWorkflows = 0;
    int blockedWorkflows = 0;
};

struct AnalysisRefreshRequest {
};

struct OverviewRow {
    int key = 0;
    QString surface;
    QString recent;
    QString complete;
    QString problems;
    QString evidence;
};

struct OutcomeRow {
    qint64 groupId = 0;
    QString group;
    QString state;
    QString jobs;
    QString created;
    QString artifacts;
    QString seedEvidence;
};

struct ProvenanceRow {
    qint64 workflowId = 0;
    QString workflow;
    QString kind;
    QString state;
    QString problems;
    QString created;
    QString completed;
    QString outputs;
};

struct AnalysisRefreshData {
    QString outcomesValue;
    QString workflowsValue;
    QString artifactsValue;
    QString problemsValue;
    QString lastRefreshValue;
    QString overviewSummary;
    QString outcomesSummary;
    QString workflowsSummary;
    std::vector<OverviewRow> overviewRows;
    std::vector<OutcomeRow> outcomeRows;
    std::vector<ProvenanceRow> workflowRows;
};

QString qs(const std::string& value)
{
    return QString::fromStdString(value);
}

QString formatTime(std::int64_t epochSeconds)
{
    if (epochSeconds <= 0) {
        return QStringLiteral("--");
    }
    return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(epochSeconds), QTimeZone::fromSecondsAheadOfUtc(0))
        .toLocalTime()
        .toString(QStringLiteral("MM-dd HH:mm"));
}

QString formatOptionalTime(const std::optional<std::int64_t>& epochSeconds)
{
    return epochSeconds.has_value() ? formatTime(*epochSeconds) : QStringLiteral("--");
}

QString formatBattleTime(const std::optional<std::int64_t>& epochMillis)
{
    if (!epochMillis.has_value() || *epochMillis <= 0) {
        return QStringLiteral("--");
    }
    return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(*epochMillis), QTimeZone::fromSecondsAheadOfUtc(0))
        .toLocalTime()
        .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

QString formatBattleTime(std::int64_t epochMillis)
{
    return formatBattleTime(std::optional<std::int64_t>{ epochMillis });
}

QString compactText(QString text, int maxLength = 90)
{
    text = text.simplified();
    if (text.size() <= maxLength) {
        return text;
    }
    return text.left(maxLength - 1) + QChar(0x2026);
}

QTableWidgetItem* createTableItem(const QString& text)
{
    auto* item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

QPushButton* createActionButton(const QString& text, QWidget* parent, bool primary = false)
{
    auto* button = new QPushButton(text, parent);
    button->setObjectName(primary ? "jobsPrimaryButton" : "jobsSecondaryButton");
    return button;
}

QFrame* createStatusPill(const QString& label, QWidget* parent, QLabel** valueLabel)
{
    auto* pill = new QFrame(parent);
    pill->setObjectName("workspaceMetricTile");
    auto* layout = new QHBoxLayout(pill);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(6);

    auto* labelText = new QLabel(label + QStringLiteral(":"), pill);
    labelText->setObjectName("sectionDescription");
    auto* valueText = new QLabel(QStringLiteral("--"), pill);
    valueText->setObjectName("panelTitle");

    layout->addWidget(labelText);
    layout->addWidget(valueText);
    if (valueLabel != nullptr) {
        *valueLabel = valueText;
    }
    return pill;
}

QPushButton* createSelectorButton(const QString& text, QWidget* parent)
{
    auto* button = new QPushButton(text, parent);
    button->setCheckable(true);
    button->setObjectName("jobsSecondaryButton");
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return button;
}

QFrame* createSectionPanel(const QString& title, QWidget* parent)
{
    auto* panel = new QFrame(parent);
    panel->setObjectName("workspaceHeroPanel");
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);

    auto* titleLabel = new QLabel(title, panel);
    titleLabel->setObjectName("panelTitle");
    layout->addWidget(titleLabel);
    return panel;
}

QLabel* createSummaryLabel(QWidget* parent)
{
    auto* label = new QLabel(QStringLiteral("--"), parent);
    label->setObjectName("sectionDescription");
    label->setWordWrap(true);
    return label;
}

void configureTable(QTableWidget* table)
{
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(true);
    table->verticalHeader()->setVisible(false);
    table->verticalHeader()->setDefaultSectionSize(30);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setShowGrid(false);
}

bool isSeedProbeComplete(const savor::db::UiSeedProbeRunSummary& run)
{
    return run.status == "done" || run.status == "completed" || run.status == "COMPLETED";
}

bool isWorkflowFailed(const savor::db::UiWorkflowInstanceSummary& workflow)
{
    return workflow.state == "FAILED" || workflow.failed_step_count > 0;
}

bool isWorkflowTerminal(const savor::db::UiWorkflowInstanceSummary& workflow)
{
    return workflow.state == "COMPLETED" || workflow.state == "SKIPPED" || workflow.state == "CANCELED";
}

QString workflowProblemText(const savor::db::UiWorkflowInstanceSummary& workflow)
{
    if (!workflow.failure_text.empty()) {
        return compactText(qs(workflow.failure_text));
    }
    if (!workflow.failure_code.empty()) {
        return qs(workflow.failure_code);
    }
    if (workflow.blocked_step_count > 0 || workflow.failed_step_count > 0) {
        return QStringLiteral("blocked %1 / failed %2")
            .arg(workflow.blocked_step_count)
            .arg(workflow.failed_step_count);
    }
    return QStringLiteral("--");
}

AnalysisSnapshot loadSnapshot()
{
    AnalysisSnapshot snapshot{};
    auto* uiRead = savorqt::SavorDbRuntime::instance().uiReadDb();
    snapshot.uiReadAvailable = uiRead != nullptr;

    if (uiRead != nullptr) {
        savor::db::UiReadSeedProbeRunListQuery seedQuery{};
        seedQuery.limit = 50;
        const auto seedPage = uiRead->ListSeedProbeRuns(seedQuery);
        snapshot.seedProbes = seedPage.items;
        for (const auto& run : snapshot.seedProbes) {
            if (isSeedProbeComplete(run)) {
                ++snapshot.completedSeedProbes;
            }
        }
    }

    savorqt::db::BattleRunGroupQuery battleQuery{};
    battleQuery.limit = 50;
    const auto battleGroups = savorqt::db::SavorDbExplorerRunService::ListBattleGroups(battleQuery);
    snapshot.battleRunsOk = battleGroups.ok;
    if (battleGroups.ok) {
        snapshot.battleGroups = battleGroups.value.groups;
        for (const auto& group : snapshot.battleGroups) {
            if (group.completed_at_utc.has_value()) {
                ++snapshot.completedBattleGroups;
            }
            if (group.failed_count > 0) {
                ++snapshot.battleGroupsWithIssues;
            }
        }
    } else {
        snapshot.battleRunsError = battleGroups.error.message;
    }

    savor::db::UiReadArtifactListQuery artifactQuery{};
    artifactQuery.limit = 100;
    const auto artifacts = savorqt::db::SavorDbArtifactService::ListArtifacts(artifactQuery);
    snapshot.artifactsOk = artifacts.ok;
    if (artifacts.ok) {
        snapshot.artifacts = artifacts.value.items;
    } else {
        snapshot.artifactsError = artifacts.error.message;
    }

    savorqt::db::WorkflowListRequest workflowRequest{};
    workflowRequest.limit = 50;
    const auto workflows = savorqt::db::SavorDbWorkflowService::ListWorkflowInstances(workflowRequest);
    snapshot.workflowsOk = workflows.ok;
    if (workflows.ok) {
        snapshot.workflows = workflows.value.items;
        for (const auto& workflow : snapshot.workflows) {
            if (isWorkflowFailed(workflow)) {
                ++snapshot.failedWorkflows;
            }
            if (workflow.blocked_step_count > 0) {
                ++snapshot.blockedWorkflows;
            }
        }
    } else {
        snapshot.workflowsError = workflows.error.message;
    }

    return snapshot;
}

bool overviewRowsEqual(const OverviewRow& lhs, const OverviewRow& rhs)
{
    return lhs.key == rhs.key
        && lhs.surface == rhs.surface
        && lhs.recent == rhs.recent
        && lhs.complete == rhs.complete
        && lhs.problems == rhs.problems
        && lhs.evidence == rhs.evidence;
}

bool outcomeRowsEqual(const OutcomeRow& lhs, const OutcomeRow& rhs)
{
    return lhs.groupId == rhs.groupId
        && lhs.group == rhs.group
        && lhs.state == rhs.state
        && lhs.jobs == rhs.jobs
        && lhs.created == rhs.created
        && lhs.artifacts == rhs.artifacts
        && lhs.seedEvidence == rhs.seedEvidence;
}

bool provenanceRowsEqual(const ProvenanceRow& lhs, const ProvenanceRow& rhs)
{
    return lhs.workflowId == rhs.workflowId
        && lhs.workflow == rhs.workflow
        && lhs.kind == rhs.kind
        && lhs.state == rhs.state
        && lhs.problems == rhs.problems
        && lhs.created == rhs.created
        && lhs.completed == rhs.completed
        && lhs.outputs == rhs.outputs;
}

void populateOverviewRow(QTableWidget* table, int row, const OverviewRow& item)
{
    table->setItem(row, 0, createTableItem(item.surface));
    table->setItem(row, 1, createTableItem(item.recent));
    table->setItem(row, 2, createTableItem(item.complete));
    table->setItem(row, 3, createTableItem(item.problems));
    table->setItem(row, 4, createTableItem(item.evidence));
}

void populateOutcomeRow(QTableWidget* table, int row, const OutcomeRow& item)
{
    table->setItem(row, 0, createTableItem(item.group));
    table->setItem(row, 1, createTableItem(item.state));
    table->setItem(row, 2, createTableItem(item.jobs));
    table->setItem(row, 3, createTableItem(item.created));
    table->setItem(row, 4, createTableItem(item.artifacts));
    table->setItem(row, 5, createTableItem(item.seedEvidence));
}

void populateProvenanceRow(QTableWidget* table, int row, const ProvenanceRow& item)
{
    table->setItem(row, 0, createTableItem(item.workflow));
    table->setItem(row, 1, createTableItem(item.kind));
    table->setItem(row, 2, createTableItem(item.state));
    table->setItem(row, 3, createTableItem(item.problems));
    table->setItem(row, 4, createTableItem(item.created));
    table->setItem(row, 5, createTableItem(item.completed));
    table->setItem(row, 6, createTableItem(item.outputs));
}

AnalysisRefreshData prepareAnalysisData(const AnalysisSnapshot& snapshot)
{
    AnalysisRefreshData data;
    data.outcomesValue = snapshot.battleRunsOk
        ? QStringLiteral("%1").arg(static_cast<int>(snapshot.battleGroups.size()))
        : QStringLiteral("--");
    data.workflowsValue = snapshot.workflowsOk
        ? QStringLiteral("%1").arg(static_cast<int>(snapshot.workflows.size()))
        : QStringLiteral("--");
    data.artifactsValue = snapshot.artifactsOk
        ? QStringLiteral("%1").arg(static_cast<int>(snapshot.artifacts.size()))
        : QStringLiteral("--");
    data.problemsValue = snapshot.battleRunsOk && snapshot.workflowsOk
        ? QStringLiteral("%1").arg(snapshot.battleGroupsWithIssues + snapshot.failedWorkflows + snapshot.blockedWorkflows)
        : QStringLiteral("--");
    data.lastRefreshValue = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));

    const int artifactCount = snapshot.artifactsOk ? static_cast<int>(snapshot.artifacts.size()) : 0;
    const int seedProbeCount = static_cast<int>(snapshot.seedProbes.size());
    const QString sampledEvidence = QStringLiteral("%1 artifacts / %2 seed probes sampled")
        .arg(artifactCount)
        .arg(seedProbeCount);

    data.overviewSummary = QStringLiteral(
        "%1 battle run groups, %2 workflows, %3 supporting artifacts, and %4 seed probe evidence rows sampled.")
        .arg(snapshot.battleRunsOk ? static_cast<int>(snapshot.battleGroups.size()) : 0)
        .arg(snapshot.workflowsOk ? static_cast<int>(snapshot.workflows.size()) : 0)
        .arg(artifactCount)
        .arg(seedProbeCount);

    const int terminalWorkflows = snapshot.workflowsOk
        ? static_cast<int>(std::count_if(snapshot.workflows.begin(), snapshot.workflows.end(), isWorkflowTerminal))
        : 0;
    data.overviewRows = {
        OverviewRow{
            0,
            QStringLiteral("Battle Runs"),
            snapshot.battleRunsOk ? QString::number(static_cast<int>(snapshot.battleGroups.size())) : QStringLiteral("--"),
            snapshot.battleRunsOk ? QString::number(snapshot.completedBattleGroups) : QStringLiteral("--"),
            snapshot.battleRunsOk ? QString::number(snapshot.battleGroupsWithIssues) : qs(snapshot.battleRunsError),
            sampledEvidence,
        },
        OverviewRow{
            1,
            QStringLiteral("Workflow Provenance"),
            snapshot.workflowsOk ? QString::number(static_cast<int>(snapshot.workflows.size())) : QStringLiteral("--"),
            snapshot.workflowsOk ? QString::number(terminalWorkflows) : QStringLiteral("--"),
            snapshot.workflowsOk
                ? QStringLiteral("%1 failed / %2 blocked").arg(snapshot.failedWorkflows).arg(snapshot.blockedWorkflows)
                : qs(snapshot.workflowsError),
            sampledEvidence,
        },
        OverviewRow{
            2,
            QStringLiteral("Supporting Artifacts"),
            snapshot.artifactsOk ? QString::number(static_cast<int>(snapshot.artifacts.size())) : QStringLiteral("--"),
            QStringLiteral("--"),
            snapshot.artifactsOk ? QStringLiteral("0") : qs(snapshot.artifactsError),
            QStringLiteral("Reached through outcome and workflow rows"),
        },
        OverviewRow{
            3,
            QStringLiteral("Seed Probe Evidence"),
            QString::number(seedProbeCount),
            QString::number(snapshot.completedSeedProbes),
            snapshot.uiReadAvailable ? QStringLiteral("0") : QStringLiteral("UIRead unavailable"),
            QStringLiteral("Shown as supporting evidence, not a top-level pane"),
        },
    };

    data.outcomesSummary = snapshot.battleRunsOk
        ? QStringLiteral("%1 sampled battle run groups, %2 complete, %3 with failed jobs. Supporting sample: %4 artifacts, %5 seed probe rows.")
            .arg(static_cast<int>(snapshot.battleGroups.size()))
            .arg(snapshot.completedBattleGroups)
            .arg(snapshot.battleGroupsWithIssues)
            .arg(artifactCount)
            .arg(seedProbeCount)
        : QStringLiteral("Battle runs unavailable: %1").arg(qs(snapshot.battleRunsError));

    data.workflowsSummary = snapshot.workflowsOk
        ? QStringLiteral("%1 sampled workflows, %2 failed, %3 blocked. Supporting sample: %4 artifacts, %5 seed probe rows.")
            .arg(static_cast<int>(snapshot.workflows.size()))
            .arg(snapshot.failedWorkflows)
            .arg(snapshot.blockedWorkflows)
            .arg(artifactCount)
            .arg(seedProbeCount)
        : QStringLiteral("Workflow provenance unavailable: %1").arg(qs(snapshot.workflowsError));
    data.workflowRows.reserve(snapshot.workflows.size());
    if (snapshot.workflowsOk) {
        for (const auto& workflow : snapshot.workflows) {
            data.workflowRows.push_back(ProvenanceRow{
                workflow.workflow_instance_id,
                QStringLiteral("#%1").arg(workflow.workflow_instance_id),
                qs(workflow.workflow_kind),
                qs(workflow.state),
                workflowProblemText(workflow),
                formatTime(workflow.created_at_utc),
                formatOptionalTime(workflow.completed_at_utc),
                QStringLiteral("%1 artifacts / %2 seed rows").arg(artifactCount).arg(seedProbeCount),
            });
        }
    }

    return data;
}

QString fieldHtml(const QString& label, const QString& value)
{
    return QStringLiteral("<tr><td style='padding:3px 12px 3px 0;color:#aac7bf;'>%1</td><td style='padding:3px 0;'>%2</td></tr>")
        .arg(label.toHtmlEscaped(), value.toHtmlEscaped());
}

QString optionalInt64Html(const std::optional<std::int64_t>& value)
{
    return value.has_value() ? QString::number(*value) : QStringLiteral("--");
}

QString optionalIntHtml(const std::optional<int>& value)
{
    return value.has_value() ? QString::number(*value) : QStringLiteral("--");
}

QString artifactSummaryHtml(const std::vector<savor::db::UiJobArtifact>& artifacts)
{
    if (artifacts.empty()) {
        return QStringLiteral("<p>No related artifacts are projected for this job.</p>");
    }
    QString html = QStringLiteral("<ul>");
    for (const auto& artifact : artifacts) {
        html += QStringLiteral("<li>#%1 %2 %3 (%4 bytes)</li>")
            .arg(artifact.artifact_id)
            .arg(qs(artifact.role_kind).toHtmlEscaped())
            .arg(qs(artifact.filename).toHtmlEscaped())
            .arg(artifact.size_bytes);
    }
    html += QStringLiteral("</ul>");
    return html;
}

} // namespace

AnalysisTab::AnalysisTab(Actions actions, QWidget* parent)
    : WorkspacePageShell(
        QStringLiteral("analysis"),
        QStringLiteral("Analysis"),
        QStringLiteral("Review completed workflow outcomes, provenance, and supporting evidence."),
        parent)
    , actions_(std::move(actions))
{
    build();
}

void AnalysisTab::build()
{
    setPageVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* statusStrip = new QFrame(this);
    statusStrip->setObjectName("workspaceHeroPanel");
    auto* statusLayout = new QHBoxLayout(statusStrip);
    statusLayout->setContentsMargins(10, 10, 10, 10);
    statusLayout->setSpacing(8);
    statusLayout->addWidget(createStatusPill(QStringLiteral("Outcomes"), statusStrip, &outcomesValueLabel_));
    statusLayout->addWidget(createStatusPill(QStringLiteral("Workflows"), statusStrip, &workflowsValueLabel_));
    statusLayout->addWidget(createStatusPill(QStringLiteral("Artifacts"), statusStrip, &artifactsValueLabel_));
    statusLayout->addWidget(createStatusPill(QStringLiteral("Problems"), statusStrip, &problemsValueLabel_));
    statusLayout->addStretch();

    lastRefreshLabel_ = new QLabel(QStringLiteral("--"), statusStrip);
    lastRefreshLabel_->setObjectName("sectionDescription");
    statusLayout->addWidget(lastRefreshLabel_);

    openBattleRunsButton_ = createActionButton(QStringLiteral("Battle Runs"), statusStrip, true);
    QObject::connect(openBattleRunsButton_, &QPushButton::clicked, statusStrip, [this]() {
        showBattleRunsPane();
    });
    statusLayout->addWidget(openBattleRunsButton_);

    openWorkflowsButton_ = createActionButton(QStringLiteral("Open Workflows"), statusStrip, true);
    QObject::connect(openWorkflowsButton_, &QPushButton::clicked, statusStrip, [this]() {
        if (actions_.openWorkflows) {
            actions_.openWorkflows();
        }
    });
    statusLayout->addWidget(openWorkflowsButton_);

    openArtifactsButton_ = createActionButton(QStringLiteral("Open Related Artifacts"), statusStrip);
    QObject::connect(openArtifactsButton_, &QPushButton::clicked, statusStrip, [this]() {
        if (actions_.openArtifacts) {
            actions_.openArtifacts();
        }
    });
    statusLayout->addWidget(openArtifactsButton_);

    canvasLayout()->addWidget(statusStrip);

    auto* workbench = new QFrame(this);
    workbench->setObjectName("workspaceCardGrid");
    workbench->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* workbenchLayout = new QHBoxLayout(workbench);
    workbenchLayout->setContentsMargins(0, 0, 0, 0);
    workbenchLayout->setSpacing(10);

    auto* selectorPanel = createSectionPanel(QStringLiteral("Analysis"), workbench);
    selectorPanel->setMinimumWidth(210);
    selectorPanel->setMaximumWidth(260);
    auto* selectorLayout = qobject_cast<QVBoxLayout*>(selectorPanel->layout());
    selectorGroup_ = new QButtonGroup(selectorPanel);
    selectorGroup_->setExclusive(true);

    auto* overviewButton = createSelectorButton(QStringLiteral("Overview"), selectorPanel);
    auto* explorerButton = createSelectorButton(QStringLiteral("Battle Runs"), selectorPanel);
    auto* workflowButton = createSelectorButton(QStringLiteral("Workflow Provenance"), selectorPanel);
    auto* futureButton = createSelectorButton(QStringLiteral("Future Analyses"), selectorPanel);
    selectorGroup_->addButton(overviewButton, OverviewPane);
    selectorGroup_->addButton(explorerButton, BattleRunsPane);
    selectorGroup_->addButton(workflowButton, WorkflowPane);
    selectorGroup_->addButton(futureButton, FuturePane);
    selectorLayout->addWidget(overviewButton);
    selectorLayout->addWidget(explorerButton);
    selectorLayout->addWidget(workflowButton);
    selectorLayout->addWidget(futureButton);
    selectorLayout->addStretch();
    overviewButton->setChecked(true);

    QObject::connect(selectorGroup_, &QButtonGroup::idClicked, this, [this](int id) {
        setCurrentPane(id);
    });
    workbenchLayout->addWidget(selectorPanel);

    paneStack_ = new QStackedWidget(workbench);
    paneStack_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* overviewPanel = createSectionPanel(QStringLiteral("Results Overview"), paneStack_);
    overviewPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* overviewLayout = qobject_cast<QVBoxLayout*>(overviewPanel->layout());
    overviewSummaryLabel_ = createSummaryLabel(overviewPanel);
    overviewLayout->addWidget(overviewSummaryLabel_);
    overviewTable_ = new QTableWidget(overviewPanel);
    configureTable(overviewTable_);
    overviewTable_->setColumnCount(5);
    overviewTable_->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("Surface"),
        QStringLiteral("Recent"),
        QStringLiteral("Complete"),
        QStringLiteral("Problems"),
        QStringLiteral("Supporting evidence"),
    });
    overviewTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    overviewTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    overviewTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    overviewTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    overviewTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    QObject::connect(overviewTable_, &QTableWidget::cellDoubleClicked, overviewTable_, [this](int row, int) {
        if (row == 0) {
            showBattleRunsPane();
        } else if (row == 1 && actions_.openWorkflows) {
            actions_.openWorkflows();
        } else if (row >= 2 && actions_.openArtifacts) {
            actions_.openArtifacts();
        }
    });
    overviewLayout->addWidget(overviewTable_, 1);
    paneStack_->addWidget(overviewPanel);

    battleRunsWidget_ = new savorqt::gui::BattleRunsWidget(savorqt::gui::BattleRunsWidget::Actions{
        [this](std::int64_t jobId) { showBattleJobDetails(static_cast<qint64>(jobId)); },
        [this](std::int64_t jobId) {
            showBattleUnavailable(
                static_cast<qint64>(jobId),
                QStringLiteral("Battle Plan"),
                QStringLiteral("Battle plan reconstruction is not projected into UIRead yet."));
        },
        [this](std::int64_t jobId) {
            showBattleUnavailable(
                static_cast<qint64>(jobId),
                QStringLiteral("Replication Details"),
                QStringLiteral("Replication details are not projected into UIRead yet."));
        },
        [this](std::int64_t jobId) {
            showBattleUnavailable(
                static_cast<qint64>(jobId),
                QStringLiteral("Turn Inputs"),
                QStringLiteral("Turn input detail is not projected into UIRead yet."));
        },
        [this](std::int64_t jobId) {
            if (actions_.replayVisual) {
                actions_.replayVisual(static_cast<qint64>(jobId));
            }
        },
        {}
    }, paneStack_);
    battleRunsWidget_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    paneStack_->addWidget(battleRunsWidget_);

    auto* workflowsPanel = createSectionPanel(QStringLiteral("Workflow Provenance"), paneStack_);
    workflowsPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* workflowsLayout = qobject_cast<QVBoxLayout*>(workflowsPanel->layout());
    auto* workflowsHeader = new QHBoxLayout();
    workflowsHeader->setContentsMargins(0, 0, 0, 0);
    workflowsHeader->setSpacing(8);
    workflowsSummaryLabel_ = createSummaryLabel(workflowsPanel);
    workflowsHeader->addWidget(workflowsSummaryLabel_, 1);
    workflowsLayout->addLayout(workflowsHeader);
    workflowsTable_ = new QTableWidget(workflowsPanel);
    configureTable(workflowsTable_);
    workflowsTable_->setColumnCount(7);
    workflowsTable_->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("Workflow"),
        QStringLiteral("Kind"),
        QStringLiteral("State"),
        QStringLiteral("Problems"),
        QStringLiteral("Created"),
        QStringLiteral("Completed"),
        QStringLiteral("Outputs / evidence"),
    });
    workflowsTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    workflowsTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    workflowsTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    workflowsTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    workflowsTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    workflowsTable_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    workflowsTable_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    QObject::connect(workflowsTable_, &QTableWidget::cellDoubleClicked, workflowsTable_, [this](int, int) {
        if (actions_.openWorkflows) {
            actions_.openWorkflows();
        }
    });
    workflowsLayout->addWidget(workflowsTable_, 1);
    paneStack_->addWidget(workflowsPanel);

    auto* futurePanel = createSectionPanel(QStringLiteral("Future Analyses"), paneStack_);
    futurePanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* futureLayout = qobject_cast<QVBoxLayout*>(futurePanel->layout());
    futureSummaryLabel_ = createSummaryLabel(futurePanel);
    futureSummaryLabel_->setText(QStringLiteral(
        "Additional analysis families will appear here as their workflow units and UIRead projections land. "
        "This placeholder keeps the workbench stable without advertising unsupported drill-downs."));
    futureLayout->addWidget(futureSummaryLabel_);
    futureLayout->addStretch();
    paneStack_->addWidget(futurePanel);

    workbenchLayout->addWidget(paneStack_, 1);
    canvasLayout()->addWidget(workbench, 1);

    auto* refreshPipeline = new savorqt::gui::AsyncRefreshPipeline<AnalysisRefreshRequest, AnalysisRefreshData>(this);
    auto overviewRows = std::make_shared<std::vector<OverviewRow>>();
    auto workflowRows = std::make_shared<std::vector<ProvenanceRow>>();
    refreshPipeline->setRefreshIntervalMs(5000);
    refreshPipeline->setRequestBuilder([](savorqt::gui::RefreshReason) {
        return AnalysisRefreshRequest{};
    });
    refreshPipeline->setLoadAndPrepare([](AnalysisRefreshRequest) {
        return savorqt::gui::AsyncRefreshResult<AnalysisRefreshData>::Ok(prepareAnalysisData(loadSnapshot()));
    });
    refreshPipeline->setApply([=](const AnalysisRefreshData& data, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        outcomesValueLabel_->setText(data.outcomesValue);
        workflowsValueLabel_->setText(data.workflowsValue);
        artifactsValueLabel_->setText(data.artifactsValue);
        problemsValueLabel_->setText(data.problemsValue);
        lastRefreshLabel_->setText(data.lastRefreshValue);

        overviewSummaryLabel_->setText(data.overviewSummary);
        savorqt::gui::ApplyTableRowsByKey(
            overviewTable_,
            *overviewRows,
            data.overviewRows,
            [](const OverviewRow& row) { return row.key; },
            overviewRowsEqual,
            populateOverviewRow);

        workflowsSummaryLabel_->setText(data.workflowsSummary);
        savorqt::gui::ApplyTableRowsByKey(
            workflowsTable_,
            *workflowRows,
            data.workflowRows,
            [](const ProvenanceRow& row) { return row.workflowId; },
            provenanceRowsEqual,
            populateProvenanceRow);

        setCurrentPane(currentPaneIndex_);
    });
    refreshPipeline->setApplyError([this](const QString& error, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        overviewSummaryLabel_->setText(error);
        workflowsSummaryLabel_->setText(error);
    });
    requestResultsRefresh_ = [refreshPipeline]() {
        refreshPipeline->requestRefresh(savorqt::gui::RefreshReason::Manual);
    };

    refreshPipeline->setActive(true);
    refreshPipeline->requestRefresh(savorqt::gui::RefreshReason::Initial);
}

void AnalysisTab::setCurrentPane(int index)
{
    currentPaneIndex_ = index;
    if (paneStack_ != nullptr) {
        paneStack_->setCurrentIndex(index);
    }
    if (battleRunsWidget_ != nullptr) {
        battleRunsWidget_->setPageActive(pageActive_ && index == BattleRunsPane);
    }
    if (openBattleRunsButton_ != nullptr) {
        openBattleRunsButton_->setVisible(index == OverviewPane || index == BattleRunsPane);
    }
    if (openWorkflowsButton_ != nullptr) {
        openWorkflowsButton_->setVisible(index == OverviewPane || index == WorkflowPane);
    }
    if (openArtifactsButton_ != nullptr) {
        openArtifactsButton_->setVisible(index == BattleRunsPane || index == WorkflowPane);
    }
}

void AnalysisTab::refreshResults()
{
    if (requestResultsRefresh_) {
        requestResultsRefresh_();
    }
}

void AnalysisTab::showBattleRunsPane()
{
    if (selectorGroup_ != nullptr) {
        if (auto* button = selectorGroup_->button(BattleRunsPane); button != nullptr) {
            button->setChecked(true);
        }
    }
    setCurrentPane(BattleRunsPane);
}

void AnalysisTab::setPageActive(bool active)
{
    pageActive_ = active;
    if (battleRunsWidget_ != nullptr) {
        battleRunsWidget_->setPageActive(pageActive_ && currentPaneIndex_ == BattleRunsPane);
    }
}

void AnalysisTab::showBattleJobDetails(qint64 jobId)
{
    const auto detail = savorqt::db::SavorDbExplorerRunService::GetBattleTurnJobDetail(jobId);
    if (!detail.ok) {
        showBattleUnavailable(
            jobId,
            QStringLiteral("Battle Job #%1").arg(jobId),
            QStringLiteral("Battle job detail is unavailable: %1").arg(qs(detail.error.message)));
        return;
    }

    const auto& battle = detail.value.battle;
    const auto& job = battle.summary;
    const qint64 execJobId = job.exec_job_id.has_value() ? static_cast<qint64>(*job.exec_job_id) : 0;
    QString body;
    body += QStringLiteral("<table>");
    body += fieldHtml(QStringLiteral("Turn job"), QStringLiteral("#%1").arg(job.turn_job_id));
    body += fieldHtml(QStringLiteral("Execution job"), execJobId > 0 ? QStringLiteral("#%1").arg(execJobId) : QStringLiteral("--"));
    body += fieldHtml(QStringLiteral("Battle set"), battle.group.has_value()
        ? QStringLiteral("#%1 %2").arg(battle.group->battle_set_id).arg(qs(battle.group->name))
        : QStringLiteral("#%1").arg(job.battle_set_id));
    body += fieldHtml(QStringLiteral("Wave"), battle.wave.has_value()
        ? QStringLiteral("#%1 turn %2").arg(battle.wave->wave_id).arg(battle.wave->turn_index + 1)
        : QStringLiteral("#%1").arg(job.wave_id));
    body += fieldHtml(QStringLiteral("State"), qs(job.job_state));
    body += fieldHtml(QStringLiteral("Outcome"), optionalIntHtml(job.battle_outcome));
    body += fieldHtml(QStringLiteral("Predicates"), QStringLiteral("%1/%2")
        .arg(optionalIntHtml(job.pred_passed), optionalIntHtml(job.pred_total)));
    body += fieldHtml(QStringLiteral("Delta VI"), optionalInt64Html(job.delta_vi));
    body += fieldHtml(QStringLiteral("RNG seed"), optionalInt64Html(job.rng_seed));
    body += fieldHtml(QStringLiteral("Fake attacks"), QStringLiteral("%1 turn / %2 before")
        .arg(job.fake_attacks_this_turn)
        .arg(job.fake_attacks_used_before));
    if (job.followup.has_value()) {
        body += fieldHtml(QStringLiteral("Victory"), job.followup->is_victory ? QStringLiteral("yes") : QStringLiteral("no"));
        body += fieldHtml(QStringLiteral("Follow-up"), qs(job.followup->manual_followup_status));
        body += fieldHtml(QStringLiteral("Recorded DTM"), job.followup->recorded_dtm_artifact_id.has_value()
            ? QStringLiteral("#%1").arg(*job.followup->recorded_dtm_artifact_id)
            : QStringLiteral("--"));
        body += fieldHtml(QStringLiteral("Note"), qs(job.followup->note));
        body += fieldHtml(QStringLiteral("Follow-up updated"), formatBattleTime(job.followup->updated_at_utc));
    } else {
        body += fieldHtml(QStringLiteral("Follow-up"), QStringLiteral("--"));
    }
    body += fieldHtml(QStringLiteral("Started"), formatBattleTime(job.started_at_utc));
    body += fieldHtml(QStringLiteral("Ended"), formatBattleTime(job.ended_at_utc));
    if (detail.value.job.has_value()) {
        const auto& summary = detail.value.job->summary;
        body += fieldHtml(QStringLiteral("Attempts"), QStringLiteral("%1/%2").arg(summary.attempts).arg(summary.max_attempts));
        body += fieldHtml(QStringLiteral("Error"), summary.error_text.empty() ? QStringLiteral("--") : qs(summary.error_text));
    }
    body += QStringLiteral("</table>");

    body += QStringLiteral("<h4>Artifacts</h4>");
    body += artifactSummaryHtml(battle.artifacts);

    body += QStringLiteral("<h4>Lifecycle Events</h4>");
    if (detail.value.events.empty()) {
        body += QStringLiteral("<p>No execution lifecycle events are available.</p>");
    } else {
        body += QStringLiteral("<ul>");
        for (const auto& event : detail.value.events) {
            QString line = QStringLiteral("%1 %2")
                .arg(formatBattleTime(event.event_ts_utc))
                .arg(qs(event.event_kind));
            if (!event.message.empty()) {
                line += QStringLiteral(" - %1").arg(qs(event.message));
            }
            body += QStringLiteral("<li>%1</li>").arg(line.toHtmlEscaped());
        }
        body += QStringLiteral("</ul>");
    }

    setContext(
        savorqt::gui::UiEntityRef{
            QStringLiteral("analysis"),
            QStringLiteral("battle_job"),
            jobId,
            QStringLiteral("battle_job:%1").arg(jobId),
        },
        QStringLiteral("Battle Job #%1").arg(jobId),
        body,
        QVector<std::pair<QString, std::function<void()>>>{
            {
                QStringLiteral("Replay Visually"),
                [this, execJobId]() {
                    if (actions_.replayVisual && execJobId > 0) {
                        actions_.replayVisual(execJobId);
                    }
                },
            },
            {
                QStringLiteral("Open Artifacts"),
                [this]() {
                    if (actions_.openArtifacts) {
                        actions_.openArtifacts();
                    }
                },
            },
        },
        savorqt::gui::ContextDrawerMode::Expanded);
}

void AnalysisTab::showBattleUnavailable(qint64 jobId, const QString& title, const QString& message)
{
    setContext(
        savorqt::gui::UiEntityRef{
            QStringLiteral("analysis"),
            QStringLiteral("battle_job"),
            jobId,
            QStringLiteral("battle_job:%1").arg(jobId),
        },
        title,
        QStringLiteral("<p>%1</p><p>Battle job: #%2</p>").arg(message.toHtmlEscaped()).arg(jobId),
        QVector<std::pair<QString, std::function<void()>>>{
            {
                QStringLiteral("View Job Details"),
                [this, jobId]() { showBattleJobDetails(jobId); },
            },
        },
        savorqt::gui::ContextDrawerMode::Expanded);
}
