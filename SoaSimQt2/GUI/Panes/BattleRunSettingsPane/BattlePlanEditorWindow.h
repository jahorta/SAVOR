#pragma once

#include <functional>

#include <QtWidgets/QWidget>

#include "GUI/Common/StatusToast.h"

class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

class BattlePlanEditorWindow final : public QWidget
{
public:
    explicit BattlePlanEditorWindow(QWidget* parent = nullptr);

    void setStatusCallback(std::function<void(const QString&, StatusToast::Severity)> callback);

private:
    void createWidgets();
    void saveBattlePlan();
    void postStatusMessage(const QString& text, StatusToast::Severity severity);

    std::function<void(const QString&, StatusToast::Severity)> statusCallback_;
    QLineEdit* nameEdit_ = nullptr;
    QSpinBox* turnCountSpin_ = nullptr;
    QPlainTextEdit* actionsText_ = nullptr;
    QPushButton* saveButton_ = nullptr;
};
