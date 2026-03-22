#include "TemplateSaveDialog.h"

#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

TemplateSaveDialog::TemplateSaveDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Save Authoring Template"));
    QVBoxLayout* root = new QVBoxLayout(this);
    QFormLayout* form = new QFormLayout();
    nameEdit_ = new QLineEdit(this);
    descriptionEdit_ = new QPlainTextEdit(this);
    descriptionEdit_->setMaximumHeight(100);
    form->addRow(QStringLiteral("Name"), nameEdit_);
    form->addRow(QStringLiteral("Description"), descriptionEdit_);
    root->addLayout(form);
    buttonBox_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttonBox_->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Save"));
    root->addWidget(buttonBox_);
    connect(buttonBox_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox_, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

QString TemplateSaveDialog::name() const
{
    return nameEdit_->text().trimmed();
}

QString TemplateSaveDialog::description() const
{
    return descriptionEdit_->toPlainText().trimmed();
}
