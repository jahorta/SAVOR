#include "BattleRunSettingsPage.h"

#include "AuthoringSpecEditorWindows.h"
#include "BattlePlanEditorWindow.h"
#include "PredicateSpecEditorWindow.h"
#include "DB/SavorDbAuthoringService.h"

#include <QtCore/QDateTime>
#include <QtCore/QStringList>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

namespace {

QString predicateText(const savor::db::PredicateSpecSnapshot& predicate)
{
    const auto requiredBps = predicate.required_breakpoint_ids.empty()
        ? std::vector<BPKey>{ predicate.breakpoint_id }
        : predicate.required_breakpoint_ids;
    QStringList breakpointIds;
    breakpointIds.reserve(static_cast<int>(requiredBps.size()));
    for (const auto bp : requiredBps) {
        if (bp != 0) {
            breakpointIds.push_back(QString::number(static_cast<qint64>(bp)));
        }
    }
    const QString breakpointLabel = breakpointIds.size() == 1
        ? QStringLiteral("Breakpoint %1").arg(breakpointIds.front())
        : QStringLiteral("Breakpoints %1").arg(breakpointIds.join(QStringLiteral(", ")));
    return QStringLiteral("#%1  %2\n%3")
        .arg(static_cast<qint64>(predicate.predicate_spec_id))
        .arg(QString::fromStdString(predicate.name))
        .arg(breakpointLabel);
}

QString battlePlanText(const savor::db::BattlePlanSnapshot& plan)
{
    int actionCount = 0;
    for (const auto& turn : plan.turns) {
        actionCount += static_cast<int>(turn.actions.size());
    }
    return QStringLiteral("#%1  %2\n%3 turns, %4 actions")
        .arg(static_cast<qint64>(plan.plan_id))
        .arg(QString::fromStdString(plan.name))
        .arg(plan.num_turns)
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

    newPredicateButton_ = new QPushButton(QStringLiteral("New Predicate"), toolbar);
    newSeedProbeSpecButton_ = new QPushButton(QStringLiteral("New Seed Probe"), toolbar);
    newTasSpecButton_ = new QPushButton(QStringLiteral("New TAS"), toolbar);
    newBattleRunSpecButton_ = new QPushButton(QStringLiteral("New Battle Run"), toolbar);
    newPredicateSetButton_ = new QPushButton(QStringLiteral("New Predicate Set"), toolbar);
    newExplorerSettingsButton_ = new QPushButton(QStringLiteral("New Battle Explorer Settings"), toolbar);
    newBattleChainSpecButton_ = new QPushButton(QStringLiteral("New Battle Chain Spec"), toolbar);
    editPredicateButton_ = new QPushButton(QStringLiteral("Edit Predicate"), toolbar);
    duplicatePredicateButton_ = new QPushButton(QStringLiteral("Duplicate Predicate"), toolbar);
    deletePredicateButton_ = new QPushButton(QStringLiteral("Delete Predicate"), toolbar);
    newBattlePlanButton_ = new QPushButton(QStringLiteral("New Battle Plan"), toolbar);
    editBattlePlanButton_ = new QPushButton(QStringLiteral("Edit Battle Plan"), toolbar);
    duplicateBattlePlanButton_ = new QPushButton(QStringLiteral("Duplicate Battle Plan"), toolbar);
    refreshButton_ = new QPushButton(QStringLiteral("Refresh"), toolbar);
    newPredicateButton_->setObjectName("jobsPrimaryButton");
    newSeedProbeSpecButton_->setObjectName("jobsPrimaryButton");
    newTasSpecButton_->setObjectName("jobsPrimaryButton");
    newBattleRunSpecButton_->setObjectName("jobsPrimaryButton");
    newPredicateSetButton_->setObjectName("jobsPrimaryButton");
    newExplorerSettingsButton_->setObjectName("jobsPrimaryButton");
    newBattleChainSpecButton_->setObjectName("jobsPrimaryButton");
    editPredicateButton_->setObjectName("jobsSecondaryButton");
    duplicatePredicateButton_->setObjectName("jobsSecondaryButton");
    deletePredicateButton_->setObjectName("jobsSecondaryButton");
    newBattlePlanButton_->setObjectName("jobsPrimaryButton");
    editBattlePlanButton_->setObjectName("jobsSecondaryButton");
    duplicateBattlePlanButton_->setObjectName("jobsSecondaryButton");
    refreshButton_->setObjectName("jobsSecondaryButton");
    toolbarLayout->addWidget(newPredicateButton_);
    toolbarLayout->addWidget(newSeedProbeSpecButton_);
    toolbarLayout->addWidget(newTasSpecButton_);
    toolbarLayout->addWidget(newBattleRunSpecButton_);
    toolbarLayout->addWidget(newPredicateSetButton_);
    toolbarLayout->addWidget(newExplorerSettingsButton_);
    toolbarLayout->addWidget(newBattleChainSpecButton_);
    toolbarLayout->addWidget(editPredicateButton_);
    toolbarLayout->addWidget(duplicatePredicateButton_);
    toolbarLayout->addWidget(deletePredicateButton_);
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
        QStringLiteral("Predicates and battle plans are edited in modeless windows and saved to the Authoring database. Workflow graphs are edited from the Workflow Launcher pane."),
        body);
    detail->setObjectName("sectionDescription");
    detail->setWordWrap(true);
    bodyLayout->addWidget(title);
    bodyLayout->addWidget(detail);

