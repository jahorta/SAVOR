#include "PresetEditorDialog.h"

#include "Phases/BattleExplorer.h"
#include "Utils/IniDoc.h"
#include "Core/Input/SoaBattle/ActionTypes.h"
#include "Core/Memory/Soa/SoaConstants.h"

#include <QtCore/QSet>
#include <QtCore/QStringList>
#include <QtCore/QSignalBlocker>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QListWidgetItem>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

using simcore::battleexplorer::TargetBindingKind;
using simcore::db::TurnActionPresetRow;
using soa::battle::actions::BattleAction;

namespace {
constexpr int kByEnemyKindData = 1000;
constexpr int kItemRole = Qt::UserRole;
constexpr int kEnemyKindRole = Qt::UserRole;

QString itemName(const int itemId)
{
    if (itemId < 0 || static_cast<std::size_t>(itemId) >= soa::text::ItemNames.size()) {
        return QStringLiteral("(none)");
    }
    return QString::fromUtf8(soa::text::ItemNames.at(static_cast<std::size_t>(itemId)).data());
}

QString enemyName(const int enemyKindId)
{
    if (enemyKindId < 0 || static_cast<std::size_t>(enemyKindId) >= soa::text::EnemyNames.size()) {
        return QString::number(enemyKindId);
    }
    return QStringLiteral("%1 · %2").arg(enemyKindId).arg(QString::fromUtf8(soa::text::EnemyNames.at(static_cast<std::size_t>(enemyKindId)).data()));
}

const char* itemCategoryLabel(const PresetEditorDialog::ItemCategory category)
{
    switch (category) {
    case PresetEditorDialog::ItemCategory::Weapon: return "Weapons";
    case PresetEditorDialog::ItemCategory::Armor: return "Armor";
    case PresetEditorDialog::ItemCategory::Accessory: return "Accessories";
    case PresetEditorDialog::ItemCategory::Consumable: return "Consumables";
    case PresetEditorDialog::ItemCategory::Special: return "Special";
    case PresetEditorDialog::ItemCategory::ShipWeapon: return "Ship Weapons";
    case PresetEditorDialog::ItemCategory::ShipAccessory: return "Ship Accessories";
    case PresetEditorDialog::ItemCategory::ShipConsumable: return "Ship Consumables";
    case PresetEditorDialog::ItemCategory::All: return "All";
    }
    return "All";
}

PresetEditorDialog::ItemCategory classifyItem(const int itemId)
{
    if (itemId < 80) return PresetEditorDialog::ItemCategory::Weapon;
    if (itemId < 160) return PresetEditorDialog::ItemCategory::Armor;
    if (itemId < 240) return PresetEditorDialog::ItemCategory::Accessory;
    if (itemId < 320) return PresetEditorDialog::ItemCategory::Consumable;
    if (itemId < 400) return PresetEditorDialog::ItemCategory::Special;
    if (itemId < 440) return PresetEditorDialog::ItemCategory::ShipWeapon;
    if (itemId < 480) return PresetEditorDialog::ItemCategory::ShipAccessory;
    return PresetEditorDialog::ItemCategory::ShipConsumable;
}

QVector<int> listByCategory(const PresetEditorDialog::ItemCategory category)
{
    QVector<int> out;
    auto pushRange = [&out](const int start, const int stop) {
        for (int id = start; id < stop; ++id) {
            out.push_back(id);
        }
    };

    switch (category) {
    case PresetEditorDialog::ItemCategory::Weapon:
        pushRange(0, 74);
        break;
    case PresetEditorDialog::ItemCategory::Armor:
        pushRange(80, 139);
        break;
    case PresetEditorDialog::ItemCategory::Accessory:
        pushRange(160, 223);
        break;
    case PresetEditorDialog::ItemCategory::Consumable:
        pushRange(240, 320);
        break;
    case PresetEditorDialog::ItemCategory::Special:
        pushRange(320, 365);
        break;
    case PresetEditorDialog::ItemCategory::ShipWeapon:
        pushRange(400, 440);
        break;
    case PresetEditorDialog::ItemCategory::ShipAccessory:
        pushRange(440, 480);
        break;
    case PresetEditorDialog::ItemCategory::ShipConsumable:
        pushRange(480, 510);
        break;
    case PresetEditorDialog::ItemCategory::All:
        for (int value = static_cast<int>(PresetEditorDialog::ItemCategory::Weapon);
             value <= static_cast<int>(PresetEditorDialog::ItemCategory::ShipConsumable);
             ++value) {
            out += listByCategory(static_cast<PresetEditorDialog::ItemCategory>(value));
        }
        break;
    }
    return out;
}
}

