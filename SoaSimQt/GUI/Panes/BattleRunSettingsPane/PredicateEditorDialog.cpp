#include "PredicateEditorDialog.h"

#include "Core/Memory/Soa/SoaAddrCatalog.h"
#include "Core/Memory/Soa/SoaAddrProgramBuilder.h"
#include "Core/Memory/Soa/SoaAddrRegistry.h"
#include "DB/AddressProgramRepo.h"
#include "Runner/Breakpoints/BPRegistry.h"
#include "Runner/Breakpoints/Predicate.h"

#include <QtCore/QSignalBlocker>
#include <QtCore/QStringList>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <sstream>

using simcore::db::PredicateSpecRow;

namespace {
QString programKindLabel(const PredicateEditorDialog::ProgramKind kind)
{
    switch (kind) {
    case PredicateEditorDialog::ProgramKind::None: return QStringLiteral("None");
    case PredicateEditorDialog::ProgramKind::TurnOrderIndex: return QStringLiteral("Turn order index (derived)");
    case PredicateEditorDialog::ProgramKind::ItemDropAmount: return QStringLiteral("Item drop amount (derived)");
    case PredicateEditorDialog::ProgramKind::BattleTreasureSlotAmount: return QStringLiteral("Battle treasure slot → amount");
    case PredicateEditorDialog::ProgramKind::EnemyItemAmount: return QStringLiteral("Enemy item → amount");
    }
    return QStringLiteral("None");
}

QString valueSourceLabel(const PredicateEditorDialog::ValueSourceMode mode, const bool lhs)
{
    switch (mode) {
    case PredicateEditorDialog::ValueSourceMode::AbsoluteAddress: return QStringLiteral("Absolute Addr (Mem1)");
    case PredicateEditorDialog::ValueSourceMode::AddrKey: return QStringLiteral("AddrKey");
    case PredicateEditorDialog::ValueSourceMode::AddrProgram: return QStringLiteral("AddrProgram");
    case PredicateEditorDialog::ValueSourceMode::Immediate: return lhs ? QStringLiteral("Absolute Addr (Mem1)") : QStringLiteral("Immediate");
    }
    return lhs ? QStringLiteral("Absolute Addr (Mem1)") : QStringLiteral("Immediate");
}

QString describeProgramDraft(const PredicateEditorDialog::ProgramDraft& draft)
{
    if (draft.blob.isEmpty()) {
        return QStringLiteral("No address program built.");
    }
    return QStringLiteral("%1 · %2 bytes%3")
        .arg(programKindLabel(draft.kind))
        .arg(draft.blob.size())
        .arg(draft.description.isEmpty() ? QString() : QStringLiteral(" · %1").arg(draft.description));
}

QVector<int> parseBpMultiCsv(const std::optional<std::string>& csvOpt)
{
    QVector<int> out;
    if (!csvOpt.has_value() || csvOpt->empty()) return out;
    const QStringList tokens = QString::fromStdString(*csvOpt).split(',', Qt::SkipEmptyParts);
    for (const QString& token : tokens) {
        bool ok = false;
        const int value = token.trimmed().toInt(&ok);
        if (ok && value > 0) out.push_back(value);
    }
    return out;
}

std::optional<std::string> toBpMultiCsv(const QVector<int>& bps)
{
    if (bps.size() <= 1) return std::nullopt;
    std::ostringstream oss;
    for (int index = 0; index < bps.size(); ++index) {
        if (index > 0) oss << ',';
        oss << bps.at(index);
    }
    return oss.str();
}
}

