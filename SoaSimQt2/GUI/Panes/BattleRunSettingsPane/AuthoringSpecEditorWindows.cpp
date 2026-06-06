#include "AuthoringSpecEditorWindows.h"

#include "DB/SimCoreDbAuthoringService.h"

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <cstdint>
#include <optional>
#include <utility>

namespace {

QFrame* createPanel(QWidget* parent)
{
    auto* panel = new QFrame(parent);
    panel->setObjectName("jobsSurfacePanel");
    return panel;
}

QLineEdit* numericEdit(QWidget* parent, const QString& text = QStringLiteral("0"))
{
    auto* edit = new QLineEdit(parent);
    edit->setText(text);
    return edit;
}

std::optional<std::int64_t> optionalComboId(const QComboBox* combo)
{
    if (combo == nullptr || combo->currentIndex() <= 0) {
        return std::nullopt;
    }
    const auto value = combo->currentData().toLongLong();
    return value > 0 ? std::optional<std::int64_t>{ value } : std::nullopt;
}

bool parseInt64(const QLineEdit* edit, const QString& fieldName, std::int64_t* out, QString* errorText)
{
    bool ok = false;
    const auto value = edit->text().trimmed().toLongLong(&ok, 0);
    if (!ok) {
        if (errorText != nullptr) {
            *errorText = QStringLiteral("%1 must be numeric.").arg(fieldName);
        }
        return false;
    }
    if (out != nullptr) {
        *out = value;
    }
    return true;
}

void addNoneOption(QComboBox* combo)
{
    combo->clear();
    combo->addItem(QStringLiteral("(none)"), 0);
}

QString seedProbeLabel(const simcore::db::SeedProbeSpecSnapshot& row)
{
    return QStringLiteral("#%1 %2 (%3/axis)")
        .arg(static_cast<qint64>(row.seed_probe_spec_id))
        .arg(QString::fromStdString(row.name))
        .arg(row.samples_per_axis);
}

QString tasLabel(const simcore::db::TasSpecSnapshot& row)
{
    return QStringLiteral("#%1 %2")
        .arg(static_cast<qint64>(row.tas_spec_id))
        .arg(QString::fromStdString(row.base_name));
}

QString battleRunLabel(const simcore::db::BattleRunSpecSnapshot& row)
{
    return QStringLiteral("#%1 %2 fake %3-%4")
        .arg(static_cast<qint64>(row.battle_run_spec_id))
        .arg(QString::fromStdString(row.name))
        .arg(row.min_fake_attacks)
        .arg(row.max_fake_attacks);
}

QString battlePlanLabel(const simcore::db::BattlePlanSnapshot& row)
{
    return QStringLiteral("#%1 %2")
        .arg(static_cast<qint64>(row.plan_id))
        .arg(QString::fromStdString(row.name));
}

QString predicateSetLabel(const simcore::db::PredicateSetSnapshot& row)
{
    return QStringLiteral("#%1 %2 predicates")
        .arg(static_cast<qint64>(row.predicate_set_id))
        .arg(static_cast<int>(row.predicates.size()));
}

QString explorerSettingsLabel(const simcore::db::ExplorerSettingsSnapshot& row)
{
    return QStringLiteral("#%1 %2")
        .arg(static_cast<qint64>(row.explorer_settings_id))
        .arg(QString::fromStdString(row.name));
}

void notifySaved(const std::function<void()>& callback)
{
    if (callback) {
        callback();
    }
}

} // namespace

SeedProbeSpecEditorWindow::SeedProbeSpecEditorWindow(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlag(Qt::Window, true);
    setWindowTitle(QStringLiteral("Seed Probe Spec Editor"));
    resize(620, 480);
    createWidgets();
}

void SeedProbeSpecEditorWindow::setStatusCallback(std::function<void(const QString&, StatusToast::Severity)> callback)
{
    statusCallback_ = std::move(callback);
}

