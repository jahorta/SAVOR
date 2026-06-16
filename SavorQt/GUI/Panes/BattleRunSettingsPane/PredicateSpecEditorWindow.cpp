#include "PredicateSpecEditorWindow.h"

#include "DB/SavorDbAuthoringService.h"
#include "Core/Memory/Soa/SoaAddrCatalog.h"
#include "Core/Memory/Soa/SoaAddrProgramBuilder.h"
#include "Core/Memory/Soa/SoaAddrRegistry.h"
#include "Runner/Breakpoints/BpRegistry.h"
#include "Runner/Breakpoints/Predicate.h"

#include <QtCore/QSignalBlocker>
#include <QtCore/QStringList>
#include <QtGui/QCloseEvent>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <limits>

namespace {

QString programKindLabel(const PredicateSpecEditorWindow::ProgramKind kind)
{
    switch (kind) {
    case PredicateSpecEditorWindow::ProgramKind::None:
        return QStringLiteral("None");
    case PredicateSpecEditorWindow::ProgramKind::TurnOrderIndex:
        return QStringLiteral("Turn order index (derived)");
    case PredicateSpecEditorWindow::ProgramKind::ItemDropAmount:
        return QStringLiteral("Item drop amount (derived)");
    case PredicateSpecEditorWindow::ProgramKind::BattleTreasureSlotAmount:
        return QStringLiteral("Battle treasure slot -> amount");
    case PredicateSpecEditorWindow::ProgramKind::EnemyItemAmount:
        return QStringLiteral("Enemy item -> amount");
    }
    return QStringLiteral("None");
}

QString valueSourceLabel(const PredicateSpecEditorWindow::ValueSourceMode mode, const bool lhs)
{
    switch (mode) {
    case PredicateSpecEditorWindow::ValueSourceMode::AbsoluteAddress:
        return QStringLiteral("Absolute Addr (Mem1)");
    case PredicateSpecEditorWindow::ValueSourceMode::AddrKey:
        return QStringLiteral("AddrKey");
    case PredicateSpecEditorWindow::ValueSourceMode::AddrProgram:
        return QStringLiteral("AddrProgram");
    case PredicateSpecEditorWindow::ValueSourceMode::Baseline:
        return QStringLiteral("Baseline");
    case PredicateSpecEditorWindow::ValueSourceMode::Immediate:
        return lhs ? QStringLiteral("Absolute Addr (Mem1)") : QStringLiteral("Immediate");
    }
    return lhs ? QStringLiteral("Absolute Addr (Mem1)") : QStringLiteral("Immediate");
}

QString describeProgramDraft(const PredicateSpecEditorWindow::ProgramDraft& draft)
{
    if (draft.blob.isEmpty()) {
        return QStringLiteral("No address program built.");
    }
    return QStringLiteral("%1 · %2 bytes%3")
        .arg(programKindLabel(draft.kind))
        .arg(draft.blob.size())
        .arg(draft.description.isEmpty() ? QString() : QStringLiteral(" · %1").arg(draft.description));
}

void setFormRowVisibility(QFormLayout* layout, QWidget* field, const bool visible)
{
    if (layout == nullptr || field == nullptr) {
        return;
    }
    QWidget* label = layout->labelForField(field);
    if (label != nullptr) {
        label->setVisible(visible);
    }
    field->setVisible(visible);
}

bool parseSignedInteger(const QString& text, std::int64_t& value)
{
    bool ok = false;
    value = text.toLongLong(&ok, 0);
    return ok;
}

bool parseUnsignedInteger(const QString& text, std::uint64_t& value)
{
    bool ok = false;
    value = text.toULongLong(&ok, 0);
    return ok;
}

QStringList errorsToTextLines(const std::vector<QString>& errors)
{
    QStringList lines;
    for (const auto& error : errors) {
        if (!error.isEmpty()) {
            lines << error;
        }
    }
    return lines;
}

} // namespace

