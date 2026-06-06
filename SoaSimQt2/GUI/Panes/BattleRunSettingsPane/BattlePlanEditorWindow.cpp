#include "BattlePlanEditorWindow.h"

#include "DB/SimCoreDbAuthoringService.h"

#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <cstdint>
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
    if (normalized == "useitem" || normalized == "use_item" || normalized == "item") return simcore::db::BattlePlanActionMacro::UseItem;
    return std::nullopt;
}

std::optional<simcore::db::BattlePlanTargetKind> parseTargetKind(const QString& text)
{
    const QString normalized = text.trimmed().toLower();
    if (normalized == "single" || normalized == "singleenemy") return simcore::db::BattlePlanTargetKind::SingleEnemy;
    if (normalized == "multi" || normalized == "multiple" || normalized == "multipleenemies") return simcore::db::BattlePlanTargetKind::MultipleEnemies;
    if (normalized == "any" || normalized == "anyenemy") return simcore::db::BattlePlanTargetKind::AnyEnemy;
    if (normalized == "samepc" || normalized == "sameasotherpc") return simcore::db::BattlePlanTargetKind::SameAsOtherPC;
    return std::nullopt;
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
        QStringLiteral("Actions: one per line as turn,actor,macro,target[,item]. Example: 1,0,Attack,Any"),
        panel);
    hint->setObjectName("sectionDescription");
    hint->setWordWrap(true);
    panelLayout->addWidget(hint);

    actionsText_ = new QPlainTextEdit(panel);
    actionsText_->setPlaceholderText(QStringLiteral("1,0,Attack,Any\n1,1,Focus,Any"));
    panelLayout->addWidget(actionsText_, 1);
    rootLayout->addWidget(panel, 1);

    auto* buttonRow = new QHBoxLayout();
    buttonRow->addStretch();
    saveButton_ = new QPushButton(QStringLiteral("Save Battle Plan"), this);
    saveButton_->setObjectName("jobsPrimaryButton");
    buttonRow->addWidget(saveButton_);
    rootLayout->addLayout(buttonRow);

    connect(saveButton_, &QPushButton::clicked, this, &BattlePlanEditorWindow::saveBattlePlan);
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

    const QStringList lines = actionsText_->toPlainText().split('\n', Qt::SkipEmptyParts);
    int ordinal = 0;
    for (const QString& line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith('#')) {
            continue;
        }
        const QStringList parts = trimmed.split(',', Qt::KeepEmptyParts);
        if (parts.size() < 4) {
            postStatusMessage(QStringLiteral("Action line requires turn,actor,macro,target."), StatusToast::Severity::Warn);
            return;
        }

        bool ok = false;
        const int turnIndex = parts.at(0).trimmed().toInt(&ok);
        if (!ok || turnIndex < 1 || turnIndex > draft.num_turns) {
            postStatusMessage(QStringLiteral("Action turn is outside the plan turn range."), StatusToast::Severity::Warn);
            return;
        }
        const int actorSlot = parts.at(1).trimmed().toInt(&ok);
        if (!ok || actorSlot < 0 || actorSlot > 3) {
            postStatusMessage(QStringLiteral("Actor slot must be 0-3."), StatusToast::Severity::Warn);
            return;
        }
        const auto macro = parseMacro(parts.at(2));
        if (!macro.has_value()) {
            postStatusMessage(QStringLiteral("Unknown action macro."), StatusToast::Severity::Warn);
            return;
        }
        const auto targetKind = parseTargetKind(parts.at(3));
        if (!targetKind.has_value()) {
            postStatusMessage(QStringLiteral("Unknown target kind."), StatusToast::Severity::Warn);
            return;
        }

        soasimqt2::db::BattlePlanActionDraft action{};
        action.actor_slot = actorSlot;
        action.macro = *macro;
        action.target_kind = *targetKind;
        action.ordinal = ordinal++;
        if (parts.size() >= 5 && !parts.at(4).trimmed().isEmpty()) {
            const int itemId = parts.at(4).trimmed().toInt(&ok);
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
    postStatusMessage(QStringLiteral("Saved battle plan %1.").arg(static_cast<qint64>(result.value)), StatusToast::Severity::Info);
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
