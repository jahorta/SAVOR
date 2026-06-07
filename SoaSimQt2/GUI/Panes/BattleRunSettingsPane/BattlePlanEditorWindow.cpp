#include "BattlePlanEditorWindow.h"

#include "DB/SimCoreDbAuthoringService.h"

#include <QtCore/QMimeData>
#include <QtCore/QStringList>
#include <QtGui/QCloseEvent>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDragMoveEvent>
#include <QtGui/QDropEvent>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QTreeWidgetItem>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <sstream>
#include <utility>

namespace {

constexpr int kNodeKindRole = Qt::UserRole + 1;
constexpr int kTurnIndexRole = Qt::UserRole + 2;
constexpr int kActionIndexRole = Qt::UserRole + 3;
constexpr int kMacroRole = Qt::UserRole + 4;
constexpr int kNodeTurn = 1;
constexpr int kNodeAction = 2;

QString actionMimeType()
{
    return QStringLiteral("application/x-soasimqt2-battle-plan-action");
}

std::string fingerprintForDraft(const soasimqt2::db::BattlePlanDraft& draft)
{
    std::string content = draft.name + ":" + std::to_string(draft.num_turns) + "\n";
    for (const auto& turn : draft.turns) {
        content += "turn:" + std::to_string(turn.turn_index) + "\n";
        for (const auto& action : turn.actions) {
            content += "action:" + std::to_string(action.actor_slot)
                + ":" + std::to_string(static_cast<int>(action.macro))
                + ":" + std::to_string(static_cast<int>(action.target_kind))
                + ":" + std::to_string(action.target_slot.value_or(-1))
                + ":" + std::to_string(action.target_mask_bits.value_or(-1))
                + ":" + std::to_string(action.target_single_slot.value_or(-1))
                + ":" + std::to_string(action.target_same_as_actor_slot.value_or(-1))
                + ":" + std::to_string(action.item_id.value_or(-1))
                + ":" + std::to_string(action.ordinal) + "\n";
        }
    }

    std::uint64_t hash = 1469598103934665603ull;
    for (const auto ch : content) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 1099511628211ull;
    }
    std::ostringstream out;
    out << "battle-plan-fnv1a64-" << std::hex << hash;
    return out.str();
}

QString macroLabel(simcore::db::BattlePlanActionMacro macro)
{
    switch (macro) {
    case simcore::db::BattlePlanActionMacro::Attack: return QStringLiteral("Attack");
    case simcore::db::BattlePlanActionMacro::Defend: return QStringLiteral("Guard");
    case simcore::db::BattlePlanActionMacro::Focus: return QStringLiteral("Focus");
    case simcore::db::BattlePlanActionMacro::FakeAttack: return QStringLiteral("Fake Attack");
    case simcore::db::BattlePlanActionMacro::UseItem: return QStringLiteral("Use Item");
    }
    return QStringLiteral("Attack");
}

QString targetKindLabel(simcore::db::BattlePlanTargetKind kind)
{
    switch (kind) {
    case simcore::db::BattlePlanTargetKind::SingleEnemy: return QStringLiteral("Single Enemy");
    case simcore::db::BattlePlanTargetKind::MultipleEnemies: return QStringLiteral("Multiple Enemies");
    case simcore::db::BattlePlanTargetKind::AnyEnemy: return QStringLiteral("Any Enemy");
    case simcore::db::BattlePlanTargetKind::SameAsOtherPC: return QStringLiteral("Same As Actor");
    }
    return QStringLiteral("Any Enemy");
}

QString actionSummary(const BattlePlanEditorWindow::ActionDraft& action)
{
    QString detail;
    switch (action.target_kind) {
    case simcore::db::BattlePlanTargetKind::SingleEnemy:
        detail = QStringLiteral("target %1").arg(action.target_single_slot);
        break;
    case simcore::db::BattlePlanTargetKind::MultipleEnemies:
        detail = QStringLiteral("mask 0x%1").arg(action.target_mask_bits, 0, 16);
        break;
    case simcore::db::BattlePlanTargetKind::AnyEnemy:
        detail = QStringLiteral("any enemy");
        break;
    case simcore::db::BattlePlanTargetKind::SameAsOtherPC:
        detail = QStringLiteral("same as actor %1").arg(action.target_same_as_actor_slot);
        break;
    }
    if (action.has_item_id) {
        detail += QStringLiteral(", item %1").arg(action.item_id);
    }
    return QStringLiteral("Actor %1  %2  (%3)")
        .arg(action.actor_slot)
        .arg(macroLabel(action.macro))
        .arg(detail);
}