void SeedProbeSpecEditorWindow::setSavedCallback(std::function<void()> callback)
{
    savedCallback_ = std::move(callback);
}

void SeedProbeSpecEditorWindow::createWidgets()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    auto* panel = createPanel(this);
    auto* form = new QFormLayout(panel);
    form->setContentsMargins(14, 14, 14, 14);
    nameEdit_ = new QLineEdit(panel);
    prioritySpin_ = new QSpinBox(panel);
    prioritySpin_->setRange(-100000, 100000);
    runMsEdit_ = numericEdit(panel, QStringLiteral("30000"));
    viStallMsEdit_ = numericEdit(panel, QStringLiteral("2500"));
    samplesPerAxisSpin_ = new QSpinBox(panel);
    samplesPerAxisSpin_->setRange(1, 255);
    samplesPerAxisSpin_->setValue(5);
    minValueEdit_ = numericEdit(panel, QStringLiteral("-128"));
    maxValueEdit_ = numericEdit(panel, QStringLiteral("127"));
    capTriggerTopCheck_ = new QCheckBox(panel);
    ignoreTriggerMinMaxCheck_ = new QCheckBox(panel);
    comboAttemptsSpin_ = new QSpinBox(panel);
    comboAttemptsSpin_->setRange(0, 100000);
    comboAttemptsSpin_->setValue(3);
    comboSamplerTriesSpin_ = new QSpinBox(panel);
    comboSamplerTriesSpin_->setRange(0, 100000);
    comboSamplerTriesSpin_->setValue(32);
    autoScheduleBattleRunCheck_ = new QCheckBox(panel);
    form->addRow(QStringLiteral("Name"), nameEdit_);
    form->addRow(QStringLiteral("Priority"), prioritySpin_);
    form->addRow(QStringLiteral("Run ms"), runMsEdit_);
    form->addRow(QStringLiteral("VI stall ms"), viStallMsEdit_);
    form->addRow(QStringLiteral("Samples per axis"), samplesPerAxisSpin_);
    form->addRow(QStringLiteral("Min value"), minValueEdit_);
    form->addRow(QStringLiteral("Max value"), maxValueEdit_);
    form->addRow(QStringLiteral("Cap trigger top"), capTriggerTopCheck_);
    form->addRow(QStringLiteral("Ignore trigger min/max"), ignoreTriggerMinMaxCheck_);
    form->addRow(QStringLiteral("Combo attempts/target"), comboAttemptsSpin_);
    form->addRow(QStringLiteral("Combo sampler tries"), comboSamplerTriesSpin_);
    form->addRow(QStringLiteral("Auto schedule battle run"), autoScheduleBattleRunCheck_);
    root->addWidget(panel, 1);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch();
    saveButton_ = new QPushButton(QStringLiteral("Save Seed Probe Spec"), this);
    saveButton_->setObjectName("jobsPrimaryButton");
    buttons->addWidget(saveButton_);
    root->addLayout(buttons);
    connect(saveButton_, &QPushButton::clicked, this, &SeedProbeSpecEditorWindow::saveSpec);
}

