#include "BattlePlanEditorWindow.h"

#include "DB/SavorDbAuthoringService.h"
#include "BattlePlanActionPresetEditorWindow.h"
#include "BattleRunSettingsDragDrop.h"
#include "GUI/Refresh/RowUpdate.h"
#include "GUI/Widgets/ScrollBarStabilizer.h"

#include <QtCore/QMimeData>
#include <QtCore/QSignalBlocker>
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
#include <QtWidgets/QMenu>
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
constexpr int kSlotIndexRole = Qt::UserRole + 5;
constexpr int kNodeTurn = 1;
constexpr int kNodeAction = 2;
constexpr int kMinPlayerCombatants = 1;
constexpr int kMaxPlayerCombatants = 4;

std::string fingerprintForDraft(const savorqt::db::BattlePlanDraft& draft)
{
    std::string content = draft.name + "\n";
    for (const auto& turn : draft.turns) {
        content += "turn:" + std::to_string(turn.turn_index) + "\n";
        content += "predicate_group:"
            + std::to_string(turn.predicate_group_revision_id.value_or(0)) + "\n";
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

QString macroLabel(savor::db::BattlePlanActionMacro macro)
{
    switch (macro) {
    case savor::db::BattlePlanActionMacro::Attack: return QStringLiteral("Attack");
    case savor::db::BattlePlanActionMacro::Defend: return QStringLiteral("Guard");
    case savor::db::BattlePlanActionMacro::Focus: return QStringLiteral("Focus");
    case savor::db::BattlePlanActionMacro::FakeAttack: return QStringLiteral("Fake Attack");
    case savor::db::BattlePlanActionMacro::UseItem: return QStringLiteral("Use Item");
    }
    return QStringLiteral("Attack");
}

QString targetKindLabel(savor::db::BattlePlanTargetKind kind)
{
    switch (kind) {
    case savor::db::BattlePlanTargetKind::SingleEnemy: return QStringLiteral("Single Enemy");
    case savor::db::BattlePlanTargetKind::MultipleEnemies: return QStringLiteral("Multiple Enemies");
    case savor::db::BattlePlanTargetKind::AnyEnemy: return QStringLiteral("Any Enemy");
    case savor::db::BattlePlanTargetKind::SameAsOtherPC: return QStringLiteral("Same As Actor");
    }
    return QStringLiteral("Any Enemy");
}

QString actionSummary(const BattlePlanEditorWindow::ActionDraft& action, const savor::db::BattlePlanActionPresetSnapshot* preset)
{
    if (preset == nullptr) {
        if (action.action_preset_id > 0) {
            return QStringLiteral("missing preset %1").arg(QString::number(action.action_preset_id));
        }
        return QStringLiteral("[empty]");
    }
    QString summary = QStringLiteral("%1  [#%2]  %3 / %4")
        .arg(QString::fromStdString(preset->name))
        .arg(preset->action_preset_id)
        .arg(macroLabel(preset->macro))
        .arg(targetKindLabel(preset->target_kind));
    if (preset->item_id.has_value()) {
        summary += QStringLiteral(" / item %1").arg(*preset->item_id);
    }
    if (preset->target_mask_bits.has_value()) {
        summary += QStringLiteral(" / mask 0x%1").arg(QString::number(preset->target_mask_bits.value(), 16));
    }
    if (preset->target_same_as_actor_slot.has_value()) {
        summary += QStringLiteral(" / same as actor %1").arg(preset->target_same_as_actor_slot.value());
    }
    return summary;
}

QString presetLibraryLabel(const savor::db::BattlePlanActionPresetSnapshot& preset)
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

    std::function<void(std::int64_t, int, int, int)> actionDropped;

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
        int dropSlotIndex = -1;
        if (auto* hit = itemAt(event->position().toPoint()); hit != nullptr) {
            if (hit->data(0, kNodeKindRole).toInt() == kNodeAction) {
                dropActionIndex = hit->data(0, kActionIndexRole).toInt();
                dropSlotIndex = hit->data(0, kSlotIndexRole).toInt();
                hit = hit->parent();
            }
            if (hit != nullptr && hit->data(0, kNodeKindRole).toInt() == kNodeTurn) {
                dropTurnIndex = hit->data(0, kTurnIndexRole).toInt();
            }
            setCurrentItem(hit);
        }
        actionDropped(presetId, dropTurnIndex, dropActionIndex, dropSlotIndex);
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

void BattlePlanEditorWindow::loadSnapshot(const savor::db::BattlePlanSnapshot& snapshot, bool duplicate)
{
    setWindowTitle(duplicate
        ? QStringLiteral("Battle Plan Editor - Duplicate")
        : QStringLiteral("Battle Plan Editor - Edit Copy"));
    nameEdit_->setText(QString::fromStdString(snapshot.name) + (duplicate ? QStringLiteral(" copy") : QString()));

    turns_.clear();
    int turnCount = 1;
    for (const auto& turn : snapshot.turns) {
        turnCount = std::max(turnCount, turn.turn_index);
    }
    turns_.resize(static_cast<std::size_t>(turnCount));
    int combatantCount = kMinPlayerCombatants;
    for (int index = 0; index < turnCount; ++index) {
        turns_[static_cast<std::size_t>(index)].turn_index = index + 1;
        turns_[static_cast<std::size_t>(index)].player_combatants = kMinPlayerCombatants;
    }

    for (const auto& turn : snapshot.turns) {
        if (turn.turn_index < 1 || turn.turn_index > turnCount) {
            continue;
        }
        auto& targetTurn = turns_[static_cast<std::size_t>(turn.turn_index - 1)];
        targetTurn.predicate_group_revision_id =
            turn.default_predicate_group_revision_id;
        for (const auto& action : turn.actions) {
            const auto& preset = action.action_preset;
            ActionDraft draft{};
            draft.action_preset_id = preset.action_preset_id;
            draft.actor_slot = action.actor_slot;
            if (draft.actor_slot < 0 || draft.actor_slot >= kMaxPlayerCombatants) {
                continue;
            }
            if (preset.action_preset_id > 0) {
                const auto it = std::find_if(actionPresets_.begin(), actionPresets_.end(), [&preset](const savor::db::BattlePlanActionPresetSnapshot& item) {
                    return item.action_preset_id == preset.action_preset_id;
                });
                if (it == actionPresets_.end()) {
                    actionPresets_.push_back(preset);
                }
            }
            combatantCount = std::max(combatantCount, draft.actor_slot + 1);
            targetTurn.actions.push_back(std::move(draft));
        }
    }
    combatantCount = std::clamp(combatantCount, kMinPlayerCombatants, kMaxPlayerCombatants);
    {
        const QSignalBlocker blocker(combatantCountSpin_);
        combatantCountSpin_->setValue(combatantCount);
    }
    applyCombatantCountToAllTurns(combatantCount);

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
    combatantCountSpin_ = new QSpinBox(topPanel);
    combatantCountSpin_->setRange(kMinPlayerCombatants, kMaxPlayerCombatants);
    combatantCountSpin_->setValue(kMinPlayerCombatants);
    topLayout->addRow(QStringLiteral("Name"), nameEdit_);
    topLayout->addRow(QStringLiteral("Turns"), turnCountSpin_);
    topLayout->addRow(QStringLiteral("Combatants"), combatantCountSpin_);
    predicateGroupCombo_ = new QComboBox(topPanel);
    predicateGroupCombo_->addItem(QStringLiteral("No predicates"), QVariant::fromValue<qint64>(0));
    topLayout->addRow(QStringLiteral("Selected turn predicates"), predicateGroupCombo_);
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
    auto* libraryButtons = new QHBoxLayout();
    auto* newPresetButton = new QPushButton(QStringLiteral("New Preset"), libraryPanel);
    auto* editPresetButton = new QPushButton(QStringLiteral("Edit Preset"), libraryPanel);
    newPresetButton->setObjectName("jobsSecondaryButton");
    editPresetButton->setObjectName("jobsSecondaryButton");
    libraryButtons->addWidget(newPresetButton);
    libraryButtons->addWidget(editPresetButton);
    libraryButtons->addStretch();
    libraryLayout->addLayout(libraryButtons);
    auto* predicateTitle = new QLabel(QStringLiteral("Predicate Groups"), libraryPanel);
    predicateTitle->setObjectName("sectionHeading");
    predicateSearchEdit_ = new QLineEdit(libraryPanel);
    predicateSearchEdit_->setPlaceholderText(QStringLiteral("Search Predicate Groups"));
    includeDraftPredicatesCheck_ = new QCheckBox(QStringLiteral("Include drafts"), libraryPanel);
    predicateLibraryList_ = new QListWidget(libraryPanel);
    predicateDetailLabel_ = new QLabel(QStringLiteral("Select a Predicate Group to inspect its published contract."), libraryPanel);
    predicateDetailLabel_->setWordWrap(true);
    libraryLayout->addWidget(predicateTitle);
    libraryLayout->addWidget(predicateSearchEdit_);
    libraryLayout->addWidget(includeDraftPredicatesCheck_);
    libraryLayout->addWidget(predicateLibraryList_, 1);
    libraryLayout->addWidget(predicateDetailLabel_);
    populateActionLibrary();
    configurePredicateLibraryRefresh();

    auto* planPanel = new QFrame(splitter);
    planPanel->setObjectName("jobsSurfacePanel");
    auto* planLayout = new QVBoxLayout(planPanel);
    planLayout->setContentsMargins(12, 12, 12, 12);
    planLayout->setSpacing(8);

    auto* tree = new BattlePlanTreeWidget(planPanel);
    planTree_ = tree;
    planTree_->setColumnCount(2);
    planTree_->setHeaderLabels(QStringList{ QStringLiteral("Turn Tree"), QStringLiteral("Actions") });
    planTree_->setSelectionMode(QAbstractItemView::SingleSelection);
    planTree_->setContextMenuPolicy(Qt::CustomContextMenu);
    planTree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    planTree_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    tree->actionDropped = [this](std::int64_t presetId, int turnIndex, int actionIndex, int slotIndex) {
        assignActionPreset(presetId, turnIndex, actionIndex, slotIndex);
    };
    planLayout->addWidget(planTree_, 1);

    splitter->addWidget(libraryPanel);
    splitter->addWidget(planPanel);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
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
    connect(actionLibraryList_, &QListWidget::itemDoubleClicked, this, &BattlePlanEditorWindow::addActionFromLibrarySelection);
    connect(newPresetButton, &QPushButton::clicked, this, &BattlePlanEditorWindow::openNewActionPresetEditor);
    connect(editPresetButton, &QPushButton::clicked, this, &BattlePlanEditorWindow::openActionPresetEditorForSelection);
    connect(planTree_, &QWidget::customContextMenuRequested, this, &BattlePlanEditorWindow::showPlanContextMenu);
    connect(planTree_, &QTreeWidget::currentItemChanged, this, [this]() {
        syncPredicateSelectorToTurn();
    });
    connect(predicateGroupCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() {
        applyPredicateGroupSelection();
    });
    connect(predicateSearchEdit_, &QLineEdit::textChanged, this, [this]() {
        requestPredicateLibraryRefresh();
    });
    connect(includeDraftPredicatesCheck_, &QCheckBox::toggled, this, [this]() {
        requestPredicateLibraryRefresh();
    });
    connect(predicateLibraryList_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* current) {
        showPredicateGroupDetail(current ? current->data(Qt::UserRole).toLongLong() : 0);
    });
    connect(nameEdit_, &QLineEdit::textChanged, this, [this]() { markDirty(); });
    connect(turnCountSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        ensureTurnCount(value);
        markDirty();
    });
    connect(combatantCountSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        applyCombatantCountToAllTurns(value);
        markDirty();
        rebuildPlanTree();
    });
    requestPredicateLibraryRefresh();
}