class ActionLibraryListWidget final : public QListWidget
{
public:
    explicit ActionLibraryListWidget(QWidget* parent = nullptr)
        : QListWidget(parent)
    {
        setDragEnabled(true);
        setSelectionMode(QAbstractItemView::SingleSelection);
    }

    QStringList mimeTypes() const override
    {
        return QStringList{ actionMimeType() };
    }

    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override
    {
        if (items.empty() || items.front() == nullptr) {
            return nullptr;
        }
        auto* mime = new QMimeData();
        mime->setData(actionMimeType(), QByteArray::number(items.front()->data(kMacroRole).toInt()));
        return mime;
    }
};

class BattlePlanTreeWidget final : public QTreeWidget
{
public:
    explicit BattlePlanTreeWidget(QWidget* parent = nullptr)
        : QTreeWidget(parent)
    {
        setAcceptDrops(true);
        setDragDropMode(QAbstractItemView::DropOnly);
    }

    std::function<void(simcore::db::BattlePlanActionMacro)> actionDropped;

protected:
    void dragEnterEvent(QDragEnterEvent* event) override
    {
        if (event != nullptr && event->mimeData() != nullptr && event->mimeData()->hasFormat(actionMimeType())) {
            event->acceptProposedAction();
            return;
        }
        QTreeWidget::dragEnterEvent(event);
    }

    void dragMoveEvent(QDragMoveEvent* event) override
    {
        if (event != nullptr && event->mimeData() != nullptr && event->mimeData()->hasFormat(actionMimeType())) {
            event->acceptProposedAction();
            return;
        }
        QTreeWidget::dragMoveEvent(event);
    }

    void dropEvent(QDropEvent* event) override
    {
        if (event == nullptr || event->mimeData() == nullptr || !event->mimeData()->hasFormat(actionMimeType())) {
            QTreeWidget::dropEvent(event);
            return;
        }

        bool ok = false;
        const int macroValue = QString::fromUtf8(event->mimeData()->data(actionMimeType())).toInt(&ok);
        if (!ok || actionDropped == nullptr) {
            return;
        }

        if (auto* hit = itemAt(event->position().toPoint()); hit != nullptr) {
            setCurrentItem(hit);
        }
        actionDropped(static_cast<simcore::db::BattlePlanActionMacro>(macroValue));
        event->acceptProposedAction();
    }
};

} // namespace

BattlePlanEditorWindow::BattlePlanEditorWindow(QWidget* parent, bool embeddedInContainer)
    : QWidget(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlag(Qt::Window, !embeddedInContainer);
    setWindowTitle(QStringLiteral("Battle Plan Editor"));
    resize(1180, 720);
    createWidgets();
}

void BattlePlanEditorWindow::setStatusCallback(std::function<void(const QString&, StatusToast::Severity)> callback)
{
    statusCallback_ = std::move(callback);
}

void BattlePlanEditorWindow::setSavedCallback(std::function<void()> callback)
{
    savedCallback_ = std::move(callback);
}

void BattlePlanEditorWindow::loadSnapshot(const simcore::db::BattlePlanSnapshot& snapshot, bool duplicate)
{
    setWindowTitle(duplicate
        ? QStringLiteral("Battle Plan Editor - Duplicate")
        : QStringLiteral("Battle Plan Editor - Edit Copy"));
    nameEdit_->setText(QString::fromStdString(snapshot.name) + (duplicate ? QStringLiteral(" copy") : QString()));

    turns_.clear();
    const int turnCount = std::max(1, snapshot.num_turns);
    turns_.resize(static_cast<std::size_t>(turnCount));
    for (int index = 0; index < turnCount; ++index) {
        turns_[static_cast<std::size_t>(index)].turn_index = index + 1;
        turns_[static_cast<std::size_t>(index)].player_combatants = 1;
    }

    for (const auto& turn : snapshot.turns) {
        if (turn.turn_index < 1 || turn.turn_index > turnCount) {
            continue;
        }
        auto& targetTurn = turns_[static_cast<std::size_t>(turn.turn_index - 1)];
        for (const auto& action : turn.actions) {
            const auto& preset = action.action_preset;
            ActionDraft draft{};
            draft.actor_slot = action.actor_slot;
            draft.macro = preset.macro;
            draft.target_kind = preset.target_kind;
            draft.target_slot = preset.target_single_slot.value_or(4);
            draft.target_mask_bits = preset.target_mask_bits.value_or(0);
            draft.target_single_slot = preset.target_single_slot.value_or(4);
            draft.target_same_as_actor_slot = preset.target_same_as_actor_slot.value_or(0);
            draft.item_id = preset.item_id.value_or(0);
            draft.has_item_id = preset.item_id.has_value();
            targetTurn.player_combatants = std::max(targetTurn.player_combatants, draft.actor_slot + 1);
            targetTurn.actions.push_back(std::move(draft));
        }
    }

    turnCountSpin_->setValue(turnCount);
    rebuildPlanTree();
    dirty_ = false;
}