    auto* listsLayout = new QGridLayout();
    listsLayout->setContentsMargins(0, 0, 0, 0);
    listsLayout->setHorizontalSpacing(12);
    listsLayout->setVerticalSpacing(8);
    auto* predicateLabel = new QLabel(QStringLiteral("Predicates"), body);
    auto* battlePlanLabel = new QLabel(QStringLiteral("Battle Plans"), body);
    predicateLabel->setObjectName("sectionHeading");
    battlePlanLabel->setObjectName("sectionHeading");
    predicateList_ = new QListWidget(body);
    battlePlanList_ = new QListWidget(body);
    listsLayout->addWidget(predicateLabel, 0, 0);
    listsLayout->addWidget(battlePlanLabel, 0, 1);
    listsLayout->addWidget(predicateList_, 1, 0);
    listsLayout->addWidget(battlePlanList_, 1, 1);
    listsLayout->setColumnStretch(0, 1);
    listsLayout->setColumnStretch(1, 1);
    bodyLayout->addLayout(listsLayout, 1);

    libraryStatusLabel_ = new QLabel(body);
    libraryStatusLabel_->setObjectName("sectionDescription");
    bodyLayout->addWidget(libraryStatusLabel_);
    rootLayout->addWidget(body, 1);

    connect(newPredicateButton_, &QPushButton::clicked, this, &BattleRunSettingsPage::openPredicateEditor);
    connect(newSeedProbeSpecButton_, &QPushButton::clicked, this, &BattleRunSettingsPage::openSeedProbeSpecEditor);
    connect(newTasSpecButton_, &QPushButton::clicked, this, &BattleRunSettingsPage::openTasSpecEditor);
    connect(newBattleRunSpecButton_, &QPushButton::clicked, this, &BattleRunSettingsPage::openBattleRunSpecEditor);
    connect(newPredicateSetButton_, &QPushButton::clicked, this, &BattleRunSettingsPage::openPredicateSetEditor);
    connect(newExplorerSettingsButton_, &QPushButton::clicked, this, &BattleRunSettingsPage::openExplorerSettingsEditor);
    connect(newBattleChainSpecButton_, &QPushButton::clicked, this, &BattleRunSettingsPage::openBattleChainSpecEditor);
    connect(editPredicateButton_, &QPushButton::clicked, this, &BattleRunSettingsPage::editSelectedPredicate);
    connect(duplicatePredicateButton_, &QPushButton::clicked, this, &BattleRunSettingsPage::duplicateSelectedPredicate);
    connect(deletePredicateButton_, &QPushButton::clicked, this, &BattleRunSettingsPage::deleteSelectedPredicate);
    connect(newBattlePlanButton_, &QPushButton::clicked, this, &BattleRunSettingsPage::openBattlePlanEditor);
    connect(editBattlePlanButton_, &QPushButton::clicked, this, &BattleRunSettingsPage::editSelectedBattlePlan);
    connect(duplicateBattlePlanButton_, &QPushButton::clicked, this, &BattleRunSettingsPage::duplicateSelectedBattlePlan);
    connect(refreshButton_, &QPushButton::clicked, this, &BattleRunSettingsPage::refreshAuthoringLists);
    connect(predicateList_, &QListWidget::itemDoubleClicked, this, [this]() { editSelectedPredicate(); });
    connect(battlePlanList_, &QListWidget::itemDoubleClicked, this, [this]() { editSelectedBattlePlan(); });