PredicateSpecEditorWindow::PredicateSpecEditorWindow(QWidget* parent, bool embeddedInContainer)
    : QWidget(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlag(Qt::Window, !embeddedInContainer);
    setWindowTitle(QStringLiteral("Predicate Editor"));
    resize(980, 640);
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

void PredicateSpecEditorWindow::loadSnapshot(const savor::db::PredicateSpecSnapshot& snapshot, bool duplicate)
{
    predicateSpecId_.reset();
    bool saveAsCopy = duplicate;
    if (!duplicate) {
        const auto usage = savorqt::db::SavorDbAuthoringService::GetPredicateSpecUsage(snapshot.predicate_spec_id);
        if (!usage.ok) {
            saveAsCopy = true;
            postStatusMessage(QString::fromStdString(usage.error.message), StatusToast::Severity::Warn);
        } else if (usage.value.used()) {
            saveAsCopy = true;
            postStatusMessage(
                QStringLiteral("Predicate is used by %1 predicate set(s); edits will save as a copy.")
                    .arg(usage.value.predicate_set_count),
                StatusToast::Severity::Warn);
        } else {
            predicateSpecId_ = snapshot.predicate_spec_id;
        }
    }

    if (predicateSpecId_.has_value()) {
        setWindowTitle(QStringLiteral("Predicate Editor - Edit"));
        if (saveButton_ != nullptr) {
            saveButton_->setText(QStringLiteral("Save Predicate In Place"));
        }
    } else {
        setWindowTitle(duplicate
            ? QStringLiteral("Predicate Editor - Duplicate")
            : QStringLiteral("Predicate Editor - Edit Copy"));
        if (saveButton_ != nullptr) {
            saveButton_->setText(QStringLiteral("Save Predicate Copy"));
        }
    }
    nameEdit_->setText(QString::fromStdString(snapshot.name) + (saveAsCopy ? QStringLiteral(" copy") : QString()));

    while (breakpointCombos_.size() > 1) {
        removeRequiredBreakpointField(static_cast<int>(breakpointCombos_.size()) - 1);
    }
    if (breakpointCombos_.empty()) {
        addRequiredBreakpointField();
    }
    breakpointCombos_[0]->setCurrentIndex(std::max(0, breakpointCombos_[0]->findData(snapshot.breakpoint_id)));

    while (baselineBreakpointCombos_.size() > 1) {
        removeBaselineBreakpointField(static_cast<int>(baselineBreakpointCombos_.size()) - 1);
    }
    if (baselineBreakpointCombos_.empty()) {
        addBaselineBreakpointField();
    }
    const auto baselineBps = snapshot.baseline_breakpoint_ids;
    baselineBreakpointCombos_[0]->setCurrentIndex(std::max(0, baselineBreakpointCombos_[0]->findData(
        baselineBps.empty() ? 0 : static_cast<int>(baselineBps.front()))));
    for (size_t i = 1; i < baselineBps.size(); ++i) {
        addBaselineBreakpointField(static_cast<int>(baselineBps[i]));
    }

    lhsValueEdit_->setText(QString::number(snapshot.lhs_value));
    rhsValueEdit_->setText(QString::number(snapshot.rhs_value));

    setComparison(snapshot.cmp_op);
    widthCombo_->setCurrentIndex(std::max(0, widthCombo_->findData(snapshot.width)));

    const std::uint32_t flags = static_cast<std::uint32_t>(snapshot.flag_mask.value_or(0));
    abortOnFailCheck_->setChecked(snapshot.abort_on_fail);
    activeCheck_->setChecked((flags & static_cast<std::uint32_t>(savor::pred::PredFlag::Active)) != 0);
    lhsNegateCheck_->setChecked((flags & static_cast<std::uint32_t>(savor::pred::PredFlag::LhsIsNeg)) != 0);
    rhsNegateCheck_->setChecked((flags & static_cast<std::uint32_t>(savor::pred::PredFlag::RhsIsNeg)) != 0);

    turnMaskEdit_->setText(QStringLiteral("0x%1").arg(static_cast<std::uint32_t>(snapshot.value_mask.value_or(0xFFFFFFFFu)), 0, 16));

    lhsProgramDraft_ = {};
    rhsProgramDraft_ = {};

    const ValueSourceMode lhsMode = snapshot.lhs_address_program_id.has_value()
        ? ValueSourceMode::AddrProgram
        : ((flags & static_cast<std::uint32_t>(savor::pred::PredFlag::LhsIsKey)) != 0
            ? ValueSourceMode::AddrKey
            : ValueSourceMode::AbsoluteAddress);
    const bool rhsIsDelta = (flags & static_cast<std::uint32_t>(savor::pred::PredFlag::RhsIsDelta)) != 0;
    const ValueSourceMode rhsMode = rhsIsDelta
        ? ValueSourceMode::Baseline
        : (snapshot.rhs_address_program_id.has_value()
            ? ValueSourceMode::AddrProgram
            : ((flags & static_cast<std::uint32_t>(savor::pred::PredFlag::RhsIsKey)) != 0
                ? ValueSourceMode::AddrKey
                : ValueSourceMode::Immediate));

    lhsModeCombo_->setCurrentIndex(std::max(0, lhsModeCombo_->findData(static_cast<int>(lhsMode))));
    rhsModeCombo_->setCurrentIndex(std::max(0, rhsModeCombo_->findData(static_cast<int>(rhsMode))));

    if (lhsMode == ValueSourceMode::AddrKey) {
        lhsKeyCombo_->setCurrentIndex(std::max(0, lhsKeyCombo_->findData(static_cast<int>(snapshot.lhs_value))));
    }
    if (rhsMode == ValueSourceMode::AddrKey) {
        rhsKeyCombo_->setCurrentIndex(std::max(0, rhsKeyCombo_->findData(static_cast<int>(snapshot.rhs_value))));
    }

    if (snapshot.lhs_address_program_id.has_value()) {
        const auto lhsProgramResult = savorqt::db::SavorDbAuthoringService::GetAddressProgram(*snapshot.lhs_address_program_id);
        if (lhsProgramResult.ok) {
            lhsProgramDraft_.blob = QByteArray(
                reinterpret_cast<const char*>(lhsProgramResult.value.prog_bytes.data()),
                static_cast<int>(lhsProgramResult.value.prog_bytes.size()));
            lhsProgramDraft_.description = QString::fromStdString(lhsProgramResult.value.description);
        }
    }

    if (snapshot.rhs_address_program_id.has_value()) {
        const auto rhsProgramResult = savorqt::db::SavorDbAuthoringService::GetAddressProgram(*snapshot.rhs_address_program_id);
        if (rhsProgramResult.ok) {
            rhsProgramDraft_.blob = QByteArray(
                reinterpret_cast<const char*>(rhsProgramResult.value.prog_bytes.data()),
                static_cast<int>(rhsProgramResult.value.prog_bytes.size()));
            rhsProgramDraft_.description = QString::fromStdString(rhsProgramResult.value.description);
        }
    }

    applyProgramDraftToWidgets(lhsProgramDraft_, true);
    applyProgramDraftToWidgets(rhsProgramDraft_, false);
    refreshUi();
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
    populateAddrKeys();

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto* panel = new QFrame(this);
    panel->setObjectName("jobsSurfacePanel");
    auto* panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(12, 12, 12, 12);
    panelLayout->setSpacing(10);

    contentLayout_ = panelLayout;
    createCoreSection();
    createExecutionSection();
    createFlagsSection();
    createMatchSection();

    auto* buttonRow = new QHBoxLayout();
    buttonRow->addStretch();
    saveButton_ = new QPushButton(QStringLiteral("Save Predicate"), panel);
    saveButton_->setObjectName("jobsPrimaryButton");
    buttonRow->addWidget(saveButton_);
    panelLayout->addLayout(buttonRow);

    root->addWidget(panel, 1);

    connect(saveButton_, &QPushButton::clicked, this, &PredicateSpecEditorWindow::savePredicate);

    const auto markDirtyAndRefresh = [this]() {
        markDirty();
    };
    for (auto* combo : {
             widthCombo_, lhsModeCombo_, rhsModeCombo_, lhsKeyCombo_, rhsKeyCombo_,
             lhsProgramKindCombo_, rhsProgramKindCombo_ }) {
        if (combo != nullptr) {
            connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this, markDirtyAndRefresh);
        }
    }
    for (auto* checkbox : {
             abortOnFailCheck_, activeCheck_, lhsNegateCheck_, rhsNegateCheck_ }) {
        if (checkbox != nullptr) {
            connect(checkbox, &QCheckBox::toggled, this, markDirtyAndRefresh);
        }
    }
    for (auto* line : {
             nameEdit_, lhsValueEdit_, rhsValueEdit_, turnMaskEdit_, lhsProgramAEdit_, lhsProgramBEdit_, rhsProgramAEdit_, rhsProgramBEdit_ }) {
        if (line != nullptr) {
            connect(line, &QLineEdit::textChanged, this, markDirtyAndRefresh);
        }
    }
    for (auto* button : { addRequiredBpButton_, addBaselineBpButton_, lhsBuildProgramButton_, rhsBuildProgramButton_ }) {
        if (button != nullptr) {
            connect(button, &QPushButton::clicked, this, markDirtyAndRefresh);
        }
    }

    refreshUi();
    abortOnFailCheck_->setChecked(false);
    activeCheck_->setChecked(true);
    turnMaskEdit_->setText(QStringLiteral("0xFFFFFFFF"));
    turnMaskEdit_->setPlaceholderText(QStringLiteral("0xFFFFFFFF"));
}