PresetEditorDialog::PresetEditorDialog(const soa::battle::ctx::BattleContext* context, QWidget* parent)
    : QDialog(parent)
    , context_(context)
{
    setWindowTitle(QStringLiteral("UI Action Preset"));
    resize(760, 640);

    rebuildContextCaches();

    QVBoxLayout* root = new QVBoxLayout(this);
    QFormLayout* form = new QFormLayout();

    nameEdit_ = new QLineEdit(this);
    form->addRow(QStringLiteral("Name"), nameEdit_);

    actionCombo_ = new QComboBox(this);
    actionCombo_->addItem(QStringLiteral("Attack"), static_cast<int>(BattleAction::Attack));
    actionCombo_->addItem(QStringLiteral("Defend"), static_cast<int>(BattleAction::Defend));
    actionCombo_->addItem(QStringLiteral("Focus"), static_cast<int>(BattleAction::Focus));
    actionCombo_->addItem(QStringLiteral("Use Item"), static_cast<int>(BattleAction::UseItem));
    form->addRow(QStringLiteral("Action"), actionCombo_);

    targetCombo_ = new QComboBox(this);
    targetCombo_->addItem(QStringLiteral("Single Enemy"), static_cast<int>(TargetBindingKind::SingleEnemy));
    targetCombo_->addItem(QStringLiteral("Multiple Enemies"), static_cast<int>(TargetBindingKind::MultipleEnemies));
    targetCombo_->addItem(QStringLiteral("Any Enemy"), static_cast<int>(TargetBindingKind::AnyEnemy));
    targetCombo_->addItem(QStringLiteral("Same As Other PC"), static_cast<int>(TargetBindingKind::SameAsOtherPC));
    targetCombo_->addItem(QStringLiteral("By Enemy Kind"), kByEnemyKindData);
    form->addRow(QStringLiteral("Target"), targetCombo_);

    root->addLayout(form);

    QGroupBox* targetBox = new QGroupBox(QStringLiteral("Target Details"), this);
    QVBoxLayout* targetLayout = new QVBoxLayout(targetBox);

    QWidget* singleWidget = new QWidget(targetBox);
    QHBoxLayout* singleLayout = new QHBoxLayout(singleWidget);
    singleLayout->setContentsMargins(0, 0, 0, 0);
    singleLayout->addWidget(new QLabel(QStringLiteral("Single enemy slot:"), singleWidget));
    for (int slot = 4; slot <= 11; ++slot) {
        QRadioButton* button = new QRadioButton(QString::number(slot), singleWidget);
        singleSlotButtons_.push_back(button);
        singleLayout->addWidget(button);
    }
    targetLayout->addWidget(singleWidget);

    QWidget* multiWidget = new QWidget(targetBox);
    QHBoxLayout* multiLayout = new QHBoxLayout(multiWidget);
    multiLayout->setContentsMargins(0, 0, 0, 0);
    multiLayout->addWidget(new QLabel(QStringLiteral("Multi-target slots:"), multiWidget));
    for (int slot = 4; slot <= 11; ++slot) {
        QCheckBox* checkbox = new QCheckBox(QString::number(slot), multiWidget);
        multiTargetChecks_.push_back(checkbox);
        multiLayout->addWidget(checkbox);
    }
    targetLayout->addWidget(multiWidget);

    samePcCombo_ = new QComboBox(targetBox);
    samePcCombo_->addItem(QStringLiteral("Vyse"), 0);
    samePcCombo_->addItem(QStringLiteral("Aika"), 1);
    samePcCombo_->addItem(QStringLiteral("Fina"), 2);
    samePcCombo_->addItem(QStringLiteral("Drachma"), 3);
    targetLayout->addWidget(new QLabel(QStringLiteral("Same-as PC"), targetBox));
    targetLayout->addWidget(samePcCombo_);

    showAllKindsCheck_ = new QCheckBox(QStringLiteral("Show all enemy kinds"), targetBox);
    enemyKindCombo_ = new QComboBox(targetBox);
    quantifierCombo_ = new QComboBox(targetBox);
    quantifierCombo_->addItems(QStringList{ QStringLiteral("All"), QStringLiteral("Any"), QStringLiteral("First") });
    targetLayout->addWidget(showAllKindsCheck_);
    targetLayout->addWidget(new QLabel(QStringLiteral("Enemy kind"), targetBox));
    targetLayout->addWidget(enemyKindCombo_);
    targetLayout->addWidget(new QLabel(QStringLiteral("Quantifier"), targetBox));
    targetLayout->addWidget(quantifierCombo_);

    targetPreviewLabel_ = new QLabel(targetBox);
    targetPreviewLabel_->setWordWrap(true);
    targetLayout->addWidget(targetPreviewLabel_);
    root->addWidget(targetBox);

    QGroupBox* itemBox = new QGroupBox(QStringLiteral("Item Details"), this);
    QVBoxLayout* itemLayout = new QVBoxLayout(itemBox);
    itemCategoryCombo_ = new QComboBox(itemBox);
    for (int value = static_cast<int>(ItemCategory::Weapon); value <= static_cast<int>(ItemCategory::All); ++value) {
        itemCategoryCombo_->addItem(QString::fromUtf8(itemCategoryLabel(static_cast<ItemCategory>(value))), value);
    }
    showAllItemsCheck_ = new QCheckBox(QStringLiteral("Show all items"), itemBox);
    itemList_ = new QListWidget(itemBox);
    itemList_->setSelectionMode(QAbstractItemView::SingleSelection);
    itemPreviewLabel_ = new QLabel(itemBox);
    itemPreviewLabel_->setWordWrap(true);
    itemLayout->addWidget(new QLabel(QStringLiteral("Category"), itemBox));
    itemLayout->addWidget(itemCategoryCombo_);
    itemLayout->addWidget(showAllItemsCheck_);
    itemLayout->addWidget(itemList_, 1);
    itemLayout->addWidget(itemPreviewLabel_);
    root->addWidget(itemBox, 1);

    contextLabel_ = new QLabel(this);
    contextLabel_->setWordWrap(true);
    root->addWidget(contextLabel_);

    errorLabel_ = new QLabel(this);
    errorLabel_->setWordWrap(true);
    errorLabel_->setStyleSheet(QStringLiteral("color: #d9534f;"));
    errorLabel_->hide();
    root->addWidget(errorLabel_);

    buttonBox_ = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    saveAsNewButton_ = buttonBox_->addButton(QStringLiteral("Save As New Preset"), QDialogButtonBox::AcceptRole);
    updateCurrentButton_ = buttonBox_->addButton(QStringLiteral("Update Current Preset"), QDialogButtonBox::AcceptRole);
    root->addWidget(buttonBox_);

    connect(buttonBox_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(saveAsNewButton_, &QPushButton::clicked, this, [this]() {
        saveAsNewRequested_ = true;
        accept();
    });
    connect(updateCurrentButton_, &QPushButton::clicked, this, [this]() {
        saveAsNewRequested_ = false;
        accept();
    });
    connect(actionCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() { refreshUi(); });
    connect(targetCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() { refreshUi(); });
    connect(itemCategoryCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() { rebuildItemList(); refreshUi(); });
    connect(itemList_, &QListWidget::currentRowChanged, this, [this](int) { refreshUi(); });
    connect(showAllItemsCheck_, &QCheckBox::toggled, this, [this](bool) { rebuildItemList(); refreshUi(); });
    connect(showAllKindsCheck_, &QCheckBox::toggled, this, [this](bool) { rebuildEnemyKindList(); refreshUi(); });
    connect(enemyKindCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() { refreshUi(); });
    connect(samePcCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() { refreshUi(); });
    connect(quantifierCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() { refreshUi(); });
    for (QRadioButton* button : singleSlotButtons_) {
        connect(button, &QRadioButton::toggled, this, [this](bool) { refreshUi(); });
    }
    for (QCheckBox* checkbox : multiTargetChecks_) {
        connect(checkbox, &QCheckBox::toggled, this, [this](bool) { refreshUi(); });
    }

    if (!singleSlotButtons_.isEmpty()) {
        singleSlotButtons_.front()->setChecked(true);
    }
    itemCategoryCombo_->setCurrentIndex(itemCategoryCombo_->findData(static_cast<int>(ItemCategory::Consumable)));
    rebuildEnemyKindList();
    rebuildItemList();
    refreshUi();
}

void PresetEditorDialog::rebuildContextCaches()
{
    presentEnemyKinds_.clear();
    presentItems_.clear();
    if (!context_) {
        return;
    }

    QSet<int> seenEnemyKinds;
    for (int slot = 4; slot <= 11; ++slot) {
        if (context_->slots_[slot].present == 1) {
            const int enemyKindId = static_cast<int>(context_->slots_[slot].id);
            if (!seenEnemyKinds.contains(enemyKindId)) {
                seenEnemyKinds.insert(enemyKindId);
                presentEnemyKinds_.push_back(enemyKindId);
            }
        }
    }

    QSet<int> seenItems;
    for (int index = 0; index < 80; ++index) {
        const int itemId = static_cast<int>(context_->state.useable_items[index].item_id);
        if (itemId == 0xFFFF || seenItems.contains(itemId)) {
            continue;
        }
        seenItems.insert(itemId);
        presentItems_.push_back(itemId);
    }
}

void PresetEditorDialog::rebuildEnemyKindList()
{
    const int currentId = enemyKindCombo_->currentData(kEnemyKindRole).toInt();
    enemyKindCombo_->clear();

    QVector<int> kinds = presentEnemyKinds_;
    if (!context_ || showAllKindsCheck_->isChecked()) {
        kinds.clear();
        kinds.reserve(static_cast<int>(soa::text::EnemyNames.size()));
        for (int index = 0; index < static_cast<int>(soa::text::EnemyNames.size()); ++index) {
            kinds.push_back(index);
        }
    }

    for (const int kindId : kinds) {
        enemyKindCombo_->addItem(enemyName(kindId), kindId);
    }

    int restoreIndex = enemyKindCombo_->findData(currentId);
    if (restoreIndex < 0 && enemyKindCombo_->count() > 0) {
        restoreIndex = 0;
    }
    enemyKindCombo_->setCurrentIndex(restoreIndex);
}

void PresetEditorDialog::rebuildItemList()
{
    const int currentItemId = itemList_->currentItem() ? itemList_->currentItem()->data(kItemRole).toInt() : row_.item_id;
    itemList_->clear();

    const ItemCategory category = static_cast<ItemCategory>(itemCategoryCombo_->currentData().toInt());
    QVector<int> ids = listByCategory(category);
    if (context_ && !showAllItemsCheck_->isChecked()) {
        QVector<int> filtered;
        for (const int itemId : presentItems_) {
            if (category == ItemCategory::All || classifyItem(itemId) == category) {
                filtered.push_back(itemId);
            }
        }
        ids = filtered;
    }

    for (const int itemId : ids) {
        QListWidgetItem* item = new QListWidgetItem(QStringLiteral("%1 · %2").arg(itemId).arg(itemName(itemId)), itemList_);
        item->setData(kItemRole, itemId);
    }

    for (int row = 0; row < itemList_->count(); ++row) {
        if (itemList_->item(row)->data(kItemRole).toInt() == currentItemId) {
            itemList_->setCurrentRow(row);
            return;
        }
    }
    if (itemList_->count() > 0) {
        itemList_->setCurrentRow(0);
    }
}

void PresetEditorDialog::loadRow(const TurnActionPresetRow& row)
{
    row_ = row;
    nameEdit_->setText(QString::fromStdString(row.name));
    actionCombo_->setCurrentIndex(std::max(0, actionCombo_->findData(row.macro)));

    if (row.item_id >= 0) {
        itemCategoryCombo_->setCurrentIndex(itemCategoryCombo_->findData(static_cast<int>(classifyItem(row.item_id))));
    }

    if (!row.target_expr_ini.empty()) {
        const IniDoc ini = IniDoc::parse(row.target_expr_ini);
        if (ini.get("target", "kind", "") == "ByEnemyKind") {
            targetCombo_->setCurrentIndex(targetCombo_->findData(kByEnemyKindData));
            const int enemyKindId = static_cast<int>(ini.get_i64("target", "enemy_kind_id", 0));
            showAllKindsCheck_->setChecked(!context_ || !presentEnemyKinds_.contains(enemyKindId));
            rebuildEnemyKindList();
            enemyKindCombo_->setCurrentIndex(std::max(0, enemyKindCombo_->findData(enemyKindId)));
            const QString quantifier = QString::fromStdString(ini.get("target", "quantifier", "Any"));
            quantifierCombo_->setCurrentIndex(std::max(0, quantifierCombo_->findText(quantifier)));
        }
    } else {
        targetCombo_->setCurrentIndex(std::max(0, targetCombo_->findData(row.target_kind)));
        const int singleIndex = std::clamp(row.single_slot, 4, 11) - 4;
        if (singleIndex >= 0 && singleIndex < singleSlotButtons_.size()) {
            singleSlotButtons_.at(singleIndex)->setChecked(true);
        }
        for (int slot = 4; slot <= 11; ++slot) {
            const int index = slot - 4;
            multiTargetChecks_.at(index)->setChecked((row.mask_bits & (1u << slot)) != 0);
        }
        samePcCombo_->setCurrentIndex(std::max(0, samePcCombo_->findData(std::clamp(row.same_as_pc, 0, 3))));
    }

    showAllItemsCheck_->setChecked(!context_ || !presentItems_.contains(row.item_id));
    rebuildItemList();
    for (int itemRow = 0; itemRow < itemList_->count(); ++itemRow) {
        if (itemList_->item(itemRow)->data(kItemRole).toInt() == row.item_id) {
            itemList_->setCurrentRow(itemRow);
            break;
        }
    }

    saveAsNewRequested_ = (row.id <= 0);
    refreshUi();
}

TurnActionPresetRow PresetEditorDialog::buildRow(bool* ok, QString* errorText) const
{
    TurnActionPresetRow row = row_;
    if (saveAsNewRequested_) {
        row.id = 0;
    }
    row.name = nameEdit_->text().trimmed().toStdString();
    row.macro = actionCombo_->currentData().toInt();
    row.target_expr_ini.clear();
    row.target_kind = static_cast<int>(TargetBindingKind::SingleEnemy);
    row.mask_bits = 0;
    row.single_slot = 4;
    row.same_as_pc = 0;

    if (row.name.empty()) {
        if (ok) *ok = false;
        if (errorText) *errorText = QStringLiteral("Preset name is required.");
        return row;
    }

    const BattleAction action = static_cast<BattleAction>(row.macro);
    const bool needsTarget = action == BattleAction::Attack || action == BattleAction::UseItem;
    const bool needsItem = action == BattleAction::UseItem;
    const bool checkAgainstContext = context_ != nullptr;

    if (needsItem) {
        QListWidgetItem* selectedItem = itemList_->currentItem();
        if (!selectedItem) {
            if (ok) *ok = false;
            if (errorText) *errorText = QStringLiteral("Choose an item for a Use Item preset.");
            return row;
        }
        row.item_id = selectedItem->data(kItemRole).toInt();
        if (checkAgainstContext && !showAllItemsCheck_->isChecked() && !presentItems_.contains(row.item_id)) {
            if (ok) *ok = false;
            if (errorText) *errorText = QStringLiteral("Selected item is not usable in the current BattleContext.");
            return row;
        }
    } else {
        row.item_id = -1;
    }

    if (needsTarget) {
        const int targetData = targetCombo_->currentData().toInt();
        if (targetData == kByEnemyKindData) {
            if (enemyKindCombo_->currentIndex() < 0) {
                if (ok) *ok = false;
                if (errorText) *errorText = QStringLiteral("Choose an enemy kind.");
                return row;
            }
            const int enemyKindId = enemyKindCombo_->currentData(kEnemyKindRole).toInt();
            if (checkAgainstContext && !showAllKindsCheck_->isChecked() && !presentEnemyKinds_.contains(enemyKindId)) {
                if (ok) *ok = false;
                if (errorText) *errorText = QStringLiteral("Selected enemy kind is not present in the current BattleContext.");
                return row;
            }
            IniDoc ini;
            ini.ensure_section("target");
            ini.set("target", "kind", "ByEnemyKind");
            ini.set("target", "enemy_kind_id", std::to_string(enemyKindId));
            ini.set("target", "quantifier", quantifierCombo_->currentText().toStdString());
            row.target_expr_ini = ini.to_string_sorted();
            row.target_kind = static_cast<int>(TargetBindingKind::AnyEnemy);
            row.mask_bits = 0;
            row.single_slot = 0xFF;
            row.same_as_pc = 0xFF;
        } else {
            row.target_kind = targetData;
            switch (static_cast<TargetBindingKind>(targetData)) {
            case TargetBindingKind::SingleEnemy: {
                int chosenSlot = -1;
                for (int slot = 4; slot <= 11; ++slot) {
                    if (singleSlotButtons_.at(slot - 4)->isChecked()) {
                        chosenSlot = slot;
                        break;
                    }
                }
                if (chosenSlot < 0) {
                    if (ok) *ok = false;
                    if (errorText) *errorText = QStringLiteral("Choose a single enemy slot.");
                    return row;
                }
                if (checkAgainstContext && context_->slots_[chosenSlot].present != 1) {
                    if (ok) *ok = false;
                    if (errorText) *errorText = QStringLiteral("Selected enemy slot is not present in the current BattleContext.");
                    return row;
                }
                row.single_slot = chosenSlot;
                row.mask_bits = (1u << chosenSlot);
                row.same_as_pc = 0xFF;
                break;
            }
            case TargetBindingKind::MultipleEnemies: {
                quint32 mask = 0;
                for (int slot = 4; slot <= 11; ++slot) {
                    if (multiTargetChecks_.at(slot - 4)->isChecked()) {
                        mask |= (1u << slot);
                    }
                }
                if (mask == 0) {
                    if (ok) *ok = false;
                    if (errorText) *errorText = QStringLiteral("Select at least one enemy slot for a multi-target preset.");
                    return row;
                }
                if (checkAgainstContext) {
                    quint32 presentMask = 0;
                    for (int slot = 4; slot <= 11; ++slot) {
                        if (context_->slots_[slot].present == 1) {
                            presentMask |= (1u << slot);
                        }
                    }
                    if ((mask & presentMask) == 0) {
                        if (ok) *ok = false;
                        if (errorText) *errorText = QStringLiteral("Selected enemy slots are not present in the current BattleContext.");
                        return row;
                    }
                }
                row.mask_bits = mask;
                row.single_slot = 0xFF;
                row.same_as_pc = 0xFF;
                break;
            }
            case TargetBindingKind::AnyEnemy:
                row.mask_bits = 0;
                row.single_slot = 0xFF;
                row.same_as_pc = 0xFF;
                break;
            case TargetBindingKind::SameAsOtherPC: {
                const int pcSlot = samePcCombo_->currentData().toInt();
                if (checkAgainstContext && context_->slots_[pcSlot].present != 1) {
                    if (ok) *ok = false;
                    if (errorText) *errorText = QStringLiteral("Selected party member is not present in the current BattleContext.");
                    return row;
                }
                row.same_as_pc = pcSlot;
                row.mask_bits = 0;
                row.single_slot = 0xFF;
                break;
            }
            }
        }
    }

    if (ok) *ok = true;
    if (errorText) errorText->clear();
    return row;
}

bool PresetEditorDialog::saveAsNewRequested() const
{
    return saveAsNewRequested_;
}

void PresetEditorDialog::accept()
{
    bool ok = false;
    QString errorText;
    buildRow(&ok, &errorText);
    if (!ok) {
        setDialogError(errorText);
        return;
    }
    setDialogError(QString());
    QDialog::accept();
}

void PresetEditorDialog::setDialogError(const QString& text)
{
    errorLabel_->setText(text);
    errorLabel_->setVisible(!text.isEmpty());
}

void PresetEditorDialog::refreshUi()
{
    const BattleAction action = static_cast<BattleAction>(actionCombo_->currentData().toInt());
    const int targetData = targetCombo_->currentData().toInt();
    const bool needsTarget = action == BattleAction::Attack || action == BattleAction::UseItem;
    const bool needsItem = action == BattleAction::UseItem;

    const bool hasAnyUsableItems = !presentItems_.isEmpty();
    const bool itemContextMode = context_ != nullptr;

    for (int slot = 4; slot <= 11; ++slot) {
        const bool present = !context_ || context_->slots_[slot].present == 1;
        singleSlotButtons_.at(slot - 4)->setEnabled(!needsTarget || present);
        multiTargetChecks_.at(slot - 4)->setEnabled(!needsTarget || present);
        if (!present && multiTargetChecks_.at(slot - 4)->isChecked()) {
            multiTargetChecks_.at(slot - 4)->setChecked(false);
        }
    }
    for (int slot = 0; slot <= 3; ++slot) {
        const bool present = !context_ || context_->slots_[slot].present == 1;
        samePcCombo_->setItemData(slot, present ? QVariant() : QVariant(0), Qt::UserRole - 1);
    }

    const bool enableUseItem = !itemContextMode || hasAnyUsableItems;
    const int useItemIndex = actionCombo_->findData(static_cast<int>(BattleAction::UseItem));
    actionCombo_->setItemData(useItemIndex, enableUseItem ? QVariant() : QVariant(0), Qt::UserRole - 1);

    targetCombo_->setEnabled(needsTarget);
    if (!needsTarget) {
        targetPreviewLabel_->setText(QStringLiteral("No target binding needed for this action."));
    }

    const bool isSingle = needsTarget && targetData == static_cast<int>(TargetBindingKind::SingleEnemy);
    const bool isMulti = needsTarget && targetData == static_cast<int>(TargetBindingKind::MultipleEnemies);
    const bool isSamePc = needsTarget && targetData == static_cast<int>(TargetBindingKind::SameAsOtherPC);
    const bool isByKind = needsTarget && targetData == kByEnemyKindData;

    for (QRadioButton* button : singleSlotButtons_) button->parentWidget()->setVisible(isSingle);
    for (QCheckBox* checkbox : multiTargetChecks_) checkbox->parentWidget()->setVisible(isMulti);
    samePcCombo_->setVisible(isSamePc);
    showAllKindsCheck_->setVisible(isByKind && context_);
    enemyKindCombo_->setVisible(isByKind);
    quantifierCombo_->setVisible(isByKind);

    const bool showItemSection = needsItem;
    itemCategoryCombo_->parentWidget()->setVisible(showItemSection);
    showAllItemsCheck_->setVisible(showItemSection && context_);
    itemList_->setVisible(showItemSection);
    itemPreviewLabel_->setVisible(showItemSection);

    QListWidgetItem* currentItem = itemList_->currentItem();
    const int selectedItemId = currentItem ? currentItem->data(kItemRole).toInt() : -1;
    itemPreviewLabel_->setText(showItemSection
        ? QStringLiteral("Selected item: %1%2")
            .arg(selectedItemId >= 0 ? QStringLiteral("%1 · %2").arg(selectedItemId).arg(itemName(selectedItemId)) : QStringLiteral("(none)"))
            .arg((context_ && !showAllItemsCheck_->isChecked() && selectedItemId >= 0 && !presentItems_.contains(selectedItemId))
                ? QStringLiteral("\nNot currently usable in the loaded BattleContext.")
                : QString())
        : QString());

    QString targetPreview;
    if (isSingle) {
        int selectedSlot = -1;
        for (int slot = 4; slot <= 11; ++slot) {
            if (singleSlotButtons_.at(slot - 4)->isChecked()) {
                selectedSlot = slot;
                break;
            }
        }
        targetPreview = selectedSlot >= 0 ? QStringLiteral("Single target slot %1.").arg(selectedSlot) : QStringLiteral("Choose a target slot.");
    } else if (isMulti) {
        QStringList slots;
        for (int slot = 4; slot <= 11; ++slot) {
            if (multiTargetChecks_.at(slot - 4)->isChecked()) {
                slots << QString::number(slot);
            }
        }
        targetPreview = slots.isEmpty() ? QStringLiteral("Choose one or more target slots.") : QStringLiteral("Selected slots: %1").arg(slots.join(QStringLiteral(", ")));
    } else if (isSamePc) {
        targetPreview = QStringLiteral("Target follows %1.").arg(samePcCombo_->currentText());
    } else if (isByKind) {
        targetPreview = enemyKindCombo_->currentIndex() >= 0
            ? QStringLiteral("Enemy kind: %1 · %2").arg(enemyKindCombo_->currentText()).arg(quantifierCombo_->currentText())
            : QStringLiteral("Choose an enemy kind.");
    } else if (needsTarget) {
        targetPreview = QStringLiteral("Any enemy target.");
    }
    targetPreviewLabel_->setText(targetPreview);

    if (!context_) {
        contextLabel_->setText(QStringLiteral("No BattleContext loaded. All targets/items remain editable."));
    } else {
        contextLabel_->setText(QStringLiteral("BattleContext loaded: %1 enemy kinds, %2 usable items. Context-constrained options are shown by default.")
            .arg(presentEnemyKinds_.size())
            .arg(presentItems_.size()));
    }

    saveAsNewButton_->setVisible(true);
    updateCurrentButton_->setVisible(row_.id > 0);
    updateCurrentButton_->setEnabled(row_.id > 0);
    if (row_.id <= 0) {
        saveAsNewButton_->setText(QStringLiteral("Save"));
    } else {
        saveAsNewButton_->setText(QStringLiteral("Save As New Preset"));
    }
}
