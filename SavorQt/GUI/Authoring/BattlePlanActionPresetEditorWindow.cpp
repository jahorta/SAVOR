#include "BattlePlanActionPresetEditorWindow.h"

#include "DB/SavorDbAuthoringService.h"

#include <QtCore/QRegularExpression>
#include <QtGui/QRegularExpressionValidator>
#include <QtGui/QCloseEvent>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cstdint>

namespace {

QString macroLabel(savor::db::BattlePlanActionMacro macro)
{
    switch (macro) {
    case savor::db::BattlePlanActionMacro::Attack: return QStringLiteral("Attack");
    case savor::db::BattlePlanActionMacro::Defend: return QStringLiteral("Guard");
    case savor::db::BattlePlanActionMacro::Focus: return QStringLiteral("Focus");
    case savor::db::BattlePlanActionMacro::FakeAttack: return QStringLiteral("Fake Attack");
    case savor::db::BattlePlanActionMacro::UseItem: return QStringLiteral("Use Item");
    default: return QStringLiteral("Attack");
    }
}

QString targetKindLabel(savor::db::BattlePlanTargetKind kind)
{
    switch (kind) {
    case savor::db::BattlePlanTargetKind::SingleEnemy: return QStringLiteral("Single Enemy");
    case savor::db::BattlePlanTargetKind::MultipleEnemies: return QStringLiteral("Multiple Enemies");
    case savor::db::BattlePlanTargetKind::AnyEnemy: return QStringLiteral("Any Enemy");
    case savor::db::BattlePlanTargetKind::SameAsOtherPC: return QStringLiteral("Same As Other PC");
    default: return QStringLiteral("Any Enemy");
    }
}

constexpr int kMinTargetSingleSlot = 4;
constexpr int kMaxTargetSingleSlot = 11;

} // namespace

BattlePlanActionPresetEditorWindow::BattlePlanActionPresetEditorWindow(QWidget* parent)
    : PersistentToolWindow(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("Battle Plan Action Preset Editor"));
    resize(640, 420);
    createWidgets();
    loadNew();
}

void BattlePlanActionPresetEditorWindow::setStatusCallback(std::function<void(const QString&, StatusToast::Severity)> callback)
{
    statusCallback_ = std::move(callback);
}

void BattlePlanActionPresetEditorWindow::setSavedCallback(std::function<void()> callback)
{
    savedCallback_ = std::move(callback);
}