void SeedProbeSpecEditorWindow::saveSpec()
{
    if (nameEdit_->text().trimmed().isEmpty()) {
        postStatusMessage(QStringLiteral("Seed probe spec name is required."), StatusToast::Severity::Warn);
        return;
    }
    QString error;
    std::int64_t runMs = 0;
    std::int64_t viStallMs = 0;
    std::int64_t minValue = 0;
    std::int64_t maxValue = 0;
    if (!parseInt64(runMsEdit_, QStringLiteral("Run ms"), &runMs, &error)
        || !parseInt64(viStallMsEdit_, QStringLiteral("VI stall ms"), &viStallMs, &error)
        || !parseInt64(minValueEdit_, QStringLiteral("Min value"), &minValue, &error)
        || !parseInt64(maxValueEdit_, QStringLiteral("Max value"), &maxValue, &error)) {
        postStatusMessage(error, StatusToast::Severity::Warn);
        return;
    }

    soasimqt2::db::SeedProbeSpecDraft draft{};
    draft.name = nameEdit_->text().trimmed().toStdString();
    draft.priority = prioritySpin_->value();
    draft.run_ms = runMs;
    draft.vi_stall_ms = viStallMs;
    draft.samples_per_axis = samplesPerAxisSpin_->value();
    draft.min_value = minValue;
    draft.max_value = maxValue;
    draft.cap_trigger_top = capTriggerTopCheck_->isChecked();
    draft.ignore_trigger_minmax = ignoreTriggerMinMaxCheck_->isChecked();
    draft.combo_attempts_per_target = comboAttemptsSpin_->value();
    draft.combo_sampler_tries = comboSamplerTriesSpin_->value();
    draft.auto_schedule_battle_run = autoScheduleBattleRunCheck_->isChecked();
    const auto result = soasimqt2::db::SimCoreDbAuthoringService::SaveSeedProbeSpec(draft);
    if (!result.ok) {
        postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
        return;
    }
    notifySaved(savedCallback_);
    postStatusMessage(QStringLiteral("Saved seed probe spec %1.").arg(static_cast<qint64>(result.value)), StatusToast::Severity::Info);
}

void SeedProbeSpecEditorWindow::postStatusMessage(const QString& text, StatusToast::Severity severity)
{
    if (statusCallback_ && !text.isEmpty()) statusCallback_(text, severity);
}

TasSpecEditorWindow::TasSpecEditorWindow(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlag(Qt::Window, true);
    setWindowTitle(QStringLiteral("TAS Spec Editor"));
    resize(620, 440);
    createWidgets();
}

void TasSpecEditorWindow::setStatusCallback(std::function<void(const QString&, StatusToast::Severity)> callback)
{
    statusCallback_ = std::move(callback);
}

void TasSpecEditorWindow::setSavedCallback(std::function<void()> callback)
{
    savedCallback_ = std::move(callback);
}

void TasSpecEditorWindow::createWidgets()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    auto* panel = createPanel(this);
    auto* form = new QFormLayout(panel);
    form->setContentsMargins(14, 14, 14, 14);
    nameEdit_ = new QLineEdit(panel);
    prioritySpin_ = new QSpinBox(panel);
    prioritySpin_->setRange(-100000, 100000);
    runMsEdit_ = numericEdit(panel, QStringLiteral("60000"));
    viStallMsEdit_ = numericEdit(panel, QStringLiteral("2500"));
    headroomSpin_ = new QSpinBox(panel);
    headroomSpin_->setRange(0, 255);
    headroomSpin_->setValue(10);
    progressCheck_ = new QCheckBox(panel);
    autoQueueSeedsCheck_ = new QCheckBox(panel);
    form->addRow(QStringLiteral("Name"), nameEdit_);
    form->addRow(QStringLiteral("Priority"), prioritySpin_);
    form->addRow(QStringLiteral("Run ms"), runMsEdit_);
    form->addRow(QStringLiteral("VI stall ms"), viStallMsEdit_);
    form->addRow(QStringLiteral("Headroom x10"), headroomSpin_);
    form->addRow(QStringLiteral("Progress"), progressCheck_);
    form->addRow(QStringLiteral("Auto queue seeds"), autoQueueSeedsCheck_);
    root->addWidget(panel, 1);
    auto* buttons = new QHBoxLayout();
    buttons->addStretch();
    saveButton_ = new QPushButton(QStringLiteral("Save TAS Spec"), this);
    saveButton_->setObjectName("jobsPrimaryButton");
    buttons->addWidget(saveButton_);
    root->addLayout(buttons);
    connect(saveButton_, &QPushButton::clicked, this, &TasSpecEditorWindow::saveSpec);
}