PredicateEditorDialog::PredicateEditorDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Predicate Specification"));
    resize(820, 720);

    populateAddrKeys();

    QVBoxLayout* root = new QVBoxLayout(this);

    errorLabel_ = new QLabel(this);
    errorLabel_->setWordWrap(true);
    errorLabel_->setStyleSheet(QStringLiteral("color: #d9534f;"));
    errorLabel_->hide();
    root->addWidget(errorLabel_);

    QGroupBox* coreBox = new QGroupBox(QStringLiteral("Core"), this);
    QFormLayout* coreLayout = new QFormLayout(coreBox);
    nameEdit_ = new QLineEdit(this);
    descriptionEdit_ = new QPlainTextEdit(this);
    descriptionEdit_->setMaximumHeight(100);
    kindCombo_ = new QComboBox(this);
    kindCombo_->addItem(QStringLiteral("ABS"), static_cast<int>(simcore::pred::PredKind::ABS));
    kindCombo_->addItem(QStringLiteral("DELTA"), static_cast<int>(simcore::pred::PredKind::DELTA));
    widthCombo_ = new QComboBox(this);
    widthCombo_->addItem(QStringLiteral("1"), 1);
    widthCombo_->addItem(QStringLiteral("2"), 2);
    widthCombo_->addItem(QStringLiteral("4"), 4);
    widthCombo_->addItem(QStringLiteral("8"), 8);
    cmpCombo_ = new QComboBox(this);
    cmpCombo_->addItem(QStringLiteral("=="), static_cast<int>(simcore::pred::CmpOp::EQ));
    cmpCombo_->addItem(QStringLiteral("!="), static_cast<int>(simcore::pred::CmpOp::NE));
    cmpCombo_->addItem(QStringLiteral("<"), static_cast<int>(simcore::pred::CmpOp::LT));
    cmpCombo_->addItem(QStringLiteral("<="), static_cast<int>(simcore::pred::CmpOp::LE));
    cmpCombo_->addItem(QStringLiteral(">"), static_cast<int>(simcore::pred::CmpOp::GT));
    cmpCombo_->addItem(QStringLiteral(">="), static_cast<int>(simcore::pred::CmpOp::GE));
    coreLayout->addRow(QStringLiteral("Name"), nameEdit_);
    coreLayout->addRow(QStringLiteral("Description"), descriptionEdit_);
    coreLayout->addRow(QStringLiteral("Kind"), kindCombo_);
    coreLayout->addRow(QStringLiteral("Width (bytes)"), widthCombo_);
    coreLayout->addRow(QStringLiteral("Compare"), cmpCombo_);
    root->addWidget(coreBox);

    QGroupBox* executionBox = new QGroupBox(QStringLiteral("Execution"), this);
    QFormLayout* executionLayout = new QFormLayout(executionBox);
    requiredBpRowsWidget_ = new QWidget(this);
    requiredBpRowsLayout_ = new QVBoxLayout(requiredBpRowsWidget_);
    requiredBpRowsLayout_->setContentsMargins(0, 0, 0, 0);
    requiredBpRowsLayout_->setSpacing(6);
    addRequiredBreakpointField();
    addRequiredBpButton_ = new QPushButton(QStringLiteral("Add required breakpoint"), this);
    turnMaskEdit_ = new QLineEdit(this);
    turnMaskEdit_->setPlaceholderText(QStringLiteral("0xFFFFFFFF"));
    executionLayout->addRow(QStringLiteral("Required breakpoints"), requiredBpRowsWidget_);
    executionLayout->addRow(QString(), addRequiredBpButton_);
    executionLayout->addRow(QStringLiteral("Turn mask"), turnMaskEdit_);
    connect(addRequiredBpButton_, &QPushButton::clicked, this, [this]() { addRequiredBreakpointField(); });
    root->addWidget(executionBox);

    QGroupBox* flagsBox = new QGroupBox(QStringLiteral("Flags"), this);
    QHBoxLayout* flagsLayout = new QHBoxLayout(flagsBox);
    activeCheck_ = new QCheckBox(QStringLiteral("Active"), this);
    abortCheck_ = new QCheckBox(QStringLiteral("Abort on fail"), this);
    captureCheck_ = new QCheckBox(QStringLiteral("Capture baseline"), this);
    lhsNegateCheck_ = new QCheckBox(QStringLiteral("Negate LHS"), this);
    rhsNegateCheck_ = new QCheckBox(QStringLiteral("Negate RHS"), this);
    for (QCheckBox* box : { activeCheck_, abortCheck_, captureCheck_, lhsNegateCheck_, rhsNegateCheck_ }) {
        flagsLayout->addWidget(box);
    }
    root->addWidget(flagsBox);

    QHBoxLayout* matchLayout = new QHBoxLayout();
    auto buildValueBox = [this](const QString& title, const bool lhs) {
        QGroupBox* box = new QGroupBox(title, this);
        QFormLayout* layout = new QFormLayout(box);
        QComboBox*& modeCombo = lhs ? lhsModeCombo_ : rhsModeCombo_;
        modeCombo = new QComboBox(box);
        if (lhs) {
            modeCombo->addItem(valueSourceLabel(ValueSourceMode::AbsoluteAddress, true), static_cast<int>(ValueSourceMode::AbsoluteAddress));
            modeCombo->addItem(valueSourceLabel(ValueSourceMode::AddrKey, true), static_cast<int>(ValueSourceMode::AddrKey));
            modeCombo->addItem(valueSourceLabel(ValueSourceMode::AddrProgram, true), static_cast<int>(ValueSourceMode::AddrProgram));
        } else {
            modeCombo->addItem(valueSourceLabel(ValueSourceMode::Immediate, false), static_cast<int>(ValueSourceMode::Immediate));
            modeCombo->addItem(valueSourceLabel(ValueSourceMode::AddrKey, false), static_cast<int>(ValueSourceMode::AddrKey));
            modeCombo->addItem(valueSourceLabel(ValueSourceMode::AddrProgram, false), static_cast<int>(ValueSourceMode::AddrProgram));
        }
        layout->addRow(QStringLiteral("Source"), modeCombo);

        QLineEdit*& valueEdit = lhs ? lhsAddrEdit_ : rhsValueEdit_;
        valueEdit = new QLineEdit(box);
        valueEdit->setPlaceholderText(lhs ? QStringLiteral("0x80000000") : QStringLiteral("0"));
        layout->addRow(lhs ? QStringLiteral("Absolute VA") : QStringLiteral("Immediate"), valueEdit);

        QComboBox*& keyCombo = lhs ? lhsKeyCombo_ : rhsKeyCombo_;
        keyCombo = new QComboBox(box);
        for (int index = 0; index < addrKeys_.size(); ++index) {
            keyCombo->addItem(addrNames_.at(index), addrKeys_.at(index));
        }
        layout->addRow(QStringLiteral("AddrKey"), keyCombo);

        QComboBox*& programKindCombo = lhs ? lhsProgramKindCombo_ : rhsProgramKindCombo_;
        programKindCombo = new QComboBox(box);
        for (int kind = static_cast<int>(ProgramKind::None); kind <= static_cast<int>(ProgramKind::EnemyItemAmount); ++kind) {
            programKindCombo->addItem(programKindLabel(static_cast<ProgramKind>(kind)), kind);
        }
        layout->addRow(QStringLiteral("Program"), programKindCombo);

        QLineEdit*& programAEdit = lhs ? lhsProgramAEdit_ : rhsProgramAEdit_;
        programAEdit = new QLineEdit(box);
        layout->addRow(QStringLiteral("Program arg A"), programAEdit);

        QLineEdit*& programBEdit = lhs ? lhsProgramBEdit_ : rhsProgramBEdit_;
        programBEdit = new QLineEdit(box);
        layout->addRow(QStringLiteral("Program arg B"), programBEdit);

        QPushButton*& buildButton = lhs ? lhsBuildProgramButton_ : rhsBuildProgramButton_;
        buildButton = new QPushButton(QStringLiteral("Build program"), box);
        layout->addRow(QString(), buildButton);

        QLabel*& summaryLabel = lhs ? lhsProgramSummaryLabel_ : rhsProgramSummaryLabel_;
        summaryLabel = new QLabel(QStringLiteral("No address program built."), box);
        summaryLabel->setWordWrap(true);
        layout->addRow(QStringLiteral("Program status"), summaryLabel);

        connect(buildButton, &QPushButton::clicked, this, [this, lhs]() { buildProgram(lhs); });
        connect(modeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() { refreshUi(); });
        connect(programKindCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() { refreshUi(); });

        return box;
    };

    matchLayout->addWidget(buildValueBox(QStringLiteral("LHS"), true), 1);
    matchLayout->addWidget(buildValueBox(QStringLiteral("RHS"), false), 1);
    root->addLayout(matchLayout);

    buttonBox_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttonBox_->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Save"));
    root->addWidget(buttonBox_);

    connect(buttonBox_, &QDialogButtonBox::accepted, this, &PredicateEditorDialog::accept);
    connect(buttonBox_, &QDialogButtonBox::rejected, this, &QDialog::reject);

    activeCheck_->setChecked(true);
    captureCheck_->setChecked(true);
    turnMaskEdit_->setText(QStringLiteral("0xFFFFFFFF"));
    refreshUi();
}

