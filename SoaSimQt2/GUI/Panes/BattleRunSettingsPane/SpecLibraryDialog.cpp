#include "SpecLibraryDialog.h"

#include "AuthoringSpecEditorWindows.h"
#include "BattlePlanEditorWindow.h"
#include "DB/SimCoreDbAuthoringService.h"
#include "PredicateSpecEditorWindow.h"

#include <QtCore/QDateTime>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QVBoxLayout>

namespace {

QFrame* createSurfacePanel(QWidget* parent)
{
    auto* panel = new QFrame(parent);
    panel->setObjectName("jobsSurfacePanel");
    return panel;
}

} // namespace

SpecLibraryDialog::SpecLibraryDialog(SpecKind kind, QWidget* parent)
    : QDialog(parent)
    , kind_(kind)
{
    setWindowTitle(dialogTitle());
    resize(1120, 640);
    setAttribute(Qt::WA_DeleteOnClose);
    createWidgets();
    refreshLibrary();
}

void SpecLibraryDialog::createWidgets()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto* toolbar = createSurfacePanel(this);
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(12, 10, 12, 10);
    toolbarLayout->setSpacing(10);
    newButton_ = new QPushButton(QStringLiteral("New"), toolbar);
    newButton_->setObjectName("jobsPrimaryButton");
    editButton_ = new QPushButton(QStringLiteral("Edit Selected (copy)"), toolbar);
    editButton_->setObjectName("jobsSecondaryButton");
    editButton_->setEnabled(false);
    refreshButton_ = new QPushButton(QStringLiteral("Refresh"), toolbar);
    refreshButton_->setObjectName("jobsSecondaryButton");
    toolbarLayout->addWidget(newButton_);
    toolbarLayout->addWidget(editButton_);
    toolbarLayout->addWidget(refreshButton_);
    toolbarLayout->addStretch();
    root->addWidget(toolbar);

    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->setChildrenCollapsible(false);
    auto* leftPanel = createSurfacePanel(splitter_);
    auto* leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(12, 12, 12, 12);
    leftLayout->setSpacing(8);
    leftLayout->addWidget(new QLabel(QStringLiteral("Saved specs"), leftPanel));
    libraryList_ = new QListWidget(leftPanel);
    leftLayout->addWidget(libraryList_, 1);
    libraryStatusLabel_ = new QLabel(leftPanel);
    libraryStatusLabel_->setObjectName("sectionDescription");
    leftLayout->addWidget(libraryStatusLabel_);

    rightPane_ = createSurfacePanel(splitter_);
    rightPaneLayout_ = new QVBoxLayout(rightPane_);
    rightPaneLayout_->setContentsMargins(12, 12, 12, 12);
    rightPaneLayout_->setSpacing(10);
    placeholderLabel_ = new QLabel(rightPane_);
    placeholderLabel_->setObjectName("sectionDescription");
    placeholderLabel_->setWordWrap(true);
    placeholderLabel_->setText(QStringLiteral("Create a new spec, or select one from the library to edit a copy."));
    rightPaneLayout_->addWidget(placeholderLabel_);
    rightPaneLayout_->addStretch(1);

    splitter_->addWidget(leftPanel);
    splitter_->addWidget(rightPane_);
    splitter_->setStretchFactor(0, 1);
    splitter_->setStretchFactor(1, 2);
    root->addWidget(splitter_, 1);

    connect(newButton_, &QPushButton::clicked, this, [this]() {
        clearRightPane();
        switch (kind_) {
        case SpecKind::SeedProbe:
            openNewSeedProbeSpecEditor();
            break;
        case SpecKind::Tas:
            openNewTasSpecEditor();
            break;
        case SpecKind::BattleRun:
            openNewBattleRunSpecEditor();
            break;
        case SpecKind::Predicate:
            openNewPredicateEditor();
            break;
        case SpecKind::BattlePlan:
            openNewBattlePlanEditor();
            break;
        }
    });
    connect(editButton_, &QPushButton::clicked, this, [this]() {
        const int row = selectedLibraryRow();
        if (row < 0) {
            return;
        }
        showForRow(row, true);
    });
    connect(refreshButton_, &QPushButton::clicked, this, &SpecLibraryDialog::refreshLibrary);
    connect(libraryList_, &QListWidget::currentRowChanged, this, &SpecLibraryDialog::handleLibrarySelectionChanged);
    connect(libraryList_, &QListWidget::itemDoubleClicked, this, &SpecLibraryDialog::onLibraryActivated);
}

