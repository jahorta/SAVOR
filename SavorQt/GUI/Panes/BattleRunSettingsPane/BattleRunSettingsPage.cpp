#include "BattleRunSettingsPage.h"

#include "AuthoringSpecEditorWindows.h"
#include "BattlePlanEditorWindow.h"
#include "DB/SavorDbAuthoringService.h"

#include <QtCore/QDateTime>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

namespace {

QString battlePlanText(const savor::db::BattlePlanSnapshot& plan)
{
    int actionCount = 0;
    for (const auto& turn : plan.turns) {
        actionCount += static_cast<int>(turn.actions.size());
    }
    return QStringLiteral("#%1  %2\n%3 turns, %4 actions")
        .arg(static_cast<qint64>(plan.plan_id))
        .arg(QString::fromStdString(plan.name))
        .arg(static_cast<qint64>(plan.turns.size()))
        .arg(actionCount);
}

} // namespace

BattleRunSettingsPage::BattleRunSettingsPage(QWidget* parent)
    : QWidget(parent)
{
    createWidgets();
}

void BattleRunSettingsPage::createWidgets()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(10);

    auto* toolbar = new QFrame(this);
    toolbar->setObjectName("jobsToolbarPanel");
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(12, 10, 12, 10);
    toolbarLayout->setSpacing(10);

    newSeedProbeSpecButton_ = new QPushButton(QStringLiteral("New Seed Probe"), toolbar);
    newBattlePlanButton_ = new QPushButton(QStringLiteral("New Battle Plan"), toolbar);
    editBattlePlanButton_ = new QPushButton(QStringLiteral("Edit Battle Plan"), toolbar);
    duplicateBattlePlanButton_ = new QPushButton(QStringLiteral("Duplicate Battle Plan"), toolbar);
    refreshButton_ = new QPushButton(QStringLiteral("Refresh"), toolbar);
    newSeedProbeSpecButton_->setObjectName("jobsPrimaryButton");
    newBattlePlanButton_->setObjectName("jobsPrimaryButton");
    editBattlePlanButton_->setObjectName("jobsSecondaryButton");
    duplicateBattlePlanButton_->setObjectName("jobsSecondaryButton");
    refreshButton_->setObjectName("jobsSecondaryButton");
    toolbarLayout->addWidget(newSeedProbeSpecButton_);
    toolbarLayout->addWidget(newBattlePlanButton_);
    toolbarLayout->addWidget(editBattlePlanButton_);
    toolbarLayout->addWidget(duplicateBattlePlanButton_);
    toolbarLayout->addWidget(refreshButton_);
    toolbarLayout->addStretch();
    rootLayout->addWidget(toolbar);

    auto* body = new QFrame(this);
    body->setObjectName("jobsSurfacePanel");
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(18, 18, 18, 18);
    bodyLayout->setSpacing(10);
    auto* title = new QLabel(QStringLiteral("Authoring Library"), body);
    title->setObjectName("panelTitle");
    auto* detail = new QLabel(
        QStringLiteral("Battle plans and supporting phase specifications are edited in modeless windows and saved to the Authoring database. Workflow graphs are edited from the Workflow Launcher pane."),
        body);
    detail->setObjectName("sectionDescription");
    detail->setWordWrap(true);
    bodyLayout->addWidget(title);
    bodyLayout->addWidget(detail);

    auto* listsLayout = new QGridLayout();
    listsLayout->setContentsMargins(0, 0, 0, 0);
    listsLayout->setHorizontalSpacing(12);
    listsLayout->setVerticalSpacing(8);
    auto* battlePlanLabel = new QLabel(QStringLiteral("Battle Plans"), body);
    battlePlanLabel->setObjectName("sectionHeading");
    battlePlanList_ = new QListWidget(body);
    listsLayout->addWidget(battlePlanLabel, 0, 0);
    listsLayout->addWidget(battlePlanList_, 1, 0);
    listsLayout->setColumnStretch(0, 1);
    bodyLayout->addLayout(listsLayout, 1);

    libraryStatusLabel_ = new QLabel(body);
    libraryStatusLabel_->setObjectName("sectionDescription");
    bodyLayout->addWidget(libraryStatusLabel_);
    rootLayout->addWidget(body, 1);

    connect(newSeedProbeSpecButton_, &QPushButton::clicked, this, &BattleRunSettingsPage::openSeedProbeSpecEditor);
    connect(newBattlePlanButton_, &QPushButton::clicked, this, &BattleRunSettingsPage::openBattlePlanEditor);
    connect(editBattlePlanButton_, &QPushButton::clicked, this, &BattleRunSettingsPage::editSelectedBattlePlan);
    connect(duplicateBattlePlanButton_, &QPushButton::clicked, this, &BattleRunSettingsPage::duplicateSelectedBattlePlan);
    connect(refreshButton_, &QPushButton::clicked, this, &BattleRunSettingsPage::refreshAuthoringLists);
    connect(battlePlanList_, &QListWidget::itemDoubleClicked, this, [this]() { editSelectedBattlePlan(); });

    refreshAuthoringLists();
}