void PredicateEditorDialog::populateAddrKeys()
{
    addrKeys_.clear();
    addrNames_.clear();
    for (const auto& rec : addr::Registry::all()) {
        addrKeys_.push_back(static_cast<int>(rec.key));
        addrNames_.push_back(QString::fromUtf8(rec.name));
    }
}

void PredicateEditorDialog::populateBreakpointCombo(QComboBox* combo) const
{
    if (!combo) return;
    combo->clear();
    combo->addItem(QStringLiteral("(none)"), 0);
    for (const BPAddr& bp : bp::BPRegistry::all()) {
        combo->addItem(QStringLiteral("%1 @ 0x%2")
            .arg(QString::fromUtf8(bp.name))
            .arg(bp.pc, 8, 16, QLatin1Char('0')),
            static_cast<int>(bp.key));
    }
}

void PredicateEditorDialog::addRequiredBreakpointField(const int selectedBp)
{
    QHBoxLayout* rowLayout = new QHBoxLayout();
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(6);

    QComboBox* combo = new QComboBox(requiredBpRowsWidget_);
    populateBreakpointCombo(combo);
    combo->setCurrentIndex(std::max(0, combo->findData(selectedBp)));
    rowLayout->addWidget(combo, 1);

    QPushButton* removeButton = new QPushButton(QStringLiteral("Delete"), requiredBpRowsWidget_);
    rowLayout->addWidget(removeButton);

    breakpointCombos_.push_back(combo);
    removeBreakpointButtons_.push_back(removeButton);
    breakpointRowLayouts_.push_back(rowLayout);
    requiredBpRowsLayout_->addLayout(rowLayout);

    connect(removeButton, &QPushButton::clicked, this, [this, removeButton]() {
        const int index = removeBreakpointButtons_.indexOf(removeButton);
        if (index >= 0) removeRequiredBreakpointField(index);
    });

    rebuildRequiredBreakpointRows();
}