void BattlePlanEditorWindow::closeEvent(QCloseEvent* event)
{
    if (confirmDiscardIfDirty()) {
        event->accept();
    } else {
        event->ignore();
    }
}

void BattlePlanEditorWindow::createWidgets()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(12, 12, 12, 12);
    rootLayout->setSpacing(10);

    auto* topPanel = new QFrame(this);
    topPanel->setObjectName("jobsSurfacePanel");
    auto* topLayout = new QFormLayout(topPanel);
    topLayout->setContentsMargins(14, 12, 14, 12);
    nameEdit_ = new QLineEdit(topPanel);
    turnCountSpin_ = new QSpinBox(topPanel);
    turnCountSpin_->setRange(1, 20);
    turnCountSpin_->setValue(1);
    topLayout->addRow(QStringLiteral("Name"), nameEdit_);
    topLayout->addRow(QStringLiteral("Turns"), turnCountSpin_);
    rootLayout->addWidget(topPanel);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);

    auto* libraryPanel = new QFrame(splitter);
    libraryPanel->setObjectName("jobsSurfacePanel");
    auto* libraryLayout = new QVBoxLayout(libraryPanel);
    libraryLayout->setContentsMargins(12, 12, 12, 12);
    libraryLayout->setSpacing(8);
    auto* libraryTitle = new QLabel(QStringLiteral("Action Commands"), libraryPanel);
    libraryTitle->setObjectName("sectionHeading");
    actionLibraryList_ = new ActionLibraryListWidget(libraryPanel);
    libraryLayout->addWidget(libraryTitle);
    libraryLayout->addWidget(actionLibraryList_, 1);
    populateActionLibrary();

    auto* planPanel = new QFrame(splitter);
    planPanel->setObjectName("jobsSurfacePanel");
    auto* planLayout = new QVBoxLayout(planPanel);
    planLayout->setContentsMargins(12, 12, 12, 12);
    planLayout->setSpacing(8);

    auto* actionToolbar = new QHBoxLayout();
    addActionButton_ = new QPushButton(QStringLiteral("Add"), planPanel);
    duplicateActionButton_ = new QPushButton(QStringLiteral("Duplicate"), planPanel);
    removeNodeButton_ = new QPushButton(QStringLiteral("Remove"), planPanel);
    moveUpButton_ = new QPushButton(QStringLiteral("Up"), planPanel);
    moveDownButton_ = new QPushButton(QStringLiteral("Down"), planPanel);
    for (auto* button : { addActionButton_, duplicateActionButton_, removeNodeButton_, moveUpButton_, moveDownButton_ }) {
        button->setObjectName("jobsSecondaryButton");
        actionToolbar->addWidget(button);
    }
    actionToolbar->addStretch();
    planLayout->addLayout(actionToolbar);

    auto* tree = new BattlePlanTreeWidget(planPanel);
    planTree_ = tree;
    planTree_->setColumnCount(2);
    planTree_->setHeaderLabels(QStringList{ QStringLiteral("Turn Tree"), QStringLiteral("Actions") });
    planTree_->setSelectionMode(QAbstractItemView::SingleSelection);
    planTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    planTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree->actionDropped = [this](simcore::db::BattlePlanActionMacro macro) {
        addActionToSelectedTurn(macro);
    };
    planLayout->addWidget(planTree_, 1);

    auto* inspectorPanel = new QFrame(splitter);
    inspectorPanel->setObjectName("jobsSurfacePanel");
    auto* inspectorLayout = new QVBoxLayout(inspectorPanel);
    inspectorLayout->setContentsMargins(12, 12, 12, 12);
    inspectorLayout->setSpacing(8);
    selectionLabel_ = new QLabel(QStringLiteral("No selection"), inspectorPanel);
    selectionLabel_->setObjectName("sectionHeading");
    inspectorLayout->addWidget(selectionLabel_);

    auto* inspectorForm = new QFormLayout();
    combatantCountSpin_ = new QSpinBox(inspectorPanel);
    combatantCountSpin_->setRange(1, 4);
    actorSlotSpin_ = new QSpinBox(inspectorPanel);
    actorSlotSpin_->setRange(0, 3);
    macroCombo_ = new QComboBox(inspectorPanel);
    targetKindCombo_ = new QComboBox(inspectorPanel);
    targetSlotSpin_ = new QSpinBox(inspectorPanel);
    targetSlotSpin_->setRange(4, 11);
    targetMaskSpin_ = new QSpinBox(inspectorPanel);
    targetMaskSpin_->setRange(0, 0xFFF);
    targetMaskSpin_->setDisplayIntegerBase(16);
    sameAsActorSpin_ = new QSpinBox(inspectorPanel);
    sameAsActorSpin_->setRange(0, 3);
    itemIdCheck_ = new QCheckBox(QStringLiteral("Set item id"), inspectorPanel);
    itemIdSpin_ = new QSpinBox(inspectorPanel);
    itemIdSpin_->setRange(0, 0xFFFF);

    for (const auto macro : {
        simcore::db::BattlePlanActionMacro::Attack,
        simcore::db::BattlePlanActionMacro::Defend,
        simcore::db::BattlePlanActionMacro::Focus,
        simcore::db::BattlePlanActionMacro::FakeAttack,
        simcore::db::BattlePlanActionMacro::UseItem }) {
        macroCombo_->addItem(macroLabel(macro), static_cast<int>(macro));
    }
    for (const auto kind : {
        simcore::db::BattlePlanTargetKind::SingleEnemy,
        simcore::db::BattlePlanTargetKind::MultipleEnemies,
        simcore::db::BattlePlanTargetKind::AnyEnemy,
        simcore::db::BattlePlanTargetKind::SameAsOtherPC }) {
        targetKindCombo_->addItem(targetKindLabel(kind), static_cast<int>(kind));
    }

    inspectorForm->addRow(QStringLiteral("Combatants"), combatantCountSpin_);
    inspectorForm->addRow(QStringLiteral("Actor"), actorSlotSpin_);
    inspectorForm->addRow(QStringLiteral("Action"), macroCombo_);
    inspectorForm->addRow(QStringLiteral("Targeting"), targetKindCombo_);
    inspectorForm->addRow(QStringLiteral("Single target"), targetSlotSpin_);
    inspectorForm->addRow(QStringLiteral("Target mask"), targetMaskSpin_);
    inspectorForm->addRow(QStringLiteral("Same-as actor"), sameAsActorSpin_);
    inspectorForm->addRow(QString(), itemIdCheck_);
    inspectorForm->addRow(QStringLiteral("Item id"), itemIdSpin_);
    inspectorLayout->addLayout(inspectorForm);
    inspectorLayout->addStretch();

    splitter->addWidget(libraryPanel);
    splitter->addWidget(planPanel);
    splitter->addWidget(inspectorPanel);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    splitter->setStretchFactor(2, 1);
    rootLayout->addWidget(splitter, 1);

    auto* buttonRow = new QHBoxLayout();
    buttonRow->addStretch();
    saveButton_ = new QPushButton(QStringLiteral("Save Battle Plan"), this);
    saveButton_->setObjectName("jobsPrimaryButton");
    buttonRow->addWidget(saveButton_);
    rootLayout->addLayout(buttonRow);

    turns_.push_back(TurnDraft{});
    rebuildPlanTree();

    connect(saveButton_, &QPushButton::clicked, this, &BattlePlanEditorWindow::saveBattlePlan);
    connect(addActionButton_, &QPushButton::clicked, this, &BattlePlanEditorWindow::addActionFromLibrarySelection);
    connect(duplicateActionButton_, &QPushButton::clicked, this, &BattlePlanEditorWindow::duplicateSelectedAction);
    connect(removeNodeButton_, &QPushButton::clicked, this, &BattlePlanEditorWindow::removeSelectedNode);
    connect(moveUpButton_, &QPushButton::clicked, this, [this]() { moveSelectedAction(-1); });
    connect(moveDownButton_, &QPushButton::clicked, this, [this]() { moveSelectedAction(1); });
    connect(actionLibraryList_, &QListWidget::itemDoubleClicked, this, [this]() { addActionFromLibrarySelection(); });
    connect(planTree_, &QTreeWidget::currentItemChanged, this, [this]() { refreshSelectionPanel(); });
    connect(nameEdit_, &QLineEdit::textChanged, this, [this]() { markDirty(); });
    connect(turnCountSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        ensureTurnCount(value);
        markDirty();
    });

    const auto syncAction = [this]() {
        if (!refreshingSelection_) {
            syncSelectionPanelToAction();
        }
    };
    connect(combatantCountSpin_, qOverload<int>(&QSpinBox::valueChanged), this, syncAction);
    connect(actorSlotSpin_, qOverload<int>(&QSpinBox::valueChanged), this, syncAction);
    connect(macroCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, syncAction);
    connect(targetKindCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, syncAction);
    connect(targetSlotSpin_, qOverload<int>(&QSpinBox::valueChanged), this, syncAction);
    connect(targetMaskSpin_, qOverload<int>(&QSpinBox::valueChanged), this, syncAction);
    connect(sameAsActorSpin_, qOverload<int>(&QSpinBox::valueChanged), this, syncAction);
    connect(itemIdCheck_, &QCheckBox::toggled, this, syncAction);
    connect(itemIdSpin_, qOverload<int>(&QSpinBox::valueChanged), this, syncAction);
}