void TasSpecEditorWindow::saveSpec()
{
    if (nameEdit_->text().trimmed().isEmpty()) {
        postStatusMessage(QStringLiteral("TAS spec name is required."), StatusToast::Severity::Warn);
        return;
    }
    QString error;
    std::int64_t runMs = 0;
    std::int64_t viStallMs = 0;
    if (!parseInt64(runMsEdit_, QStringLiteral("Run ms"), &runMs, &error)
        || !parseInt64(viStallMsEdit_, QStringLiteral("VI stall ms"), &viStallMs, &error)) {
        postStatusMessage(error, StatusToast::Severity::Warn);
        return;
    }
    soasimqt2::db::TasSpecDraft draft{};
    draft.base_name = nameEdit_->text().trimmed().toStdString();
    draft.priority = prioritySpin_->value();
    draft.run_ms = runMs;
    draft.vi_stall_ms = viStallMs;
    draft.headroom_x10 = headroomSpin_->value();
    draft.progress_enable = progressCheck_->isChecked();
    draft.auto_queue_seeds = autoQueueSeedsCheck_->isChecked();
    draft.base_dtm_artifact_id = 0;
    draft.rtc_low = 0;
    draft.rtc_high = 0;
    const auto result = soasimqt2::db::SimCoreDbAuthoringService::SaveTasSpec(draft);
    if (!result.ok) {
        postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
        return;
    }
    notifySaved(savedCallback_);
    postStatusMessage(QStringLiteral("Saved TAS spec %1.").arg(static_cast<qint64>(result.value)), StatusToast::Severity::Info);
}

void TasSpecEditorWindow::postStatusMessage(const QString& text, StatusToast::Severity severity)
{
    if (statusCallback_ && !text.isEmpty()) statusCallback_(text, severity);
}

BattleRunSpecEditorWindow::BattleRunSpecEditorWindow(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlag(Qt::Window, true);
    setWindowTitle(QStringLiteral("Battle Run Spec Editor"));
    resize(620, 420);
    createWidgets();
}

void BattleRunSpecEditorWindow::setStatusCallback(std::function<void(const QString&, StatusToast::Severity)> callback)
{
    statusCallback_ = std::move(callback);
}

void BattleRunSpecEditorWindow::setSavedCallback(std::function<void()> callback)
{
    savedCallback_ = std::move(callback);
}

void BattleRunSpecEditorWindow::createWidgets()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    auto* panel = createPanel(this);
    auto* form = new QFormLayout(panel);
    form->setContentsMargins(14, 14, 14, 14);
    nameEdit_ = new QLineEdit(panel);
    prioritySpin_ = new QSpinBox(panel);
    prioritySpin_->setRange(-100000, 100000);
    runMsEdit_ = numericEdit(panel, QStringLiteral("30000"));
    viStallMsEdit_ = numericEdit(panel, QStringLiteral("2500"));
    progressCheck_ = new QCheckBox(panel);
    singleTurnRunnerCheck_ = new QCheckBox(panel);
    singleTurnRunnerCheck_->setChecked(true);
    autoWaveTriggerCheck_ = new QCheckBox(panel);
    minFakeAttacksSpin_ = new QSpinBox(panel);
    minFakeAttacksSpin_->setRange(0, 1000);
    maxFakeAttacksSpin_ = new QSpinBox(panel);
    maxFakeAttacksSpin_->setRange(0, 1000);
    form->addRow(QStringLiteral("Name"), nameEdit_);
    form->addRow(QStringLiteral("Priority"), prioritySpin_);
    form->addRow(QStringLiteral("Run ms"), runMsEdit_);
    form->addRow(QStringLiteral("VI stall ms"), viStallMsEdit_);
    form->addRow(QStringLiteral("Progress"), progressCheck_);
    form->addRow(QStringLiteral("Single turn runner"), singleTurnRunnerCheck_);
    form->addRow(QStringLiteral("Auto wave trigger"), autoWaveTriggerCheck_);
    form->addRow(QStringLiteral("Min fake attacks"), minFakeAttacksSpin_);
    form->addRow(QStringLiteral("Max fake attacks"), maxFakeAttacksSpin_);
    root->addWidget(panel, 1);
    auto* buttons = new QHBoxLayout();
    buttons->addStretch();
    saveButton_ = new QPushButton(QStringLiteral("Save Battle Run Spec"), this);
    saveButton_->setObjectName("jobsPrimaryButton");
    buttons->addWidget(saveButton_);
    root->addLayout(buttons);
    connect(saveButton_, &QPushButton::clicked, this, &BattleRunSpecEditorWindow::saveSpec);
}

