#include "BattlePlanEditorWindow.h"

#include "DB/SimCoreDbAuthoringService.h"

#include <QtCore/QItemSelectionModel>
#include <QtGui/QCloseEvent>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTableWidgetItem>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <sstream>
#include <utility>

namespace {

std::string fingerprintForDraft(const soasimqt2::db::BattlePlanDraft& draft)
{
    std::string content = draft.name + ":" + std::to_string(draft.num_turns) + "\n";
    for (const auto& turn : draft.turns) {
        content += "turn:" + std::to_string(turn.turn_index) + "\n";
        for (const auto& action : turn.actions) {
            content += "action:" + std::to_string(action.actor_slot)
                + ":" + std::to_string(static_cast<int>(action.macro))
                + ":" + std::to_string(static_cast<int>(action.target_kind))
                + ":" + std::to_string(action.ordinal) + "\n";
        }
    }

    std::uint64_t hash = 1469598103934665603ull;
    for (const auto ch : content) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 1099511628211ull;
    }
    std::ostringstream out;
    out << "battle-plan-fnv1a64-" << std::hex << hash;
    return out.str();
}

std::optional<simcore::db::BattlePlanActionMacro> parseMacro(const QString& text)
{
    const QString normalized = text.trimmed().toLower();
    if (normalized == "attack") return simcore::db::BattlePlanActionMacro::Attack;
    if (normalized == "defend") return simcore::db::BattlePlanActionMacro::Defend;
    if (normalized == "focus") return simcore::db::BattlePlanActionMacro::Focus;
    if (normalized == "fakeattack" || normalized == "fake_attack") return simcore::db::BattlePlanActionMacro::FakeAttack;
    if (normalized == "useitem" || normalized == "use_item" || normalized == "item") return simcore::db::BattlePlanActionMacro::UseItem;
    bool ok = false;
    const int value = normalized.toInt(&ok);
    if (ok && value >= static_cast<int>(simcore::db::BattlePlanActionMacro::Attack)
        && value <= static_cast<int>(simcore::db::BattlePlanActionMacro::UseItem)) {
        return static_cast<simcore::db::BattlePlanActionMacro>(value);
    }
    return std::nullopt;
}

std::optional<simcore::db::BattlePlanTargetKind> parseTargetKind(const QString& text)
{
    const QString normalized = text.trimmed().toLower();
    if (normalized == "single" || normalized == "singleenemy") return simcore::db::BattlePlanTargetKind::SingleEnemy;
    if (normalized == "multi" || normalized == "multiple" || normalized == "multipleenemies") return simcore::db::BattlePlanTargetKind::MultipleEnemies;
    if (normalized == "any" || normalized == "anyenemy") return simcore::db::BattlePlanTargetKind::AnyEnemy;
    if (normalized == "samepc" || normalized == "sameasotherpc") return simcore::db::BattlePlanTargetKind::SameAsOtherPC;
    bool ok = false;
    const int value = normalized.toInt(&ok);
    if (ok && value >= static_cast<int>(simcore::db::BattlePlanTargetKind::SingleEnemy)
        && value <= static_cast<int>(simcore::db::BattlePlanTargetKind::SameAsOtherPC)) {
        return static_cast<simcore::db::BattlePlanTargetKind>(value);
    }
    return std::nullopt;
}

QString macroLabel(simcore::db::BattlePlanActionMacro macro)
{
    switch (macro) {
    case simcore::db::BattlePlanActionMacro::Attack: return QStringLiteral("Attack");
    case simcore::db::BattlePlanActionMacro::Defend: return QStringLiteral("Defend");
    case simcore::db::BattlePlanActionMacro::Focus: return QStringLiteral("Focus");
    case simcore::db::BattlePlanActionMacro::FakeAttack: return QStringLiteral("FakeAttack");
    case simcore::db::BattlePlanActionMacro::UseItem: return QStringLiteral("UseItem");
    }
    return QStringLiteral("Attack");
}

QString targetKindLabel(simcore::db::BattlePlanTargetKind kind)
{
    switch (kind) {
    case simcore::db::BattlePlanTargetKind::SingleEnemy: return QStringLiteral("Single");
    case simcore::db::BattlePlanTargetKind::MultipleEnemies: return QStringLiteral("Multiple");
    case simcore::db::BattlePlanTargetKind::AnyEnemy: return QStringLiteral("Any");
    case simcore::db::BattlePlanTargetKind::SameAsOtherPC: return QStringLiteral("SamePc");
    }
    return QStringLiteral("Any");
}

} // namespace

BattlePlanEditorWindow::BattlePlanEditorWindow(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlag(Qt::Window, true);
    setWindowTitle(QStringLiteral("Battle Plan Editor"));
    resize(760, 560);
    createWidgets();
}