void PredicateEditorDialog::removeRequiredBreakpointField(const int index)
{
    if (index < 0 || index >= breakpointCombos_.size() || breakpointCombos_.size() <= 1) return;
    QComboBox* combo = breakpointCombos_.takeAt(index);
    QPushButton* button = removeBreakpointButtons_.takeAt(index);
    QHBoxLayout* rowLayout = breakpointRowLayouts_.takeAt(index);
    requiredBpRowsLayout_->removeItem(rowLayout);
    delete combo;
    delete button;
    delete rowLayout;
    rebuildRequiredBreakpointRows();
}

void PredicateEditorDialog::rebuildRequiredBreakpointRows()
{
    const bool showDelete = breakpointCombos_.size() > 1;
    for (QPushButton* button : removeBreakpointButtons_) {
        button->setVisible(showDelete);
    }
}

QVector<int> PredicateEditorDialog::selectedRequiredBreakpoints() const
{
    QVector<int> out;
    out.reserve(breakpointCombos_.size());
    for (QComboBox* combo : breakpointCombos_) {
        const int value = combo ? combo->currentData().toInt() : 0;
        if (value > 0) out.push_back(value);
    }
    return out;
}

void PredicateEditorDialog::applyProgramDraftToWidgets(const ProgramDraft& draft, const bool lhs)
{
    QComboBox* kindCombo = lhs ? lhsProgramKindCombo_ : rhsProgramKindCombo_;
    QLineEdit* aEdit = lhs ? lhsProgramAEdit_ : rhsProgramAEdit_;
    QLineEdit* bEdit = lhs ? lhsProgramBEdit_ : rhsProgramBEdit_;
    QLabel* summaryLabel = lhs ? lhsProgramSummaryLabel_ : rhsProgramSummaryLabel_;

    kindCombo->setCurrentIndex(std::max(0, kindCombo->findData(static_cast<int>(draft.kind))));
    aEdit->setText(QString::number(draft.a));
    bEdit->setText(QString::number(draft.b));
    summaryLabel->setText(describeProgramDraft(draft));
}

PredicateEditorDialog::ProgramDraft PredicateEditorDialog::programDraftFromWidgets(const bool lhs) const
{
    ProgramDraft draft = lhs ? lhsProgramDraft_ : rhsProgramDraft_;
    draft.kind = static_cast<ProgramKind>((lhs ? lhsProgramKindCombo_ : rhsProgramKindCombo_)->currentData().toInt());
    draft.a = static_cast<quint16>((lhs ? lhsProgramAEdit_ : rhsProgramAEdit_)->text().trimmed().toUInt());
    draft.b = static_cast<quint16>((lhs ? lhsProgramBEdit_ : rhsProgramBEdit_)->text().trimmed().toUInt());
    return draft;
}