void BattlePlanEditorWindow::populateActionLibrary()
{
    actionLibraryList_->clear();
    const auto result = savorqt::db::SavorDbAuthoringService::ListBattlePlanActionPresets();
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

void BattlePlanEditorWindow::configurePredicateLibraryRefresh()
{
    predicateRefreshPipeline_ = new savorqt::gui::AsyncRefreshPipeline<PredicateRefreshRequest, PredicateRefreshData>(this);
    predicateRefreshPipeline_->setAutoRefreshEnabled(false);
    predicateRefreshPipeline_->setRequestBuilder([this](savorqt::gui::RefreshReason) -> std::optional<PredicateRefreshRequest> {
        return PredicateRefreshRequest{
            .revision_state = includeDraftPredicatesCheck_->isChecked()
                ? std::nullopt : std::optional<std::string>("PUBLISHED"),
            .search_text = predicateSearchEdit_->text().trimmed().toStdString(),
        };
    });
    predicateRefreshPipeline_->setLoadAndPrepare([](PredicateRefreshRequest request) {
        PredicateRefreshData data{};
        data.library = savorqt::db::SavorDbAuthoringService::ListPredicateGroupRevisions(
            std::move(request.revision_state), std::move(request.search_text), std::nullopt, 250);
        data.published = savorqt::db::SavorDbAuthoringService::ListPredicateGroupRevisions(
            std::optional<std::string>("PUBLISHED"), {}, std::nullopt, 500);
        return savorqt::gui::AsyncRefreshResult<PredicateRefreshData>::Ok(std::move(data));
    });
    predicateRefreshPipeline_->setApply([this](const PredicateRefreshData& data, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        if (!data.library.ok || !data.published.ok) {
            const auto& error = !data.library.ok ? data.library.error : data.published.error;
            postStatusMessage(QString::fromStdString(error.message), StatusToast::Severity::Warn);
            return;
        }
        const auto selectedLibraryId = predicateLibraryList_->currentItem()
            ? predicateLibraryList_->currentItem()->data(Qt::UserRole).toLongLong() : 0;
        predicateGroups_ = data.published.value.items;
        predicateLibraryList_->clear();
        for (const auto& group : data.library.value.items) {
            auto* item = new QListWidgetItem(
                QStringLiteral("%1 r%2 [%3]")
                    .arg(QString::fromStdString(group.name))
                    .arg(group.revision_number)
                    .arg(QString::fromStdString(group.revision_state)),
                predicateLibraryList_);
            item->setData(Qt::UserRole, QVariant::fromValue<qint64>(group.predicate_group_revision_id));
            item->setToolTip(QString::fromStdString(group.description));
            if (group.predicate_group_revision_id == selectedLibraryId) predicateLibraryList_->setCurrentItem(item);
        }

        const auto desiredTurnId = selectedTurn()
            ? selectedTurn()->predicate_group_revision_id.value_or(0) : 0;
        const QSignalBlocker blocker(predicateGroupCombo_);
        predicateGroupCombo_->clear();
        predicateGroupCombo_->addItem(QStringLiteral("No predicates"), QVariant::fromValue<qint64>(0));
        bool resolved = desiredTurnId == 0;
        for (const auto& group : predicateGroups_) {
            if (group.revision_state != "PUBLISHED") continue;
            predicateGroupCombo_->addItem(
                QStringLiteral("%1 r%2 (#%3)")
                    .arg(QString::fromStdString(group.name))
                    .arg(group.revision_number)
                    .arg(group.predicate_group_revision_id),
                QVariant::fromValue<qint64>(group.predicate_group_revision_id));
            if (group.predicate_group_revision_id == desiredTurnId) {
                predicateGroupCombo_->setCurrentIndex(predicateGroupCombo_->count() - 1);
                resolved = true;
            }
        }
        if (!resolved) {
            predicateGroupCombo_->addItem(
                QStringLiteral("Unresolved group #%1").arg(desiredTurnId),
                QVariant::fromValue<qint64>(desiredTurnId));
            predicateGroupCombo_->setCurrentIndex(predicateGroupCombo_->count() - 1);
        }
    });
    predicateRefreshPipeline_->setApplyError([this](const QString& error, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        postStatusMessage(error, StatusToast::Severity::Warn);
    });
    predicateRefreshPipeline_->setActive(true);

    predicateDetailPipeline_ = new savorqt::gui::AsyncRefreshPipeline<std::int64_t, PredicateDetailData>(this);
    predicateDetailPipeline_->setAutoRefreshEnabled(false);
    predicateDetailPipeline_->setRequestBuilder([](savorqt::gui::RefreshReason) -> std::optional<std::int64_t> {
        return std::nullopt;
    });
    predicateDetailPipeline_->setLoadAndPrepare([](std::int64_t revisionId) {
        return savorqt::gui::AsyncRefreshResult<PredicateDetailData>::Ok(
            savorqt::db::SavorDbAuthoringService::GetPredicateGroupRevision(revisionId));
    });
    predicateDetailPipeline_->setApply([this](const PredicateDetailData& result, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        if (!result.ok) {
            predicateDetailLabel_->setText(QString::fromStdString(result.error.message));
            return;
        }
        const auto& group = result.value.group;
        QStringList lines;
        lines << QStringLiteral("%1\n%2\nHash: %3")
            .arg(QString::fromStdString(result.value.name))
            .arg(QString::fromStdString(result.value.description))
            .arg(QString::fromStdString(group.content_sha256));
        lines << QStringLiteral("Members: %1").arg(group.members.size());
        for (const auto& member : group.members) {
            QStringList hooks;
            for (const auto& hook : member.semantic_hook_ids)
                hooks << QString::fromStdString(hook);
            lines << QStringLiteral("Binding #%1 at %2: %3, aggregate=%4, evidence=%5")
                .arg(member.execution_binding_revision_id)
                .arg(hooks.join(QStringLiteral(", ")))
                .arg(member.reaction == savor::runtime::program::composition::PredicateReaction::AbortOnFail
                    ? QStringLiteral("AbortOnFail") : QStringLiteral("RecordAndContinue"))
                .arg(member.participates_in_aggregation ? QStringLiteral("yes") : QStringLiteral("no"))
                .arg(member.emit_evidence ? QStringLiteral("yes") : QStringLiteral("no"));
        }
        predicateDetailLabel_->setText(lines.join(QStringLiteral("\n")));
    });
    predicateDetailPipeline_->setActive(true);
}

void BattlePlanEditorWindow::requestPredicateLibraryRefresh()
{
    if (predicateRefreshPipeline_) predicateRefreshPipeline_->requestRefresh(savorqt::gui::RefreshReason::Manual);
}

void BattlePlanEditorWindow::applyPredicateGroupSelection()
{
    auto* turn = selectedTurn();
    if (turn == nullptr || predicateGroupCombo_ == nullptr) return;
    const auto id = predicateGroupCombo_->currentData().toLongLong();
    const auto next = id > 0 ? std::optional<std::int64_t>(id) : std::nullopt;
    if (turn->predicate_group_revision_id == next) return;
    turn->predicate_group_revision_id = next;
    markDirty();
    rebuildPlanTree();
}

void BattlePlanEditorWindow::syncPredicateSelectorToTurn()
{
    if (!predicateGroupCombo_) return;
    const auto id = selectedTurn() ? selectedTurn()->predicate_group_revision_id.value_or(0) : 0;
    const QSignalBlocker blocker(predicateGroupCombo_);
    for (int index = 0; index < predicateGroupCombo_->count(); ++index) {
        if (predicateGroupCombo_->itemData(index).toLongLong() == id) {
            predicateGroupCombo_->setCurrentIndex(index);
            return;
        }
    }
}

void BattlePlanEditorWindow::showPredicateGroupDetail(std::int64_t revisionId)
{
    if (revisionId <= 0 || predicateDetailPipeline_ == nullptr) {
        predicateDetailLabel_->setText(QStringLiteral("Select a Predicate Group to inspect its contract."));
        return;
    }
    predicateDetailPipeline_->setRequestBuilder([revisionId](savorqt::gui::RefreshReason) -> std::optional<std::int64_t> {
        return revisionId;
    });
    predicateDetailPipeline_->requestRefresh(savorqt::gui::RefreshReason::Manual);
}

void BattlePlanEditorWindow::rebuildPlanTree()
{
    if (planTree_ == nullptr) {
        return;
    }
    rebuildingTree_ = true;
    const int previousTurn = selectedTurnIndex();
    const int previousSlot = selectedSlotIndex();
    std::vector<PlanTreeTurnRow> rows;
    rows.reserve(turns_.size());
    for (int turnIndex = 0; turnIndex < static_cast<int>(turns_.size()); ++turnIndex) {
        auto& turn = turns_[static_cast<std::size_t>(turnIndex)];
        normalizeTurnSlots(turn);

        const int assignedActions = static_cast<int>(std::count_if(turn.actions.begin(), turn.actions.end(), [](const ActionDraft& action) {
            return action.action_preset_id > 0;
        }));
        PlanTreeTurnRow row{};
        row.turnIndex = turnIndex;
        row.label = QStringLiteral("Turn %1").arg(turn.turn_index);
        row.summary = QStringLiteral("%1/%2 slots assigned")
            .arg(assignedActions)
            .arg(turn.player_combatants);
        row.summary += turn.predicate_group_revision_id
            ? QStringLiteral(" / predicates #%1").arg(*turn.predicate_group_revision_id)
            : QStringLiteral(" / no predicates");

        for (int slotIndex = 0; slotIndex < turn.player_combatants; ++slotIndex) {
            const int actionIndex = findActionIndexBySlot(turn, slotIndex);
            const bool hasAction = actionIndex >= 0;
            const auto* action = hasAction ? &turn.actions[static_cast<std::size_t>(actionIndex)] : nullptr;
            const auto* preset = hasAction ? actionPresetById(action->action_preset_id) : nullptr;
            row.actions.push_back(PlanTreeActionRow{
                slotIndex,
                hasAction ? actionIndex : -1,
                actionSummary(hasAction ? *action : ActionDraft{}, preset),
            });
        }
        rows.push_back(std::move(row));
    }

    savorqt::gui::ApplyTreeRowsByKey(
        planTree_,
        currentPlanTreeRows_,
        rows,
        [](const PlanTreeTurnRow& row) { return row.turnIndex; },
        [](const PlanTreeTurnRow& lhs, const PlanTreeTurnRow& rhs) {
            return lhs.turnIndex == rhs.turnIndex
                && lhs.label == rhs.label
                && lhs.summary == rhs.summary
                && savorqt::gui::RowsEqual(lhs.actions, rhs.actions, [](const PlanTreeActionRow& lhsAction, const PlanTreeActionRow& rhsAction) {
                    return lhsAction.slotIndex == rhsAction.slotIndex
                        && lhsAction.actionIndex == rhsAction.actionIndex
                        && lhsAction.summary == rhsAction.summary;
                });
        },
        [](QTreeWidget*, QTreeWidgetItem* turnItem, const PlanTreeTurnRow& row) {
            if (turnItem == nullptr) {
                return;
            }
            turnItem->setText(0, row.label);
            turnItem->setText(1, row.summary);
            turnItem->setData(0, kNodeKindRole, kNodeTurn);
            turnItem->setData(0, kTurnIndexRole, row.turnIndex);

            const auto oldChildren = turnItem->takeChildren();
            for (QTreeWidgetItem* child : oldChildren) {
                delete child;
            }
            for (const auto& action : row.actions) {
                auto* actionItem = new QTreeWidgetItem(turnItem);
                actionItem->setText(0, QStringLiteral("Slot %1").arg(action.slotIndex + 1));
                actionItem->setText(1, action.summary);
                actionItem->setData(0, kNodeKindRole, kNodeAction);
                actionItem->setData(0, kTurnIndexRole, row.turnIndex);
                actionItem->setData(0, kSlotIndexRole, action.slotIndex);
                actionItem->setData(0, kActionIndexRole, action.actionIndex);
            }
            turnItem->setExpanded(true);
        });

    QTreeWidgetItem* restoreItem = nullptr;
    if (previousTurn >= 0 && previousTurn < planTree_->topLevelItemCount()) {
        restoreItem = planTree_->topLevelItem(previousTurn);
        if (previousSlot >= 0 && previousSlot < restoreItem->childCount()) {
            restoreItem = restoreItem->child(previousSlot);
        }
    }
    if (restoreItem == nullptr && planTree_->topLevelItemCount() > 0) {
        restoreItem = planTree_->topLevelItem(0);
    }
    if (restoreItem != nullptr) {
        planTree_->setCurrentItem(restoreItem);
    }
    rebuildingTree_ = false;
}

void BattlePlanEditorWindow::applyCombatantCountToAllTurns(int combatantCount)
{
    combatantCount = std::clamp(combatantCount, kMinPlayerCombatants, kMaxPlayerCombatants);
    for (auto& turn : turns_) {
        turn.player_combatants = combatantCount;
        normalizeTurnSlots(turn);
    }
}

void BattlePlanEditorWindow::addActionFromLibrarySelection()
{
    auto presetId = std::int64_t{0};
    if (auto* item = actionLibraryList_->currentItem(); item != nullptr) {
        presetId = item->data(kActionPresetIdRole).toLongLong();
    }
    assignPresetToSelection(presetId);
}

void BattlePlanEditorWindow::openNewActionPresetEditor()
{
    if (presetEditor_ != nullptr) {
        presetEditor_->close();
    }
    auto* editor = new BattlePlanActionPresetEditorWindow(this);
    presetEditor_ = editor;
    editor->setStatusCallback([this](const QString& text, StatusToast::Severity severity) {
        postStatusMessage(text, severity);
    });
    editor->setSavedCallback([this]() {
        populateActionLibrary();
    });
    editor->loadNew();
    editor->show();
    editor->raise();
    editor->activateWindow();
}

void BattlePlanEditorWindow::openActionPresetEditorForSelection()
{
    auto presetId = selectedPresetIdFromAction();
    if (presetId <= 0) {
        presetId = selectedPresetIdFromLibrary();
    }
    if (presetId <= 0) {
        openNewActionPresetEditor();
        return;
    }
    openPresetEditor(presetId, false);
}

void BattlePlanEditorWindow::openActionPresetEditorForAction()
{
    openPresetEditor(selectedPresetIdFromAction(), false);
}

std::int64_t BattlePlanEditorWindow::selectedPresetIdFromLibrary() const
{
    const auto* item = actionLibraryList_->currentItem();
    if (item == nullptr) {
        return 0;
    }
    return item->data(kActionPresetIdRole).toLongLong();
}

std::int64_t BattlePlanEditorWindow::selectedPresetIdFromAction() const
{
    const auto* action = selectedAction();
    return action != nullptr ? action->action_preset_id : 0;
}

void BattlePlanEditorWindow::openPresetEditor(std::int64_t presetId, bool duplicate)
{
    if (presetId <= 0) {
        openNewActionPresetEditor();
        return;
    }
    const auto result = savorqt::db::SavorDbAuthoringService::GetBattlePlanActionPreset(presetId);
    if (!result.ok) {
        postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
        return;
    }
    if (presetEditor_ != nullptr) {
        presetEditor_->close();
    }

    auto* editor = new BattlePlanActionPresetEditorWindow(this);
    presetEditor_ = editor;
    editor->setStatusCallback([this](const QString& text, StatusToast::Severity severity) {
        postStatusMessage(text, severity);
    });
    editor->setSavedCallback([this]() {
        populateActionLibrary();
        rebuildPlanTree();
    });
    editor->loadSnapshot(result.value, duplicate);
    editor->show();
    editor->raise();
    editor->activateWindow();
}

void BattlePlanEditorWindow::clearSelectedSlot()
{
    const int turnIndex = selectedTurnIndex();
    const int slotIndex = selectedSlotIndex();
    if (turnIndex < 0 || turnIndex >= static_cast<int>(turns_.size()) || slotIndex < 0) {
        return;
    }
    auto& turn = turns_[static_cast<std::size_t>(turnIndex)];
    const int actionIndex = findActionIndexBySlot(turn, slotIndex);
    if (actionIndex < 0) {
        return;
    }
    turn.actions[static_cast<std::size_t>(actionIndex)].action_preset_id = 0;
    markDirty();
    rebuildPlanTree();
    postStatusMessage(QStringLiteral("Slot cleared. Assign a preset to keep this plan valid."), StatusToast::Severity::Warn);
}

void BattlePlanEditorWindow::showPlanContextMenu(const QPoint& position)
{
    auto* hit = planTree_->itemAt(position);
    if (hit == nullptr) {
        return;
    }
    planTree_->setCurrentItem(hit);
    const bool hitTurn = isTurnItem(hit);
    const bool hitAction = isActionItem(hit);
    if (!hitTurn && !hitAction) {
        return;
    }

    auto* turn = selectedTurn();
    const int slotIndex = selectedSlotIndex();
    const int selectedCombatants = turn != nullptr ? turn->player_combatants : 0;
    const int selectedAction = selectedActionIndex();
    const bool canClear = turn != nullptr && slotIndex >= 0;
    const bool canAssign = canClear && actionLibraryList_->currentItem() != nullptr
        && actionLibraryList_->currentItem()->data(kActionPresetIdRole).toLongLong() > 0;
    const bool hasAssignedAction = selectedAction >= 0 && selectedAction < static_cast<int>(turn->actions.size())
        && turn->actions[static_cast<std::size_t>(selectedAction)].action_preset_id > 0;
    QMenu menu(planTree_);
    if (hitAction) {
        QAction* assignAction = menu.addAction(QStringLiteral("Assign selected preset"));
        QAction* clearAction = menu.addAction(QStringLiteral("Delete slot"));
        QAction* editPresetAction = menu.addAction(QStringLiteral("Edit preset"));
        menu.addSeparator();
        QAction* duplicateAction = menu.addAction(QStringLiteral("Duplicate"));
        QAction* moveUpAction = menu.addAction(QStringLiteral("Move slot up"));
        QAction* moveDownAction = menu.addAction(QStringLiteral("Move slot down"));

        assignAction->setEnabled(canAssign);
        clearAction->setEnabled(canClear);
        editPresetAction->setEnabled(hasAssignedAction);
        duplicateAction->setEnabled(hasAssignedAction);
        moveUpAction->setEnabled(slotIndex > 0);
        moveDownAction->setEnabled(slotIndex >= 0 && slotIndex + 1 < selectedCombatants);

        QAction* chosen = menu.exec(planTree_->viewport()->mapToGlobal(position));
        if (chosen == assignAction) {
            addActionFromLibrarySelection();
        } else if (chosen == clearAction) {
            clearSelectedSlot();
        } else if (chosen == editPresetAction) {
            openActionPresetEditorForAction();
        } else if (chosen == duplicateAction) {
            duplicateSelectedAction();
        } else if (chosen == moveUpAction) {
            moveSelectedAction(-1);
        } else if (chosen == moveDownAction) {
            moveSelectedAction(1);
        }
    } else if (hitTurn) {
        QAction* clearTurnAction = menu.addAction(QStringLiteral("Clear turn actions"));
        QAction* deleteTurnAction = menu.addAction(QStringLiteral("Delete turn"));
        const bool canDeleteTurn = static_cast<int>(turns_.size()) > 1;
        const bool canClearTurn = turn != nullptr && turn->player_combatants > 0;
        clearTurnAction->setEnabled(canClearTurn);
        deleteTurnAction->setEnabled(canDeleteTurn);
        QAction* chosen = menu.exec(planTree_->viewport()->mapToGlobal(position));
        if (chosen == clearTurnAction && turn != nullptr && turn->actions.size() > 0) {
            for (auto& action : turn->actions) {
                action.action_preset_id = 0;
            }
            markDirty();
            rebuildPlanTree();
        } else if (chosen == deleteTurnAction) {
            removeSelectedNode();
        }
    }
}

void BattlePlanEditorWindow::assignPresetToSelection(std::int64_t presetId)
{
    const int turnIndex = selectedTurnIndex();
    const int actionIndex = selectedActionIndex();
    const int slotIndex = selectedSlotIndex();
    if (turnIndex < 0 && turns_.empty()) {
        return;
    }
    if (actionIndex >= 0 && turnIndex >= 0) {
        const int resolvedSlot = slotIndex >= 0 ? slotIndex : -1;
        assignActionPreset(presetId, turnIndex, actionIndex, resolvedSlot);
    } else {
        addActionToSelectedTurn(presetId, turnIndex, slotIndex);
    }
}

void BattlePlanEditorWindow::assignActionPreset(std::int64_t presetId, int turnIndex, int actionIndex, int slotIndex)
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
    normalizeTurnSlots(turn);

    if (slotIndex < 0 && actionIndex >= 0 && actionIndex < static_cast<int>(turn.actions.size())) {
        slotIndex = turn.actions[static_cast<std::size_t>(actionIndex)].actor_slot;
    }
    if (slotIndex < 0 || slotIndex >= turn.player_combatants) {
        if (selectedTurnIndex() == targetTurnIndex) {
            slotIndex = selectedSlotIndex();
        }
        if (slotIndex < 0 || slotIndex >= turn.player_combatants) {
            slotIndex = 0;
        }
    }
    const auto existingAction = findActionIndexBySlot(turn, slotIndex);
    if (existingAction >= 0) {
        turn.actions[static_cast<std::size_t>(existingAction)].action_preset_id = presetId;
    } else {
        ActionDraft action{};
        action.actor_slot = slotIndex;
        action.action_preset_id = presetId;
        turn.actions.push_back(std::move(action));
        normalizeTurnSlots(turn);
    }
    markDirty();
    rebuildPlanTree();
}