void SpecLibraryDialog::refreshLibrary()
{
    switch (kind_) {
    case SpecKind::SeedProbe:
        refreshSeedProbeSpecs();
        break;
    case SpecKind::Tas:
        refreshTasSpecs();
        break;
    case SpecKind::BattleRun:
        refreshBattleRunSpecs();
        break;
    case SpecKind::Predicate:
        refreshPredicates();
        break;
    case SpecKind::BattlePlan:
        refreshBattlePlans();
        break;
    }
}

void SpecLibraryDialog::refreshSeedProbeSpecs()
{
    const auto result = soasimqt2::db::SimCoreDbAuthoringService::ListSeedProbeSpecs();
    if (!result.ok) {
        postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
        return;
    }
    seedProbeSpecs_ = result.value;
    libraryList_->clear();
    for (const auto& row : seedProbeSpecs_) {
        auto* item = new QListWidgetItem(seedProbeText(row), libraryList_);
        item->setData(Qt::UserRole, static_cast<qint64>(row.seed_probe_spec_id));
    }
    libraryStatusLabel_->setText(libraryCountText(seedProbeSpecs_.size()));
    updateEditButtonState();
}

void SpecLibraryDialog::refreshTasSpecs()
{
    const auto result = soasimqt2::db::SimCoreDbAuthoringService::ListTasSpecs();
    if (!result.ok) {
        postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
        return;
    }
    tasSpecs_ = result.value;
    libraryList_->clear();
    for (const auto& row : tasSpecs_) {
        auto* item = new QListWidgetItem(tasText(row), libraryList_);
        item->setData(Qt::UserRole, static_cast<qint64>(row.tas_spec_id));
    }
    libraryStatusLabel_->setText(libraryCountText(tasSpecs_.size()));
    updateEditButtonState();
}

void SpecLibraryDialog::refreshBattleRunSpecs()
{
    const auto result = soasimqt2::db::SimCoreDbAuthoringService::ListBattleRunSpecs();
    if (!result.ok) {
        postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
        return;
    }
    battleRunSpecs_ = result.value;
    libraryList_->clear();
    for (const auto& row : battleRunSpecs_) {
        auto* item = new QListWidgetItem(battleRunText(row), libraryList_);
        item->setData(Qt::UserRole, static_cast<qint64>(row.battle_run_spec_id));
    }
    libraryStatusLabel_->setText(libraryCountText(battleRunSpecs_.size()));
    updateEditButtonState();
}

void SpecLibraryDialog::refreshPredicates()
{
    const auto result = soasimqt2::db::SimCoreDbAuthoringService::ListPredicateSpecs();
    if (!result.ok) {
        postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
        return;
    }
    predicateSpecs_ = result.value;
    libraryList_->clear();
    for (const auto& row : predicateSpecs_) {
        auto* item = new QListWidgetItem(predicateText(row), libraryList_);
        item->setData(Qt::UserRole, static_cast<qint64>(row.predicate_spec_id));
    }
    libraryStatusLabel_->setText(libraryCountText(predicateSpecs_.size()));
    updateEditButtonState();
}

void SpecLibraryDialog::refreshBattlePlans()
{
    const auto result = soasimqt2::db::SimCoreDbAuthoringService::ListBattlePlans();
    if (!result.ok) {
        postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
        return;
    }
    battlePlanSpecs_ = result.value;
    libraryList_->clear();
    for (const auto& row : battlePlanSpecs_) {
        auto* item = new QListWidgetItem(battlePlanText(row), libraryList_);
        item->setData(Qt::UserRole, static_cast<qint64>(row.plan_id));
    }
    libraryStatusLabel_->setText(libraryCountText(battlePlanSpecs_.size()));
    updateEditButtonState();
}

void SpecLibraryDialog::handleLibrarySelectionChanged()
{
    updateEditButtonState();
    const int row = selectedLibraryRow();
    if (row < 0) {
        clearRightPane();
        if (placeholderLabel_) {
            placeholderLabel_->setText(QStringLiteral("Create a new spec, or select one from the library to edit a copy."));
        }
        return;
    }
    showForRow(row, true);
}

void SpecLibraryDialog::onLibraryActivated()
{
    const int row = selectedLibraryRow();
    if (row < 0) {
        return;
    }
    showForRow(row, true);
}

void SpecLibraryDialog::showForRow(int row, bool duplicate)
{
    switch (kind_) {
    case SpecKind::SeedProbe:
        showEditorForSeedProbe(row, duplicate);
        break;
    case SpecKind::Tas:
        showEditorForTas(row, duplicate);
        break;
    case SpecKind::BattleRun:
        showEditorForBattleRun(row, duplicate);
        break;
    case SpecKind::Predicate:
        showEditorForPredicate(row, duplicate);
        break;
    case SpecKind::BattlePlan:
        showEditorForBattlePlan(row, duplicate);
        break;
    }
}

