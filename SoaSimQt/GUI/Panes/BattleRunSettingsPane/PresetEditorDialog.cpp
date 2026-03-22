#include "PresetEditorDialog.h"

#include "Phases/BattleExplorer.h"
#include "Utils/IniDoc.h"
#include "Core/Input/SoaBattle/ActionTypes.h"
#include "Core/Memory/Soa/SoaConstants.h"

#include <QtCore/QStringList>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

using simcore::battleexplorer::TargetBindingKind;
using simcore::db::TurnActionPresetRow;
using soa::battle::actions::BattleAction;

namespace {
QString itemName(const int itemId)
{
    if (itemId < 0 || static_cast<std::size_t>(itemId) >= soa::text::ItemNames.size()) {
        return QStringLiteral("(none)");
    }
    return QString::fromUtf8(soa::text::ItemNames.at(static_cast<std::size_t>(itemId)).data());
}
}

PresetEditorDialog::PresetEditorDialog(const soa::battle::ctx::BattleContext* context, QWidget* parent)
    : QDialog(parent)
    , context_(context)
{
    setWindowTitle(QStringLiteral("UI Action Preset"));
    resize(560, 420);

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
    targetCombo_->addItem(QStringLiteral("Multiple Enemies (mask)"), static_cast<int>(TargetBindingKind::MultipleEnemies));
    targetCombo_->addItem(QStringLiteral("Any Enemy"), static_cast<int>(TargetBindingKind::AnyEnemy));
    targetCombo_->addItem(QStringLiteral("Same As Other PC"), static_cast<int>(TargetBindingKind::SameAsOtherPC));
    targetCombo_->addItem(QStringLiteral("By Enemy Kind"), 1000);
    form->addRow(QStringLiteral("Target"), targetCombo_);

    singleSlotSpin_ = new QSpinBox(this);
    singleSlotSpin_->setRange(4, 11);
    form->addRow(QStringLiteral("Single enemy slot"), singleSlotSpin_);

    maskBitsEdit_ = new QLineEdit(this);
    maskBitsEdit_->setPlaceholderText(QStringLiteral("Hex mask, example: 0x30"));
    form->addRow(QStringLiteral("Multi-target mask"), maskBitsEdit_);

    samePcSpin_ = new QSpinBox(this);
    samePcSpin_->setRange(0, 3);
    form->addRow(QStringLiteral("Same-as PC slot"), samePcSpin_);

    enemyKindSpin_ = new QSpinBox(this);
    enemyKindSpin_->setRange(0, 65535);
    form->addRow(QStringLiteral("Enemy kind ID"), enemyKindSpin_);

    quantifierCombo_ = new QComboBox(this);
    quantifierCombo_->addItems(QStringList{ QStringLiteral("All"), QStringLiteral("Any"), QStringLiteral("First") });
    form->addRow(QStringLiteral("Enemy kind quantifier"), quantifierCombo_);

    itemSpin_ = new QSpinBox(this);
    itemSpin_->setRange(0, static_cast<int>(soa::text::ItemNames.size()) - 1);
    form->addRow(QStringLiteral("Item ID"), itemSpin_);

    itemPreviewLabel_ = new QLabel(this);
    itemPreviewLabel_->setWordWrap(true);
    form->addRow(QStringLiteral("Item"), itemPreviewLabel_);

    root->addLayout(form);

    contextLabel_ = new QLabel(this);
    contextLabel_->setWordWrap(true);
    root->addWidget(contextLabel_);

    errorLabel_ = new QLabel(this);
    errorLabel_->setWordWrap(true);
    root->addWidget(errorLabel_);

    buttonBox_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttonBox_->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Save"));
    root->addWidget(buttonBox_);

    connect(buttonBox_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(actionCombo_, &QComboBox::currentIndexChanged, this, [this]() { refreshUi(); });
    connect(targetCombo_, &QComboBox::currentIndexChanged, this, [this]() { refreshUi(); });
    connect(itemSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) { refreshUi(); });

    refreshUi();
}

