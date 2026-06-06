#pragma once

#include <functional>

#include <QtWidgets/QWidget>

#include "GUI/Common/StatusToast.h"
#include "Authoring/IAuthoringDb.h"

class QCloseEvent;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

class BattlePlanEditorWindow final : public QWidget
{
public:
    explicit BattlePlanEditorWindow(QWidget* parent = nullptr, bool embeddedInContainer = false);

    void setStatusCallback(std::function<void(const QString&, StatusToast::Severity)> callback);
    void setSavedCallback(std::function<void()> callback);
    void loadSnapshot(const simcore::db::BattlePlanSnapshot& snapshot, bool duplicate);

private:
    void closeEvent(QCloseEvent* event) override;
    void createWidgets();
    void addActionRow();
    void removeSelectedActionRows();
    void saveBattlePlan();
    void markDirty();
    bool confirmDiscardIfDirty();
    void postStatusMessage(const QString& text, StatusToast::Severity severity);

    std::function<void(const QString&, StatusToast::Severity)> statusCallback_;
    std::function<void()> savedCallback_;
    bool dirty_ = false;
    QLineEdit* nameEdit_ = nullptr;
    QSpinBox* turnCountSpin_ = nullptr;
    QTableWidget* actionsTable_ = nullptr;
    QPushButton* addActionButton_ = nullptr;
    QPushButton* removeActionButton_ = nullptr;
    QPushButton* saveButton_ = nullptr;
};