void SpecLibraryDialog::openNewSeedProbeSpecEditor()
{
    auto* editor = new SeedProbeSpecEditorWindow(rightPane_, true);
    editor->setStatusCallback([this](const QString& text, StatusToast::Severity severity) {
        postStatusMessage(text, severity);
    });
    editor->setSavedCallback([this]() {
        refreshSeedProbeSpecs();
    });
    installEditorWidget(editor);
}

void SpecLibraryDialog::openNewTasSpecEditor()
{
    auto* editor = new TasSpecEditorWindow(rightPane_, true);
    editor->setStatusCallback([this](const QString& text, StatusToast::Severity severity) {
        postStatusMessage(text, severity);
    });
    editor->setSavedCallback([this]() {
        refreshTasSpecs();
    });
    installEditorWidget(editor);
}

void SpecLibraryDialog::openNewBattleRunSpecEditor()
{
    auto* editor = new BattleRunSpecEditorWindow(rightPane_, true);
    editor->setStatusCallback([this](const QString& text, StatusToast::Severity severity) {
        postStatusMessage(text, severity);
    });
    editor->setSavedCallback([this]() {
        refreshBattleRunSpecs();
    });
    installEditorWidget(editor);
}

void SpecLibraryDialog::openNewPredicateEditor()
{
    auto* editor = new PredicateSpecEditorWindow(rightPane_, true);
    editor->setStatusCallback([this](const QString& text, StatusToast::Severity severity) {
        postStatusMessage(text, severity);
    });
    editor->setSavedCallback([this]() {
        refreshPredicates();
    });
    installEditorWidget(editor);
}

void SpecLibraryDialog::openNewBattlePlanEditor()
{
    auto* editor = new BattlePlanEditorWindow(rightPane_, true);
    editor->setStatusCallback([this](const QString& text, StatusToast::Severity severity) {
        postStatusMessage(text, severity);
    });
    editor->setSavedCallback([this]() {
        refreshBattlePlans();
    });
    installEditorWidget(editor);
}

void SpecLibraryDialog::showEditorForSeedProbe(std::int64_t row, bool duplicate)
{
    if (row < 0 || row >= static_cast<int>(seedProbeSpecs_.size())) {
        return;
    }
    auto* editor = new SeedProbeSpecEditorWindow(rightPane_, true);
    editor->setStatusCallback([this](const QString& text, StatusToast::Severity severity) {
        postStatusMessage(text, severity);
    });
    editor->setSavedCallback([this]() {
        refreshSeedProbeSpecs();
    });
    editor->loadSnapshot(seedProbeSpecs_.at(static_cast<std::size_t>(row)), duplicate);
    installEditorWidget(editor);
}

void SpecLibraryDialog::showEditorForTas(std::int64_t row, bool duplicate)
{
    if (row < 0 || row >= static_cast<int>(tasSpecs_.size())) {
        return;
    }
    auto* editor = new TasSpecEditorWindow(rightPane_, true);
    editor->setStatusCallback([this](const QString& text, StatusToast::Severity severity) {
        postStatusMessage(text, severity);
    });
    editor->setSavedCallback([this]() {
        refreshTasSpecs();
    });
    editor->loadSnapshot(tasSpecs_.at(static_cast<std::size_t>(row)), duplicate);
    installEditorWidget(editor);
}

void SpecLibraryDialog::showEditorForBattleRun(std::int64_t row, bool duplicate)
{
    if (row < 0 || row >= static_cast<int>(battleRunSpecs_.size())) {
        return;
    }
    auto* editor = new BattleRunSpecEditorWindow(rightPane_, true);
    editor->setStatusCallback([this](const QString& text, StatusToast::Severity severity) {
        postStatusMessage(text, severity);
    });
    editor->setSavedCallback([this]() {
        refreshBattleRunSpecs();
    });
    editor->loadSnapshot(battleRunSpecs_.at(static_cast<std::size_t>(row)), duplicate);
    installEditorWidget(editor);
}

void SpecLibraryDialog::showEditorForPredicate(std::int64_t row, bool duplicate)
{
    if (row < 0 || row >= static_cast<int>(predicateSpecs_.size())) {
        return;
    }
    auto* editor = new PredicateSpecEditorWindow(rightPane_, true);
    editor->setStatusCallback([this](const QString& text, StatusToast::Severity severity) {
        postStatusMessage(text, severity);
    });
    editor->setSavedCallback([this]() {
        refreshPredicates();
    });
    editor->loadSnapshot(predicateSpecs_.at(static_cast<std::size_t>(row)), duplicate);
    installEditorWidget(editor);
}