void PredicateSpecEditorWindow::createCoreSection()
{
    auto* coreBox = new QGroupBox(QStringLiteral("Core"), this);
    auto* coreForm = new QFormLayout(coreBox);
    coreForm->setContentsMargins(12, 12, 12, 12);
    coreForm->setSpacing(8);

    nameEdit_ = new QLineEdit(coreBox);

    widthCombo_ = new QComboBox(coreBox);
    widthCombo_->addItem(QStringLiteral("1"), 1);
    widthCombo_->addItem(QStringLiteral("2"), 2);
    widthCombo_->addItem(QStringLiteral("4"), 4);
    widthCombo_->addItem(QStringLiteral("8"), 8);
    widthCombo_->setCurrentIndex(widthCombo_->findData(4));

    coreForm->addRow(QStringLiteral("Name"), nameEdit_);
    coreForm->addRow(QStringLiteral("Width"), widthCombo_);
    contentLayout_->addWidget(coreBox);
}

void PredicateSpecEditorWindow::createExecutionSection()
{
    auto* executionBox = new QGroupBox(QStringLiteral("Execution"), this);
    auto* executionLayout = new QFormLayout(executionBox);
    executionLayout->setContentsMargins(12, 12, 12, 12);
    executionLayout->setSpacing(8);

    requiredBpRowsWidget_ = new QWidget(executionBox);
    requiredBpRowsLayout_ = new QVBoxLayout(requiredBpRowsWidget_);
    requiredBpRowsLayout_->setContentsMargins(0, 0, 0, 0);
    requiredBpRowsLayout_->setSpacing(6);

    addRequiredBreakpointField();
    addRequiredBpButton_ = new QPushButton(QStringLiteral("Add required breakpoint"), executionBox);
    addRequiredBpButton_->setObjectName("jobsSecondaryButton");
    connect(addRequiredBpButton_, &QPushButton::clicked, this, [this]() { addRequiredBreakpointField(); });

    turnMaskEdit_ = new QLineEdit(executionBox);
    turnMaskEdit_->setPlaceholderText(QStringLiteral("0xFFFFFFFF"));
    turnMaskEdit_->setText(QStringLiteral("0xFFFFFFFF"));

    executionLayout->addRow(QStringLiteral("Required breakpoints"), requiredBpRowsWidget_);
    executionLayout->addRow(QString(), addRequiredBpButton_);
    executionLayout->addRow(QStringLiteral("Turn mask"), turnMaskEdit_);

    contentLayout_->addWidget(executionBox);
}

void PredicateSpecEditorWindow::createFlagsSection()
{
    auto* flagsBox = new QGroupBox(QStringLiteral("Flags"), this);
    auto* flagsLayout = new QHBoxLayout(flagsBox);
    flagsLayout->setContentsMargins(12, 12, 12, 12);
    activeCheck_ = new QCheckBox(QStringLiteral("Active"), flagsBox);
    abortOnFailCheck_ = new QCheckBox(QStringLiteral("Abort on fail"), flagsBox);
    lhsNegateCheck_ = new QCheckBox(QStringLiteral("Negate LHS"), flagsBox);
    rhsNegateCheck_ = new QCheckBox(QStringLiteral("Negate RHS"), flagsBox);
    flagsLayout->addWidget(activeCheck_);
    flagsLayout->addWidget(lhsNegateCheck_);
    flagsLayout->addWidget(rhsNegateCheck_);
    flagsLayout->addWidget(abortOnFailCheck_);
    flagsLayout->addStretch();
    contentLayout_->addWidget(flagsBox);
}