void BattlePlanEditorWindow::populateActionLibrary()
{
    actionLibraryList_->clear();
    for (const auto macro : {
        simcore::db::BattlePlanActionMacro::Attack,
        simcore::db::BattlePlanActionMacro::Defend,
        simcore::db::BattlePlanActionMacro::Focus,
        simcore::db::BattlePlanActionMacro::FakeAttack,
        simcore::db::BattlePlanActionMacro::UseItem }) {
        auto* item = new QListWidgetItem(macroLabel(macro), actionLibraryList_);
        item->setData(kMacroRole, static_cast<int>(macro));
    }
    if (actionLibraryList_->count() > 0) {
        actionLibraryList_->setCurrentRow(0);
    }
}

void BattlePlanEditorWindow::rebuildPlanTree()
{
    if (planTree_ == nullptr) {
        return;
    }
    rebuildingTree_ = true;
    const int previousTurn = selectedTurnIndex();
    const int previousAction = selectedActionIndex();
    planTree_->clear();
    for (int turnIndex = 0; turnIndex < static_cast<int>(turns_.size()); ++turnIndex) {
        const auto& turn = turns_[static_cast<std::size_t>(turnIndex)];
        auto* turnItem = new QTreeWidgetItem(planTree_);
        turnItem->setText(0, QStringLiteral("Turn %1").arg(turn.turn_index));
        turnItem->setText(1, QStringLiteral("%1 combatants, %2 actions")
            .arg(turn.player_combatants)
            .arg(static_cast<int>(turn.actions.size())));
        turnItem->setData(0, kNodeKindRole, kNodeTurn);
        turnItem->setData(0, kTurnIndexRole, turnIndex);
        turnItem->setExpanded(true);

        for (int actionIndex = 0; actionIndex < static_cast<int>(turn.actions.size()); ++actionIndex) {
            const auto& action = turn.actions[static_cast<std::size_t>(actionIndex)];
            auto* actionItem = new QTreeWidgetItem(turnItem);
            actionItem->setText(0, actionSummary(action));
            actionItem->setText(1, QString::number(actionIndex + 1));
            actionItem->setData(0, kNodeKindRole, kNodeAction);
            actionItem->setData(0, kTurnIndexRole, turnIndex);
            actionItem->setData(0, kActionIndexRole, actionIndex);
        }
    }

    QTreeWidgetItem* restoreItem = nullptr;
    if (previousTurn >= 0 && previousTurn < planTree_->topLevelItemCount()) {
        restoreItem = planTree_->topLevelItem(previousTurn);
        if (previousAction >= 0 && previousAction < restoreItem->childCount()) {
            restoreItem = restoreItem->child(previousAction);
        }
    }
    if (restoreItem == nullptr && planTree_->topLevelItemCount() > 0) {
        restoreItem = planTree_->topLevelItem(0);
    }
    if (restoreItem != nullptr) {
        planTree_->setCurrentItem(restoreItem);
    }
    rebuildingTree_ = false;
    refreshSelectionPanel();
}