    refreshAuthoringLists();
}

void BattleRunSettingsPage::openPredicateEditor()
{
    if (predicateEditor_) {
        predicateEditor_->show();
        predicateEditor_->raise();
        predicateEditor_->activateWindow();
        return;
    }

    auto* editor = new PredicateSpecEditorWindow(nullptr);
    predicateEditor_ = editor;
    editor->setStatusCallback([this](const QString& text, StatusToast::Severity severity) {
        postStatusMessage(text, severity);
    });
    editor->setSavedCallback([this]() { refreshAuthoringLists(); });
    connect(editor, &QObject::destroyed, this, [this]() { predicateEditor_.clear(); });
    editor->show();
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

void BattleRunSettingsPage::openTasSpecEditor()
{
    if (tasSpecEditor_) {
        tasSpecEditor_->show();
        tasSpecEditor_->raise();
        tasSpecEditor_->activateWindow();
        return;
    }
    auto* editor = new TasSpecEditorWindow(nullptr);
    tasSpecEditor_ = editor;
    editor->setStatusCallback([this](const QString& text, StatusToast::Severity severity) { postStatusMessage(text, severity); });
    editor->setSavedCallback([this]() { refreshAuthoringLists(); });
    connect(editor, &QObject::destroyed, this, [this]() { tasSpecEditor_.clear(); });
    editor->show();
}

void BattleRunSettingsPage::openBattleRunSpecEditor()
{
    if (battleRunSpecEditor_) {
        battleRunSpecEditor_->show();
        battleRunSpecEditor_->raise();
        battleRunSpecEditor_->activateWindow();
        return;
    }
    auto* editor = new BattleRunSpecEditorWindow(nullptr);
    battleRunSpecEditor_ = editor;
    editor->setStatusCallback([this](const QString& text, StatusToast::Severity severity) { postStatusMessage(text, severity); });
    editor->setSavedCallback([this]() { refreshAuthoringLists(); });
    connect(editor, &QObject::destroyed, this, [this]() { battleRunSpecEditor_.clear(); });
    editor->show();
}

void BattleRunSettingsPage::openPredicateSetEditor()
{
    if (predicateSetEditor_) {
        predicateSetEditor_->show();
        predicateSetEditor_->raise();
        predicateSetEditor_->activateWindow();
        return;
    }
    auto* editor = new PredicateSetEditorWindow(nullptr);
    predicateSetEditor_ = editor;
    editor->setStatusCallback([this](const QString& text, StatusToast::Severity severity) { postStatusMessage(text, severity); });
    editor->setSavedCallback([this]() { refreshAuthoringLists(); });
    connect(editor, &QObject::destroyed, this, [this]() { predicateSetEditor_.clear(); });
    editor->show();
}

void BattleRunSettingsPage::openExplorerSettingsEditor()
{
    if (explorerSettingsEditor_) {
        explorerSettingsEditor_->show();
        explorerSettingsEditor_->raise();
        explorerSettingsEditor_->activateWindow();
        return;
    }
    auto* editor = new ExplorerSettingsEditorWindow(nullptr);
    explorerSettingsEditor_ = editor;
    editor->setStatusCallback([this](const QString& text, StatusToast::Severity severity) { postStatusMessage(text, severity); });
    editor->setSavedCallback([this]() { refreshAuthoringLists(); });
    connect(editor, &QObject::destroyed, this, [this]() { explorerSettingsEditor_.clear(); });
    editor->show();
}

void BattleRunSettingsPage::openBattleChainSpecEditor()
{
    if (battleChainSpecEditor_) {
        battleChainSpecEditor_->show();
        battleChainSpecEditor_->raise();
        battleChainSpecEditor_->activateWindow();
        return;
    }
    auto* editor = new BattleChainSpecEditorWindow(nullptr);
    battleChainSpecEditor_ = editor;
    editor->setStatusCallback([this](const QString& text, StatusToast::Severity severity) { postStatusMessage(text, severity); });
    editor->setSavedCallback([this]() { refreshAuthoringLists(); });
    connect(editor, &QObject::destroyed, this, [this]() { battleChainSpecEditor_.clear(); });
    editor->show();
}

void BattleRunSettingsPage::editSelectedPredicate()
{
    const int row = predicateList_ != nullptr ? predicateList_->currentRow() : -1;
    if (row < 0 || row >= static_cast<int>(predicates_.size())) {
        postStatusMessage(QStringLiteral("Select a predicate to edit."), StatusToast::Severity::Warn);
        return;
    }
    openPredicateEditor();
    if (predicateEditor_) {
        predicateEditor_->loadSnapshot(predicates_[static_cast<std::size_t>(row)], false);
    }
}

void BattleRunSettingsPage::duplicateSelectedPredicate()
{
    const int row = predicateList_ != nullptr ? predicateList_->currentRow() : -1;
    if (row < 0 || row >= static_cast<int>(predicates_.size())) {
        postStatusMessage(QStringLiteral("Select a predicate to duplicate."), StatusToast::Severity::Warn);
        return;
    }
    openPredicateEditor();
    if (predicateEditor_) {
        predicateEditor_->loadSnapshot(predicates_[static_cast<std::size_t>(row)], true);
    }
}

void BattleRunSettingsPage::deleteSelectedPredicate()
{
    const int row = predicateList_ != nullptr ? predicateList_->currentRow() : -1;
    if (row < 0 || row >= static_cast<int>(predicates_.size())) {
        postStatusMessage(QStringLiteral("Select a predicate to delete."), StatusToast::Severity::Warn);
        return;
    }

    const auto& predicate = predicates_[static_cast<std::size_t>(row)];
    const auto usage = savorqt::db::SavorDbAuthoringService::GetPredicateSpecUsage(predicate.predicate_spec_id);
    if (!usage.ok) {
        postStatusMessage(QString::fromStdString(usage.error.message), StatusToast::Severity::Error);
        return;
    }
    if (usage.value.used()) {
        postStatusMessage(
            QStringLiteral("Predicate is used by %1 predicate set(s) and cannot be deleted.")
                .arg(usage.value.predicate_set_count),
            StatusToast::Severity::Warn);
        return;
    }

    const auto response = QMessageBox::question(
        this,
        QStringLiteral("Delete Predicate"),
        QStringLiteral("Delete predicate #%1 \"%2\"?")
            .arg(static_cast<qint64>(predicate.predicate_spec_id))
            .arg(QString::fromStdString(predicate.name)),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (response != QMessageBox::Yes) {
        return;
    }

    const auto result = savorqt::db::SavorDbAuthoringService::DeletePredicateSpec(predicate.predicate_spec_id);
    if (!result.ok) {
        postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
        return;
    }
    refreshAuthoringLists();
    postStatusMessage(QStringLiteral("Deleted predicate."), StatusToast::Severity::Info);
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
    const auto predicates = savorqt::db::SavorDbAuthoringService::ListPredicateSpecs();
    if (!predicates.ok) {
        postStatusMessage(QString::fromStdString(predicates.error.message), StatusToast::Severity::Error);
        return;
    }
    const auto plans = savorqt::db::SavorDbAuthoringService::ListBattlePlans();
    if (!plans.ok) {
        postStatusMessage(QString::fromStdString(plans.error.message), StatusToast::Severity::Error);
        return;
    }

    predicates_ = predicates.value;
    battlePlans_ = plans.value;

    predicateList_->clear();
    for (const auto& predicate : predicates_) {
        auto* item = new QListWidgetItem(predicateText(predicate), predicateList_);
        item->setData(Qt::UserRole, static_cast<qint64>(predicate.predicate_spec_id));
    }

    battlePlanList_->clear();
    for (const auto& plan : battlePlans_) {
        auto* item = new QListWidgetItem(battlePlanText(plan), battlePlanList_);
        item->setData(Qt::UserRole, static_cast<qint64>(plan.plan_id));
    }

    libraryStatusLabel_->setText(QStringLiteral("%1 predicates, %2 battle plans")
        .arg(static_cast<int>(predicates_.size()))
        .arg(static_cast<int>(battlePlans_.size())));
}

void BattleRunSettingsPage::postStatusMessage(const QString& text, StatusToast::Severity severity)
{
    if (text.isEmpty()) {
        return;
    }
    emit statusToastRequested(StatusToast{ severity, text, {}, 1, QDateTime{}, 4000 });
}