void PredicateSpecEditorWindow::createMatchSection()
{
    auto* matchContainer = new QWidget(this);
    matchContainer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* matchLayout = new QHBoxLayout(matchContainer);
    matchLayout->setContentsMargins(0, 0, 0, 0);
    matchLayout->setSpacing(10);

    auto buildValueBox = [this](const QString& title, const bool lhs) -> QGroupBox* {
        auto* box = new QGroupBox(title, this);
        box->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        auto* layout = new QFormLayout(box);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(8);

        QFormLayout*& matchLayoutRef = lhs ? lhsMatchLayout_ : rhsMatchLayout_;
        matchLayoutRef = layout;

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
            modeCombo->addItem(valueSourceLabel(ValueSourceMode::Baseline, false), static_cast<int>(ValueSourceMode::Baseline));
        }
        connect(modeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() { refreshUi(); markDirty(); });
        layout->addRow(QStringLiteral("Source"), modeCombo);

        QLineEdit*& valueEdit = lhs ? lhsValueEdit_ : rhsValueEdit_;
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
        connect(programKindCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() { refreshUi(); markDirty(); });
        layout->addRow(QStringLiteral("Program"), programKindCombo);

        QLineEdit*& programAEdit = lhs ? lhsProgramAEdit_ : rhsProgramAEdit_;
        programAEdit = new QLineEdit(box);
        layout->addRow(QStringLiteral("Program arg A"), programAEdit);
        QLineEdit*& programBEdit = lhs ? lhsProgramBEdit_ : rhsProgramBEdit_;
        programBEdit = new QLineEdit(box);
        layout->addRow(QStringLiteral("Program arg B"), programBEdit);

        QPushButton*& buildButton = lhs ? lhsBuildProgramButton_ : rhsBuildProgramButton_;
        buildButton = new QPushButton(QStringLiteral("Build program"), box);
        connect(buildButton, &QPushButton::clicked, this, [this, lhs]() { buildProgram(lhs); });
        layout->addRow(QString(), buildButton);

        QLabel*& summaryLabel = lhs ? lhsProgramSummaryLabel_ : rhsProgramSummaryLabel_;
        summaryLabel = new QLabel(QStringLiteral("No address program built."), box);
        summaryLabel->setWordWrap(true);
        layout->addRow(QStringLiteral("Program status"), summaryLabel);

        if (!lhs) {
            baselineBpRowsWidget_ = new QWidget(box);
            baselineBpRowsLayout_ = new QVBoxLayout(baselineBpRowsWidget_);
            baselineBpRowsLayout_->setContentsMargins(0, 0, 0, 0);
            baselineBpRowsLayout_->setSpacing(6);
            addBaselineBreakpointField();
            addBaselineBpButton_ = new QPushButton(QStringLiteral("Add baseline breakpoint"), box);
            addBaselineBpButton_->setObjectName("jobsSecondaryButton");
            connect(addBaselineBpButton_, &QPushButton::clicked, this, [this]() { addBaselineBreakpointField(); });
            layout->addRow(QStringLiteral("Baseline breakpoints"), baselineBpRowsWidget_);
            layout->addRow(QString(), addBaselineBpButton_);
        }

        return box;
    };

    auto* compareBox = new QGroupBox(QStringLiteral("Compare"), matchContainer);
    auto* compareLayout = new QVBoxLayout(compareBox);
    compareLayout->setContentsMargins(12, 12, 12, 12);
    compareLayout->setSpacing(6);
    compareLayout->setAlignment(Qt::AlignCenter);

    cmpButtonGroup_ = new QButtonGroup(compareBox);
    cmpButtonGroup_->setExclusive(true);

    const auto addCompareButton = [this, compareLayout, compareBox](const QString& text, const savor::db::PredicateComparisonOp op) {
        auto* compareButton = new QPushButton(text, compareBox);
        compareButton->setCheckable(true);
        compareButton->setObjectName("predicateCompareButton");
        cmpButtonGroup_->addButton(compareButton, static_cast<int>(op));
        compareLayout->addWidget(compareButton);
        connect(compareButton, &QPushButton::toggled, this, [this](const bool checked) {
            if (checked) {
                markDirty();
            }
        });
    };

    addCompareButton(QStringLiteral("=="), savor::db::PredicateComparisonOp::EQ);
    addCompareButton(QStringLiteral("!="), savor::db::PredicateComparisonOp::NE);
    addCompareButton(QStringLiteral("<"), savor::db::PredicateComparisonOp::LT);
    addCompareButton(QStringLiteral("<="), savor::db::PredicateComparisonOp::LE);
    addCompareButton(QStringLiteral(">"), savor::db::PredicateComparisonOp::GT);
    addCompareButton(QStringLiteral(">="), savor::db::PredicateComparisonOp::GE);
    compareLayout->addStretch();

    setComparison(savor::db::PredicateComparisonOp::EQ);

    matchLayout->addWidget(buildValueBox(QStringLiteral("LHS"), true), 1);
    matchLayout->addWidget(compareBox);
    matchLayout->addWidget(buildValueBox(QStringLiteral("RHS"), false), 1);
    contentLayout_->addWidget(matchContainer, 1);
}

void PredicateSpecEditorWindow::populateAddrKeys()
{
    addrKeys_.clear();
    addrNames_.clear();
    for (const auto& rec : addr::AddrRegistry::all()) {
        addrKeys_.push_back(static_cast<int>(rec.key));
        addrNames_.push_back(QString::fromUtf8(rec.name));
    }
}

void PredicateSpecEditorWindow::populateBreakpointCombo(QComboBox* combo) const
{
    if (combo == nullptr) {
        return;
    }
    combo->clear();
    combo->addItem(QStringLiteral("(none)"), 0);
    for (const BPAddr& bp : bp::BpRegistry::all()) {
        combo->addItem(QStringLiteral("%1 @ 0x%2")
                           .arg(QString::fromUtf8(bp.name))
                           .arg(bp.pc, 8, 16, QLatin1Char('0')),
                       static_cast<int>(bp.key));
    }
}

void PredicateSpecEditorWindow::addRequiredBreakpointField(const int selectedBp)
{
    if (requiredBpRowsLayout_ == nullptr || requiredBpRowsWidget_ == nullptr) {
        return;
    }
    auto* rowLayout = new QHBoxLayout();
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(6);

    auto* combo = new QComboBox(requiredBpRowsWidget_);
    populateBreakpointCombo(combo);
    combo->setCurrentIndex(std::max(0, combo->findData(selectedBp)));
    rowLayout->addWidget(combo, 1);

    auto* removeButton = new QPushButton(QStringLiteral("Delete"), requiredBpRowsWidget_);
    rowLayout->addWidget(removeButton);

    breakpointCombos_.push_back(combo);
    removeBreakpointButtons_.push_back(removeButton);
    breakpointRowLayouts_.push_back(rowLayout);
    requiredBpRowsLayout_->addLayout(rowLayout);

    connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() { markDirty(); });
    connect(removeButton, &QPushButton::clicked, this, [this, removeButton]() {
        const auto index = std::find(removeBreakpointButtons_.begin(), removeBreakpointButtons_.end(), removeButton);
        if (index != removeBreakpointButtons_.end()) {
            const int rowIndex = static_cast<int>(std::distance(removeBreakpointButtons_.begin(), index));
            removeRequiredBreakpointField(rowIndex);
        }
    });

    rebuildRequiredBreakpointRows();
    markDirty();
}

