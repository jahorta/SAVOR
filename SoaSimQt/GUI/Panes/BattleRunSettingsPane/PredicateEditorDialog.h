#pragma once

#include <QtWidgets/QDialog>

#include "DB/PredicateSpecRepo.h"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;

class PredicateEditorDialog final : public QDialog
{
public:
    explicit PredicateEditorDialog(QWidget* parent = nullptr);

    void loadRow(const simcore::db::PredicateSpecRow& row);
    simcore::db::PredicateSpecRow buildRow(bool* ok, QString* errorText) const;

private:
    simcore::db::PredicateSpecRow row_{};
    QLineEdit* nameEdit_ = nullptr;
    QPlainTextEdit* descriptionEdit_ = nullptr;
    QComboBox* breakpointCombo_ = nullptr;
    QComboBox* kindCombo_ = nullptr;
    QComboBox* widthCombo_ = nullptr;
    QComboBox* cmpCombo_ = nullptr;
    QLineEdit* lhsAddrEdit_ = nullptr;
    QLineEdit* rhsValueEdit_ = nullptr;
    QLineEdit* turnMaskEdit_ = nullptr;
    QCheckBox* activeCheck_ = nullptr;
    QCheckBox* abortCheck_ = nullptr;
    QCheckBox* captureCheck_ = nullptr;
    QLabel* errorLabel_ = nullptr;
    QDialogButtonBox* buttonBox_ = nullptr;
};
