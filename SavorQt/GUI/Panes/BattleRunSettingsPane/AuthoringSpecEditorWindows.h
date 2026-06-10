#pragma once

#include <functional>
#include <optional>

#include "Authoring/IAuthoringDb.h"
#include <QtWidgets/QWidget>

#include "GUI/Common/StatusToast.h"

class QCheckBox;
class QComboBox;
class QCloseEvent;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

class SeedProbeSpecEditorWindow final : public QWidget
{
public:
    explicit SeedProbeSpecEditorWindow(QWidget* parent = nullptr, bool embeddedInContainer = false);
    void setStatusCallback(std::function<void(const QString&, StatusToast::Severity)> callback);
    void setSavedCallback(std::function<void()> callback);
    void loadSnapshot(const savor::db::SeedProbeSpecSnapshot& snapshot, bool duplicate);

private:
    void createWidgets();
    void saveSpec();
    void postStatusMessage(const QString& text, StatusToast::Severity severity);

    std::function<void(const QString&, StatusToast::Severity)> statusCallback_;
    std::function<void()> savedCallback_;
    QLineEdit* nameEdit_ = nullptr;
    QSpinBox* prioritySpin_ = nullptr;
    QLineEdit* runMsEdit_ = nullptr;
    QLineEdit* viStallMsEdit_ = nullptr;
    QSpinBox* samplesPerAxisSpin_ = nullptr;
    QLineEdit* minValueEdit_ = nullptr;
    QLineEdit* maxValueEdit_ = nullptr;
    QCheckBox* capTriggerTopCheck_ = nullptr;
    QCheckBox* ignoreTriggerMinMaxCheck_ = nullptr;
    QSpinBox* comboAttemptsSpin_ = nullptr;
    QSpinBox* comboSamplerTriesSpin_ = nullptr;
    QPushButton* saveButton_ = nullptr;
};

class TasSpecEditorWindow final : public QWidget
{
public:
    explicit TasSpecEditorWindow(QWidget* parent = nullptr, bool embeddedInContainer = false);
    void setStatusCallback(std::function<void(const QString&, StatusToast::Severity)> callback);
    void setSavedCallback(std::function<void()> callback);
    void loadSnapshot(const savor::db::TasSpecSnapshot& snapshot, bool duplicate);

private:
    void createWidgets();
    void saveSpec();
    void postStatusMessage(const QString& text, StatusToast::Severity severity);

    std::function<void(const QString&, StatusToast::Severity)> statusCallback_;
    std::function<void()> savedCallback_;
    QLineEdit* nameEdit_ = nullptr;
    QSpinBox* prioritySpin_ = nullptr;
    QLineEdit* runMsEdit_ = nullptr;
    QLineEdit* viStallMsEdit_ = nullptr;
    QSpinBox* headroomSpin_ = nullptr;
    QCheckBox* progressCheck_ = nullptr;
    QPushButton* saveButton_ = nullptr;
};

class BattleRunSpecEditorWindow final : public QWidget
{
public:
    explicit BattleRunSpecEditorWindow(QWidget* parent = nullptr, bool embeddedInContainer = false);
    void setStatusCallback(std::function<void(const QString&, StatusToast::Severity)> callback);
    void setSavedCallback(std::function<void()> callback);
    void loadSnapshot(const savor::db::BattleRunSpecSnapshot& snapshot, bool duplicate);

private:
    void createWidgets();
    void saveSpec();
    void postStatusMessage(const QString& text, StatusToast::Severity severity);

    std::function<void(const QString&, StatusToast::Severity)> statusCallback_;
    std::function<void()> savedCallback_;
    QLineEdit* nameEdit_ = nullptr;
    QSpinBox* prioritySpin_ = nullptr;
    QLineEdit* runMsEdit_ = nullptr;
    QLineEdit* viStallMsEdit_ = nullptr;
    QCheckBox* progressCheck_ = nullptr;
    QCheckBox* singleTurnRunnerCheck_ = nullptr;
    QCheckBox* autoWaveTriggerCheck_ = nullptr;
    QSpinBox* minFakeAttacksSpin_ = nullptr;
    QSpinBox* maxFakeAttacksSpin_ = nullptr;
    QPushButton* saveButton_ = nullptr;
};

class PredicateSetEditorWindow final : public QWidget
{
public:
    explicit PredicateSetEditorWindow(QWidget* parent = nullptr, bool embeddedInContainer = false);
    void loadSnapshot(const savor::db::PredicateSetSnapshot& snapshot, bool duplicate);
    void setStatusCallback(std::function<void(const QString&, StatusToast::Severity)> callback);
    void setSavedCallback(std::function<void()> callback);

private:
    void createWidgets();
    void refreshPredicates();
    void saveSpec();
    void postStatusMessage(const QString& text, StatusToast::Severity severity);

    std::function<void(const QString&, StatusToast::Severity)> statusCallback_;
    std::function<void()> savedCallback_;
    QListWidget* predicateList_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QPushButton* saveButton_ = nullptr;
};

class ExplorerSettingsEditorWindow final : public QWidget
{
public:
    explicit ExplorerSettingsEditorWindow(QWidget* parent = nullptr, bool embeddedInContainer = false);
    void setStatusCallback(std::function<void(const QString&, StatusToast::Severity)> callback);
    void setSavedCallback(std::function<void()> callback);
    void loadSnapshot(const savor::db::ExplorerSettingsSnapshot& snapshot, bool duplicate);

private:
    void createWidgets();
    void refreshChoices();
    void saveSpec();
    void postStatusMessage(const QString& text, StatusToast::Severity severity);

    std::function<void(const QString&, StatusToast::Severity)> statusCallback_;
    std::function<void()> savedCallback_;
    bool duplicateLoad_ = false;
    std::optional<std::int64_t> duplicateSourceId_;
    QLineEdit* nameEdit_ = nullptr;
    QPlainTextEdit* descriptionEdit_ = nullptr;
    QComboBox* battlePlanCombo_ = nullptr;
    QComboBox* predicateSetCombo_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QPushButton* saveButton_ = nullptr;
};

class BattleChainSpecEditorWindow final : public QWidget
{
public:
    explicit BattleChainSpecEditorWindow(QWidget* parent = nullptr);
    void setStatusCallback(std::function<void(const QString&, StatusToast::Severity)> callback);
    void setSavedCallback(std::function<void()> callback);

private:
    void createWidgets();
    void refreshChoices();
    void saveSpec();
    void postStatusMessage(const QString& text, StatusToast::Severity severity);

    std::function<void(const QString&, StatusToast::Severity)> statusCallback_;
    std::function<void()> savedCallback_;
    QLineEdit* nameEdit_ = nullptr;
    QPlainTextEdit* descriptionEdit_ = nullptr;
    QComboBox* battleRunCombo_ = nullptr;
    QComboBox* explorerSettingsCombo_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QPushButton* saveButton_ = nullptr;
};
