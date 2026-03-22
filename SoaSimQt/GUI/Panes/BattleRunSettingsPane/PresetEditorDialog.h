#pragma once

#include <QtCore/QVector>
#include <QtWidgets/QDialog>

#include "Core/Memory/Soa/Battle/BattleContext.h"
#include "DB/TurnActionPresetRepo.h"

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QCheckBox;
class QRadioButton;

class PresetEditorDialog final : public QDialog
{
public:
    enum class ItemCategory : int {
        Weapon = 0,
        Armor,
        Accessory,
        Consumable,
        Special,
        ShipWeapon,
        ShipAccessory,
        ShipConsumable,
        All,
    };

    explicit PresetEditorDialog(const soa::battle::ctx::BattleContext* context, QWidget* parent = nullptr);

    void loadRow(const simcore::db::TurnActionPresetRow& row);
    simcore::db::TurnActionPresetRow buildRow(bool* ok, QString* errorText) const;
    bool saveAsNewRequested() const;

private:
    void accept() override;
    void rebuildContextCaches();
    void rebuildItemList();
    void rebuildEnemyKindList();
    void refreshUi();
    void setDialogError(const QString& text);

    const soa::battle::ctx::BattleContext* context_ = nullptr;
    simcore::db::TurnActionPresetRow row_{};
    QLineEdit* nameEdit_ = nullptr;
    QComboBox* actionCombo_ = nullptr;
    QComboBox* targetCombo_ = nullptr;
    QVector<QRadioButton*> singleSlotButtons_;
    QVector<QCheckBox*> multiTargetChecks_;
    QComboBox* samePcCombo_ = nullptr;
    QComboBox* enemyKindCombo_ = nullptr;
    QComboBox* quantifierCombo_ = nullptr;
    QComboBox* itemCategoryCombo_ = nullptr;
    QListWidget* itemList_ = nullptr;
    QCheckBox* showAllItemsCheck_ = nullptr;
    QCheckBox* showAllKindsCheck_ = nullptr;
    QLabel* itemPreviewLabel_ = nullptr;
    QLabel* targetPreviewLabel_ = nullptr;
    QLabel* contextLabel_ = nullptr;
    QLabel* errorLabel_ = nullptr;
    QDialogButtonBox* buttonBox_ = nullptr;
    QPushButton* saveAsNewButton_ = nullptr;
    QPushButton* updateCurrentButton_ = nullptr;

    QVector<int> presentEnemyKinds_;
    QVector<int> presentItems_;
    bool saveAsNewRequested_ = false;
};