void BattlePlanEditorWindow::refreshSelectionPanel()
{
    if (rebuildingTree_) {
        return;
    }
    refreshingSelection_ = true;
    const auto* turn = selectedTurn();
    const auto* action = selectedAction();
    const bool hasTurn = turn != nullptr;
    const bool hasAction = action != nullptr;

    selectionLabel_->setText(hasAction
        ? QStringLiteral("Selected Action")
        : (hasTurn ? QStringLiteral("Selected Turn") : QStringLiteral("No Selection")));
    combatantCountSpin_->setEnabled(hasTurn);
    actorSlotSpin_->setEnabled(hasAction);
    macroCombo_->setEnabled(hasAction);
    targetKindCombo_->setEnabled(hasAction);
    const auto targetKind = hasAction ? action->target_kind : simcore::db::BattlePlanTargetKind::AnyEnemy;
    targetSlotSpin_->setEnabled(hasAction && targetKind == simcore::db::BattlePlanTargetKind::SingleEnemy);
    targetMaskSpin_->setEnabled(hasAction && targetKind == simcore::db::BattlePlanTargetKind::MultipleEnemies);
    sameAsActorSpin_->setEnabled(hasAction && targetKind == simcore::db::BattlePlanTargetKind::SameAsOtherPC);
    itemIdCheck_->setEnabled(hasAction);
    itemIdSpin_->setEnabled(hasAction && itemIdCheck_->isChecked());
    duplicateActionButton_->setEnabled(hasAction);
    removeNodeButton_->setEnabled(hasTurn || hasAction);
    moveUpButton_->setEnabled(hasAction && selectedActionIndex() > 0);
    moveDownButton_->setEnabled(hasAction && turn != nullptr && selectedActionIndex() + 1 < static_cast<int>(turn->actions.size()));

    if (hasTurn) {
        combatantCountSpin_->setValue(turn->player_combatants);
    }
    if (hasAction) {
        actorSlotSpin_->setValue(action->actor_slot);
        macroCombo_->setCurrentIndex(std::max(0, macroCombo_->findData(static_cast<int>(action->macro))));
        targetKindCombo_->setCurrentIndex(std::max(0, targetKindCombo_->findData(static_cast<int>(action->target_kind))));
        targetSlotSpin_->setValue(action->target_single_slot);
        targetMaskSpin_->setValue(action->target_mask_bits);
        sameAsActorSpin_->setValue(action->target_same_as_actor_slot);
        itemIdCheck_->setChecked(action->has_item_id);
        itemIdSpin_->setValue(action->item_id);
    }
    itemIdSpin_->setEnabled(hasAction && itemIdCheck_->isChecked());
    refreshingSelection_ = false;
}

