#pragma once

#include <QtCore/QPointer>
#include <QtWidgets/QWidget>

#include "GUI/Common/StatusToast.h"

class BattlePlanEditorWindow;
class PredicateSpecEditorWindow;
class QPushButton;

class BattleRunSettingsPage final : public QWidget
{
    Q_OBJECT

public:
    explicit BattleRunSettingsPage(QWidget* parent = nullptr);
    ~BattleRunSettingsPage() override = default;

signals:
    void statusToastRequested(StatusToast toast);

private:
    void createWidgets();
    void openPredicateEditor();
    void openBattlePlanEditor();
    void postStatusMessage(const QString& text, StatusToast::Severity severity);

    QPushButton* newPredicateButton_ = nullptr;
    QPushButton* newBattlePlanButton_ = nullptr;
    QPointer<PredicateSpecEditorWindow> predicateEditor_;
    QPointer<BattlePlanEditorWindow> battlePlanEditor_;
};