void BattlePlanEditorWindow::addActionToSelectedTurn(std::int64_t presetId, int turnIndex, int slotIndex)
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
    normalizeTurnSlots(*turn);
    if (slotIndex < 0) {
        slotIndex = selectedSlotIndex();
    }
    if (slotIndex < 0 || slotIndex >= turn->player_combatants) {
        slotIndex = 0;
    }

    const int existingAction = findActionIndexBySlot(*turn, slotIndex);
    if (existingAction >= 0) {
        turn->actions[static_cast<std::size_t>(existingAction)].action_preset_id = presetId;
    } else {
        ActionDraft action{};
        action.action_preset_id = presetId;
        action.actor_slot = slotIndex;
        turn->actions.push_back(std::move(action));
        normalizeTurnSlots(*turn);
    }
    markDirty();
    rebuildPlanTree();
}

void BattlePlanEditorWindow::duplicateSelectedAction()
{
    auto* turn = selectedTurn();
    const int sourceActionIndex = selectedActionIndex();
    const int sourceSlot = selectedSlotIndex();
    if (turn == nullptr || sourceActionIndex < 0 || sourceSlot < 0 || sourceActionIndex >= static_cast<int>(turn->actions.size())) {
        return;
    }
    const int maxSlots = std::clamp(turn->player_combatants, kMinPlayerCombatants, kMaxPlayerCombatants);
    if (maxSlots <= 0) {
        return;
    }
    int duplicateSlot = -1;
    for (int slotIndex = 0; slotIndex < maxSlots; ++slotIndex) {
        const int actionIndex = findActionIndexBySlot(*turn, slotIndex);
        if (slotIndex == sourceSlot) {
            continue;
        }
        if (actionIndex < 0 || turn->actions[static_cast<std::size_t>(actionIndex)].action_preset_id <= 0) {
            duplicateSlot = slotIndex;
            break;
        }
    }
    if (duplicateSlot < 0) {
        postStatusMessage(QStringLiteral("No free slot available to duplicate into."), StatusToast::Severity::Warn);
        return;
    }
    auto copy = turn->actions[static_cast<std::size_t>(sourceActionIndex)];
    copy.actor_slot = duplicateSlot;
    turn->actions.push_back(std::move(copy));
    normalizeTurnSlots(*turn);
    markDirty();
    rebuildPlanTree();
}