void BattlePlanEditorWindow::syncSelectionPanelToAction()
{
    auto* turn = selectedTurn();
    auto* action = selectedAction();
    if (turn != nullptr) {
        turn->player_combatants = combatantCountSpin_->value();
        actorSlotSpin_->setMaximum(std::max(0, turn->player_combatants - 1));
    }
    if (action != nullptr) {
        action->actor_slot = std::min(actorSlotSpin_->value(), actorSlotSpin_->maximum());
        action->macro = static_cast<simcore::db::BattlePlanActionMacro>(macroCombo_->currentData().toInt());
        action->target_kind = static_cast<simcore::db::BattlePlanTargetKind>(targetKindCombo_->currentData().toInt());
        action->target_slot = targetSlotSpin_->value();
        action->target_single_slot = targetSlotSpin_->value();
        action->target_mask_bits = targetMaskSpin_->value();
        action->target_same_as_actor_slot = sameAsActorSpin_->value();
        action->has_item_id = itemIdCheck_->isChecked();
        action->item_id = itemIdSpin_->value();
    }
    markDirty();
    rebuildPlanTree();
}

void BattlePlanEditorWindow::addActionFromLibrarySelection()
{
    auto macro = simcore::db::BattlePlanActionMacro::Attack;
    if (auto* item = actionLibraryList_->currentItem(); item != nullptr) {
        macro = static_cast<simcore::db::BattlePlanActionMacro>(item->data(kMacroRole).toInt());
    }
    addActionToSelectedTurn(macro);
}

void BattlePlanEditorWindow::addActionToSelectedTurn(simcore::db::BattlePlanActionMacro macro)
{
    auto* turn = selectedTurn();
    if (turn == nullptr && !turns_.empty()) {
        turn = &turns_.front();
    }
    if (turn == nullptr) {
        return;
    }

    ActionDraft action{};
    action.actor_slot = 0;
    action.macro = macro;
    action.target_kind = macro == simcore::db::BattlePlanActionMacro::Defend || macro == simcore::db::BattlePlanActionMacro::Focus
        ? simcore::db::BattlePlanTargetKind::AnyEnemy
        : simcore::db::BattlePlanTargetKind::SingleEnemy;
    action.target_slot = 4;
    action.target_single_slot = 4;
    action.target_mask_bits = 1 << 4;
    action.has_item_id = macro == simcore::db::BattlePlanActionMacro::UseItem;
    turn->actions.push_back(std::move(action));
    markDirty();
    rebuildPlanTree();
}