void PredicateSpecEditorWindow::removeRequiredBreakpointField(const int index)
{
    if (index < 0 || index >= static_cast<int>(breakpointCombos_.size()) || breakpointCombos_.size() <= 1) {
        return;
    }
    QComboBox* combo = breakpointCombos_[index];
    QPushButton* button = removeBreakpointButtons_[index];
    QHBoxLayout* rowLayout = breakpointRowLayouts_[index];

    requiredBpRowsLayout_->removeItem(rowLayout);
    breakpointCombos_.erase(breakpointCombos_.begin() + index);
    removeBreakpointButtons_.erase(removeBreakpointButtons_.begin() + index);
    breakpointRowLayouts_.erase(breakpointRowLayouts_.begin() + index);

    delete combo;
    delete button;
    delete rowLayout;
    rebuildRequiredBreakpointRows();
    markDirty();
}

void PredicateSpecEditorWindow::rebuildRequiredBreakpointRows()
{
    const bool showDelete = breakpointCombos_.size() > 1;
    for (auto* button : removeBreakpointButtons_) {
        button->setVisible(showDelete);
    }
}

void PredicateSpecEditorWindow::setComparison(const savor::db::PredicateComparisonOp op)
{
    const int opId = static_cast<int>(op);
    if (cmpButtonGroup_ == nullptr) {
        return;
    }
    QSignalBlocker blocker(cmpButtonGroup_);
    auto* button = cmpButtonGroup_->button(opId);
    if (button == nullptr) {
        auto* defaultButton = cmpButtonGroup_->button(static_cast<int>(savor::db::PredicateComparisonOp::EQ));
        if (defaultButton != nullptr) {
            defaultButton->setChecked(true);
        }
        return;
    }
    button->setChecked(true);
}

savor::db::PredicateComparisonOp PredicateSpecEditorWindow::comparison() const
{
    if (cmpButtonGroup_ == nullptr) {
        return savor::db::PredicateComparisonOp::EQ;
    }
    auto* checked = cmpButtonGroup_->checkedButton();
    if (checked == nullptr) {
        return savor::db::PredicateComparisonOp::EQ;
    }
    return static_cast<savor::db::PredicateComparisonOp>(cmpButtonGroup_->id(checked));
}

std::vector<int> PredicateSpecEditorWindow::selectedRequiredBreakpoints() const
{
    std::vector<int> out;
    out.reserve(breakpointCombos_.size());
    for (auto* combo : breakpointCombos_) {
        const int value = combo != nullptr ? combo->currentData().toInt() : 0;
        if (value > 0) {
            out.push_back(value);
        }
    }
    return out;
}

void PredicateSpecEditorWindow::addBaselineBreakpointField(const int selectedBp)
{
    if (baselineBpRowsLayout_ == nullptr || baselineBpRowsWidget_ == nullptr) {
        return;
    }
    auto* rowLayout = new QHBoxLayout();
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(6);

    auto* combo = new QComboBox(baselineBpRowsWidget_);
    populateBreakpointCombo(combo);
    combo->setCurrentIndex(std::max(0, combo->findData(selectedBp)));
    rowLayout->addWidget(combo, 1);

    auto* removeButton = new QPushButton(QStringLiteral("Delete"), baselineBpRowsWidget_);
    rowLayout->addWidget(removeButton);

    baselineBreakpointCombos_.push_back(combo);
    removeBaselineBreakpointButtons_.push_back(removeButton);
    baselineBreakpointRowLayouts_.push_back(rowLayout);
    baselineBpRowsLayout_->addLayout(rowLayout);

    connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() { markDirty(); });
    connect(removeButton, &QPushButton::clicked, this, [this, removeButton]() {
        const auto index = std::find(removeBaselineBreakpointButtons_.begin(), removeBaselineBreakpointButtons_.end(), removeButton);
        if (index != removeBaselineBreakpointButtons_.end()) {
            const int rowIndex = static_cast<int>(std::distance(removeBaselineBreakpointButtons_.begin(), index));
            removeBaselineBreakpointField(rowIndex);
        }
    });

    rebuildBaselineBreakpointRows();
    markDirty();
}

void PredicateSpecEditorWindow::removeBaselineBreakpointField(const int index)
{
    if (index < 0 || index >= static_cast<int>(baselineBreakpointCombos_.size()) || baselineBreakpointCombos_.size() <= 1) {
        return;
    }
    QComboBox* combo = baselineBreakpointCombos_[index];
    QPushButton* button = removeBaselineBreakpointButtons_[index];
    QHBoxLayout* rowLayout = baselineBreakpointRowLayouts_[index];

    baselineBpRowsLayout_->removeItem(rowLayout);
    baselineBreakpointCombos_.erase(baselineBreakpointCombos_.begin() + index);
    removeBaselineBreakpointButtons_.erase(removeBaselineBreakpointButtons_.begin() + index);
    baselineBreakpointRowLayouts_.erase(baselineBreakpointRowLayouts_.begin() + index);

    delete combo;
    delete button;
    delete rowLayout;
    rebuildBaselineBreakpointRows();
    markDirty();
}

void PredicateSpecEditorWindow::rebuildBaselineBreakpointRows()
{
    const bool showDelete = baselineBreakpointCombos_.size() > 1;
    for (auto* button : removeBaselineBreakpointButtons_) {
        button->setVisible(showDelete);
    }
}