void SpecLibraryDialog::showEditorForBattlePlan(std::int64_t row, bool duplicate)
{
    if (row < 0 || row >= static_cast<int>(battlePlanSpecs_.size())) {
        return;
    }
    auto* editor = new BattlePlanEditorWindow(rightPane_, true);
    editor->setStatusCallback([this](const QString& text, StatusToast::Severity severity) {
        postStatusMessage(text, severity);
    });
    editor->setSavedCallback([this]() {
        refreshBattlePlans();
    });
    editor->loadSnapshot(battlePlanSpecs_.at(static_cast<std::size_t>(row)), duplicate);
    installEditorWidget(editor);
}

void SpecLibraryDialog::clearRightPane()
{
    if (activeEditor_ != nullptr) {
        rightPaneLayout_->removeWidget(activeEditor_);
        activeEditor_->hide();
        activeEditor_->deleteLater();
        activeEditor_ = nullptr;
    }
    if (placeholderLabel_ && rightPaneLayout_->indexOf(placeholderLabel_) < 0) {
        rightPaneLayout_->insertWidget(0, placeholderLabel_);
        placeholderLabel_->show();
    }
}

void SpecLibraryDialog::installEditorWidget(QWidget* editor)
{
    if (editor == nullptr || rightPaneLayout_ == nullptr) {
        return;
    }

    clearRightPane();
    if (placeholderLabel_ != nullptr) {
        rightPaneLayout_->removeWidget(placeholderLabel_);
        placeholderLabel_->hide();
    }

    activeEditor_ = editor;
    rightPaneLayout_->addWidget(editor, 1);
    editor->show();
}

void SpecLibraryDialog::updateEditButtonState()
{
    editButton_->setEnabled(selectedLibraryRow() >= 0);
}

int SpecLibraryDialog::selectedLibraryRow() const
{
    const int row = libraryList_ != nullptr ? libraryList_->currentRow() : -1;
    return (row < 0 || row >= (libraryList_ != nullptr ? libraryList_->count() : 0)) ? -1 : row;
}

void SpecLibraryDialog::postStatusMessage(const QString& text, StatusToast::Severity severity)
{
    if (text.isEmpty()) {
        return;
    }
    emit statusToastRequested(StatusToast{
        severity,
        text,
        {},
        1,
        QDateTime{},
        4000
    });
}

QString SpecLibraryDialog::seedProbeText(const simcore::db::SeedProbeSpecSnapshot& snapshot)
{
    return QStringLiteral("#%1  %2")
        .arg(static_cast<qint64>(snapshot.seed_probe_spec_id))
        .arg(QString::fromStdString(snapshot.name));
}

QString SpecLibraryDialog::tasText(const simcore::db::TasSpecSnapshot& snapshot)
{
    return QStringLiteral("#%1  %2")
        .arg(static_cast<qint64>(snapshot.tas_spec_id))
        .arg(QString::fromStdString(snapshot.base_name));
}

QString SpecLibraryDialog::battleRunText(const simcore::db::BattleRunSpecSnapshot& snapshot)
{
    return QStringLiteral("#%1  %2")
        .arg(static_cast<qint64>(snapshot.battle_run_spec_id))
        .arg(QString::fromStdString(snapshot.name));
}

QString SpecLibraryDialog::predicateText(const simcore::db::PredicateSpecSnapshot& snapshot)
{
    return QStringLiteral("#%1  %2")
        .arg(static_cast<qint64>(snapshot.predicate_spec_id))
        .arg(QString::fromStdString(snapshot.name));
}

QString SpecLibraryDialog::battlePlanText(const simcore::db::BattlePlanSnapshot& snapshot)
{
    return QStringLiteral("#%1  %2")
        .arg(static_cast<qint64>(snapshot.plan_id))
        .arg(QString::fromStdString(snapshot.name));
}

QString SpecLibraryDialog::dialogTitle() const
{
    switch (kind_) {
    case SpecKind::SeedProbe:
        return QStringLiteral("Seed Probe Specs");
    case SpecKind::Tas:
        return QStringLiteral("TAS Specs");
    case SpecKind::BattleRun:
        return QStringLiteral("Battle Run Specs");
    case SpecKind::Predicate:
        return QStringLiteral("Predicates");
    case SpecKind::BattlePlan:
        return QStringLiteral("Battle Plans");
    }
    return QStringLiteral("Authoring Specs");
}

QString SpecLibraryDialog::libraryCountText(std::size_t count) const
{
    return QStringLiteral("Saved: %1").arg(static_cast<int>(count));
}
