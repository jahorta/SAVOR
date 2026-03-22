#pragma once

#include <QtWidgets/QDialog>

#include "Core/Memory/Soa/Battle/BattleContext.h"
#include "DB/TurnActionPresetRepo.h"

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QSpinBox;

class PresetEditorDialog final : public QDialog
{
public:
    explicit PresetEditorDialog(const soa::battle::ctx::BattleContext* context, QWidget* parent = nullptr);

    void loadRow(const simcore::db::TurnActionPresetRow& row);
    simcore::db::TurnActionPresetRow buildRow(bool* ok, QString* errorText) const;

private:
    void refreshUi();

    const soa::battle::ctx::BattleContext* context_ = nullptr;
    simcore::db::TurnActionPresetRow row_{};
    QLineEdit* nameEdit_ = nullptr;
    QComboBox* actionCombo_ = nullptr;
    QComboBox* targetCombo_ = nullptr;
    QSpinBox* singleSlotSpin_ = nullptr;
    QLineEdit* maskBitsEdit_ = nullptr;
    QSpinBox* samePcSpin_ = nullptr;
    QSpinBox* enemyKindSpin_ = nullptr;
    QComboBox* quantifierCombo_ = nullptr;
    QSpinBox* itemSpin_ = nullptr;
    QLabel* itemPreviewLabel_ = nullptr;
    QLabel* contextLabel_ = nullptr;
    QLabel* errorLabel_ = nullptr;
    QDialogButtonBox* buttonBox_ = nullptr;
};
