#include "BattlePlanEditorWindow.h"

#include "DB/SimCoreDbAuthoringService.h"
#include "BattleRunSettingsDragDrop.h"

#include <QtCore/QMimeData>
#include <QtCore/QStringList>
#include <QtGui/QCloseEvent>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDragMoveEvent>
#include <QtGui/QDropEvent>
#include <QtWidgets/QAbstractItemView>
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
constexpr int kActionPresetIdRole = Qt::UserRole + 4;
constexpr int kNodeTurn = 1;
constexpr int kNodeAction = 2;

std::string fingerprintForDraft(const soasimqt2::db::BattlePlanDraft& draft)
{
    std::string content = draft.name + ":" + std::to_string(draft.num_turns) + "\n";
    for (const auto& turn : draft.turns) {
        content += "turn:" + std::to_string(turn.turn_index) + "\n";
        for (const auto& action : turn.actions) {
            content += "action:" + std::to_string(action.actor_slot)
                + ":" + std::to_string(action.action_preset_id.value_or(0))
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

QString presetDetailText(const simcore::db::BattlePlanActionPresetSnapshot& preset)
{
    QString detail = macroLabel(preset.macro);
    detail += QStringLiteral(" / ");
    detail += targetKindLabel(preset.target_kind);
    if (preset.target_kind == simcore::db::BattlePlanTargetKind::SingleEnemy) {
        detail += QStringLiteral(" %1").arg(preset.target_single_slot.value_or(4));
    }
    if (preset.item_id.has_value()) {
        detail += QStringLiteral(" / item %1").arg(*preset.item_id);
    }
    if (preset.target_mask_bits.has_value()) {
        detail += QStringLiteral(" / mask 0x%1").arg(QString::number(preset.target_mask_bits.value(), 16));
    }
    if (preset.target_same_as_actor_slot.has_value()) {
        detail += QStringLiteral(" / same as actor %1").arg(preset.target_same_as_actor_slot.value());
    }
    return detail;
}

QString actionSummary(const BattlePlanEditorWindow::ActionDraft& action, const simcore::db::BattlePlanActionPresetSnapshot* preset)
{
    QString detail;
    if (preset == nullptr) {
        detail = QStringLiteral("missing preset %1").arg(action.action_preset_id);
        return QStringLiteral("Actor %1  %2").arg(action.actor_slot).arg(detail);
    }
    return QStringLiteral("Actor %1  %2  (%3)")
        .arg(action.actor_slot)
        .arg(QString::fromStdString(preset->name))
        .arg(presetDetailText(*preset));
}

QString presetLibraryLabel(const simcore::db::BattlePlanActionPresetSnapshot& preset)
{
    return QStringLiteral("%1  [#%2]").arg(QString::fromStdString(preset.name)).arg(preset.action_preset_id);
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
        return QStringList{ battlerunsettings::presetMimeType() };
    }

    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override
    {
        if (items.empty() || items.front() == nullptr) {
            return nullptr;
        }
        auto* mime = new QMimeData();
        const qint64 presetId = static_cast<qint64>(items.front()->data(kActionPresetIdRole).toLongLong());
        if (presetId <= 0) {
            return nullptr;
        }
        mime->setData(battlerunsettings::presetMimeType(), battlerunsettings::encodePresetId(presetId));
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

    std::function<void(std::int64_t, int, int)> actionDropped;

protected:
    void dragEnterEvent(QDragEnterEvent* event) override
    {
        if (event != nullptr && event->mimeData() != nullptr && event->mimeData()->hasFormat(battlerunsettings::presetMimeType())) {
            event->acceptProposedAction();
            return;
        }
        QTreeWidget::dragEnterEvent(event);
    }

    void dragMoveEvent(QDragMoveEvent* event) override
    {
        if (event != nullptr && event->mimeData() != nullptr && event->mimeData()->hasFormat(battlerunsettings::presetMimeType())) {
            event->acceptProposedAction();
            return;
        }
        QTreeWidget::dragMoveEvent(event);
    }

    void dropEvent(QDropEvent* event) override
    {
        if (event == nullptr || event->mimeData() == nullptr || !event->mimeData()->hasFormat(battlerunsettings::presetMimeType())) {
            QTreeWidget::dropEvent(event);
            return;
        }

        qint64 presetId = 0;
        if (!battlerunsettings::decodePresetId(event->mimeData(), &presetId) || actionDropped == nullptr) {
            return;
        }

        int dropTurnIndex = -1;
        int dropActionIndex = -1;
        if (auto* hit = itemAt(event->position().toPoint()); hit != nullptr) {
            if (hit->data(0, kNodeKindRole).toInt() == kNodeAction) {
                dropActionIndex = hit->data(0, kActionIndexRole).toInt();
                hit = hit->parent();
            }
            if (hit != nullptr && hit->data(0, kNodeKindRole).toInt() == kNodeTurn) {
                dropTurnIndex = hit->data(0, kTurnIndexRole).toInt();
            }
            setCurrentItem(hit);
        }
        actionDropped(presetId, dropTurnIndex, dropActionIndex);
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
            draft.action_preset_id = preset.action_preset_id;
            draft.actor_slot = action.actor_slot;
            if (preset.action_preset_id > 0) {
                const auto it = std::find_if(actionPresets_.begin(), actionPresets_.end(), [&preset](const simcore::db::BattlePlanActionPresetSnapshot& item) {
                    return item.action_preset_id == preset.action_preset_id;
                });
                if (it == actionPresets_.end()) {
                    actionPresets_.push_back(preset);
                }
            }
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
    auto* libraryTitle = new QLabel(QStringLiteral("Action Presets"), libraryPanel);
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
    tree->actionDropped = [this](std::int64_t presetId, int turnIndex, int actionIndex) {
        assignActionPreset(presetId, turnIndex, actionIndex);
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
    presetSummaryLabel_ = new QLabel(QStringLiteral("No preset"), inspectorPanel);
    presetSummaryLabel_->setObjectName("sectionDescription");
    presetDetailLabel_ = new QLabel(QStringLiteral(""), inspectorPanel);
    presetDetailLabel_->setObjectName("sectionDescription");
    presetDetailLabel_->setWordWrap(true);

    inspectorForm->addRow(QStringLiteral("Combatants"), combatantCountSpin_);
    inspectorForm->addRow(QStringLiteral("Actor"), actorSlotSpin_);
    inspectorForm->addRow(QStringLiteral("Preset"), presetSummaryLabel_);
    inspectorForm->addRow(QStringLiteral("Details"), presetDetailLabel_);
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
}

void BattlePlanEditorWindow::populateActionLibrary()
{
    actionLibraryList_->clear();
    const auto result = soasimqt2::db::SimCoreDbAuthoringService::ListBattlePlanActionPresets();
    if (!result.ok) {
        postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Warn);
        actionLibraryList_->addItem(QStringLiteral("No action presets available"));
        return;
    }
    actionPresets_ = result.value;
    std::sort(actionPresets_.begin(), actionPresets_.end(), [](const auto& a, const auto& b) {
        return a.action_preset_id < b.action_preset_id;
    });
    for (const auto& preset : actionPresets_) {
        auto* item = new QListWidgetItem(presetLibraryLabel(preset), actionLibraryList_);
        item->setData(kActionPresetIdRole, static_cast<qint64>(preset.action_preset_id));
        item->setToolTip(QStringLiteral("macro: %1, target: %2")
            .arg(macroLabel(preset.macro))
            .arg(targetKindLabel(preset.target_kind)));
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
            const auto* preset = actionPresetById(action.action_preset_id);
            auto* actionItem = new QTreeWidgetItem(turnItem);
            actionItem->setText(0, actionSummary(action, preset));
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
    duplicateActionButton_->setEnabled(hasAction);
    removeNodeButton_->setEnabled(hasTurn || hasAction);
    moveUpButton_->setEnabled(hasAction && selectedActionIndex() > 0);
    moveDownButton_->setEnabled(hasAction && turn != nullptr && selectedActionIndex() + 1 < static_cast<int>(turn->actions.size()));
    presetSummaryLabel_->setEnabled(hasAction);
    presetDetailLabel_->setEnabled(hasAction);
    presetSummaryLabel_->setText(QStringLiteral("No preset assigned"));
    presetDetailLabel_->setText(QStringLiteral("Drag a preset here to assign it. Double-click also assigns to action if one is selected."));

    if (hasTurn) {
        combatantCountSpin_->setValue(turn->player_combatants);
    }
    if (hasAction) {
        actorSlotSpin_->setValue(action->actor_slot);
        const auto* preset = actionPresetById(action->action_preset_id);
        if (preset != nullptr) {
            presetSummaryLabel_->setText(QStringLiteral("%1 [#%2]")
                .arg(QString::fromStdString(preset->name))
                .arg(preset->action_preset_id));
            presetDetailLabel_->setText(presetDetailText(*preset));
        } else {
            presetSummaryLabel_->setText(QStringLiteral("Missing preset %1").arg(action->action_preset_id));
            presetDetailLabel_->setText(QStringLiteral("This action references a preset that no longer exists."));
        }
    }
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
    }
    markDirty();
    rebuildPlanTree();
}

void BattlePlanEditorWindow::addActionFromLibrarySelection()
{
    auto presetId = std::int64_t{0};
    if (auto* item = actionLibraryList_->currentItem(); item != nullptr) {
        presetId = item->data(kActionPresetIdRole).toLongLong();
    }
    assignPresetToSelection(presetId);
}

void BattlePlanEditorWindow::assignPresetToSelection(std::int64_t presetId)
{
    const int turnIndex = selectedTurnIndex();
    const int actionIndex = selectedActionIndex();
    if (turnIndex < 0 && turns_.empty()) {
        return;
    }
    if (actionIndex >= 0 && turnIndex >= 0) {
        assignActionPreset(presetId, turnIndex, actionIndex);
    } else {
        addActionToSelectedTurn(presetId, turnIndex);
    }
}

void BattlePlanEditorWindow::assignActionPreset(std::int64_t presetId, int turnIndex, int actionIndex)
{
    if (presetId <= 0) {
        postStatusMessage(QStringLiteral("Select a valid action preset before adding."), StatusToast::Severity::Warn);
        return;
    }
    if (turns_.empty()) {
        return;
    }
    int targetTurnIndex = turnIndex;
    if (targetTurnIndex < 0 || targetTurnIndex >= static_cast<int>(turns_.size())) {
        if (const int selected = selectedTurnIndex(); selected >= 0 && selected < static_cast<int>(turns_.size())) {
            targetTurnIndex = selected;
        } else {
            targetTurnIndex = 0;
        }
    }
    auto& turn = turns_[static_cast<std::size_t>(targetTurnIndex)];

    if (actionIndex >= 0 && actionIndex < static_cast<int>(turn.actions.size())) {
        turn.actions[static_cast<std::size_t>(actionIndex)].action_preset_id = presetId;
        markDirty();
        rebuildPlanTree();
        return;
    }

    addActionToSelectedTurn(presetId, targetTurnIndex);
}

void BattlePlanEditorWindow::addActionToSelectedTurn(std::int64_t presetId, int turnIndex)
{
    TurnDraft* turn = nullptr;
    if (turnIndex >= 0 && turnIndex < static_cast<int>(turns_.size())) {
        turn = &turns_[static_cast<std::size_t>(turnIndex)];
    } else {
        turn = selectedTurn();
    }
    if (turn == nullptr && !turns_.empty()) {
        turn = &turns_.front();
    }
    if (turn == nullptr) {
        return;
    }
    if (presetId <= 0) {
        postStatusMessage(QStringLiteral("Select a valid action preset before adding."), StatusToast::Severity::Warn);
        return;
    }

    ActionDraft action{};
    action.action_preset_id = presetId;
    action.actor_slot = 0;
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
            if (sourceAction.action_preset_id <= 0) {
                postStatusMessage(QStringLiteral("Each action must be assigned to an action preset."), StatusToast::Severity::Warn);
                return;
            }
            if (sourceAction.actor_slot < 0 || sourceAction.actor_slot >= sourceTurn.player_combatants) {
                postStatusMessage(QStringLiteral("Actor slot is outside the turn combatant count."), StatusToast::Severity::Warn);
                return;
            }

            soasimqt2::db::BattlePlanActionDraft action{};
            action.actor_slot = sourceAction.actor_slot;
            action.action_preset_id = sourceAction.action_preset_id;
            action.ordinal = actionIndex;
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

const simcore::db::BattlePlanActionPresetSnapshot* BattlePlanEditorWindow::actionPresetById(std::int64_t presetId) const
{
    if (presetId <= 0) {
        return nullptr;
    }
    for (const auto& preset : actionPresets_) {
        if (preset.action_preset_id == presetId) {
            return &preset;
        }
    }
    return nullptr;
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