void BattlePlanEditorWindow::duplicateSelectedAction()
{
    auto* turn = selectedTurn();
    const int actionIndex = selectedActionIndex();
    if (turn == nullptr || actionIndex < 0 || actionIndex >= static_cast<int>(turn->actions.size())) {
        return;
    }
    const auto copy = turn->actions[static_cast<std::size_t>(actionIndex)];
    turn->actions.insert(turn->actions.begin() + actionIndex + 1, copy);
    markDirty();
    rebuildPlanTree();
}

void BattlePlanEditorWindow::removeSelectedNode()
{
    const int turnIndex = selectedTurnIndex();
    const int actionIndex = selectedActionIndex();
    if (turnIndex < 0 || turnIndex >= static_cast<int>(turns_.size())) {
        return;
    }
    auto& turn = turns_[static_cast<std::size_t>(turnIndex)];
    if (actionIndex >= 0 && actionIndex < static_cast<int>(turn.actions.size())) {
        turn.actions.erase(turn.actions.begin() + actionIndex);
    } else if (turn.actions.empty()) {
        if (turns_.size() <= 1) {
            return;
        }
        turns_.erase(turns_.begin() + turnIndex);
        for (int index = 0; index < static_cast<int>(turns_.size()); ++index) {
            turns_[static_cast<std::size_t>(index)].turn_index = index + 1;
        }
        turnCountSpin_->setValue(static_cast<int>(turns_.size()));
    } else {
        postStatusMessage(QStringLiteral("Remove actions before removing a turn."), StatusToast::Severity::Warn);
        return;
    }
    markDirty();
    rebuildPlanTree();
}

void BattlePlanEditorWindow::moveSelectedAction(int delta)
{
    auto* turn = selectedTurn();
    const int actionIndex = selectedActionIndex();
    if (turn == nullptr || actionIndex < 0) {
        return;
    }
    const int nextIndex = actionIndex + delta;
    if (nextIndex < 0 || nextIndex >= static_cast<int>(turn->actions.size())) {
        return;
    }
    std::swap(turn->actions[static_cast<std::size_t>(actionIndex)], turn->actions[static_cast<std::size_t>(nextIndex)]);
    markDirty();
    rebuildPlanTree();
}

void BattlePlanEditorWindow::ensureTurnCount(int count)
{
    count = std::max(1, count);
    const int previous = static_cast<int>(turns_.size());
    if (count == previous) {
        return;
    }
    turns_.resize(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        auto& turn = turns_[static_cast<std::size_t>(index)];
        turn.turn_index = index + 1;
        if (turn.player_combatants <= 0) {
            turn.player_combatants = 1;
        }
    }
    rebuildPlanTree();
}

void BattlePlanEditorWindow::saveBattlePlan()
{
    if (nameEdit_->text().trimmed().isEmpty()) {
        postStatusMessage(QStringLiteral("Battle plan name is required."), StatusToast::Severity::Warn);
        return;
    }

    soasimqt2::db::BattlePlanDraft draft{};
    draft.name = nameEdit_->text().trimmed().toStdString();
    draft.num_turns = static_cast<int>(turns_.size());
    draft.turns.resize(turns_.size());

    for (int turnIndex = 0; turnIndex < static_cast<int>(turns_.size()); ++turnIndex) {
        const auto& sourceTurn = turns_[static_cast<std::size_t>(turnIndex)];
        auto& targetTurn = draft.turns[static_cast<std::size_t>(turnIndex)];
        targetTurn.turn_index = turnIndex + 1;
        for (int actionIndex = 0; actionIndex < static_cast<int>(sourceTurn.actions.size()); ++actionIndex) {
            const auto& sourceAction = sourceTurn.actions[static_cast<std::size_t>(actionIndex)];
            if (sourceAction.actor_slot < 0 || sourceAction.actor_slot >= sourceTurn.player_combatants) {
                postStatusMessage(QStringLiteral("Actor slot is outside the turn combatant count."), StatusToast::Severity::Warn);
                return;
            }

            soasimqt2::db::BattlePlanActionDraft action{};
            action.actor_slot = sourceAction.actor_slot;
            action.macro = sourceAction.macro;
            action.target_kind = sourceAction.target_kind;
            action.ordinal = actionIndex;
            switch (sourceAction.target_kind) {
            case simcore::db::BattlePlanTargetKind::SingleEnemy:
                action.target_slot = sourceAction.target_single_slot;
                action.target_single_slot = sourceAction.target_single_slot;
                break;
            case simcore::db::BattlePlanTargetKind::MultipleEnemies:
                action.target_mask_bits = sourceAction.target_mask_bits;
                break;
            case simcore::db::BattlePlanTargetKind::AnyEnemy:
                break;
            case simcore::db::BattlePlanTargetKind::SameAsOtherPC:
                action.target_same_as_actor_slot = sourceAction.target_same_as_actor_slot;
                break;
            }
            if (sourceAction.has_item_id) {
                action.item_id = sourceAction.item_id;
            }
            targetTurn.actions.push_back(std::move(action));
        }
    }
    draft.fingerprint = fingerprintForDraft(draft);

    saveButton_->setEnabled(false);
    const auto result = soasimqt2::db::SimCoreDbAuthoringService::SaveBattlePlan(draft);
    saveButton_->setEnabled(true);
    if (!result.ok) {
        postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
        return;
    }
    dirty_ = false;
    if (savedCallback_) {
        savedCallback_();
    }
    postStatusMessage(QStringLiteral("Saved battle plan %1.").arg(static_cast<qint64>(result.value)), StatusToast::Severity::Info);
}

