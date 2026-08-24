#include "AuthoringSpecEditorWindows.h"

#include "DB/SavorDbAuthoringService.h"

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
#include <QtWidgets/QSizePolicy>
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


void notifySaved(const std::function<void()>& callback)
{
    if (callback) {
        callback();
    }
}

} // namespace

SeedProbeSpecEditorWindow::SeedProbeSpecEditorWindow(QWidget* parent, bool embeddedInContainer)
    : QWidget(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    if (!embeddedInContainer) {
        setWindowFlag(Qt::Window, true);
    }
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

void SeedProbeSpecEditorWindow::loadSnapshot(const savor::db::SeedProbeSpecSnapshot& snapshot, bool duplicate)
{
    setWindowTitle(duplicate
        ? QStringLiteral("Seed Probe Spec Editor - Duplicate")
        : QStringLiteral("Seed Probe Spec Editor - Edit Copy"));
    nameEdit_->setText(QString::fromStdString(snapshot.name) + (duplicate ? QStringLiteral(" copy") : QString()));
    prioritySpin_->setValue(snapshot.priority);
    minValueEdit_->setText(QString::number(snapshot.min_value));
    maxValueEdit_->setText(QString::number(snapshot.max_value));
    capTriggerTopCheck_->setChecked(snapshot.cap_trigger_top);
    ignoreTriggerMinMaxCheck_->setChecked(snapshot.ignore_trigger_minmax);
    comboAttemptsSpin_->setValue(snapshot.combo_attempts_per_target);
    comboSamplerTriesSpin_->setValue(snapshot.combo_sampler_tries);
}

void SeedProbeSpecEditorWindow::createWidgets()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    auto* panel = createPanel(this);
    auto* panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(14, 14, 14, 14);
    auto* form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    nameEdit_ = new QLineEdit(panel);
    prioritySpin_ = new QSpinBox(panel);
    prioritySpin_->setRange(0, 100000);
    minValueEdit_ = numericEdit(panel, QStringLiteral("48"));
    maxValueEdit_ = numericEdit(panel, QStringLiteral("207"));
    capTriggerTopCheck_ = new QCheckBox(panel);
    capTriggerTopCheck_->setChecked(true);
    ignoreTriggerMinMaxCheck_ = new QCheckBox(panel);
	ignoreTriggerMinMaxCheck_->setChecked(true);
    comboAttemptsSpin_ = new QSpinBox(panel);
    comboAttemptsSpin_->setRange(0, 512);
    comboAttemptsSpin_->setValue(32);
    comboSamplerTriesSpin_ = new QSpinBox(panel);
    comboSamplerTriesSpin_->setRange(0, 128);
    comboSamplerTriesSpin_->setValue(8);
    form->addRow(QStringLiteral("Name"), nameEdit_);
    form->addRow(QStringLiteral("Priority"), prioritySpin_);
    form->addRow(QStringLiteral("Min value"), minValueEdit_);
    form->addRow(QStringLiteral("Max value"), maxValueEdit_);
    form->addRow(QStringLiteral("Cap trigger top"), capTriggerTopCheck_);
    form->addRow(QStringLiteral("Ignore trigger min/max"), ignoreTriggerMinMaxCheck_);
    form->addRow(QStringLiteral("Combo attempts/target"), comboAttemptsSpin_);
    form->addRow(QStringLiteral("Combo sampler tries"), comboSamplerTriesSpin_);
    panelLayout->addLayout(form);
    panelLayout->addStretch(1);
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
    std::int64_t minValue = 0;
    std::int64_t maxValue = 0;
    if (!parseInt64(minValueEdit_, QStringLiteral("Min value"), &minValue, &error)
        || !parseInt64(maxValueEdit_, QStringLiteral("Max value"), &maxValue, &error)) {
        postStatusMessage(error, StatusToast::Severity::Warn);
        return;
    }

    savorqt::db::SeedProbeSpecDraft draft{};
    draft.name = nameEdit_->text().trimmed().toStdString();
    draft.priority = prioritySpin_->value();
    draft.min_value = minValue;
    draft.max_value = maxValue;
    draft.cap_trigger_top = capTriggerTopCheck_->isChecked();
    draft.ignore_trigger_minmax = ignoreTriggerMinMaxCheck_->isChecked();
    draft.combo_attempts_per_target = comboAttemptsSpin_->value();
    draft.combo_sampler_tries = comboSamplerTriesSpin_->value();
    const auto result = savorqt::db::SavorDbAuthoringService::SaveSeedProbeSpec(draft);
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
