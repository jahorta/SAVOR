#pragma once

#include <functional>

#include <QtWidgets/QWidget>

#include "GUI/Common/StatusToast.h"
#include "Authoring/IAuthoringDb.h"

class QCheckBox;
class QComboBox;
class QCloseEvent;
class QLineEdit;
class QPushButton;

class PredicateSpecEditorWindow final : public QWidget
{
public:
    explicit PredicateSpecEditorWindow(QWidget* parent = nullptr);

    void setStatusCallback(std::function<void(const QString&, StatusToast::Severity)> callback);
    void setSavedCallback(std::function<void()> callback);
    void loadSnapshot(const simcore::db::PredicateSpecSnapshot& snapshot, bool duplicate);

private:
    void closeEvent(QCloseEvent* event) override;
    void createWidgets();
    void savePredicate();
    void markDirty();
    bool confirmDiscardIfDirty();
    void postStatusMessage(const QString& text, StatusToast::Severity severity);

    std::function<void(const QString&, StatusToast::Severity)> statusCallback_;
    std::function<void()> savedCallback_;
    bool dirty_ = false;
    QLineEdit* nameEdit_ = nullptr;
    QLineEdit* breakpointEdit_ = nullptr;
    QComboBox* lhsKindCombo_ = nullptr;
    QLineEdit* lhsValueEdit_ = nullptr;
    QComboBox* rhsKindCombo_ = nullptr;
    QLineEdit* rhsValueEdit_ = nullptr;
    QComboBox* cmpCombo_ = nullptr;
    QComboBox* widthCombo_ = nullptr;
    QCheckBox* abortOnFailCheck_ = nullptr;
    QPushButton* saveButton_ = nullptr;
};