void BattlePlanEditorWindow::removeSelectedNode()
{
    const int turnIndex = selectedTurnIndex();
    const int actionIndex = selectedActionIndex();
    const int slotIndex = selectedSlotIndex();
    if (turnIndex < 0 || turnIndex >= static_cast<int>(turns_.size())) {
        return;
    }
    auto& turn = turns_[static_cast<std::size_t>(turnIndex)];
    if (actionIndex >= 0 && actionIndex < static_cast<int>(turn.actions.size()) && slotIndex >= 0) {
        postStatusMessage(QStringLiteral("Slots must remain assigned. Replace with another preset instead."), StatusToast::Severity::Warn);
        return;
    } else {
        const bool hasAssignedActions = std::any_of(turn.actions.begin(), turn.actions.end(), [](const ActionDraft& action) {
            return action.action_preset_id > 0;
        });
        if (hasAssignedActions) {
            postStatusMessage(QStringLiteral("Clear assigned slots before removing a turn."), StatusToast::Severity::Warn);
            return;
        }
        if (turns_.size() <= 1) {
            return;
        }
        turns_.erase(turns_.begin() + turnIndex);
        for (int index = 0; index < static_cast<int>(turns_.size()); ++index) {
            turns_[static_cast<std::size_t>(index)].turn_index = index + 1;
        }
        turnCountSpin_->setValue(static_cast<int>(turns_.size()));
    }
    markDirty();
    rebuildPlanTree();
}

