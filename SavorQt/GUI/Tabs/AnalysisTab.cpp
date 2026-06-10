#include "GUI/Tabs/AnalysisTab.h"

#include "DB/SavorDbArtifactService.h"
#include "DB/SavorDbExplorerRunService.h"
#include "SavorDbRuntime.h"

#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

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

AnalysisTab::AnalysisTab(Actions actions, QWidget* parent)
    : WorkspacePageShell(
        QStringLiteral("analysis"),
        QStringLiteral("Analysis"),
        QStringLiteral("Interpret completed workflow outputs, inspect seed probes, compare candidates, and decide follow-up work."),
        parent)
    , actions_(std::move(actions))
{
    build();
}

void AnalysisTab::build()
{
    int seedRuns = 0;
    int completedSeedRuns = 0;
    auto* db = savorqt::SavorDbRuntime::instance().uiReadDb();
    if (db != nullptr) {
        savor::db::UiReadSeedProbeRunListQuery seedQuery{};
        seedQuery.limit = 20;
        const auto page = db->ListSeedProbeRuns(seedQuery);
        seedRuns = static_cast<int>(page.items.size());
        for (const auto& run : page.items) {
            if (run.status == "done" || run.status == "completed" || run.status == "COMPLETED") {
                ++completedSeedRuns;
            }
        }
    }

    savorqt::db::ExplorerRunGroupQuery explorerQuery{};
    explorerQuery.limit = 20;
    const auto explorerGroups = savorqt::db::SavorDbExplorerRunService::ListGroups(explorerQuery);
    int completedExplorerGroups = 0;
    int explorerGroupsWithIssues = 0;
    if (explorerGroups.ok) {
        for (const auto& group : explorerGroups.value.groups) {
            if (group.completed_jobs >= group.total_jobs && group.total_jobs > 0) {
                ++completedExplorerGroups;
            }
            if (group.failed_jobs > 0 || group.canceled_jobs > 0) {
                ++explorerGroupsWithIssues;
            }
        }
    }

    savor::db::UiReadArtifactListQuery artifactQuery{};
    artifactQuery.limit = 25;
    const auto artifacts = savorqt::db::SavorDbArtifactService::ListArtifacts(artifactQuery);

    auto* hero = new QFrame(this);
    hero->setObjectName("workspaceHeroPanel");
    auto* heroLayout = new QHBoxLayout(hero);
    heroLayout->setContentsMargins(14, 14, 14, 14);
    heroLayout->setSpacing(12);
    auto* heroText = new QFrame(hero);
    auto* heroTextLayout = new QVBoxLayout(heroText);
    heroTextLayout->setContentsMargins(0, 0, 0, 0);
    heroTextLayout->setSpacing(6);
    auto* heroTitle = new QLabel(QStringLiteral("Results and candidate provenance"), heroText);
    heroTitle->setObjectName("panelTitle");
    auto* heroBody = new QLabel(QStringLiteral("Use this workspace to answer what was found, where it came from, and which workflow group produced it."), heroText);
    heroBody->setObjectName("sectionDescription");
    heroBody->setWordWrap(true);
    heroTextLayout->addWidget(heroTitle);
    heroTextLayout->addWidget(heroBody);
    heroTextLayout->addStretch();
    heroLayout->addWidget(heroText, 2);

    auto* heroMetrics = new QGridLayout();
    heroMetrics->setContentsMargins(0, 0, 0, 0);
    heroMetrics->setSpacing(8);
    heroMetrics->addWidget(createMetricTile(QStringLiteral("Seed probes"), QString::number(seedRuns), QStringLiteral("Recent runs"), hero), 0, 0);
    heroMetrics->addWidget(createMetricTile(QStringLiteral("Seed complete"), QString::number(completedSeedRuns), QStringLiteral("Completed runs"), hero), 0, 1);
    heroMetrics->addWidget(createMetricTile(QStringLiteral("Explorer groups"), explorerGroups.ok ? QString::number(static_cast<int>(explorerGroups.value.groups.size())) : QStringLiteral("--"), QStringLiteral("Recent groups"), hero), 1, 0);
    heroMetrics->addWidget(createMetricTile(QStringLiteral("Artifacts"), artifacts.ok ? QString::number(static_cast<int>(artifacts.value.items.size())) : QStringLiteral("--"), QStringLiteral("Recent outputs/inputs"), hero), 1, 1);
    heroLayout->addLayout(heroMetrics, 3);
    canvasLayout()->addWidget(hero);

    auto* insightGrid = new QFrame(this);
    insightGrid->setObjectName("workspaceCardGrid");
    auto* insightLayout = new QGridLayout(insightGrid);
    insightLayout->setContentsMargins(0, 0, 0, 0);
    insightLayout->setSpacing(10);
    insightLayout->addWidget(createInfoPanel(
        QStringLiteral("Candidate funnel"),
        QStringLiteral("Seed probe runs: %1\nCompleted seed probes: %2\nExplorer groups complete: %3\nExplorer groups with issues: %4")
            .arg(seedRuns)
            .arg(completedSeedRuns)
            .arg(completedExplorerGroups)
            .arg(explorerGroupsWithIssues),
        insightGrid), 0, 0);
    insightLayout->addWidget(createInfoPanel(
        QStringLiteral("Provenance map"),
        QStringLiteral("Trace results through workflow activations: TAS movie -> seed probe -> battle/explorer. Open Workflows for hierarchy or focused result pages for domain details."),
        insightGrid), 0, 1);
    canvasLayout()->addWidget(insightGrid);

    auto* gridHost = new QFrame(this);
    gridHost->setObjectName("workspaceCardGrid");
    auto* grid = new QGridLayout(gridHost);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(10);

    grid->addWidget(createToolCard(
        this,
        { QStringLiteral("analysis"), QStringLiteral("seed_probe"), 0, QStringLiteral("seed_probe") },
        QStringLiteral("Seed probe results"),
        QStringLiteral("Grid maps, unique seed lists, neutral seeds, and selected seed probe run details."),
        QStringLiteral("Seed probe results"),
        QStringLiteral("Use the focused seed probe view for grid maps, unique seed lists, and selected result details."),
        QStringLiteral("Open Seed Probe"),
        actions_.openSeedProbe), 0, 0);
    grid->addWidget(createToolCard(
        this,
        { QStringLiteral("analysis"), QStringLiteral("explorer_run"), 0, QStringLiteral("explorer_runs") },
        QStringLiteral("Explorer and battle outcomes"),
        QStringLiteral("Explorer run groups, battle job details, visual replay, and outcome inspection."),
        QStringLiteral("Explorer and battle outcomes"),
        QStringLiteral("Use Explorer Runs to inspect run groups and job details. Candidate comparison can be layered here later."),
        QStringLiteral("Open Explorer Runs"),
        actions_.openExplorerRuns), 0, 1);
    grid->addWidget(createToolCard(
        this,
        { QStringLiteral("analysis"), QStringLiteral("artifact"), 0, QStringLiteral("result_artifacts") },
        QStringLiteral("Result artifacts"),
        QStringLiteral("Browse output artifacts and materialize results for external inspection."),
        QStringLiteral("Result artifacts"),
        QStringLiteral("Artifacts remains available from both Setup and Analysis because it is both an input library and output browser."),
        QStringLiteral("Open Artifacts"),
        actions_.openArtifacts), 1, 0);
    grid->addWidget(createToolCard(
        this,
        { QStringLiteral("analysis"), QStringLiteral("workflow"), 0, QStringLiteral("provenance") },
        QStringLiteral("Workflow provenance"),
        QStringLiteral("Trace TAS -> seed probe -> battle chains through workflow unit activations."),
        QStringLiteral("Workflow provenance"),
        QStringLiteral("For now, open Workflows for activation hierarchy and job-set drilldown. This card is the future home for chain-level summaries."),
        QStringLiteral("Open Workflows"),
        actions_.openWorkflows), 1, 1);

    canvasLayout()->addWidget(gridHost);
    canvasLayout()->addStretch();
}