void BattleRunSpecEditorWindow::saveSpec()
{
    if (nameEdit_->text().trimmed().isEmpty()) {
        postStatusMessage(QStringLiteral("Battle run spec name is required."), StatusToast::Severity::Warn);
        return;
    }
    if (maxFakeAttacksSpin_->value() < minFakeAttacksSpin_->value()) {
        postStatusMessage(QStringLiteral("Max fake attacks must be >= min fake attacks."), StatusToast::Severity::Warn);
        return;
    }
    QString error;
    std::int64_t runMs = 0;
    std::int64_t viStallMs = 0;
    if (!parseInt64(runMsEdit_, QStringLiteral("Run ms"), &runMs, &error)
        || !parseInt64(viStallMsEdit_, QStringLiteral("VI stall ms"), &viStallMs, &error)) {
        postStatusMessage(error, StatusToast::Severity::Warn);
        return;
    }
    soasimqt2::db::BattleRunSpecDraft draft{};
    draft.name = nameEdit_->text().trimmed().toStdString();
    draft.priority = prioritySpin_->value();
    draft.run_ms = runMs;
    draft.vi_stall_ms = viStallMs;
    draft.progress_enable = progressCheck_->isChecked();
    draft.use_single_turn_runner = singleTurnRunnerCheck_->isChecked();
    draft.auto_wave_trigger_enable = autoWaveTriggerCheck_->isChecked();
    draft.min_fake_attacks = minFakeAttacksSpin_->value();
    draft.max_fake_attacks = maxFakeAttacksSpin_->value();
    const auto result = soasimqt2::db::SimCoreDbAuthoringService::SaveBattleRunSpec(draft);
    if (!result.ok) {
        postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
        return;
    }
    notifySaved(savedCallback_);
    postStatusMessage(QStringLiteral("Saved battle run spec %1.").arg(static_cast<qint64>(result.value)), StatusToast::Severity::Info);
}

void BattleRunSpecEditorWindow::postStatusMessage(const QString& text, StatusToast::Severity severity)
{
    if (statusCallback_ && !text.isEmpty()) statusCallback_(text, severity);
}

PredicateSetEditorWindow::PredicateSetEditorWindow(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlag(Qt::Window, true);
    setWindowTitle(QStringLiteral("Predicate Set Editor"));
    resize(520, 520);
    createWidgets();
    refreshPredicates();
}

void PredicateSetEditorWindow::setStatusCallback(std::function<void(const QString&, StatusToast::Severity)> callback)
{
    statusCallback_ = std::move(callback);
}

void PredicateSetEditorWindow::setSavedCallback(std::function<void()> callback)
{
    savedCallback_ = std::move(callback);
}