void PredicateEditorDialog::buildProgram(const bool lhs)
{
    ProgramDraft draft = programDraftFromWidgets(lhs);
    draft.blob.clear();
    draft.description.clear();

    if (draft.kind != ProgramKind::None) {
        addrprog::Builder builder;
        std::string desc;
        switch (draft.kind) {
        case ProgramKind::TurnOrderIndex:
            addrprog::catalog::turn_order_idx(builder, draft.a, desc);
            break;
        case ProgramKind::ItemDropAmount:
            addrprog::catalog::item_drop_amt(builder, draft.a, desc);
            break;
        case ProgramKind::BattleTreasureSlotAmount:
            addrprog::catalog::battle_treasure_slot(builder, draft.a, &soa::BattleItemDropSlot::count, desc);
            break;
        case ProgramKind::EnemyItemAmount:
            addrprog::catalog::enemy_item_field(builder, draft.a, draft.b, &soa::ItemDrop::amount, desc);
            break;
        case ProgramKind::None:
            break;
        }
        draft.description = QString::fromStdString(desc);
        draft.blob = QByteArray(reinterpret_cast<const char*>(builder.blob().data()), static_cast<int>(builder.blob().size()));
    }

    if (lhs) {
        lhsProgramDraft_ = draft;
    } else {
        rhsProgramDraft_ = draft;
    }
    applyProgramDraftToWidgets(draft, lhs);
    refreshUi();
}

void PredicateEditorDialog::loadRow(const PredicateSpecRow& row)
{
    row_ = row;
    nameEdit_->setText(QString::fromStdString(row.name));
    descriptionEdit_->setPlainText(QString::fromStdString(row.description));
    QVector<int> requiredBps = parseBpMultiCsv(row.required_bp_multi);
    if (requiredBps.isEmpty() && row.required_bp > 0) requiredBps.push_back(row.required_bp);
    if (requiredBps.isEmpty()) requiredBps.push_back(0);

    while (breakpointCombos_.size() > requiredBps.size()) {
        removeRequiredBreakpointField(breakpointCombos_.size() - 1);
    }
    while (breakpointCombos_.size() < requiredBps.size()) {
        addRequiredBreakpointField();
    }
    for (int i = 0; i < breakpointCombos_.size(); ++i) {
        breakpointCombos_[i]->setCurrentIndex(std::max(0, breakpointCombos_[i]->findData(requiredBps.at(i))));
    }
    kindCombo_->setCurrentIndex(std::max(0, kindCombo_->findData(row.kind)));
    widthCombo_->setCurrentIndex(std::max(0, widthCombo_->findData(row.width)));
    cmpCombo_->setCurrentIndex(std::max(0, cmpCombo_->findData(row.cmp_op)));
    turnMaskEdit_->setText(QStringLiteral("0x%1").arg(static_cast<quint32>(row.turn_mask), 0, 16));
    activeCheck_->setChecked((row.flags & static_cast<int>(simcore::pred::PredFlag::Active)) != 0);
    abortCheck_->setChecked((row.flags & static_cast<int>(simcore::pred::PredFlag::AbortOnFail)) != 0);
    //captureCheck_->setChecked((row.flags & static_cast<int>(simcore::pred::PredFlag::CaptureBaseline)) != 0);
    lhsNegateCheck_->setChecked((row.flags & static_cast<int>(simcore::pred::PredFlag::LhsIsNeg)) != 0);
    rhsNegateCheck_->setChecked((row.flags & static_cast<int>(simcore::pred::PredFlag::RhsIsNeg)) != 0);

    lhsAddrEdit_->setText(QStringLiteral("0x%1").arg(static_cast<quint32>(row.lhs_addr), 0, 16));
    rhsValueEdit_->setText(QString::number(row.rhs_value));

    if (row.lhs_prog_id.has_value()) {
        lhsModeCombo_->setCurrentIndex(lhsModeCombo_->findData(static_cast<int>(ValueSourceMode::AddrProgram)));
        const auto lhsProgramResult = simcore::db::AddressProgramRepo::Get(*row.lhs_prog_id);
        if (lhsProgramResult.ok) {
            lhsProgramDraft_.blob = QByteArray(reinterpret_cast<const char*>(lhsProgramResult.value.prog_bytes.data()), static_cast<int>(lhsProgramResult.value.prog_bytes.size()));
            lhsProgramDraft_.description = QString::fromStdString(lhsProgramResult.value.description);
        }
    } else if (row.lhs_key.has_value()) {
        lhsModeCombo_->setCurrentIndex(lhsModeCombo_->findData(static_cast<int>(ValueSourceMode::AddrKey)));
        lhsKeyCombo_->setCurrentIndex(std::max(0, lhsKeyCombo_->findData(*row.lhs_key)));
    } else {
        lhsModeCombo_->setCurrentIndex(lhsModeCombo_->findData(static_cast<int>(ValueSourceMode::AbsoluteAddress)));
    }

    if (row.rhs_prog_id.has_value()) {
        rhsModeCombo_->setCurrentIndex(rhsModeCombo_->findData(static_cast<int>(ValueSourceMode::AddrProgram)));
        const auto rhsProgramResult = simcore::db::AddressProgramRepo::Get(*row.rhs_prog_id);
        if (rhsProgramResult.ok) {
            rhsProgramDraft_.blob = QByteArray(reinterpret_cast<const char*>(rhsProgramResult.value.prog_bytes.data()), static_cast<int>(rhsProgramResult.value.prog_bytes.size()));
            rhsProgramDraft_.description = QString::fromStdString(rhsProgramResult.value.description);
        }
    } else if (row.rhs_key.has_value()) {
        rhsModeCombo_->setCurrentIndex(rhsModeCombo_->findData(static_cast<int>(ValueSourceMode::AddrKey)));
        rhsKeyCombo_->setCurrentIndex(std::max(0, rhsKeyCombo_->findData(*row.rhs_key)));
    } else {
        rhsModeCombo_->setCurrentIndex(rhsModeCombo_->findData(static_cast<int>(ValueSourceMode::Immediate)));
    }

    applyProgramDraftToWidgets(lhsProgramDraft_, true);
    applyProgramDraftToWidgets(rhsProgramDraft_, false);
    refreshUi();
}