void BattlePlanEditorWindow::setStatusCallback(std::function<void(const QString&, StatusToast::Severity)> callback)
{
    statusCallback_ = std::move(callback);
}

void BattlePlanEditorWindow::setSavedCallback(std::function<void()> callback)
{
    savedCallback_ = std::move(callback);
}

void BattlePlanEditorWindow::loadSnapshot(const simcore::db::BattlePlanSnapshot& snapshot, bool duplicate)
{
    setWindowTitle(duplicate
        ? QStringLiteral("Battle Plan Editor - Duplicate")
        : QStringLiteral("Battle Plan Editor - Edit Copy"));
    nameEdit_->setText(QString::fromStdString(snapshot.name) + (duplicate ? QStringLiteral(" copy") : QString()));
    turnCountSpin_->setValue(std::max(1, snapshot.num_turns));
    actionsTable_->setRowCount(0);
    for (const auto& turn : snapshot.turns) {
        for (const auto& action : turn.actions) {
            addActionRow();
            const int row = actionsTable_->rowCount() - 1;
            actionsTable_->item(row, 0)->setText(QString::number(turn.turn_index));
            actionsTable_->item(row, 1)->setText(QString::number(action.actor_slot));
            actionsTable_->item(row, 2)->setText(macroLabel(action.macro));
            actionsTable_->item(row, 3)->setText(targetKindLabel(action.target_kind));
            actionsTable_->item(row, 4)->setText(action.item_id.has_value() ? QString::number(*action.item_id) : QString());
        }
    }
    dirty_ = false;
}

void BattlePlanEditorWindow::closeEvent(QCloseEvent* event)
{
    if (confirmDiscardIfDirty()) {
        event->accept();
    } else {
        event->ignore();
    }
}

void BattlePlanEditorWindow::createWidgets()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(12, 12, 12, 12);
    rootLayout->setSpacing(10);

    auto* panel = new QFrame(this);
    panel->setObjectName("jobsSurfacePanel");
    auto* panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(14, 14, 14, 14);
    panelLayout->setSpacing(10);

    auto* form = new QFormLayout();
    nameEdit_ = new QLineEdit(panel);
    turnCountSpin_ = new QSpinBox(panel);
    turnCountSpin_->setRange(1, 20);
    turnCountSpin_->setValue(1);
    form->addRow(QStringLiteral("Name"), nameEdit_);
    form->addRow(QStringLiteral("Turns"), turnCountSpin_);
    panelLayout->addLayout(form);

    auto* hint = new QLabel(
        QStringLiteral("Action rows use turn, actor slot, macro, target kind, and optional item id. Macro values: Attack, Defend, Focus, UseItem. Target values: Any, Single, Multiple, SamePc."),
        panel);
    hint->setObjectName("sectionDescription");
    hint->setWordWrap(true);
    panelLayout->addWidget(hint);

    auto* actionToolbar = new QHBoxLayout();
    addActionButton_ = new QPushButton(QStringLiteral("Add Action"), panel);
    removeActionButton_ = new QPushButton(QStringLiteral("Remove Selected"), panel);
    addActionButton_->setObjectName("jobsSecondaryButton");
    removeActionButton_->setObjectName("jobsSecondaryButton");
    actionToolbar->addWidget(addActionButton_);
    actionToolbar->addWidget(removeActionButton_);
    actionToolbar->addStretch();
    panelLayout->addLayout(actionToolbar);

    actionsTable_ = new QTableWidget(panel);
    actionsTable_->setColumnCount(5);
    actionsTable_->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("Turn"),
        QStringLiteral("Actor"),
        QStringLiteral("Macro"),
        QStringLiteral("Target"),
        QStringLiteral("Item")
    });
    actionsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    actionsTable_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    panelLayout->addWidget(actionsTable_, 1);
    rootLayout->addWidget(panel, 1);

    auto* buttonRow = new QHBoxLayout();
    buttonRow->addStretch();
    saveButton_ = new QPushButton(QStringLiteral("Save Battle Plan"), this);
    saveButton_->setObjectName("jobsPrimaryButton");
    buttonRow->addWidget(saveButton_);
    rootLayout->addLayout(buttonRow);

    connect(saveButton_, &QPushButton::clicked, this, &BattlePlanEditorWindow::saveBattlePlan);
    connect(addActionButton_, &QPushButton::clicked, this, &BattlePlanEditorWindow::addActionRow);
    connect(removeActionButton_, &QPushButton::clicked, this, &BattlePlanEditorWindow::removeSelectedActionRows);
    connect(nameEdit_, &QLineEdit::textChanged, this, [this]() { markDirty(); });
    connect(turnCountSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this]() { markDirty(); });
    connect(actionsTable_, &QTableWidget::itemChanged, this, [this]() { markDirty(); });
}