void PredicateSetEditorWindow::createWidgets()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    auto* label = new QLabel(QStringLiteral("Select predicates to include in the set."), this);
    label->setObjectName("sectionDescription");
    root->addWidget(label);
    predicateList_ = new QListWidget(this);
    predicateList_->setSelectionMode(QListWidget::ExtendedSelection);
    root->addWidget(predicateList_, 1);
    auto* buttons = new QHBoxLayout();
    refreshButton_ = new QPushButton(QStringLiteral("Refresh"), this);
    refreshButton_->setObjectName("jobsSecondaryButton");
    saveButton_ = new QPushButton(QStringLiteral("Save Predicate Set"), this);
    saveButton_->setObjectName("jobsPrimaryButton");
    buttons->addWidget(refreshButton_);
    buttons->addStretch();
    buttons->addWidget(saveButton_);
    root->addLayout(buttons);
    connect(refreshButton_, &QPushButton::clicked, this, &PredicateSetEditorWindow::refreshPredicates);
    connect(saveButton_, &QPushButton::clicked, this, &PredicateSetEditorWindow::saveSpec);
}

void PredicateSetEditorWindow::refreshPredicates()
{
    const auto result = soasimqt2::db::SimCoreDbAuthoringService::ListPredicateSpecs();
    if (!result.ok) {
        postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
        return;
    }
    predicateList_->clear();
    for (const auto& predicate : result.value) {
        auto* item = new QListWidgetItem(
            QStringLiteral("#%1 %2")
                .arg(static_cast<qint64>(predicate.predicate_spec_id))
                .arg(QString::fromStdString(predicate.name)),
            predicateList_);
        item->setData(Qt::UserRole, static_cast<qint64>(predicate.predicate_spec_id));
    }
}

void PredicateSetEditorWindow::saveSpec()
{
    soasimqt2::db::PredicateSetDraft draft{};
    for (const auto& item : predicateList_->selectedItems()) {
        const auto id = item->data(Qt::UserRole).toLongLong();
        if (id > 0) {
            draft.predicate_spec_ids.push_back(id);
        }
    }
    const auto result = soasimqt2::db::SimCoreDbAuthoringService::SavePredicateSet(draft);
    if (!result.ok) {
        postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
        return;
    }
    notifySaved(savedCallback_);
    postStatusMessage(QStringLiteral("Saved predicate set %1.").arg(static_cast<qint64>(result.value)), StatusToast::Severity::Info);
}

void PredicateSetEditorWindow::postStatusMessage(const QString& text, StatusToast::Severity severity)
{
    if (statusCallback_ && !text.isEmpty()) statusCallback_(text, severity);
}

ExplorerSettingsEditorWindow::ExplorerSettingsEditorWindow(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlag(Qt::Window, true);
    setWindowTitle(QStringLiteral("Explorer Settings Editor"));
    resize(620, 420);
    createWidgets();
    refreshChoices();
}

void ExplorerSettingsEditorWindow::setStatusCallback(std::function<void(const QString&, StatusToast::Severity)> callback)
{
    statusCallback_ = std::move(callback);
}

void ExplorerSettingsEditorWindow::setSavedCallback(std::function<void()> callback)
{
    savedCallback_ = std::move(callback);
}

void ExplorerSettingsEditorWindow::createWidgets()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    auto* panel = createPanel(this);
    auto* form = new QFormLayout(panel);
    form->setContentsMargins(14, 14, 14, 14);
    nameEdit_ = new QLineEdit(panel);
    descriptionEdit_ = new QPlainTextEdit(panel);
    descriptionEdit_->setMaximumHeight(80);
    battlePlanCombo_ = new QComboBox(panel);
    predicateSetCombo_ = new QComboBox(panel);
    form->addRow(QStringLiteral("Name"), nameEdit_);
    form->addRow(QStringLiteral("Description"), descriptionEdit_);
    form->addRow(QStringLiteral("Default battle plan"), battlePlanCombo_);
    form->addRow(QStringLiteral("Default predicate set"), predicateSetCombo_);
    root->addWidget(panel, 1);
    auto* buttons = new QHBoxLayout();
    refreshButton_ = new QPushButton(QStringLiteral("Refresh"), this);
    refreshButton_->setObjectName("jobsSecondaryButton");
    saveButton_ = new QPushButton(QStringLiteral("Save Explorer Settings"), this);
    saveButton_->setObjectName("jobsPrimaryButton");
    buttons->addWidget(refreshButton_);
    buttons->addStretch();
    buttons->addWidget(saveButton_);
    root->addLayout(buttons);
    connect(refreshButton_, &QPushButton::clicked, this, &ExplorerSettingsEditorWindow::refreshChoices);
    connect(saveButton_, &QPushButton::clicked, this, &ExplorerSettingsEditorWindow::saveSpec);
}