void BattlePlanActionPresetEditorWindow::createWidgets()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto* panel = new QFrame(this);
    panel->setObjectName("jobsSurfacePanel");
    auto* form = new QFormLayout(panel);
    form->setContentsMargins(14, 14, 14, 14);
    form->setSpacing(10);

    nameEdit_ = new QLineEdit(panel);
    macroCombo_ = new QComboBox(panel);
    macroCombo_->addItem(macroLabel(savor::db::BattlePlanActionMacro::Attack), static_cast<int>(savor::db::BattlePlanActionMacro::Attack));
    macroCombo_->addItem(macroLabel(savor::db::BattlePlanActionMacro::Defend), static_cast<int>(savor::db::BattlePlanActionMacro::Defend));
    macroCombo_->addItem(macroLabel(savor::db::BattlePlanActionMacro::Focus), static_cast<int>(savor::db::BattlePlanActionMacro::Focus));
    macroCombo_->addItem(macroLabel(savor::db::BattlePlanActionMacro::FakeAttack), static_cast<int>(savor::db::BattlePlanActionMacro::FakeAttack));
    macroCombo_->addItem(macroLabel(savor::db::BattlePlanActionMacro::UseItem), static_cast<int>(savor::db::BattlePlanActionMacro::UseItem));

    targetKindCombo_ = new QComboBox(panel);
    targetKindCombo_->addItem(targetKindLabel(savor::db::BattlePlanTargetKind::SingleEnemy), static_cast<int>(savor::db::BattlePlanTargetKind::SingleEnemy));
    targetKindCombo_->addItem(targetKindLabel(savor::db::BattlePlanTargetKind::MultipleEnemies), static_cast<int>(savor::db::BattlePlanTargetKind::MultipleEnemies));
    targetKindCombo_->addItem(targetKindLabel(savor::db::BattlePlanTargetKind::AnyEnemy), static_cast<int>(savor::db::BattlePlanTargetKind::AnyEnemy));
    targetKindCombo_->addItem(targetKindLabel(savor::db::BattlePlanTargetKind::SameAsOtherPC), static_cast<int>(savor::db::BattlePlanTargetKind::SameAsOtherPC));

    targetSingleSlotSpin_ = new QSpinBox(panel);
    targetSingleSlotSpin_->setRange(kMinTargetSingleSlot, kMaxTargetSingleSlot);
    targetSingleSlotSpin_->setValue(kMinTargetSingleSlot);
    targetMaskEdit_ = new QLineEdit(QStringLiteral("0"), panel);
    targetMaskEdit_->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-9A-Fa-fxX]+")), targetMaskEdit_));
    targetSameAsActorSpin_ = new QSpinBox(panel);
    targetSameAsActorSpin_->setRange(0, 3);
    itemIdEdit_ = new QLineEdit(panel);
    itemIdEdit_->setPlaceholderText(QStringLiteral("Optional item id"));
    itemIdEdit_->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("-?[0-9]+")), itemIdEdit_));
    noteLabel_ = new QLabel(panel);
    noteLabel_->setWordWrap(true);
    noteLabel_->setObjectName("sectionDescription");

    form->addRow(QStringLiteral("Preset name"), nameEdit_);
    form->addRow(QStringLiteral("Action"), macroCombo_);
    form->addRow(QStringLiteral("Target kind"), targetKindCombo_);
    form->addRow(QStringLiteral("Single enemy slot"), targetSingleSlotSpin_);
    form->addRow(QStringLiteral("Target mask bits"), targetMaskEdit_);
    form->addRow(QStringLiteral("Same as actor slot"), targetSameAsActorSpin_);
    form->addRow(QStringLiteral("Item id"), itemIdEdit_);
    form->addRow(QStringLiteral(""), noteLabel_);
    root->addWidget(panel, 1);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch();
    saveAsNewButton_ = new QPushButton(QStringLiteral("Save As New"), this);
    saveAsNewButton_->setObjectName("jobsSecondaryButton");
    updateCurrentButton_ = new QPushButton(QStringLiteral("Update Current"), this);
    updateCurrentButton_->setObjectName("jobsPrimaryButton");
    buttons->addWidget(saveAsNewButton_);
    buttons->addWidget(updateCurrentButton_);
    root->addLayout(buttons);

    connect(saveAsNewButton_, &QPushButton::clicked, this, &BattlePlanActionPresetEditorWindow::saveAsNew);
    connect(updateCurrentButton_, &QPushButton::clicked, this, &BattlePlanActionPresetEditorWindow::saveCurrent);
    connect(macroCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, &BattlePlanActionPresetEditorWindow::syncControlsForPresetMode);
    connect(targetKindCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, &BattlePlanActionPresetEditorWindow::syncControlsForPresetMode);

    syncControlsForPresetMode();
}

void BattlePlanActionPresetEditorWindow::loadNew()
{
    loadedPresetId_.reset();
    loadedFromSnapshot_ = false;
    loadedSnapshot_ = {};
    nameEdit_->setText(QStringLiteral("New Action Preset"));
    macroCombo_->setCurrentIndex(0);
    targetKindCombo_->setCurrentIndex(static_cast<int>(savor::db::BattlePlanTargetKind::AnyEnemy));
    targetSingleSlotSpin_->setValue(kMinTargetSingleSlot);
    targetMaskEdit_->setText(QStringLiteral("0"));
    targetSameAsActorSpin_->setValue(0);
    itemIdEdit_->clear();
    noteLabel_->setText(QStringLiteral("Create a new preset to be reused across battle plans."));
    syncControlsForPresetMode();
    refreshTitleForMode();
}

void BattlePlanActionPresetEditorWindow::applyLoadedSnapshot(const savor::db::BattlePlanActionPresetSnapshot& snapshot)
{
    nameEdit_->setText(QString::fromStdString(snapshot.name));
    macroCombo_->setCurrentIndex(std::max(0, macroCombo_->findData(static_cast<int>(snapshot.macro))));
    targetKindCombo_->setCurrentIndex(std::max(0, targetKindCombo_->findData(static_cast<int>(snapshot.target_kind))));
    targetSingleSlotSpin_->setValue(snapshot.target_single_slot.value_or(kMinTargetSingleSlot));
    targetMaskEdit_->setText(QString::number(snapshot.target_mask_bits.value_or(0)));
    targetSameAsActorSpin_->setValue(snapshot.target_same_as_actor_slot.value_or(0));
    itemIdEdit_->setText(snapshot.item_id ? QString::number(*snapshot.item_id) : QString());
    noteLabel_->clear();
}

