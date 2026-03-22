#include "PredicateEditorDialog.h"

#include "Runner/Breakpoints/BPRegistry.h"
#include "Runner/Breakpoints/Predicate.h"

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

using simcore::db::PredicateSpecRow;

PredicateEditorDialog::PredicateEditorDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Predicate Specification"));
    resize(620, 520);

    QVBoxLayout* root = new QVBoxLayout(this);
    QFormLayout* form = new QFormLayout();

    nameEdit_ = new QLineEdit(this);
    form->addRow(QStringLiteral("Name"), nameEdit_);
    descriptionEdit_ = new QPlainTextEdit(this);
    descriptionEdit_->setMaximumHeight(100);
    form->addRow(QStringLiteral("Description"), descriptionEdit_);

    breakpointCombo_ = new QComboBox(this);
    for (const BPAddr& bp : bp::BPRegistry::all()) {
        breakpointCombo_->addItem(QStringLiteral("%1 @ 0x%2")
            .arg(QString::fromUtf8(bp.name))
            .arg(bp.pc, 8, 16, QLatin1Char('0')),
            static_cast<int>(bp.key));
    }
    form->addRow(QStringLiteral("Required breakpoint"), breakpointCombo_);

    kindCombo_ = new QComboBox(this);
    kindCombo_->addItem(QStringLiteral("ABS"), static_cast<int>(simcore::pred::PredKind::ABS));
    kindCombo_->addItem(QStringLiteral("DELTA"), static_cast<int>(simcore::pred::PredKind::DELTA));
    form->addRow(QStringLiteral("Kind"), kindCombo_);

    widthCombo_ = new QComboBox(this);
    widthCombo_->addItem(QStringLiteral("1"), 1);
    widthCombo_->addItem(QStringLiteral("2"), 2);
    widthCombo_->addItem(QStringLiteral("4"), 4);
    widthCombo_->addItem(QStringLiteral("8"), 8);
    form->addRow(QStringLiteral("Width"), widthCombo_);

    cmpCombo_ = new QComboBox(this);
    cmpCombo_->addItem(QStringLiteral("=="), static_cast<int>(simcore::pred::CmpOp::EQ));
    cmpCombo_->addItem(QStringLiteral("!="), static_cast<int>(simcore::pred::CmpOp::NE));
    cmpCombo_->addItem(QStringLiteral("<"), static_cast<int>(simcore::pred::CmpOp::LT));
    cmpCombo_->addItem(QStringLiteral("<="), static_cast<int>(simcore::pred::CmpOp::LE));
    cmpCombo_->addItem(QStringLiteral(">"), static_cast<int>(simcore::pred::CmpOp::GT));
    cmpCombo_->addItem(QStringLiteral(">="), static_cast<int>(simcore::pred::CmpOp::GE));
    form->addRow(QStringLiteral("Compare"), cmpCombo_);

    lhsAddrEdit_ = new QLineEdit(this);
    lhsAddrEdit_->setPlaceholderText(QStringLiteral("0x8C000000"));
    form->addRow(QStringLiteral("LHS absolute address"), lhsAddrEdit_);

    rhsValueEdit_ = new QLineEdit(this);
    rhsValueEdit_->setPlaceholderText(QStringLiteral("0"));
    form->addRow(QStringLiteral("RHS immediate"), rhsValueEdit_);

    turnMaskEdit_ = new QLineEdit(this);
    turnMaskEdit_->setPlaceholderText(QStringLiteral("0xFFFFFFFF"));
    form->addRow(QStringLiteral("Turn mask"), turnMaskEdit_);

    activeCheck_ = new QCheckBox(QStringLiteral("Active"), this);
    abortCheck_ = new QCheckBox(QStringLiteral("Abort on fail"), this);
    captureCheck_ = new QCheckBox(QStringLiteral("Capture baseline"), this);
    QHBoxLayout* flagLayout = new QHBoxLayout();
    flagLayout->addWidget(activeCheck_);
    flagLayout->addWidget(abortCheck_);
    flagLayout->addWidget(captureCheck_);
    form->addRow(QStringLiteral("Flags"), flagLayout);

    root->addLayout(form);

    errorLabel_ = new QLabel(this);
    errorLabel_->setWordWrap(true);
    root->addWidget(errorLabel_);

    buttonBox_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttonBox_->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Save"));
    root->addWidget(buttonBox_);

    connect(buttonBox_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox_, &QDialogButtonBox::rejected, this, &QDialog::reject);

    activeCheck_->setChecked(true);
    turnMaskEdit_->setText(QStringLiteral("0xFFFFFFFF"));
}

