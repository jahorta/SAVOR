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
    TasMoviesPane = 3,
    FuturePane = 4,
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
    std::vector<savor::db::UiTasMovieRootSummary> tasRoots;
    std::vector<savor::db::UiTasMovieTreeSummary> tasTrees;
    std::vector<savor::db::UiTasMovieValidationRequestSummary> tasValidationRequests;
    std::vector<savor::db::UiTasMovieSterilizationRequestSummary> tasSterilizationRequests;
    std::vector<savor::db::UiSavestateSummary> tasSavestates;

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
struct TasMovieRow { qint64 key=0; QString kind; qint64 refId=0; QString lineage; QString source; QString status; QString checkpoint; QString workflow; QString actionUnit; QString actionInput; };

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
    std::vector<TasMovieRow> tasMovieRows;
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

bool isWorkflowFinal(const savor::db::UiWorkflowInstanceSummary& workflow)
{
    return workflow.state == "COMPLETED" || workflow.state == "CANCELED";
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
        snapshot.tasRoots = uiRead->ListTasMovieRoots(200);
        snapshot.tasTrees = uiRead->ListTasMovieTrees(200);
        snapshot.tasValidationRequests = uiRead->ListTasMovieValidationRequests(300);
        snapshot.tasSterilizationRequests = uiRead->ListTasMovieSterilizationRequests(300);
        snapshot.tasSavestates = uiRead->ListSavestates("MOVIE_PAIRED",true,"",200);
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
bool tasMovieRowsEqual(const TasMovieRow& a,const TasMovieRow& b){return a.key==b.key&&a.kind==b.kind&&a.refId==b.refId&&a.lineage==b.lineage&&a.source==b.source&&a.status==b.status&&a.checkpoint==b.checkpoint&&a.workflow==b.workflow&&a.actionUnit==b.actionUnit&&a.actionInput==b.actionInput;}

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
void populateTasMovieRow(QTableWidget* table,int row,const TasMovieRow& item){auto* kind=createTableItem(item.kind);kind->setData(Qt::UserRole,item.actionUnit);kind->setData(Qt::UserRole+1,item.actionInput);kind->setData(Qt::UserRole+2,item.refId);table->setItem(row,0,kind);table->setItem(row,1,createTableItem(QStringLiteral("#%1").arg(item.refId)));table->setItem(row,2,createTableItem(item.lineage));table->setItem(row,3,createTableItem(item.source));table->setItem(row,4,createTableItem(item.status));table->setItem(row,5,createTableItem(item.checkpoint));table->setItem(row,6,createTableItem(item.workflow));}

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

    const int finalWorkflows = snapshot.workflowsOk
        ? static_cast<int>(std::count_if(snapshot.workflows.begin(), snapshot.workflows.end(), isWorkflowFinal))
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
            snapshot.workflowsOk ? QString::number(finalWorkflows) : QStringLiteral("--"),
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
    for(const auto& artifact:snapshot.artifacts)if(artifact.artifact_kind=="DTM")data.tasMovieRows.push_back({artifact.artifact_id,QStringLiteral("DTM"),artifact.artifact_id,QStringLiteral("--"),QStringLiteral("%1 · %2").arg(qs(artifact.filename),qs(artifact.sha256).left(12)),QStringLiteral("COMPLETE"),QStringLiteral("%1 bytes").arg(artifact.size_bytes),QStringLiteral("--"),QStringLiteral("tas_movie_establish_root_cursor"),QStringLiteral("root_dtm")});
    for(const auto& root:snapshot.tasRoots)data.tasMovieRows.push_back({1000000000LL+root.tas_movie_root_id,QStringLiteral("Root"),root.tas_movie_root_id,QStringLiteral("RTC %1").arg(root.rtc_value),QStringLiteral("source DTM #%1 / effective #%2 / itinerary #%3").arg(root.source_dtm_artifact_id).arg(root.dtm_artifact_id).arg(root.itinerary_artifact_id),QStringLiteral("ESTABLISHED"),QStringLiteral("state #%1 · PC 0x%2").arg(root.checkpoint_savestate_id).arg(root.required_final_breakpoint_pc,8,16,QChar('0')),QStringLiteral("%1 #%2").arg(qs(root.source_context_kind)).arg(root.source_context_id),QString(),QString()});
    for(const auto& tree:snapshot.tasTrees)data.tasMovieRows.push_back({2000000000LL+tree.tas_movie_tree_id,QStringLiteral("Recorded TAS Branch"),tree.tas_movie_tree_id,QStringLiteral("root #%1 / parent %2").arg(tree.tas_movie_root_id).arg(tree.parent_tas_movie_tree_id?QString::number(*tree.parent_tas_movie_tree_id):QStringLiteral("--")),QStringLiteral("DTM #%1 / itinerary #%2").arg(tree.dtm_artifact_id).arg(tree.itinerary_artifact_id),QStringLiteral("AVAILABLE"),QStringLiteral("state #%1 · PC 0x%2").arg(tree.checkpoint_savestate_id).arg(tree.required_final_breakpoint_pc,8,16,QChar('0')),QStringLiteral("%1 #%2").arg(qs(tree.source_context_kind)).arg(tree.source_context_id),QStringLiteral("tas_movie_validate_tree"),QStringLiteral("tas_movie_tree")});
    for(const auto& state:snapshot.tasSavestates)data.tasMovieRows.push_back({2500000000LL+state.savestate_id,QStringLiteral("Paired checkpoint"),state.savestate_id,QStringLiteral("DTM #%1").arg(state.dtm_artifact_id.value_or(0)),QStringLiteral("%1 · %2").arg(qs(state.filename),qs(state.sha256).left(12)),qs(state.playback_state),QStringLiteral("%1 bytes").arg(state.size_bytes),QStringLiteral("--"),QStringLiteral("tas_movie_checkpoint_sterilize"),QStringLiteral("paired_checkpoint_savestate")});
    for(const auto& request:snapshot.tasValidationRequests)data.tasMovieRows.push_back({3000000000LL+request.validation_request_id,QStringLiteral("Validation"),request.latest_validation_attempt_id.value_or(request.validation_request_id),QStringLiteral("%1 #%2").arg(qs(request.source_kind)).arg(request.source_ref_id),QStringLiteral("DTM #%1 · %2").arg(request.source_dtm_artifact_id).arg(qs(request.effective_dtm_sha256).left(12)),request.latest_outcome.empty()?QStringLiteral("REQUESTED"):qs(request.latest_outcome),QStringLiteral("PC 0x%1 · input %2 · root %3").arg(request.latest_actual_pc.value_or(request.required_final_breakpoint_pc),8,16,QChar('0')).arg(request.latest_actual_input_count?QString::number(*request.latest_actual_input_count):QStringLiteral("--")).arg(request.produced_tas_movie_root_id?QString::number(*request.produced_tas_movie_root_id):QStringLiteral("--")),QStringLiteral("workflow #%1 / step #%2").arg(request.workflow_instance_id).arg(request.workflow_step_id),request.latest_outcome=="ROOT_CURSOR_ESTABLISHED"?QStringLiteral("tas_movie_validate_root"):QString(),QStringLiteral("root_establishment")});
    for(const auto& request:snapshot.tasSterilizationRequests)data.tasMovieRows.push_back({4000000000LL+request.sterilization_request_id,QStringLiteral("Sterilization"),request.sterilization_request_id,QStringLiteral("source state #%1").arg(request.source_savestate_id),QStringLiteral("SAV %1 / DTM %2").arg(qs(request.source_savestate_sha256).left(12),qs(request.source_dtm_sha256).left(12)),request.latest_sterilization_attempt_id?QStringLiteral("COMPLETED"):QStringLiteral("REQUESTED"),QStringLiteral("result state %1").arg(request.latest_produced_savestate_id?QString::number(*request.latest_produced_savestate_id):QStringLiteral("--")),QStringLiteral("workflow #%1 / step #%2").arg(request.workflow_instance_id).arg(request.workflow_step_id),QString(),QString()});

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

QString battleCommandHtml(const soa::battle::actions::BattleCommand& command)
{
    QString out = QStringLiteral("[%1] %2")
        .arg(command.actor_slot)
        .arg(qs(soa::battle::actions::get_action_string(command.macro)));
    if (command.macro == soa::battle::actions::BattleAction::Attack) {
        out += QStringLiteral(" target=%1").arg(command.params.target_slot <= 11 ? command.params.target_slot : 0xFF);
    } else if (command.macro == soa::battle::actions::BattleAction::UseItem) {
        out += QStringLiteral(" item=%1 target=%2")
            .arg(command.params.item_id)
            .arg(command.params.target_slot <= 11 ? command.params.target_slot : 0xFF);
    }
    return out.toHtmlEscaped();
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
    auto* tasMoviesButton = createSelectorButton(QStringLiteral("TAS Movies"), selectorPanel);
    auto* futureButton = createSelectorButton(QStringLiteral("Future Analyses"), selectorPanel);
    selectorGroup_->addButton(overviewButton, OverviewPane);
    selectorGroup_->addButton(explorerButton, BattleRunsPane);
    selectorGroup_->addButton(workflowButton, WorkflowPane);
    selectorGroup_->addButton(tasMoviesButton, TasMoviesPane);
    selectorGroup_->addButton(futureButton, FuturePane);
    selectorLayout->addWidget(overviewButton);
    selectorLayout->addWidget(explorerButton);
    selectorLayout->addWidget(workflowButton);
    selectorLayout->addWidget(tasMoviesButton);
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
            showBattleReplicationDetails(static_cast<qint64>(jobId));
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

    auto* tasPanel=createSectionPanel(QStringLiteral("TAS Movies"),paneStack_);auto* tasLayout=qobject_cast<QVBoxLayout*>(tasPanel->layout());auto* tasSummary=createSummaryLabel(tasPanel);tasSummary->setText(QStringLiteral("Projected TAS roots, trees, validation attempts, paired checkpoints, and sterilization history."));tasLayout->addWidget(tasSummary);tasMoviesTable_=new QTableWidget(tasPanel);configureTable(tasMoviesTable_);tasMoviesTable_->setColumnCount(7);tasMoviesTable_->setHorizontalHeaderLabels({QStringLiteral("Kind"),QStringLiteral("ID"),QStringLiteral("Lineage"),QStringLiteral("Source / hashes"),QStringLiteral("Status"),QStringLiteral("Checkpoint"),QStringLiteral("Workflow")});tasMoviesTable_->horizontalHeader()->setSectionResizeMode(0,QHeaderView::ResizeToContents);tasMoviesTable_->horizontalHeader()->setSectionResizeMode(1,QHeaderView::ResizeToContents);tasMoviesTable_->horizontalHeader()->setSectionResizeMode(2,QHeaderView::ResizeToContents);tasMoviesTable_->horizontalHeader()->setSectionResizeMode(3,QHeaderView::Stretch);tasMoviesTable_->horizontalHeader()->setSectionResizeMode(4,QHeaderView::ResizeToContents);tasMoviesTable_->horizontalHeader()->setSectionResizeMode(5,QHeaderView::ResizeToContents);tasMoviesTable_->horizontalHeader()->setSectionResizeMode(6,QHeaderView::ResizeToContents);tasLayout->addWidget(tasMoviesTable_,1);
    QObject::connect(tasMoviesTable_,&QTableWidget::cellDoubleClicked,tasMoviesTable_,[this](int row,int){const auto* item=tasMoviesTable_->item(row,0);if(item==nullptr)return;const auto unit=item->data(Qt::UserRole).toString();const auto input=item->data(Qt::UserRole+1).toString();const auto refId=item->data(Qt::UserRole+2).toLongLong();QString body=QStringLiteral("%1 %2\n%3\n%4\n%5").arg(tasMoviesTable_->item(row,0)->text(),tasMoviesTable_->item(row,1)->text(),tasMoviesTable_->item(row,2)->text(),tasMoviesTable_->item(row,3)->text(),tasMoviesTable_->item(row,5)->text());QVector<std::pair<QString,std::function<void()>>> actions;if(!unit.isEmpty()&&actions_.openSetupLauncher)actions.push_back({QStringLiteral("Open modular action"),[this,unit,input,refId](){actions_.openSetupLauncher(unit,input,refId);}});setContext({QStringLiteral("analysis"),QStringLiteral("tas_movie"),refId,item->text()},QStringLiteral("TAS Movie evidence"),body,actions,savorqt::gui::ContextDrawerMode::Expanded);});
    paneStack_->addWidget(tasPanel);

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
    auto tasMovieRows = std::make_shared<std::vector<TasMovieRow>>();
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
        savorqt::gui::ApplyTableRowsByKey(tasMoviesTable_,*tasMovieRows,data.tasMovieRows,[](const TasMovieRow& row){return row.key;},tasMovieRowsEqual,populateTasMovieRow);

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
    QString advancement = QString(QChar(0x25CB)) + QStringLiteral(" Miss");
    if (job.advancement_rank >= 2) {
        advancement = QString(QChar(0x25CF)) + QStringLiteral(" Selected");
    } else if (job.advancement_rank == 1) {
        advancement = QString(QChar(0x25D0)) + QStringLiteral(" Candidate");
    }
    if (!job.advancement_decision_kind.empty()) {
        advancement += QStringLiteral(" (%1)").arg(qs(job.advancement_decision_kind));
    }
    body += fieldHtml(QStringLiteral("Advancement"), advancement);
    body += fieldHtml(QStringLiteral("Desired outcome"), job.has_desired_outcome ? QStringLiteral("yes") : QStringLiteral("no"));
    if (job.manual_followup.has_value()) {
        body += fieldHtml(QStringLiteral("Manual follow-up"), qs(job.manual_followup->manual_followup_status));
        body += fieldHtml(QStringLiteral("Recorded DTM"), job.manual_followup->recorded_dtm_artifact_id.has_value()
            ? QStringLiteral("#%1").arg(*job.manual_followup->recorded_dtm_artifact_id)
            : QStringLiteral("--"));
        body += fieldHtml(QStringLiteral("Note"), qs(job.manual_followup->note));
        body += fieldHtml(QStringLiteral("Manual follow-up updated"), formatBattleTime(job.manual_followup->updated_at_utc));
    } else {
        body += fieldHtml(QStringLiteral("Manual follow-up"), QStringLiteral("--"));
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

void AnalysisTab::showBattleReplicationDetails(qint64 jobId)
{
    const auto details = savorqt::db::SavorDbExplorerRunService::GetBattleReplicationDetails(jobId);
    if (!details.ok) {
        showBattleUnavailable(
            jobId,
            QStringLiteral("Replication Details #%1").arg(jobId),
            QStringLiteral("Replication details are unavailable: %1").arg(qs(details.error.message)));
        return;
    }

    const auto& origin = details.value.origin;
    QString body;
    body += QStringLiteral("<h4>Battle Origin</h4><table>");
    body += fieldHtml(QStringLiteral("Entry savestate"), origin.entry_savestate_id.has_value()
        ? QStringLiteral("#%1").arg(*origin.entry_savestate_id)
        : QStringLiteral("--"));
    body += fieldHtml(QStringLiteral("Entry savestate artifact"), origin.entry_savestate_artifact_id.has_value()
        ? QStringLiteral("#%1").arg(*origin.entry_savestate_artifact_id)
        : QStringLiteral("--"));
    body += fieldHtml(QStringLiteral("Seed candidate"), origin.seed_candidate_id.has_value()
        ? QStringLiteral("#%1").arg(*origin.seed_candidate_id)
        : QStringLiteral("--"));
    body += fieldHtml(QStringLiteral("Seed source"), origin.seed_source_kind.empty() ? QStringLiteral("--") : qs(origin.seed_source_kind));
    body += fieldHtml(QStringLiteral("SeedProbe result"), origin.source_probe_result_id.has_value()
        ? QStringLiteral("#%1").arg(*origin.source_probe_result_id)
        : QStringLiteral("--"));
    body += fieldHtml(QStringLiteral("Input frame"), origin.source_input_frame_id.has_value()
        ? QStringLiteral("#%1").arg(*origin.source_input_frame_id)
        : QStringLiteral("--"));
    if (origin.initial_input.has_value()) {
        const auto& frame = *origin.initial_input;
        body += fieldHtml(QStringLiteral("Initial input"), QStringLiteral("main=(%1,%2) c=(%3,%4) trig=(%5,%6)")
            .arg(frame.main_x).arg(frame.main_y)
            .arg(frame.c_x).arg(frame.c_y)
            .arg(frame.trig_l).arg(frame.trig_r));
    }
    body += QStringLiteral("</table>");

    body += QStringLiteral("<h4>Turn Chain</h4>");
    for (const auto& turn : details.value.turns) {
        const auto& row = turn.row;
        body += QStringLiteral("<h5>Turn %1 job #%2</h5><table>")
            .arg(row.authored_turn_index.value_or(0))
            .arg(row.turn_job_id);
        body += fieldHtml(QStringLiteral("Execution job"), row.exec_job_id.has_value()
            ? QStringLiteral("#%1").arg(*row.exec_job_id)
            : QStringLiteral("--"));
        body += fieldHtml(QStringLiteral("Source savestate"), row.source_savestate_id.has_value()
            ? QStringLiteral("#%1").arg(*row.source_savestate_id)
            : QStringLiteral("--"));
        body += fieldHtml(QStringLiteral("Output savestate"), row.output_savestate_id.has_value()
            ? QStringLiteral("#%1").arg(*row.output_savestate_id)
            : QStringLiteral("--"));
        body += fieldHtml(QStringLiteral("Fake attacks"), QStringLiteral("%1 turn / %2 before")
            .arg(row.fake_attacks_this_turn)
            .arg(row.fake_attacks_used_before));
        body += fieldHtml(QStringLiteral("Variant"), row.resolved_turn_variant_key.has_value()
            ? qs(*row.resolved_turn_variant_key)
            : QStringLiteral("--"));
        body += fieldHtml(QStringLiteral("Input trace artifact"), row.input_trace_artifact_id.has_value()
            ? QStringLiteral("#%1").arg(*row.input_trace_artifact_id)
            : QStringLiteral("--"));
        body += QStringLiteral("</table>");
        if (!turn.command_decode_ok) {
            body += QStringLiteral("<p>Resolved commands are not available for this turn.</p>");
        } else {
            body += QStringLiteral("<ol>");
            for (const auto& command : turn.commands) {
                body += QStringLiteral("<li>%1</li>").arg(battleCommandHtml(command));
            }
            body += QStringLiteral("</ol>");
        }
    }

    setContext(
        savorqt::gui::UiEntityRef{
            QStringLiteral("analysis"),
            QStringLiteral("battle_job"),
            jobId,
            QStringLiteral("battle_job:%1:replication").arg(jobId),
        },
        QStringLiteral("Replication Details #%1").arg(jobId),
        body,
        QVector<std::pair<QString, std::function<void()>>>{
            {
                QStringLiteral("View Job Details"),
                [this, jobId]() { showBattleJobDetails(jobId); },
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