void BattlePlanActionPresetEditorWindow::loadSnapshot(const savor::db::BattlePlanActionPresetSnapshot& snapshot, bool duplicate)
{
    loadedFromSnapshot_ = true;
    loadedSnapshot_ = snapshot;
    loadedPresetId_ = duplicate ? std::nullopt : (snapshot.action_preset_id > 0 ? std::optional<std::int64_t>{ snapshot.action_preset_id } : std::nullopt);
    applyLoadedSnapshot(snapshot);
    syncControlsForPresetMode();
    refreshTitleForMode();
    if (loadedPresetId_.has_value()) {
        syncControlsForPresetMode();
        noteLabel_->setText(QStringLiteral("Edit mode: existing preset loaded. Use Update Current to rename only, or Save As New for any structural change."));
    } else {
        noteLabel_->setText(QStringLiteral("Duplicate mode: no existing ID. Save As New is the only persistence path."));
    }
}

void BattlePlanActionPresetEditorWindow::refreshTitleForMode()
{
    if (!loadedPresetId_.has_value()) {
        if (!loadedFromSnapshot_) {
            setWindowTitle(QStringLiteral("Battle Plan Action Preset Editor - New"));
        } else {
            setWindowTitle(QStringLiteral("Battle Plan Action Preset Editor - Duplicate"));
        }
    } else {
        setWindowTitle(QStringLiteral("Battle Plan Action Preset Editor - Edit"));
    }
}

void BattlePlanActionPresetEditorWindow::syncControlsForPresetMode()
{
    const auto macro = static_cast<savor::db::BattlePlanActionMacro>(macroCombo_->currentData().toInt());
    const bool needsTarget = (macro == savor::db::BattlePlanActionMacro::Attack || macro == savor::db::BattlePlanActionMacro::UseItem);
    const bool needsItem = (macro == savor::db::BattlePlanActionMacro::UseItem);
    const auto targetKind = static_cast<savor::db::BattlePlanTargetKind>(targetKindCombo_->currentData().toInt());
    targetKindCombo_->setEnabled(needsTarget);
    targetSingleSlotSpin_->setEnabled(needsTarget && targetKind == savor::db::BattlePlanTargetKind::SingleEnemy);
    targetMaskEdit_->setEnabled(needsTarget && targetKind == savor::db::BattlePlanTargetKind::MultipleEnemies);
    targetSameAsActorSpin_->setEnabled(needsTarget && targetKind == savor::db::BattlePlanTargetKind::SameAsOtherPC);
    itemIdEdit_->setEnabled(needsItem);
    if (!needsItem) {
        itemIdEdit_->clear();
    }

    if (!needsTarget) {
        targetSingleSlotSpin_->setEnabled(false);
        targetMaskEdit_->setEnabled(false);
        targetSameAsActorSpin_->setEnabled(false);
        noteLabel_->setText(QStringLiteral("No target binding is required for this action."));
    } else {
        noteLabel_->clear();
    }
}

void BattlePlanActionPresetEditorWindow::postStatusMessage(const QString& text, StatusToast::Severity severity)
{
    if (statusCallback_ && !text.isEmpty()) {
        statusCallback_(text, severity);
    }
}