void BattlePlanEditorWindow::moveSelectedAction(int delta)
{
    auto* turn = selectedTurn();
    const int actionIndex = selectedActionIndex();
    const int selectedSlot = selectedSlotIndex();
    if (turn == nullptr || actionIndex < 0 || selectedSlot < 0) {
        return;
    }
    const int nextSlot = selectedSlot + delta;
    if (nextSlot < 0 || nextSlot >= turn->player_combatants) {
        return;
    }
    const int targetActionIndex = findActionIndexBySlot(*turn, nextSlot);
    if (targetActionIndex < 0) {
        turn->actions[static_cast<std::size_t>(actionIndex)].actor_slot = nextSlot;
    } else {
        std::swap(turn->actions[static_cast<std::size_t>(actionIndex)].actor_slot,
            turn->actions[static_cast<std::size_t>(targetActionIndex)].actor_slot);
    }
    normalizeTurnSlots(*turn);
    markDirty();
    rebuildPlanTree();
}

void BattlePlanEditorWindow::ensureTurnCount(int count)
{
    count = std::clamp(count, 1, 20);
    const int previous = static_cast<int>(turns_.size());
    if (count == previous) {
        return;
    }
    turns_.resize(static_cast<std::size_t>(count));
    const int combatantCount = combatantCountSpin_ != nullptr
        ? combatantCountSpin_->value()
        : kMinPlayerCombatants;
    for (int index = 0; index < count; ++index) {
        auto& turn = turns_[static_cast<std::size_t>(index)];
        turn.turn_index = index + 1;
        turn.player_combatants = std::clamp(combatantCount, kMinPlayerCombatants, kMaxPlayerCombatants);
        normalizeTurnSlots(turn);
    }
    rebuildPlanTree();
}