void ExplorerSettingsEditorWindow::refreshChoices()
{
    addNoneOption(battlePlanCombo_);
    addNoneOption(predicateSetCombo_);
    const auto plans = soasimqt2::db::SimCoreDbAuthoringService::ListBattlePlans();
    if (plans.ok) {
        for (const auto& plan : plans.value) {
            battlePlanCombo_->addItem(battlePlanLabel(plan), static_cast<qint64>(plan.plan_id));
        }
    }
    const auto predicateSets = soasimqt2::db::SimCoreDbAuthoringService::ListPredicateSets();
    if (predicateSets.ok) {
        for (const auto& set : predicateSets.value) {
            predicateSetCombo_->addItem(predicateSetLabel(set), static_cast<qint64>(set.predicate_set_id));
        }
    }
}

void ExplorerSettingsEditorWindow::saveSpec()
{
    if (nameEdit_->text().trimmed().isEmpty()) {
        postStatusMessage(QStringLiteral("Explorer settings name is required."), StatusToast::Severity::Warn);
        return;
    }
    soasimqt2::db::ExplorerSettingsDraft draft{};
    draft.name = nameEdit_->text().trimmed().toStdString();
    draft.description = descriptionEdit_->toPlainText().trimmed().toStdString();
    draft.default_plan_id = optionalComboId(battlePlanCombo_);
    draft.default_predicate_set_id = optionalComboId(predicateSetCombo_);
    const auto result = soasimqt2::db::SimCoreDbAuthoringService::SaveExplorerSettings(draft);
    if (!result.ok) {
        postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
        return;
    }
    notifySaved(savedCallback_);
    postStatusMessage(QStringLiteral("Saved explorer settings %1.").arg(static_cast<qint64>(result.value)), StatusToast::Severity::Info);
}

void ExplorerSettingsEditorWindow::postStatusMessage(const QString& text, StatusToast::Severity severity)
{
    if (statusCallback_ && !text.isEmpty()) statusCallback_(text, severity);
}

TemplateEditorWindow::TemplateEditorWindow(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlag(Qt::Window, true);
    setWindowTitle(QStringLiteral("Template Editor"));
    resize(660, 460);
    createWidgets();
    refreshChoices();
}

void TemplateEditorWindow::setStatusCallback(std::function<void(const QString&, StatusToast::Severity)> callback)
{
    statusCallback_ = std::move(callback);
}

void TemplateEditorWindow::setSavedCallback(std::function<void()> callback)
{
    savedCallback_ = std::move(callback);
}