QStringList PredicateEditorDialog::validateDraft() const
{
    QStringList errors;

    if (nameEdit_->text().trimmed().isEmpty()) {
        errors << QStringLiteral("Predicate name is required.");
    }
    const QVector<int> requiredBps = selectedRequiredBreakpoints();
    if (requiredBps.isEmpty()) {
        errors << QStringLiteral("Required breakpoint is not set.");
    }

    bool maskOk = false;
    const auto turnMask = turnMaskEdit_->text().trimmed().toUInt(&maskOk, 0);
    Q_UNUSED(turnMask);
    if (!maskOk) {
        errors << QStringLiteral("Turn mask must be a valid numeric value.");
    }

    const auto lhsMode = static_cast<ValueSourceMode>(lhsModeCombo_->currentData().toInt());
    if (lhsMode == ValueSourceMode::AbsoluteAddress) {
        bool lhsOk = false;
        const quint32 lhsAddr = lhsAddrEdit_->text().trimmed().toUInt(&lhsOk, 0);
        if (!lhsOk) {
            errors << QStringLiteral("LHS absolute address must be numeric.");
        } else if (lhsAddr < 0x80000000u || lhsAddr > 0x81FFFFFFu) {
            errors << QStringLiteral("LHS absolute address should be between 0x80000000 and 0x81FFFFFF.");
        }
    } else if (lhsMode == ValueSourceMode::AddrKey) {
        if (lhsKeyCombo_->currentIndex() < 0) {
            errors << QStringLiteral("LHS AddrKey is not set.");
        } else if (!addr::Registry::exists(static_cast<addr::AddrKey>(lhsKeyCombo_->currentData().toInt()))) {
            errors << QStringLiteral("LHS AddrKey does not exist.");
        }
    } else if (lhsMode == ValueSourceMode::AddrProgram && lhsProgramDraft_.blob.isEmpty()) {
        errors << QStringLiteral("LHS address program is empty.");
    }

    const auto rhsMode = static_cast<ValueSourceMode>(rhsModeCombo_->currentData().toInt());
    if (rhsMode == ValueSourceMode::Immediate) {
        bool rhsOk = false;
        rhsValueEdit_->text().trimmed().toULongLong(&rhsOk, 0);
        if (!rhsOk) {
            errors << QStringLiteral("RHS immediate value must be numeric.");
        }
    } else if (rhsMode == ValueSourceMode::AddrKey) {
        if (rhsKeyCombo_->currentIndex() < 0) {
            errors << QStringLiteral("RHS AddrKey is not set.");
        } else if (!addr::Registry::exists(static_cast<addr::AddrKey>(rhsKeyCombo_->currentData().toInt()))) {
            errors << QStringLiteral("RHS AddrKey does not exist.");
        }
    } else if (rhsMode == ValueSourceMode::AddrProgram && rhsProgramDraft_.blob.isEmpty()) {
        errors << QStringLiteral("RHS address program is empty.");
    }

    return errors;
}