std::vector<int> PredicateSpecEditorWindow::selectedBaselineBreakpoints() const
{
    std::vector<int> out;
    out.reserve(baselineBreakpointCombos_.size());
    for (auto* combo : baselineBreakpointCombos_) {
        const int value = combo != nullptr ? combo->currentData().toInt() : 0;
        if (value > 0) {
            out.push_back(value);
        }
    }
    return out;
}

void PredicateSpecEditorWindow::applyProgramDraftToWidgets(const ProgramDraft& draft, const bool lhs)
{
    QComboBox* kindCombo = lhs ? lhsProgramKindCombo_ : rhsProgramKindCombo_;
    QLineEdit* aEdit = lhs ? lhsProgramAEdit_ : rhsProgramAEdit_;
    QLineEdit* bEdit = lhs ? lhsProgramBEdit_ : rhsProgramBEdit_;
    QLabel* summaryLabel = lhs ? lhsProgramSummaryLabel_ : rhsProgramSummaryLabel_;
    if (kindCombo != nullptr) {
        kindCombo->setCurrentIndex(std::max(0, kindCombo->findData(static_cast<int>(draft.kind))));
    }
    if (aEdit != nullptr) {
        aEdit->setText(QString::number(draft.a));
    }
    if (bEdit != nullptr) {
        bEdit->setText(QString::number(draft.b));
    }
    if (summaryLabel != nullptr) {
        summaryLabel->setText(describeProgramDraft(draft));
    }
}

PredicateSpecEditorWindow::ProgramDraft PredicateSpecEditorWindow::programDraftFromWidgets(const bool lhs) const
{
    ProgramDraft draft = lhs ? lhsProgramDraft_ : rhsProgramDraft_;
    const QComboBox* kindCombo = lhs ? lhsProgramKindCombo_ : rhsProgramKindCombo_;
    const QLineEdit* aEdit = lhs ? lhsProgramAEdit_ : rhsProgramAEdit_;
    const QLineEdit* bEdit = lhs ? lhsProgramBEdit_ : rhsProgramBEdit_;
    if (kindCombo != nullptr) {
        draft.kind = static_cast<ProgramKind>(kindCombo->currentData().toInt());
    }
    bool ok = false;
    const auto a = aEdit != nullptr ? aEdit->text().trimmed().toUInt(&ok, 0) : 0;
    if (ok) {
        draft.a = static_cast<std::uint16_t>(a);
    } else {
        draft.a = 0;
    }
    const auto b = bEdit != nullptr ? bEdit->text().trimmed().toUInt(&ok, 0) : 0;
    if (ok) {
        draft.b = static_cast<std::uint16_t>(b);
    } else {
        draft.b = 0;
    }
    return draft;
}

void PredicateSpecEditorWindow::buildProgram(const bool lhs)
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
        if (!builder.blob().empty()) {
            draft.blob = QByteArray(reinterpret_cast<const char*>(builder.blob().data()), static_cast<int>(builder.blob().size()));
            draft.description = QString::fromStdString(desc);
        } else {
            postStatusMessage(QStringLiteral("Program has no output bytes."), StatusToast::Severity::Warn);
        }
    } else {
        postStatusMessage(QStringLiteral("No program kind selected."), StatusToast::Severity::Warn);
    }

    if (lhs) {
        lhsProgramDraft_ = draft;
    } else {
        rhsProgramDraft_ = draft;
    }
    applyProgramDraftToWidgets(draft, lhs);
    refreshUi();
    markDirty();
}

std::vector<QString> PredicateSpecEditorWindow::validateDraft() const
{
    std::vector<QString> errors;

    if (nameEdit_ == nullptr || nameEdit_->text().trimmed().isEmpty()) {
        errors.push_back(QStringLiteral("Predicate name is required."));
    }

    const auto breakpoints = selectedRequiredBreakpoints();
    if (breakpoints.empty()) {
        errors.push_back(QStringLiteral("At least one required breakpoint is required."));
    }

    std::uint64_t turnMask = 0;
    if (turnMaskEdit_ == nullptr || !parseUnsignedInteger(turnMaskEdit_->text().trimmed(), turnMask)) {
        errors.push_back(QStringLiteral("Turn mask must be a valid numeric value."));
    }

    const auto lhsMode = static_cast<ValueSourceMode>(lhsModeCombo_ != nullptr ? lhsModeCombo_->currentData().toInt() : static_cast<int>(ValueSourceMode::AbsoluteAddress));
    if (lhsMode == ValueSourceMode::AbsoluteAddress) {
        std::int64_t lhsValue = 0;
        if (lhsValueEdit_ == nullptr || !parseSignedInteger(lhsValueEdit_->text().trimmed(), lhsValue)) {
            errors.push_back(QStringLiteral("LHS absolute value must be numeric."));
        } else if (lhsValue < 0x80000000ll || lhsValue > 0x81FFFFFFll) {
            errors.push_back(QStringLiteral("LHS absolute address should be between 0x80000000 and 0x81FFFFFF."));
        }
    } else if (lhsMode == ValueSourceMode::AddrKey) {
        const int keyValue = lhsKeyCombo_ != nullptr ? lhsKeyCombo_->currentData().toInt() : 0;
        if (lhsKeyCombo_ == nullptr || keyValue <= 0 || !addr::AddrRegistry::exists(static_cast<addr::AddrKey>(keyValue))) {
            errors.push_back(QStringLiteral("LHS AddrKey is not set or invalid."));
        }
    } else if (lhsMode == ValueSourceMode::AddrProgram && lhsProgramDraft_.blob.isEmpty()) {
        errors.push_back(QStringLiteral("LHS address program is empty. Build the program first."));
    }

    const auto rhsMode = static_cast<ValueSourceMode>(rhsModeCombo_ != nullptr ? rhsModeCombo_->currentData().toInt() : static_cast<int>(ValueSourceMode::Immediate));
    const bool rhsIsBaseline = rhsMode == ValueSourceMode::Baseline;
    if (rhsIsBaseline && selectedBaselineBreakpoints().empty()) {
        errors.push_back(QStringLiteral("RHS baseline mode requires at least one baseline breakpoint."));
    }

    if (rhsIsBaseline) {
        return errors;
    }
    if (rhsMode == ValueSourceMode::Immediate) {
        std::int64_t rhsValue = 0;
        if (rhsValueEdit_ == nullptr || !parseSignedInteger(rhsValueEdit_->text().trimmed(), rhsValue)) {
            errors.push_back(QStringLiteral("RHS immediate value must be numeric."));
        }
    } else if (rhsMode == ValueSourceMode::AddrKey) {
        const int keyValue = rhsKeyCombo_ != nullptr ? rhsKeyCombo_->currentData().toInt() : 0;
        if (rhsKeyCombo_ == nullptr || keyValue <= 0 || !addr::AddrRegistry::exists(static_cast<addr::AddrKey>(keyValue))) {
            errors.push_back(QStringLiteral("RHS AddrKey is not set or invalid."));
        }
    } else if (rhsMode == ValueSourceMode::AddrProgram && rhsProgramDraft_.blob.isEmpty()) {
        errors.push_back(QStringLiteral("RHS address program is empty. Build the program first."));
    }

    return errors;
}