void TemplateEditorWindow::createWidgets()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    auto* panel = createPanel(this);
    auto* form = new QFormLayout(panel);
    form->setContentsMargins(14, 14, 14, 14);
    nameEdit_ = new QLineEdit(panel);
    descriptionEdit_ = new QPlainTextEdit(panel);
    descriptionEdit_->setMaximumHeight(80);
    seedProbeCombo_ = new QComboBox(panel);
    tasCombo_ = new QComboBox(panel);
    battleRunCombo_ = new QComboBox(panel);
    explorerSettingsCombo_ = new QComboBox(panel);
    form->addRow(QStringLiteral("Name"), nameEdit_);
    form->addRow(QStringLiteral("Description"), descriptionEdit_);
    form->addRow(QStringLiteral("Seed probe spec"), seedProbeCombo_);
    form->addRow(QStringLiteral("TAS spec"), tasCombo_);
    form->addRow(QStringLiteral("Battle run spec"), battleRunCombo_);
    form->addRow(QStringLiteral("Explorer settings"), explorerSettingsCombo_);
    root->addWidget(panel, 1);
    auto* buttons = new QHBoxLayout();
    refreshButton_ = new QPushButton(QStringLiteral("Refresh"), this);
    refreshButton_->setObjectName("jobsSecondaryButton");
    saveButton_ = new QPushButton(QStringLiteral("Save Template"), this);
    saveButton_->setObjectName("jobsPrimaryButton");
    buttons->addWidget(refreshButton_);
    buttons->addStretch();
    buttons->addWidget(saveButton_);
    root->addLayout(buttons);
    connect(refreshButton_, &QPushButton::clicked, this, &TemplateEditorWindow::refreshChoices);
    connect(saveButton_, &QPushButton::clicked, this, &TemplateEditorWindow::saveSpec);
}

void TemplateEditorWindow::refreshChoices()
{
    addNoneOption(seedProbeCombo_);
    addNoneOption(tasCombo_);
    addNoneOption(battleRunCombo_);
    addNoneOption(explorerSettingsCombo_);
    const auto seedProbeSpecs = soasimqt2::db::SimCoreDbAuthoringService::ListSeedProbeSpecs();
    if (seedProbeSpecs.ok) {
        for (const auto& spec : seedProbeSpecs.value) {
            seedProbeCombo_->addItem(seedProbeLabel(spec), static_cast<qint64>(spec.seed_probe_spec_id));
        }
    }
    const auto tasSpecs = soasimqt2::db::SimCoreDbAuthoringService::ListTasSpecs();
    if (tasSpecs.ok) {
        for (const auto& spec : tasSpecs.value) {
            tasCombo_->addItem(tasLabel(spec), static_cast<qint64>(spec.tas_spec_id));
        }
    }
    const auto battleRunSpecs = soasimqt2::db::SimCoreDbAuthoringService::ListBattleRunSpecs();
    if (battleRunSpecs.ok) {
        for (const auto& spec : battleRunSpecs.value) {
            battleRunCombo_->addItem(battleRunLabel(spec), static_cast<qint64>(spec.battle_run_spec_id));
        }
    }
    const auto explorerSettings = soasimqt2::db::SimCoreDbAuthoringService::ListExplorerSettings();
    if (explorerSettings.ok) {
        for (const auto& settings : explorerSettings.value) {
            explorerSettingsCombo_->addItem(explorerSettingsLabel(settings), static_cast<qint64>(settings.explorer_settings_id));
        }
    }
}

void TemplateEditorWindow::saveSpec()
{
    if (nameEdit_->text().trimmed().isEmpty()) {
        postStatusMessage(QStringLiteral("Template name is required."), StatusToast::Severity::Warn);
        return;
    }
    soasimqt2::db::TemplateDraft draft{};
    draft.name = nameEdit_->text().trimmed().toStdString();
    draft.description = descriptionEdit_->toPlainText().trimmed().toStdString();
    draft.seed_probe_spec_id = optionalComboId(seedProbeCombo_);
    draft.tas_spec_id = optionalComboId(tasCombo_);
    draft.battle_run_spec_id = optionalComboId(battleRunCombo_);
    draft.explorer_settings_id = optionalComboId(explorerSettingsCombo_);
    const auto result = soasimqt2::db::SimCoreDbAuthoringService::SaveTemplate(draft);
    if (!result.ok) {
        postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
        return;
    }
    notifySaved(savedCallback_);
    postStatusMessage(QStringLiteral("Saved template %1.").arg(static_cast<qint64>(result.value)), StatusToast::Severity::Info);
}

void TemplateEditorWindow::postStatusMessage(const QString& text, StatusToast::Severity severity)
{
    if (statusCallback_ && !text.isEmpty()) statusCallback_(text, severity);
}