void BattlePlanEditorWindow::saveBattlePlan()
{
    if (nameEdit_->text().trimmed().isEmpty()) {
        postStatusMessage(QStringLiteral("Battle plan name is required."), StatusToast::Severity::Warn);
        return;
    }

    savorqt::db::BattlePlanDraft draft{};
    draft.name = nameEdit_->text().trimmed().toStdString();
    draft.turns.resize(turns_.size());

    for (int turnIndex = 0; turnIndex < static_cast<int>(turns_.size()); ++turnIndex) {
        const auto& sourceTurn = turns_[static_cast<std::size_t>(turnIndex)];
        auto& targetTurn = draft.turns[static_cast<std::size_t>(turnIndex)];
        targetTurn.turn_index = turnIndex + 1;
        targetTurn.predicate_group_revision_id = sourceTurn.predicate_group_revision_id;
        if (sourceTurn.predicate_group_revision_id) {
            const auto resolved = std::find_if(predicateGroups_.begin(), predicateGroups_.end(), [&](const auto& group) {
                return group.predicate_group_revision_id == *sourceTurn.predicate_group_revision_id
                    && group.revision_state == "PUBLISHED";
            });
            if (resolved == predicateGroups_.end()) {
                postStatusMessage(
                    QStringLiteral("Turn %1 references a missing or unpublished predicate group.").arg(turnIndex + 1),
                    StatusToast::Severity::Warn);
                return;
            }
        }
        if (!hasValidSlotAssignments(sourceTurn)) {
            postStatusMessage(QStringLiteral("Every combatant slot must be assigned."), StatusToast::Severity::Warn);
            return;
        }
        for (int slotIndex = 0; slotIndex < sourceTurn.player_combatants; ++slotIndex) {
            const int actionIndex = findActionIndexBySlot(sourceTurn, slotIndex);
            const auto& sourceAction = sourceTurn.actions[static_cast<std::size_t>(actionIndex)];
            if (sourceAction.action_preset_id <= 0) {
                postStatusMessage(QStringLiteral("Each action must be assigned to an action preset."), StatusToast::Severity::Warn);
                return;
            }
            if (sourceAction.actor_slot < 0 || sourceAction.actor_slot >= sourceTurn.player_combatants) {
                postStatusMessage(QStringLiteral("Action is outside the combatant slot range."), StatusToast::Severity::Warn);
                return;
            }

            savorqt::db::BattlePlanActionDraft action{};
            action.actor_slot = sourceAction.actor_slot;
            action.action_preset_id = sourceAction.action_preset_id;
            action.ordinal = slotIndex;
            targetTurn.actions.push_back(std::move(action));
        }
    }
    draft.fingerprint = fingerprintForDraft(draft);

    saveButton_->setEnabled(false);
    const auto result = savorqt::db::SavorDbAuthoringService::SaveBattlePlan(draft);
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

int BattlePlanEditorWindow::selectedSlotIndex() const
{
    const auto* item = selectedTreeItem();
    return item != nullptr && isActionItem(item) ? item->data(0, kSlotIndexRole).toInt() : -1;
}

int BattlePlanEditorWindow::findActionIndexBySlot(const TurnDraft& turn, int slotIndex) const
{
    if (slotIndex < 0) {
        return -1;
    }
    for (int index = 0; index < static_cast<int>(turn.actions.size()); ++index) {
        if (turn.actions[static_cast<std::size_t>(index)].actor_slot == slotIndex) {
            return index;
        }
    }
    return -1;
}

void BattlePlanEditorWindow::normalizeTurnSlots(TurnDraft& turn) const
{
    const int maxSlots = std::clamp(turn.player_combatants, kMinPlayerCombatants, kMaxPlayerCombatants);
    turn.player_combatants = maxSlots;
    std::vector<ActionDraft> normalized;
    normalized.reserve(static_cast<std::size_t>(maxSlots));

    for (int slotIndex = 0; slotIndex < maxSlots; ++slotIndex) {
        ActionDraft slotAction{};
        slotAction.actor_slot = slotIndex;
        slotAction.action_preset_id = 0;
        bool foundValue = false;
        for (const auto& action : turn.actions) {
            if (action.actor_slot < 0 || action.actor_slot >= maxSlots) {
                continue;
            }
            if (action.actor_slot != slotIndex) {
                continue;
            }
            if (!foundValue || action.action_preset_id > 0) {
                slotAction = action;
                slotAction.actor_slot = slotIndex;
                foundValue = true;
            }
            if (action.action_preset_id > 0) {
                slotAction.action_preset_id = action.action_preset_id;
            }
        }
        normalized.push_back(std::move(slotAction));
    }
    normalized.shrink_to_fit();
    turn.actions = std::move(normalized);
}

bool BattlePlanEditorWindow::hasValidSlotAssignments(const TurnDraft& turn) const
{
    const int maxSlots = std::clamp(turn.player_combatants, kMinPlayerCombatants, kMaxPlayerCombatants);
    for (int slotIndex = 0; slotIndex < maxSlots; ++slotIndex) {
        const int actionIndex = findActionIndexBySlot(turn, slotIndex);
        if (actionIndex < 0) {
            return false;
        }
        const auto& action = turn.actions[static_cast<std::size_t>(actionIndex)];
        if (action.action_preset_id <= 0) {
            return false;
        }
        if (action.actor_slot != slotIndex) {
            return false;
        }
    }
    for (const auto& action : turn.actions) {
        if (action.actor_slot < 0 || action.actor_slot >= maxSlots) {
            return false;
        }
    }
    return true;
}

const savor::db::BattlePlanActionPresetSnapshot* BattlePlanEditorWindow::actionPresetById(std::int64_t presetId) const
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
