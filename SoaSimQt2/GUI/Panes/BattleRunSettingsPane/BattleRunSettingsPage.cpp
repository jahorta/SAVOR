#include "BattleRunSettingsPage.h"

#include "BattlePlanEditorWindow.h"
#include "PredicateSpecEditorWindow.h"

#include <QtCore/QDateTime>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

BattleRunSettingsPage::BattleRunSettingsPage(QWidget* parent)
    : QWidget(parent)
{
    createWidgets();
}

void BattleRunSettingsPage::createWidgets()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(10);

    auto* toolbar = new QFrame(this);
    toolbar->setObjectName("jobsToolbarPanel");
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(12, 10, 12, 10);
    toolbarLayout->setSpacing(10);

    newPredicateButton_ = new QPushButton(QStringLiteral("New Predicate"), toolbar);
    newBattlePlanButton_ = new QPushButton(QStringLiteral("New Battle Plan"), toolbar);
    newPredicateButton_->setObjectName("jobsPrimaryButton");
    newBattlePlanButton_->setObjectName("jobsPrimaryButton");
    toolbarLayout->addWidget(newPredicateButton_);
    toolbarLayout->addWidget(newBattlePlanButton_);
    toolbarLayout->addStretch();
    rootLayout->addWidget(toolbar);

    auto* body = new QFrame(this);
    body->setObjectName("jobsSurfacePanel");
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(18, 18, 18, 18);
    bodyLayout->setSpacing(8);
    auto* title = new QLabel(QStringLiteral("Authoring Library"), body);
    title->setObjectName("panelTitle");
    auto* detail = new QLabel(
        QStringLiteral("Predicates and battle plans are edited in modeless windows and saved to the Authoring database. Workflow graphs are edited from the Workflow Builder pane."),
        body);
    detail->setObjectName("sectionDescription");
    detail->setWordWrap(true);
    bodyLayout->addWidget(title);
    bodyLayout->addWidget(detail);
    bodyLayout->addStretch();
    rootLayout->addWidget(body, 1);

    connect(newPredicateButton_, &QPushButton::clicked, this, &BattleRunSettingsPage::openPredicateEditor);
    connect(newBattlePlanButton_, &QPushButton::clicked, this, &BattleRunSettingsPage::openBattlePlanEditor);
}

void BattleRunSettingsPage::openPredicateEditor()
{
    if (predicateEditor_) {
        predicateEditor_->show();
        predicateEditor_->raise();
        predicateEditor_->activateWindow();
        return;
    }

    auto* editor = new PredicateSpecEditorWindow(nullptr);
    predicateEditor_ = editor;
    editor->setStatusCallback([this](const QString& text, StatusToast::Severity severity) {
        postStatusMessage(text, severity);
    });
    connect(editor, &QObject::destroyed, this, [this]() { predicateEditor_.clear(); });
    editor->show();
}

void BattleRunSettingsPage::openBattlePlanEditor()
{
    if (battlePlanEditor_) {
        battlePlanEditor_->show();
        battlePlanEditor_->raise();
        battlePlanEditor_->activateWindow();
        return;
    }

    auto* editor = new BattlePlanEditorWindow(nullptr);
    battlePlanEditor_ = editor;
    editor->setStatusCallback([this](const QString& text, StatusToast::Severity severity) {
        postStatusMessage(text, severity);
    });
    connect(editor, &QObject::destroyed, this, [this]() { battlePlanEditor_.clear(); });
    editor->show();
}

void BattleRunSettingsPage::postStatusMessage(const QString& text, StatusToast::Severity severity)
{
    if (text.isEmpty()) {
        return;
    }
    emit statusToastRequested(StatusToast{ severity, text, {}, 1, QDateTime{}, 4000 });
}