void BattlePlanEditorWindow::markDirty()
{
    dirty_ = true;
}

bool BattlePlanEditorWindow::confirmDiscardIfDirty()
{
    if (!dirty_) {
        return true;
    }
    const auto result = QMessageBox::warning(
        this,
        QStringLiteral("Discard battle plan changes?"),
        QStringLiteral("This battle plan has unsaved changes."),
        QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Cancel);
    return result == QMessageBox::Discard;
}

void BattlePlanEditorWindow::postStatusMessage(const QString& text, StatusToast::Severity severity)
{
    if (text.isEmpty()) {
        return;
    }
    if (statusCallback_) {
        statusCallback_(text, severity);
    }
}

BattlePlanEditorWindow::TurnDraft* BattlePlanEditorWindow::selectedTurn()
{
    const int turnIndex = selectedTurnIndex();
    if (turnIndex < 0 || turnIndex >= static_cast<int>(turns_.size())) {
        return nullptr;
    }
    return &turns_[static_cast<std::size_t>(turnIndex)];
}

BattlePlanEditorWindow::ActionDraft* BattlePlanEditorWindow::selectedAction()
{
    auto* turn = selectedTurn();
    const int actionIndex = selectedActionIndex();
    if (turn == nullptr || actionIndex < 0 || actionIndex >= static_cast<int>(turn->actions.size())) {
        return nullptr;
    }
    return &turn->actions[static_cast<std::size_t>(actionIndex)];
}

const BattlePlanEditorWindow::TurnDraft* BattlePlanEditorWindow::selectedTurn() const
{
    const int turnIndex = selectedTurnIndex();
    if (turnIndex < 0 || turnIndex >= static_cast<int>(turns_.size())) {
        return nullptr;
    }
    return &turns_[static_cast<std::size_t>(turnIndex)];
}

const BattlePlanEditorWindow::ActionDraft* BattlePlanEditorWindow::selectedAction() const
{
    const auto* turn = selectedTurn();
    const int actionIndex = selectedActionIndex();
    if (turn == nullptr || actionIndex < 0 || actionIndex >= static_cast<int>(turn->actions.size())) {
        return nullptr;
    }
    return &turn->actions[static_cast<std::size_t>(actionIndex)];
}

QTreeWidgetItem* BattlePlanEditorWindow::selectedTreeItem() const
{
    return planTree_ != nullptr ? planTree_->currentItem() : nullptr;
}

int BattlePlanEditorWindow::selectedTurnIndex() const
{
    auto* item = selectedTreeItem();
    if (item == nullptr) {
        return -1;
    }
    if (isActionItem(item)) {
        item = item->parent();
    }
    return item != nullptr && isTurnItem(item) ? item->data(0, kTurnIndexRole).toInt() : -1;
}

int BattlePlanEditorWindow::selectedActionIndex() const
{
    const auto* item = selectedTreeItem();
    return item != nullptr && isActionItem(item) ? item->data(0, kActionIndexRole).toInt() : -1;
}

bool BattlePlanEditorWindow::isActionItem(const QTreeWidgetItem* item)
{
    return item != nullptr && item->data(0, kNodeKindRole).toInt() == kNodeAction;
}

bool BattlePlanEditorWindow::isTurnItem(const QTreeWidgetItem* item)
{
    return item != nullptr && item->data(0, kNodeKindRole).toInt() == kNodeTurn;
}