PredicateSpecRow PredicateEditorDialog::buildRow(bool* ok, QString* errorText) const
{
    PredicateSpecRow row = row_;
    row.spec_version = static_cast<int32_t>(simcore::pred::SPEC_VERSION);
    row.name = nameEdit_->text().trimmed().left(simcore::pred::PredNameLength).toStdString();
    row.description = descriptionEdit_->toPlainText().trimmed().toStdString();
    const QVector<int> requiredBps = selectedRequiredBreakpoints();
    row.required_bp = requiredBps.isEmpty() ? 0 : requiredBps.front();
    row.required_bp_multi = toBpMultiCsv(requiredBps);
    row.kind = kindCombo_->currentData().toInt();
    row.width = widthCombo_->currentData().toInt();
    row.cmp_op = cmpCombo_->currentData().toInt();
    row.flags = 0;
    row.lhs_key.reset();
    row.rhs_key.reset();
    row.lhs_prog_id.reset();
    row.rhs_prog_id.reset();

    const QStringList errors = validateDraft();
    if (!errors.isEmpty()) {
        if (ok) *ok = false;
        if (errorText) *errorText = errors.join('\n');
        return row;
    }

    row.lhs_addr = lhsAddrEdit_->text().trimmed().toLongLong(nullptr, 0);
    row.rhs_value = rhsValueEdit_->text().trimmed().toULongLong(nullptr, 0);
    row.turn_mask = static_cast<int32_t>(turnMaskEdit_->text().trimmed().toUInt(nullptr, 0));

    if (activeCheck_->isChecked()) row.flags |= static_cast<int>(simcore::pred::PredFlag::Active);
    if (abortCheck_->isChecked()) row.flags |= static_cast<int>(simcore::pred::PredFlag::AbortOnFail);
    //if (captureCheck_->isChecked()) row.flags |= static_cast<int>(simcore::pred::PredFlag::CaptureBaseline);
    if (lhsNegateCheck_->isChecked()) row.flags |= static_cast<int>(simcore::pred::PredFlag::LhsIsNeg);
    if (rhsNegateCheck_->isChecked()) row.flags |= static_cast<int>(simcore::pred::PredFlag::RhsIsNeg);

    simcore::pred::Spec spec{};
    spec.required_bp = static_cast<uint16_t>(row.required_bp);
    spec.required_bps.reserve(requiredBps.size());
    for (const int bp : requiredBps) spec.required_bps.push_back(static_cast<uint16_t>(bp));
    spec.kind = static_cast<simcore::pred::PredKind>(row.kind);
    spec.width = static_cast<uint8_t>(row.width);
    spec.cmp = static_cast<simcore::pred::CmpOp>(row.cmp_op);
    spec.flags = static_cast<uint32_t>(row.flags);
    spec.turn_mask = static_cast<uint32_t>(row.turn_mask);
    spec.name = row.name;
    spec.desc = row.description;

    const auto lhsMode = static_cast<ValueSourceMode>(lhsModeCombo_->currentData().toInt());
    if (lhsMode == ValueSourceMode::AbsoluteAddress) {
        spec.lhs_addr = static_cast<uint32_t>(row.lhs_addr);
    } else if (lhsMode == ValueSourceMode::AddrKey) {
        row.lhs_key = lhsKeyCombo_->currentData().toInt();
        row.lhs_addr = 0;
        spec.lhs_key = static_cast<addr::AddrKey>(*row.lhs_key);
        spec.set_flag(simcore::pred::PredFlag::LhsIsKey);
        row.flags |= static_cast<int>(simcore::pred::PredFlag::LhsIsKey);
    } else {
        spec.lhs_prog = std::vector<uint8_t>(lhsProgramDraft_.blob.begin(), lhsProgramDraft_.blob.end());
        spec.lhs_prog_desc = lhsProgramDraft_.description.toStdString();
        spec.set_flag(simcore::pred::PredFlag::LhsIsProg);
        row.flags |= static_cast<int>(simcore::pred::PredFlag::LhsIsProg);
        row.lhs_addr = 0;
    }

    const auto rhsMode = static_cast<ValueSourceMode>(rhsModeCombo_->currentData().toInt());
    if (rhsMode == ValueSourceMode::Immediate) {
        spec.rhs_value = static_cast<uint64_t>(row.rhs_value);
    } else if (rhsMode == ValueSourceMode::AddrKey) {
        row.rhs_key = rhsKeyCombo_->currentData().toInt();
        row.rhs_value = 0;
        spec.rhs_key = static_cast<addr::AddrKey>(*row.rhs_key);
        spec.set_flag(simcore::pred::PredFlag::RhsIsKey);
        row.flags |= static_cast<int>(simcore::pred::PredFlag::RhsIsKey);
    } else {
        spec.rhs_prog = std::vector<uint8_t>(rhsProgramDraft_.blob.begin(), rhsProgramDraft_.blob.end());
        spec.rhs_prog_desc = rhsProgramDraft_.description.toStdString();
        spec.set_flag(simcore::pred::PredFlag::RhsIsProg);
        row.flags |= static_cast<int>(simcore::pred::PredFlag::RhsIsProg);
        row.rhs_value = 0;
    }

    if (row.flags & static_cast<int>(simcore::pred::PredFlag::LhsIsNeg)) spec.set_flag(simcore::pred::PredFlag::LhsIsNeg);
    if (row.flags & static_cast<int>(simcore::pred::PredFlag::RhsIsNeg)) spec.set_flag(simcore::pred::PredFlag::RhsIsNeg);

    row.fingerprint = simcore::pred::fingerprint(spec);

    if (ok) *ok = true;
    if (errorText) errorText->clear();
    return row;
}