void PredicateEditorDialog::loadRow(const PredicateSpecRow& row)
{
    row_ = row;
    nameEdit_->setText(QString::fromStdString(row.name));
    descriptionEdit_->setPlainText(QString::fromStdString(row.description));
    breakpointCombo_->setCurrentIndex(std::max(0, breakpointCombo_->findData(row.required_bp)));
    kindCombo_->setCurrentIndex(std::max(0, kindCombo_->findData(row.kind)));
    widthCombo_->setCurrentIndex(std::max(0, widthCombo_->findData(row.width)));
    cmpCombo_->setCurrentIndex(std::max(0, cmpCombo_->findData(row.cmp_op)));
    lhsAddrEdit_->setText(QStringLiteral("0x%1").arg(row.lhs_addr, 0, 16));
    rhsValueEdit_->setText(QString::number(row.rhs_value));
    turnMaskEdit_->setText(QStringLiteral("0x%1").arg(row.turn_mask, 0, 16));
    activeCheck_->setChecked((row.flags & static_cast<int>(simcore::pred::PredFlag::Active)) != 0);
    abortCheck_->setChecked((row.flags & static_cast<int>(simcore::pred::PredFlag::AbortOnFail)) != 0);
    captureCheck_->setChecked((row.flags & static_cast<int>(simcore::pred::PredFlag::CaptureBaseline)) != 0);
}

PredicateSpecRow PredicateEditorDialog::buildRow(bool* ok, QString* errorText) const
{
    PredicateSpecRow row = row_;
    row.spec_version = static_cast<int32_t>(simcore::pred::SPEC_VERSION);
    row.name = nameEdit_->text().trimmed().toStdString();
    row.description = descriptionEdit_->toPlainText().trimmed().toStdString();
    row.required_bp = breakpointCombo_->currentData().toInt();
    row.kind = kindCombo_->currentData().toInt();
    row.width = widthCombo_->currentData().toInt();
    row.cmp_op = cmpCombo_->currentData().toInt();
    row.flags = 0;
    row.lhs_key.reset();
    row.rhs_key.reset();
    row.lhs_prog_id.reset();
    row.rhs_prog_id.reset();

    bool lhsOk = false;
    bool rhsOk = false;
    bool maskOk = false;
    row.lhs_addr = lhsAddrEdit_->text().trimmed().toLongLong(&lhsOk, 0);
    row.rhs_value = rhsValueEdit_->text().trimmed().toLongLong(&rhsOk, 0);
    row.turn_mask = turnMaskEdit_->text().trimmed().toInt(&maskOk, 0);
    if (row.name.empty()) {
        if (ok) *ok = false;
        if (errorText) *errorText = QStringLiteral("Predicate name is required.");
        return row;
    }
    if (!lhsOk || !rhsOk || !maskOk) {
        if (ok) *ok = false;
        if (errorText) *errorText = QStringLiteral("Address, RHS value, and turn mask must be valid numeric values.");
        return row;
    }

    if (activeCheck_->isChecked()) row.flags |= static_cast<int>(simcore::pred::PredFlag::Active);
    if (abortCheck_->isChecked()) row.flags |= static_cast<int>(simcore::pred::PredFlag::AbortOnFail);
    if (captureCheck_->isChecked()) row.flags |= static_cast<int>(simcore::pred::PredFlag::CaptureBaseline);

    simcore::pred::Spec spec{};
    spec.required_bp = static_cast<uint16_t>(row.required_bp);
    spec.kind = static_cast<simcore::pred::PredKind>(row.kind);
    spec.width = static_cast<uint8_t>(row.width);
    spec.cmp = static_cast<simcore::pred::CmpOp>(row.cmp_op);
    spec.flags = static_cast<uint32_t>(row.flags);
    spec.lhs_addr = static_cast<uint32_t>(row.lhs_addr);
    spec.rhs_value = static_cast<uint64_t>(row.rhs_value);
    spec.turn_mask = static_cast<uint32_t>(row.turn_mask);
    spec.name = row.name;
    spec.desc = row.description;
    row.fingerprint = simcore::pred::fingerprint(spec);

    if (ok) *ok = true;
    if (errorText) errorText->clear();
    return row;
}