void BattlePlanEditorWindow::addActionRow()
{
    const int row = actionsTable_->rowCount();
    actionsTable_->insertRow(row);
    const QStringList defaults{
        QStringLiteral("1"),
        QStringLiteral("0"),
        QStringLiteral("Attack"),
        QStringLiteral("Any"),
        QString()
    };
    for (int column = 0; column < defaults.size(); ++column) {
        actionsTable_->setItem(row, column, new QTableWidgetItem(defaults.at(column)));
    }
    markDirty();
}

void BattlePlanEditorWindow::removeSelectedActionRows()
{
    QList<int> rows;
    for (const QModelIndex& index : actionsTable_->selectionModel()->selectedRows()) {
        rows.push_back(index.row());
    }
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (int row : rows) {
        actionsTable_->removeRow(row);
    }
    if (!rows.empty()) {
        markDirty();
    }
}

void BattlePlanEditorWindow::saveBattlePlan()
{
    if (nameEdit_->text().trimmed().isEmpty()) {
        postStatusMessage(QStringLiteral("Battle plan name is required."), StatusToast::Severity::Warn);
        return;
    }

    soasimqt2::db::BattlePlanDraft draft{};
    draft.name = nameEdit_->text().trimmed().toStdString();
    draft.num_turns = turnCountSpin_->value();
    draft.turns.resize(static_cast<std::size_t>(draft.num_turns));
    for (int index = 0; index < draft.num_turns; ++index) {
        draft.turns[static_cast<std::size_t>(index)].turn_index = index + 1;
    }

    int ordinal = 0;
    for (int row = 0; row < actionsTable_->rowCount(); ++row) {
        bool ok = false;
        const int turnIndex = actionsTable_->item(row, 0)->text().trimmed().toInt(&ok);
        if (!ok || turnIndex < 1 || turnIndex > draft.num_turns) {
            postStatusMessage(QStringLiteral("Action turn is outside the plan turn range."), StatusToast::Severity::Warn);
            return;
        }
        const int actorSlot = actionsTable_->item(row, 1)->text().trimmed().toInt(&ok);
        if (!ok || actorSlot < 0 || actorSlot > 3) {
            postStatusMessage(QStringLiteral("Actor slot must be 0-3."), StatusToast::Severity::Warn);
            return;
        }
        const auto macro = parseMacro(actionsTable_->item(row, 2)->text());
        if (!macro.has_value()) {
            postStatusMessage(QStringLiteral("Unknown action macro."), StatusToast::Severity::Warn);
            return;
        }
        const auto targetKind = parseTargetKind(actionsTable_->item(row, 3)->text());
        if (!targetKind.has_value()) {
            postStatusMessage(QStringLiteral("Unknown target kind."), StatusToast::Severity::Warn);
            return;
        }

        soasimqt2::db::BattlePlanActionDraft action{};
        action.actor_slot = actorSlot;
        action.macro = *macro;
        action.target_kind = *targetKind;
        action.ordinal = ordinal++;
        const auto* itemCell = actionsTable_->item(row, 4);
        if (itemCell != nullptr && !itemCell->text().trimmed().isEmpty()) {
            const int itemId = itemCell->text().trimmed().toInt(&ok);
            if (!ok) {
                postStatusMessage(QStringLiteral("Item id must be numeric."), StatusToast::Severity::Warn);
                return;
            }
            action.item_id = itemId;
        }
        draft.turns[static_cast<std::size_t>(turnIndex - 1)].actions.push_back(std::move(action));
    }
    draft.fingerprint = fingerprintForDraft(draft);

    saveButton_->setEnabled(false);
    const auto result = soasimqt2::db::SimCoreDbAuthoringService::SaveBattlePlan(draft);
    saveButton_->setEnabled(true);
    if (!result.ok) {
        postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
        return;
    }
    dirty_ = false;
    if (savedCallback_) {
        savedCallback_();
    }
    postStatusMessage(QStringLiteral("Saved battle plan %1.").arg(static_cast<qint64>(result.value)), StatusToast::Severity::Info);
}

void BattlePlanEditorWindow::markDirty()
{
    dirty_ = true;
}

bool BattlePlanEditorWindow::confirmDiscardIfDirty()
{
    if (!dirty_) {
        return true;
    }
    const auto result = QMessageBox::warning(
        this,
        QStringLiteral("Discard battle plan changes?"),
        QStringLiteral("This battle plan has unsaved changes."),
        QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Cancel);
    return result == QMessageBox::Discard;
}

void BattlePlanEditorWindow::postStatusMessage(const QString& text, StatusToast::Severity severity)
{
    if (text.isEmpty()) {
        return;
    }
    if (statusCallback_) {
        statusCallback_(text, severity);
    }
}