QByteArray PredicateEditorDialog::lhsProgramBlob() const
{
    return lhsProgramDraft_.blob;
}

QString PredicateEditorDialog::lhsProgramDescription() const
{
    return lhsProgramDraft_.description;
}

QByteArray PredicateEditorDialog::rhsProgramBlob() const
{
    return rhsProgramDraft_.blob;
}

QString PredicateEditorDialog::rhsProgramDescription() const
{
    return rhsProgramDraft_.description;
}

void PredicateEditorDialog::accept()
{
    const QStringList errors = validateDraft();
    if (!errors.isEmpty()) {
        setDialogErrors(errors);
        return;
    }
    setDialogErrors({});
    QDialog::accept();
}

void PredicateEditorDialog::setDialogErrors(const QStringList& errors)
{
    errorLabel_->setVisible(!errors.isEmpty());
    errorLabel_->setText(errors.isEmpty() ? QString() : QStringLiteral("Validation Errors:\n• %1").arg(errors.join(QStringLiteral("\n• "))));
}

void PredicateEditorDialog::refreshUi()
{
    const auto lhsMode = static_cast<ValueSourceMode>(lhsModeCombo_->currentData().toInt());
    lhsAddrEdit_->setVisible(lhsMode == ValueSourceMode::AbsoluteAddress);
    lhsKeyCombo_->setVisible(lhsMode == ValueSourceMode::AddrKey);
    lhsProgramKindCombo_->setVisible(lhsMode == ValueSourceMode::AddrProgram);
    lhsProgramAEdit_->setVisible(lhsMode == ValueSourceMode::AddrProgram);
    lhsProgramBEdit_->setVisible(lhsMode == ValueSourceMode::AddrProgram && lhsProgramKindCombo_->currentData().toInt() == static_cast<int>(ProgramKind::EnemyItemAmount));
    lhsBuildProgramButton_->setVisible(lhsMode == ValueSourceMode::AddrProgram);
    lhsProgramSummaryLabel_->setVisible(lhsMode == ValueSourceMode::AddrProgram);
    lhsProgramSummaryLabel_->setText(describeProgramDraft(lhsProgramDraft_));

    const auto rhsMode = static_cast<ValueSourceMode>(rhsModeCombo_->currentData().toInt());
    rhsValueEdit_->setVisible(rhsMode == ValueSourceMode::Immediate);
    rhsKeyCombo_->setVisible(rhsMode == ValueSourceMode::AddrKey);
    rhsProgramKindCombo_->setVisible(rhsMode == ValueSourceMode::AddrProgram);
    rhsProgramAEdit_->setVisible(rhsMode == ValueSourceMode::AddrProgram);
    rhsProgramBEdit_->setVisible(rhsMode == ValueSourceMode::AddrProgram && rhsProgramKindCombo_->currentData().toInt() == static_cast<int>(ProgramKind::EnemyItemAmount));
    rhsBuildProgramButton_->setVisible(rhsMode == ValueSourceMode::AddrProgram);
    rhsProgramSummaryLabel_->setVisible(rhsMode == ValueSourceMode::AddrProgram);
    rhsProgramSummaryLabel_->setText(describeProgramDraft(rhsProgramDraft_));
}