void PresetEditorDialog::loadRow(const TurnActionPresetRow& row)
{
    row_ = row;
    nameEdit_->setText(QString::fromStdString(row.name));
    actionCombo_->setCurrentIndex(std::max(0, actionCombo_->findData(row.macro)));
    itemSpin_->setValue(std::max(0, row.item_id));

    if (!row.target_expr_ini.empty()) {
        const IniDoc ini = IniDoc::parse(row.target_expr_ini);
        if (ini.get("target", "kind", "") == "ByEnemyKind") {
            targetCombo_->setCurrentIndex(targetCombo_->findData(1000));
            enemyKindSpin_->setValue(static_cast<int>(ini.get_i64("target", "enemy_kind_id", 0)));
            const QString quantifier = QString::fromStdString(ini.get("target", "quantifier", "Any"));
            quantifierCombo_->setCurrentIndex(std::max(0, quantifierCombo_->findText(quantifier)));
        }
    } else {
        targetCombo_->setCurrentIndex(std::max(0, targetCombo_->findData(row.target_kind)));
        singleSlotSpin_->setValue(std::clamp(row.single_slot, 4, 11));
        maskBitsEdit_->setText(QStringLiteral("0x%1").arg(row.mask_bits, 0, 16));
        samePcSpin_->setValue(std::clamp(row.same_as_pc, 0, 3));
    }
    refreshUi();
}

TurnActionPresetRow PresetEditorDialog::buildRow(bool* ok, QString* errorText) const
{
    TurnActionPresetRow row = row_;
    row.name = nameEdit_->text().trimmed().toStdString();
    row.macro = actionCombo_->currentData().toInt();
    row.item_id = itemSpin_->value();
    row.target_expr_ini.clear();
    row.target_kind = static_cast<int>(TargetBindingKind::SingleEnemy);
    row.mask_bits = 0;
    row.single_slot = singleSlotSpin_->value();
    row.same_as_pc = samePcSpin_->value();

    if (row.name.empty()) {
        if (ok) *ok = false;
        if (errorText) *errorText = QStringLiteral("Preset name is required.");
        return row;
    }

    const int targetData = targetCombo_->currentData().toInt();
    if (targetData == 1000) {
        IniDoc ini;
        ini.ensure_section("target");
        ini.set("target", "kind", "ByEnemyKind");
        ini.set("target", "enemy_kind_id", std::to_string(enemyKindSpin_->value()));
        ini.set("target", "quantifier", quantifierCombo_->currentText().toStdString());
        row.target_expr_ini = ini.to_string_sorted();
    } else {
        row.target_kind = targetData;
        if (targetData == static_cast<int>(TargetBindingKind::MultipleEnemies)) {
            bool maskOk = false;
            row.mask_bits = maskBitsEdit_->text().trimmed().toUInt(&maskOk, 0);
            if (!maskOk) {
                if (ok) *ok = false;
                if (errorText) *errorText = QStringLiteral("Multi-target mask must be a valid integer or hex value.");
                return row;
            }
        }
    }

    if (static_cast<BattleAction>(row.macro) != BattleAction::UseItem) {
        row.item_id = -1;
    }

    if (ok) *ok = true;
    if (errorText) errorText->clear();
    return row;
}

void PresetEditorDialog::refreshUi()
{
    const BattleAction action = static_cast<BattleAction>(actionCombo_->currentData().toInt());
    const int targetData = targetCombo_->currentData().toInt();
    const bool needsTarget = action == BattleAction::Attack || action == BattleAction::UseItem;
    const bool needsItem = action == BattleAction::UseItem;

    targetCombo_->setEnabled(needsTarget);
    singleSlotSpin_->setVisible(needsTarget && targetData == static_cast<int>(TargetBindingKind::SingleEnemy));
    maskBitsEdit_->setVisible(needsTarget && targetData == static_cast<int>(TargetBindingKind::MultipleEnemies));
    samePcSpin_->setVisible(needsTarget && targetData == static_cast<int>(TargetBindingKind::SameAsOtherPC));
    enemyKindSpin_->setVisible(needsTarget && targetData == 1000);
    quantifierCombo_->setVisible(needsTarget && targetData == 1000);
    itemSpin_->setEnabled(needsItem);
    itemPreviewLabel_->setText(itemName(itemSpin_->value()));

    if (!context_) {
        contextLabel_->setText(QStringLiteral("No BattleContext loaded. All targets/items remain editable."));
    } else {
        contextLabel_->setText(QStringLiteral("BattleContext loaded. Party + enemy slots can be validated after assignment."));
    }
    errorLabel_->clear();
}