void PredicateSpecEditorWindow::savePredicate()
{
    const auto errors = validateDraft();
    if (!errors.empty()) {
        const QStringList lines = errorsToTextLines(errors);
        const QString text = QStringLiteral("Validation errors:\n- %1").arg(lines.join(QStringLiteral("\n- ")));
        postStatusMessage(text, StatusToast::Severity::Warn);
        return;
    }

    const auto requiredBreakpoints = selectedRequiredBreakpoints();
    const auto baselineBreakpoints = selectedBaselineBreakpoints();
    const auto lhsMode = static_cast<ValueSourceMode>(lhsModeCombo_ != nullptr ? lhsModeCombo_->currentData().toInt() : static_cast<int>(ValueSourceMode::AbsoluteAddress));
    const auto rhsMode = static_cast<ValueSourceMode>(rhsModeCombo_ != nullptr ? rhsModeCombo_->currentData().toInt() : static_cast<int>(ValueSourceMode::Immediate));

    savorqt::db::PredicateSpecDraft draft{};
    draft.name = nameEdit_ != nullptr ? nameEdit_->text().trimmed().toStdString() : std::string();
    draft.breakpoint_id = requiredBreakpoints.empty() ? 0 : requiredBreakpoints.front();
    draft.baseline_breakpoint_ids.reserve(baselineBreakpoints.size());
    for (const auto breakpoint : baselineBreakpoints) {
        draft.baseline_breakpoint_ids.push_back(static_cast<BPKey>(breakpoint));
    }
    draft.cmp_op = comparison();
    draft.width = widthCombo_ != nullptr ? widthCombo_->currentData().toInt() : 4;

    std::uint64_t turnMask = 0;
    parseUnsignedInteger(turnMaskEdit_->text().trimmed(), turnMask);
    if (turnMask > std::numeric_limits<std::uint32_t>::max()) {
        turnMask = 0xFFFFFFFFu;
    }
    draft.value_mask = static_cast<std::int64_t>(turnMask);

    std::uint32_t flags = 0;
    if (activeCheck_ != nullptr && activeCheck_->isChecked()) {
        flags |= static_cast<std::uint32_t>(savor::pred::PredFlag::Active);
    }
    const bool rhsIsBaseline = rhsMode == ValueSourceMode::Baseline;
    if (rhsIsBaseline) {
        flags |= static_cast<std::uint32_t>(savor::pred::PredFlag::RhsIsDelta);
    }
    if (lhsNegateCheck_ != nullptr && lhsNegateCheck_->isChecked()) {
        flags |= static_cast<std::uint32_t>(savor::pred::PredFlag::LhsIsNeg);
    }
    if (rhsNegateCheck_ != nullptr && rhsNegateCheck_->isChecked()) {
        flags |= static_cast<std::uint32_t>(savor::pred::PredFlag::RhsIsNeg);
    }

    draft.abort_on_fail = abortOnFailCheck_ != nullptr && abortOnFailCheck_->isChecked();
    if (draft.abort_on_fail) {
        flags |= static_cast<std::uint32_t>(savor::pred::PredFlag::AbortOnFail);
    }

    if (lhsMode == ValueSourceMode::AddrKey) {
        flags |= static_cast<std::uint32_t>(savor::pred::PredFlag::LhsIsKey);
        draft.lhs_value = lhsKeyCombo_->currentData().toLongLong();
    } else if (lhsMode == ValueSourceMode::AddrProgram) {
        flags |= static_cast<std::uint32_t>(savor::pred::PredFlag::LhsIsProg);
        if (!lhsProgramDraft_.blob.isEmpty()) {
            savorqt::db::AddressProgramDraft programDraft{};
            programDraft.prog_bytes.assign(lhsProgramDraft_.blob.begin(), lhsProgramDraft_.blob.end());
            programDraft.description = lhsProgramDraft_.description.toStdString();
            const auto savedProgram = savorqt::db::SavorDbAuthoringService::EnsureAddressProgram(programDraft);
            if (!savedProgram.ok) {
                postStatusMessage(QString::fromStdString(savedProgram.error.message), StatusToast::Severity::Error);
                return;
            }
            draft.lhs_address_program_id = savedProgram.value;
        }
    } else {
        std::int64_t lhsValue = 0;
        if (!parseSignedInteger(lhsValueEdit_->text().trimmed(), lhsValue)) {
            lhsValue = 0;
        }
        draft.lhs_value = lhsValue;
    }

    if (rhsIsBaseline) {
        draft.rhs_value = 0;
    } else if (rhsMode == ValueSourceMode::AddrKey) {
        flags |= static_cast<std::uint32_t>(savor::pred::PredFlag::RhsIsKey);
        draft.rhs_value = rhsKeyCombo_->currentData().toLongLong();
    } else if (rhsMode == ValueSourceMode::AddrProgram) {
        flags |= static_cast<std::uint32_t>(savor::pred::PredFlag::RhsIsProg);
        if (!rhsProgramDraft_.blob.isEmpty()) {
            savorqt::db::AddressProgramDraft programDraft{};
            programDraft.prog_bytes.assign(rhsProgramDraft_.blob.begin(), rhsProgramDraft_.blob.end());
            programDraft.description = rhsProgramDraft_.description.toStdString();
            const auto savedProgram = savorqt::db::SavorDbAuthoringService::EnsureAddressProgram(programDraft);
            if (!savedProgram.ok) {
                postStatusMessage(QString::fromStdString(savedProgram.error.message), StatusToast::Severity::Error);
                return;
            }
            draft.rhs_address_program_id = savedProgram.value;
        }
    } else {
        std::int64_t rhsValue = 0;
        if (!parseSignedInteger(rhsValueEdit_->text().trimmed(), rhsValue)) {
            rhsValue = 0;
        }
        draft.rhs_value = rhsValue;
    }

    draft.flag_mask = static_cast<std::int64_t>(flags);

    saveButton_->setEnabled(false);
    const auto result = predicateSpecId_.has_value()
        ? savorqt::db::SavorDbAuthoringService::UpdatePredicateSpec(*predicateSpecId_, draft)
        : savorqt::db::SavorDbAuthoringService::SavePredicateSpec(draft);
    saveButton_->setEnabled(true);
    if (!result.ok) {
        postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
        return;
    }
    predicateSpecId_ = result.value;
    setWindowTitle(QStringLiteral("Predicate Editor - Edit"));
    saveButton_->setText(QStringLiteral("Save Predicate In Place"));

    if (requiredBreakpoints.size() > 1) {
        postStatusMessage(
            QStringLiteral("Saved as single required breakpoint; additional required breakpoints are currently shown in UI only."),
            StatusToast::Severity::Warn);
    }
    dirty_ = false;
    if (savedCallback_) {
        savedCallback_();
    }
    postStatusMessage(QStringLiteral("Saved predicate %1.").arg(static_cast<qint64>(result.value)), StatusToast::Severity::Info);
}