bool BattlePlanActionPresetEditorWindow::buildDraft(savorqt::db::BattlePlanActionPresetDraft& draft, QString* errorText) const
{
    const auto macro = static_cast<savor::db::BattlePlanActionMacro>(macroCombo_->currentData().toInt());
    const auto targetKind = static_cast<savor::db::BattlePlanTargetKind>(targetKindCombo_->currentData().toInt());
    const QString name = nameEdit_->text().trimmed();
    if (name.isEmpty()) {
        if (errorText) *errorText = QStringLiteral("Preset name is required.");
        return false;
    }

    draft = {};
    draft.name = name.toStdString();
    draft.macro = macro;
    draft.target_kind = targetKind;
    if (macro == savor::db::BattlePlanActionMacro::Attack || macro == savor::db::BattlePlanActionMacro::UseItem) {
        switch (targetKind) {
        case savor::db::BattlePlanTargetKind::SingleEnemy:
            draft.target_single_slot = targetSingleSlotSpin_->value();
            draft.target_mask_bits.reset();
            draft.target_same_as_actor_slot.reset();
            break;
        case savor::db::BattlePlanTargetKind::MultipleEnemies:
            {
                bool ok = false;
                const int maskValue = targetMaskEdit_->text().trimmed().toInt(&ok, 0);
                if (!ok) {
                    if (errorText) *errorText = QStringLiteral("Target mask must be numeric.");
                    return false;
                }
                draft.target_mask_bits = maskValue;
                draft.target_single_slot.reset();
                draft.target_same_as_actor_slot.reset();
                if (maskValue == 0) {
                    if (errorText) *errorText = QStringLiteral("At least one bit should be set for multiple enemy target mode.");
                    return false;
                }
                break;
            }
        case savor::db::BattlePlanTargetKind::AnyEnemy:
            draft.target_mask_bits.reset();
            draft.target_single_slot.reset();
            draft.target_same_as_actor_slot.reset();
            break;
        case savor::db::BattlePlanTargetKind::SameAsOtherPC:
            draft.target_same_as_actor_slot = targetSameAsActorSpin_->value();
            draft.target_mask_bits.reset();
            draft.target_single_slot.reset();
            break;
        default:
            break;
        }
    } else {
        draft.target_mask_bits.reset();
        draft.target_single_slot.reset();
        draft.target_same_as_actor_slot.reset();
    }

    const QString itemText = itemIdEdit_->text().trimmed();
    if (macro == savor::db::BattlePlanActionMacro::UseItem) {
        if (itemText.isEmpty()) {
            if (errorText) *errorText = QStringLiteral("Use Item requires an item id.");
            return false;
        }
        bool ok = false;
        const int value = itemText.toInt(&ok, 0);
        if (!ok) {
            if (errorText) *errorText = QStringLiteral("Item id must be numeric.");
            return false;
        }
        if (value < 0) {
            if (errorText) *errorText = QStringLiteral("Item id must be non-negative.");
            return false;
        }
        draft.item_id = value;
    } else if (!itemText.isEmpty()) {
        bool ok = false;
        itemText.toInt(&ok, 0);
        if (ok) {
            // ignore stale typed value when macro does not need an item
        }
        draft.item_id.reset();
    } else {
        draft.item_id.reset();
    }

    if (errorText) errorText->clear();
    return true;
}

bool BattlePlanActionPresetEditorWindow::canSaveCurrentInPlace(const savorqt::db::BattlePlanActionPresetDraft& draft) const
{
    if (!loadedPresetId_.has_value() || !loadedFromSnapshot_) {
        return false;
    }

    return draft.macro == loadedSnapshot_.macro
        && draft.target_kind == loadedSnapshot_.target_kind
        && draft.target_mask_bits == loadedSnapshot_.target_mask_bits
        && draft.target_single_slot == loadedSnapshot_.target_single_slot
        && draft.target_same_as_actor_slot == loadedSnapshot_.target_same_as_actor_slot
        && draft.item_id == loadedSnapshot_.item_id
        && draft.flags == loadedSnapshot_.flags;
}

void BattlePlanActionPresetEditorWindow::saveAsNew()
{
    savorqt::db::BattlePlanActionPresetDraft draft{};
    QString error;
    if (!buildDraft(draft, &error)) {
        postStatusMessage(error, StatusToast::Severity::Warn);
        return;
    }
    const auto result = savorqt::db::SavorDbAuthoringService::SaveBattlePlanActionPreset(draft);
    if (!result.ok) {
        postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
        return;
    }
    if (savedCallback_) {
        savedCallback_();
    }
    postStatusMessage(QStringLiteral("Saved action preset %1.").arg(static_cast<qint64>(result.value)), StatusToast::Severity::Info);
}

void BattlePlanActionPresetEditorWindow::saveCurrent()
{
    if (!loadedPresetId_.has_value()) {
        saveAsNew();
        return;
    }
    savorqt::db::BattlePlanActionPresetDraft draft{};
    QString error;
    if (!buildDraft(draft, &error)) {
        postStatusMessage(error, StatusToast::Severity::Warn);
        return;
    }

    if (!canSaveCurrentInPlace(draft)) {
        postStatusMessage(QStringLiteral("Update Current only supports renaming. Use Save As New for any non-name field edits."), StatusToast::Severity::Warn);
        return;
    }
    const auto result = savorqt::db::SavorDbAuthoringService::RenameBattlePlanActionPreset(*loadedPresetId_, draft.name);
    if (!result.ok) {
        postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
        return;
    }
    if (savedCallback_) {
        savedCallback_();
    }
    loadedSnapshot_.name = draft.name;
    postStatusMessage(QStringLiteral("Renamed action preset %1.").arg(static_cast<qint64>(*loadedPresetId_)), StatusToast::Severity::Info);
}
