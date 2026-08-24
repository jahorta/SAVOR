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
    QLineEdit* minValueEdit_ = nullptr;
    QLineEdit* maxValueEdit_ = nullptr;
    QCheckBox* capTriggerTopCheck_ = nullptr;
    QCheckBox* ignoreTriggerMinMaxCheck_ = nullptr;
    QSpinBox* comboAttemptsSpin_ = nullptr;
    QSpinBox* comboSamplerTriesSpin_ = nullptr;
    QPushButton* saveButton_ = nullptr;
};