void PredicateSpecEditorWindow::refreshUi()
{
    const auto lhsMode = static_cast<ValueSourceMode>(lhsModeCombo_ != nullptr ? lhsModeCombo_->currentData().toInt() : static_cast<int>(ValueSourceMode::AbsoluteAddress));
    const bool lhsAddrVisible = lhsMode == ValueSourceMode::AbsoluteAddress;
    const bool lhsKeyVisible = lhsMode == ValueSourceMode::AddrKey;
    const bool lhsProgramVisible = lhsMode == ValueSourceMode::AddrProgram;

    setFormRowVisibility(lhsMatchLayout_, lhsValueEdit_, lhsAddrVisible);
    setFormRowVisibility(lhsMatchLayout_, lhsKeyCombo_, lhsKeyVisible);
    setFormRowVisibility(lhsMatchLayout_, lhsProgramKindCombo_, lhsProgramVisible);
    setFormRowVisibility(lhsMatchLayout_, lhsProgramAEdit_, lhsProgramVisible);
    setFormRowVisibility(lhsMatchLayout_, lhsProgramBEdit_, lhsProgramVisible
        && lhsProgramKindCombo_ != nullptr
        && lhsProgramKindCombo_->currentData().toInt() == static_cast<int>(ProgramKind::EnemyItemAmount));
    if (lhsBuildProgramButton_ != nullptr) {
        lhsBuildProgramButton_->setVisible(lhsProgramVisible);
    }
    if (lhsProgramSummaryLabel_ != nullptr) {
        setFormRowVisibility(lhsMatchLayout_, lhsProgramSummaryLabel_, lhsProgramVisible);
        lhsProgramSummaryLabel_->setText(describeProgramDraft(lhsProgramDraft_));
    }

    const auto rhsMode = static_cast<ValueSourceMode>(rhsModeCombo_ != nullptr ? rhsModeCombo_->currentData().toInt() : static_cast<int>(ValueSourceMode::Immediate));
    const bool rhsBaselineMode = rhsMode == ValueSourceMode::Baseline;
    const bool rhsImmediateVisible = !rhsBaselineMode && rhsMode == ValueSourceMode::Immediate;
    const bool rhsKeyVisible = !rhsBaselineMode && rhsMode == ValueSourceMode::AddrKey;
    const bool rhsProgramVisible = !rhsBaselineMode && rhsMode == ValueSourceMode::AddrProgram;
    const bool rhsBaselineVisible = rhsBaselineMode;

    setFormRowVisibility(rhsMatchLayout_, rhsModeCombo_, true);
    setFormRowVisibility(rhsMatchLayout_, rhsValueEdit_, rhsImmediateVisible);
    setFormRowVisibility(rhsMatchLayout_, rhsKeyCombo_, rhsKeyVisible);
    setFormRowVisibility(rhsMatchLayout_, rhsProgramKindCombo_, rhsProgramVisible);
    setFormRowVisibility(rhsMatchLayout_, rhsProgramAEdit_, rhsProgramVisible);
    setFormRowVisibility(rhsMatchLayout_, rhsProgramBEdit_, rhsProgramVisible
        && rhsProgramKindCombo_ != nullptr
        && rhsProgramKindCombo_->currentData().toInt() == static_cast<int>(ProgramKind::EnemyItemAmount));
    if (rhsBuildProgramButton_ != nullptr) {
        rhsBuildProgramButton_->setVisible(rhsProgramVisible);
    }
    if (rhsProgramSummaryLabel_ != nullptr) {
        setFormRowVisibility(rhsMatchLayout_, rhsProgramSummaryLabel_, rhsProgramVisible);
        rhsProgramSummaryLabel_->setText(describeProgramDraft(rhsProgramDraft_));
    }
    if (baselineBpRowsWidget_ != nullptr) {
        setFormRowVisibility(rhsMatchLayout_, baselineBpRowsWidget_, rhsBaselineVisible);
    }
    if (addBaselineBpButton_ != nullptr) {
        addBaselineBpButton_->setVisible(rhsBaselineVisible);
    }
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
