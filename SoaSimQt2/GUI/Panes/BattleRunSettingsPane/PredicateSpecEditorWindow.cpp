#include "PredicateSpecEditorWindow.h"

#include "DB/SimCoreDbAuthoringService.h"

#include <QtGui/QCloseEvent>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

namespace {

simcore::db::PredicateOperandKind operandKindFromCombo(const QComboBox* combo)
{
    return static_cast<simcore::db::PredicateOperandKind>(combo->currentData().toInt());
}

simcore::db::PredicateComparisonOp comparisonFromCombo(const QComboBox* combo)
{
    return static_cast<simcore::db::PredicateComparisonOp>(combo->currentData().toInt());
}

} // namespace

PredicateSpecEditorWindow::PredicateSpecEditorWindow(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlag(Qt::Window, true);
    setWindowTitle(QStringLiteral("Predicate Editor"));
    resize(640, 360);
    createWidgets();
}

void PredicateSpecEditorWindow::setStatusCallback(std::function<void(const QString&, StatusToast::Severity)> callback)
{
    statusCallback_ = std::move(callback);
}

void PredicateSpecEditorWindow::setSavedCallback(std::function<void()> callback)
{
    savedCallback_ = std::move(callback);
}

void PredicateSpecEditorWindow::loadSnapshot(const simcore::db::PredicateSpecSnapshot& snapshot, bool duplicate)
{
    setWindowTitle(duplicate
        ? QStringLiteral("Predicate Editor - Duplicate")
        : QStringLiteral("Predicate Editor - Edit Copy"));
    nameEdit_->setText(QString::fromStdString(snapshot.name) + (duplicate ? QStringLiteral(" copy") : QString()));
    breakpointEdit_->setText(QString::number(static_cast<int>(snapshot.breakpoint_id)));
    lhsKindCombo_->setCurrentIndex(std::max(0, lhsKindCombo_->findData(static_cast<int>(snapshot.lhs_kind))));
    lhsValueEdit_->setText(QString::number(snapshot.lhs_value));
    rhsKindCombo_->setCurrentIndex(std::max(0, rhsKindCombo_->findData(static_cast<int>(snapshot.rhs_kind))));
    rhsValueEdit_->setText(QString::number(snapshot.rhs_value));
    cmpCombo_->setCurrentIndex(std::max(0, cmpCombo_->findData(static_cast<int>(snapshot.cmp_op))));
    widthCombo_->setCurrentIndex(std::max(0, widthCombo_->findData(snapshot.width)));
    abortOnFailCheck_->setChecked(snapshot.abort_on_fail);
    dirty_ = false;
}

void PredicateSpecEditorWindow::closeEvent(QCloseEvent* event)
{
    if (confirmDiscardIfDirty()) {
        event->accept();
    } else {
        event->ignore();
    }
}

void PredicateSpecEditorWindow::createWidgets()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(12, 12, 12, 12);
    rootLayout->setSpacing(10);

    auto* panel = new QFrame(this);
    panel->setObjectName("jobsSurfacePanel");
    auto* form = new QFormLayout(panel);
    form->setContentsMargins(14, 14, 14, 14);
    form->setSpacing(10);

    nameEdit_ = new QLineEdit(panel);
    breakpointEdit_ = new QLineEdit(panel);
    breakpointEdit_->setPlaceholderText(QStringLiteral("0"));
    lhsKindCombo_ = new QComboBox(panel);
    lhsKindCombo_->addItem(QStringLiteral("Memory"), static_cast<int>(simcore::db::PredicateOperandKind::Memory));
    lhsKindCombo_->addItem(QStringLiteral("Absolute"), static_cast<int>(simcore::db::PredicateOperandKind::Absolute));
    lhsKindCombo_->addItem(QStringLiteral("Delta"), static_cast<int>(simcore::db::PredicateOperandKind::Delta));
    lhsKindCombo_->addItem(QStringLiteral("Literal"), static_cast<int>(simcore::db::PredicateOperandKind::Literal));
    lhsValueEdit_ = new QLineEdit(panel);
    lhsValueEdit_->setPlaceholderText(QStringLiteral("0x80000000"));
    rhsKindCombo_ = new QComboBox(panel);
    rhsKindCombo_->addItem(QStringLiteral("Literal"), static_cast<int>(simcore::db::PredicateOperandKind::Literal));
    rhsKindCombo_->addItem(QStringLiteral("Memory"), static_cast<int>(simcore::db::PredicateOperandKind::Memory));
    rhsValueEdit_ = new QLineEdit(panel);
    rhsValueEdit_->setPlaceholderText(QStringLiteral("0"));
    cmpCombo_ = new QComboBox(panel);
    cmpCombo_->addItem(QStringLiteral("=="), static_cast<int>(simcore::db::PredicateComparisonOp::EQ));
    cmpCombo_->addItem(QStringLiteral("!="), static_cast<int>(simcore::db::PredicateComparisonOp::NE));
    cmpCombo_->addItem(QStringLiteral("<"), static_cast<int>(simcore::db::PredicateComparisonOp::LT));
    cmpCombo_->addItem(QStringLiteral("<="), static_cast<int>(simcore::db::PredicateComparisonOp::LE));
    cmpCombo_->addItem(QStringLiteral(">"), static_cast<int>(simcore::db::PredicateComparisonOp::GT));
    cmpCombo_->addItem(QStringLiteral(">="), static_cast<int>(simcore::db::PredicateComparisonOp::GE));
    widthCombo_ = new QComboBox(panel);
    widthCombo_->addItem(QStringLiteral("1"), 1);
    widthCombo_->addItem(QStringLiteral("2"), 2);
    widthCombo_->addItem(QStringLiteral("4"), 4);
    widthCombo_->addItem(QStringLiteral("8"), 8);
    widthCombo_->setCurrentIndex(widthCombo_->findData(4));
    abortOnFailCheck_ = new QCheckBox(QStringLiteral("Abort on fail"), panel);

    form->addRow(QStringLiteral("Name"), nameEdit_);
    form->addRow(QStringLiteral("Breakpoint key"), breakpointEdit_);
    form->addRow(QStringLiteral("LHS kind"), lhsKindCombo_);
    form->addRow(QStringLiteral("LHS value"), lhsValueEdit_);
    form->addRow(QStringLiteral("Compare"), cmpCombo_);
    form->addRow(QStringLiteral("RHS kind"), rhsKindCombo_);
    form->addRow(QStringLiteral("RHS value"), rhsValueEdit_);
    form->addRow(QStringLiteral("Width"), widthCombo_);
    form->addRow(QString(), abortOnFailCheck_);
    rootLayout->addWidget(panel, 1);

    auto* buttonRow = new QHBoxLayout();
    buttonRow->addStretch();
    saveButton_ = new QPushButton(QStringLiteral("Save Predicate"), this);
    saveButton_->setObjectName("jobsPrimaryButton");
    buttonRow->addWidget(saveButton_);
    rootLayout->addLayout(buttonRow);

    connect(saveButton_, &QPushButton::clicked, this, &PredicateSpecEditorWindow::savePredicate);
    connect(nameEdit_, &QLineEdit::textChanged, this, [this]() { markDirty(); });
    connect(breakpointEdit_, &QLineEdit::textChanged, this, [this]() { markDirty(); });
    connect(lhsKindCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() { markDirty(); });
    connect(lhsValueEdit_, &QLineEdit::textChanged, this, [this]() { markDirty(); });
    connect(rhsKindCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() { markDirty(); });
    connect(rhsValueEdit_, &QLineEdit::textChanged, this, [this]() { markDirty(); });
    connect(cmpCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() { markDirty(); });
    connect(widthCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() { markDirty(); });
    connect(abortOnFailCheck_, &QCheckBox::toggled, this, [this]() { markDirty(); });
}