void BattleRunSettingsPage::openSeedProbeSpecEditor()
{
    if (seedProbeSpecEditor_) {
        seedProbeSpecEditor_->show();
        seedProbeSpecEditor_->raise();
        seedProbeSpecEditor_->activateWindow();
        return;
    }
    auto* editor = new SeedProbeSpecEditorWindow(nullptr);
    seedProbeSpecEditor_ = editor;
    editor->setStatusCallback([this](const QString& text, StatusToast::Severity severity) { postStatusMessage(text, severity); });
    editor->setSavedCallback([this]() { refreshAuthoringLists(); });
    connect(editor, &QObject::destroyed, this, [this]() { seedProbeSpecEditor_.clear(); });
    editor->show();
}

void BattleRunSettingsPage::openBattlePlanEditor()
{
    if (battlePlanEditor_) {
        battlePlanEditor_->show();
        battlePlanEditor_->raise();
        battlePlanEditor_->activateWindow();
        return;
    }

    auto* editor = new BattlePlanEditorWindow(nullptr);
    battlePlanEditor_ = editor;
    editor->setStatusCallback([this](const QString& text, StatusToast::Severity severity) {
        postStatusMessage(text, severity);
    });
    editor->setSavedCallback([this]() { refreshAuthoringLists(); });
    connect(editor, &QObject::destroyed, this, [this]() { battlePlanEditor_.clear(); });
    editor->show();
}

void BattleRunSettingsPage::editSelectedBattlePlan()
{
    const int row = battlePlanList_ != nullptr ? battlePlanList_->currentRow() : -1;
    if (row < 0 || row >= static_cast<int>(battlePlans_.size())) {
        postStatusMessage(QStringLiteral("Select a battle plan to edit."), StatusToast::Severity::Warn);
        return;
    }
    openBattlePlanEditor();
    if (battlePlanEditor_) {
        battlePlanEditor_->loadSnapshot(battlePlans_[static_cast<std::size_t>(row)], false);
    }
}

void BattleRunSettingsPage::duplicateSelectedBattlePlan()
{
    const int row = battlePlanList_ != nullptr ? battlePlanList_->currentRow() : -1;
    if (row < 0 || row >= static_cast<int>(battlePlans_.size())) {
        postStatusMessage(QStringLiteral("Select a battle plan to duplicate."), StatusToast::Severity::Warn);
        return;
    }
    openBattlePlanEditor();
    if (battlePlanEditor_) {
        battlePlanEditor_->loadSnapshot(battlePlans_[static_cast<std::size_t>(row)], true);
    }
}

void BattleRunSettingsPage::refreshAuthoringLists()
{
    const auto plans = savorqt::db::SavorDbAuthoringService::ListBattlePlans();
    if (!plans.ok) {
        postStatusMessage(QString::fromStdString(plans.error.message), StatusToast::Severity::Error);
        return;
    }

    battlePlans_ = plans.value;

    battlePlanList_->clear();
    for (const auto& plan : battlePlans_) {
        auto* item = new QListWidgetItem(battlePlanText(plan), battlePlanList_);
        item->setData(Qt::UserRole, static_cast<qint64>(plan.plan_id));
    }

    libraryStatusLabel_->setText(QStringLiteral("%1 battle plans")
        .arg(static_cast<int>(battlePlans_.size())));
}

void BattleRunSettingsPage::postStatusMessage(const QString& text, StatusToast::Severity severity)
{
    if (text.isEmpty()) {
        return;
    }
    emit statusToastRequested(StatusToast{ severity, text, {}, 1, QDateTime{}, 4000 });
}
