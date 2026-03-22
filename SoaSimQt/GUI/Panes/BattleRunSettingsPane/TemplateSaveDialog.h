#pragma once

#include <QtWidgets/QDialog>

class QDialogButtonBox;
class QLineEdit;
class QPlainTextEdit;

class TemplateSaveDialog final : public QDialog
{
public:
    explicit TemplateSaveDialog(QWidget* parent = nullptr);

    QString name() const;
    QString description() const;

private:
    QLineEdit* nameEdit_ = nullptr;
    QPlainTextEdit* descriptionEdit_ = nullptr;
    QDialogButtonBox* buttonBox_ = nullptr;
};