void PredicateSpecEditorWindow::savePredicate()
{
    if (nameEdit_->text().trimmed().isEmpty()) {
        postStatusMessage(QStringLiteral("Predicate name is required."), StatusToast::Severity::Warn);
        return;
    }

    bool ok = false;
    const auto breakpointId = breakpointEdit_->text().trimmed().toLongLong(&ok, 0);
    if (!ok || breakpointId <= 0) {
        postStatusMessage(QStringLiteral("Breakpoint key must be a positive number."), StatusToast::Severity::Warn);
        return;
    }
    const auto lhsValue = lhsValueEdit_->text().trimmed().toLongLong(&ok, 0);
    if (!ok) {
        postStatusMessage(QStringLiteral("LHS value must be numeric."), StatusToast::Severity::Warn);
        return;
    }
    const auto rhsValue = rhsValueEdit_->text().trimmed().toLongLong(&ok, 0);
    if (!ok) {
        postStatusMessage(QStringLiteral("RHS value must be numeric."), StatusToast::Severity::Warn);
        return;
    }

    soasimqt2::db::PredicateSpecDraft draft{};
    draft.name = nameEdit_->text().trimmed().toStdString();
    draft.breakpoint_id = static_cast<int>(breakpointId);
    draft.lhs_kind = operandKindFromCombo(lhsKindCombo_);
    draft.lhs_value = lhsValue;
    draft.rhs_kind = operandKindFromCombo(rhsKindCombo_);
    draft.rhs_value = rhsValue;
    draft.cmp_op = comparisonFromCombo(cmpCombo_);
    draft.width = widthCombo_->currentData().toInt();
    draft.abort_on_fail = abortOnFailCheck_->isChecked();

    saveButton_->setEnabled(false);
    const auto result = soasimqt2::db::SimCoreDbAuthoringService::SavePredicateSpec(draft);
    saveButton_->setEnabled(true);
    if (!result.ok) {
        postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
        return;
    }
    dirty_ = false;
    if (savedCallback_) {
        savedCallback_();
    }
    postStatusMessage(QStringLiteral("Saved predicate %1.").arg(static_cast<qint64>(result.value)), StatusToast::Severity::Info);
}

void PredicateSpecEditorWindow::markDirty()
{
    dirty_ = true;
}

bool PredicateSpecEditorWindow::confirmDiscardIfDirty()
{
    if (!dirty_) {
        return true;
    }
    const auto result = QMessageBox::warning(
        this,
        QStringLiteral("Discard predicate changes?"),
        QStringLiteral("This predicate has unsaved changes."),
        QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Cancel);
    return result == QMessageBox::Discard;
}

void PredicateSpecEditorWindow::postStatusMessage(const QString& text, StatusToast::Severity severity)
{
    if (text.isEmpty()) {
        return;
    }
    if (statusCallback_) {
        statusCallback_(text, severity);
    }
}
