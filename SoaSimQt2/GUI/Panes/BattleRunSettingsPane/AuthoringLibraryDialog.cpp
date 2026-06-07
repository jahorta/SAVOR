#include "AuthoringLibraryDialog.h"

#include "AuthoringSpecEditorWindows.h"
#include "BattlePlanEditorWindow.h"
#include "DB/SimCoreDbAuthoringService.h"
#include "PredicateSpecEditorWindow.h"

#include <QtCore/QDateTime>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
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

template <typename TEditor>
void attachCallbacks(TEditor* editor, const SpecLibraryCallbacks& callbacks)
{
    editor->setStatusCallback(callbacks.postStatus);
    editor->setSavedCallback(callbacks.refreshLibrary);
}

class SeedProbeSpecLibraryAdapter final : public ISpecLibraryAdapter {
public:
    AuthoringLibraryKey key() const override { return AuthoringLibraryKey::SeedProbe; }
    QString title() const override { return QStringLiteral("Seed Probe Specs"); }
    QString placeholderText() const override { return QStringLiteral("Create a new seed probe spec, or select one from the library to edit a copy."); }

    std::vector<SpecLibraryRow> refreshRows(QString* errorText) override
    {
        const auto result = soasimqt2::db::SimCoreDbAuthoringService::ListSeedProbeSpecs();
        if (!result.ok) {
            if (errorText != nullptr) *errorText = QString::fromStdString(result.error.message);
            return {};
        }
        rows_ = result.value;
        std::vector<SpecLibraryRow> rows;
        rows.reserve(rows_.size());
        for (const auto& row : rows_) {
            rows.push_back(SpecLibraryRow{
                static_cast<qint64>(row.seed_probe_spec_id),
                QStringLiteral("#%1  %2")
                    .arg(static_cast<qint64>(row.seed_probe_spec_id))
                    .arg(QString::fromStdString(row.name))
            });
        }
        return rows;
    }

    QWidget* createNewEditor(QWidget* parent, SpecLibraryCallbacks callbacks) override
    {
        auto* editor = new SeedProbeSpecEditorWindow(parent, true);
        attachCallbacks(editor, callbacks);
        return editor;
    }

    QWidget* createEditorForRow(int row, bool duplicate, QWidget* parent, SpecLibraryCallbacks callbacks) override
    {
        if (row < 0 || row >= static_cast<int>(rows_.size())) return nullptr;
        auto* editor = new SeedProbeSpecEditorWindow(parent, true);
        attachCallbacks(editor, callbacks);
        editor->loadSnapshot(rows_[static_cast<std::size_t>(row)], duplicate);
        return editor;
    }

private:
    std::vector<simcore::db::SeedProbeSpecSnapshot> rows_;
};

class TasSpecLibraryAdapter final : public ISpecLibraryAdapter {
public:
    AuthoringLibraryKey key() const override { return AuthoringLibraryKey::Tas; }
    QString title() const override { return QStringLiteral("TAS Specs"); }
    QString placeholderText() const override { return QStringLiteral("Create a new TAS spec, or select one from the library to edit a copy."); }

    std::vector<SpecLibraryRow> refreshRows(QString* errorText) override
    {
        const auto result = soasimqt2::db::SimCoreDbAuthoringService::ListTasSpecs();
        if (!result.ok) {
            if (errorText != nullptr) *errorText = QString::fromStdString(result.error.message);
            return {};
        }
        rows_ = result.value;
        std::vector<SpecLibraryRow> rows;
        rows.reserve(rows_.size());
        for (const auto& row : rows_) {
            rows.push_back(SpecLibraryRow{
                static_cast<qint64>(row.tas_spec_id),
                QStringLiteral("#%1  %2")
                    .arg(static_cast<qint64>(row.tas_spec_id))
                    .arg(QString::fromStdString(row.base_name))
            });
        }
        return rows;
    }

    QWidget* createNewEditor(QWidget* parent, SpecLibraryCallbacks callbacks) override
    {
        auto* editor = new TasSpecEditorWindow(parent, true);
        attachCallbacks(editor, callbacks);
        return editor;
    }

    QWidget* createEditorForRow(int row, bool duplicate, QWidget* parent, SpecLibraryCallbacks callbacks) override
    {
        if (row < 0 || row >= static_cast<int>(rows_.size())) return nullptr;
        auto* editor = new TasSpecEditorWindow(parent, true);
        attachCallbacks(editor, callbacks);
        editor->loadSnapshot(rows_[static_cast<std::size_t>(row)], duplicate);
        return editor;
    }

private:
    std::vector<simcore::db::TasSpecSnapshot> rows_;
};

class BattleRunSpecLibraryAdapter final : public ISpecLibraryAdapter {
public:
    AuthoringLibraryKey key() const override { return AuthoringLibraryKey::BattleRun; }
    QString title() const override { return QStringLiteral("Battle Run Specs"); }
    QString placeholderText() const override { return QStringLiteral("Create a new battle run spec, or select one from the library to edit a copy."); }

    std::vector<SpecLibraryRow> refreshRows(QString* errorText) override
    {
        const auto result = soasimqt2::db::SimCoreDbAuthoringService::ListBattleRunSpecs();
        if (!result.ok) {
            if (errorText != nullptr) *errorText = QString::fromStdString(result.error.message);
            return {};
        }
        rows_ = result.value;
        std::vector<SpecLibraryRow> rows;
        rows.reserve(rows_.size());
        for (const auto& row : rows_) {
            rows.push_back(SpecLibraryRow{
                static_cast<qint64>(row.battle_run_spec_id),
                QStringLiteral("#%1  %2")
                    .arg(static_cast<qint64>(row.battle_run_spec_id))
                    .arg(QString::fromStdString(row.name))
            });
        }
        return rows;
    }

    QWidget* createNewEditor(QWidget* parent, SpecLibraryCallbacks callbacks) override
    {
        auto* editor = new BattleRunSpecEditorWindow(parent, true);
        attachCallbacks(editor, callbacks);
        return editor;
    }

    QWidget* createEditorForRow(int row, bool duplicate, QWidget* parent, SpecLibraryCallbacks callbacks) override
    {
        if (row < 0 || row >= static_cast<int>(rows_.size())) return nullptr;
        auto* editor = new BattleRunSpecEditorWindow(parent, true);
        attachCallbacks(editor, callbacks);
        editor->loadSnapshot(rows_[static_cast<std::size_t>(row)], duplicate);
        return editor;
    }

private:
    std::vector<simcore::db::BattleRunSpecSnapshot> rows_;
};

class PredicateSpecLibraryAdapter final : public ISpecLibraryAdapter {
public:
    AuthoringLibraryKey key() const override { return AuthoringLibraryKey::Predicate; }
    QString title() const override { return QStringLiteral("Predicates"); }
    QString placeholderText() const override { return QStringLiteral("Create a new predicate, or select one from the library to edit it."); }
    bool supportsDelete() const override { return true; }
    bool editCreatesCopy() const override { return false; }

    std::vector<SpecLibraryRow> refreshRows(QString* errorText) override
    {
        const auto result = soasimqt2::db::SimCoreDbAuthoringService::ListPredicateSpecs();
        if (!result.ok) {
            if (errorText != nullptr) *errorText = QString::fromStdString(result.error.message);
            return {};
        }
        rows_ = result.value;
        std::vector<SpecLibraryRow> rows;
        rows.reserve(rows_.size());
        for (const auto& row : rows_) {
            rows.push_back(SpecLibraryRow{
                static_cast<qint64>(row.predicate_spec_id),
                QStringLiteral("#%1  %2")
                    .arg(static_cast<qint64>(row.predicate_spec_id))
                    .arg(QString::fromStdString(row.name))
            });
        }
        return rows;
    }

    QWidget* createNewEditor(QWidget* parent, SpecLibraryCallbacks callbacks) override
    {
        auto* editor = new PredicateSpecEditorWindow(parent, true);
        attachCallbacks(editor, callbacks);
        return editor;
    }

    QWidget* createEditorForRow(int row, bool duplicate, QWidget* parent, SpecLibraryCallbacks callbacks) override
    {
        if (row < 0 || row >= static_cast<int>(rows_.size())) return nullptr;
        auto* editor = new PredicateSpecEditorWindow(parent, true);
        attachCallbacks(editor, callbacks);
        editor->loadSnapshot(rows_[static_cast<std::size_t>(row)], duplicate);
        return editor;
    }

    SpecLibraryOperationResult deleteRow(int row, QWidget* parent) override
    {
        if (row < 0 || row >= static_cast<int>(rows_.size())) {
            return { false, QStringLiteral("Select a predicate to delete."), StatusToast::Severity::Warn };
        }

        const auto& predicate = rows_[static_cast<std::size_t>(row)];
        const auto usage = soasimqt2::db::SimCoreDbAuthoringService::GetPredicateSpecUsage(predicate.predicate_spec_id);
        if (!usage.ok) {
            return { false, QString::fromStdString(usage.error.message), StatusToast::Severity::Error };
        }
        if (usage.value.used()) {
            return {
                false,
                QStringLiteral("Predicate is used by %1 predicate set(s) and cannot be deleted.")
                    .arg(usage.value.predicate_set_count),
                StatusToast::Severity::Warn
            };
        }

        const auto response = QMessageBox::question(
            parent,
            QStringLiteral("Delete Predicate"),
            QStringLiteral("Delete predicate #%1 \"%2\"?")
                .arg(static_cast<qint64>(predicate.predicate_spec_id))
                .arg(QString::fromStdString(predicate.name)),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (response != QMessageBox::Yes) {
            return { false, QString(), StatusToast::Severity::Info };
        }

        const auto result = soasimqt2::db::SimCoreDbAuthoringService::DeletePredicateSpec(predicate.predicate_spec_id);
        if (!result.ok) {
            return { false, QString::fromStdString(result.error.message), StatusToast::Severity::Error };
        }
        return { true, QStringLiteral("Deleted predicate."), StatusToast::Severity::Info };
    }

private:
    std::vector<simcore::db::PredicateSpecSnapshot> rows_;
};

class PredicateSetSpecLibraryAdapter final : public ISpecLibraryAdapter {
public:
    AuthoringLibraryKey key() const override { return AuthoringLibraryKey::PredicateSet; }
    QString title() const override { return QStringLiteral("Predicate Sets"); }
    QString placeholderText() const override { return QStringLiteral("Create a new predicate set, or select one from the library to edit a copy."); }

    std::vector<SpecLibraryRow> refreshRows(QString* errorText) override
    {
        const auto result = soasimqt2::db::SimCoreDbAuthoringService::ListPredicateSets();
        if (!result.ok) {
            if (errorText != nullptr) *errorText = QString::fromStdString(result.error.message);
            return {};
        }
        rows_ = result.value;
        std::vector<SpecLibraryRow> rows;
        rows.reserve(rows_.size());
        for (const auto& row : rows_) {
            rows.push_back(SpecLibraryRow{
                static_cast<qint64>(row.predicate_set_id),
                QStringLiteral("#%1  %2 predicates")
                    .arg(static_cast<qint64>(row.predicate_set_id))
                    .arg(static_cast<int>(row.predicates.size()))
            });
        }
        return rows;
    }

    QWidget* createNewEditor(QWidget* parent, SpecLibraryCallbacks callbacks) override
    {
        auto* editor = new PredicateSetEditorWindow(parent, true);
        attachCallbacks(editor, callbacks);
        return editor;
    }

    QWidget* createEditorForRow(int row, bool duplicate, QWidget* parent, SpecLibraryCallbacks callbacks) override
    {
        if (row < 0 || row >= static_cast<int>(rows_.size())) return nullptr;
        auto* editor = new PredicateSetEditorWindow(parent, true);
        attachCallbacks(editor, callbacks);
        editor->loadSnapshot(rows_[static_cast<std::size_t>(row)], duplicate);
        return editor;
    }

private:
    std::vector<simcore::db::PredicateSetSnapshot> rows_;
};

class BattlePlanSpecLibraryAdapter final : public ISpecLibraryAdapter {
public:
    AuthoringLibraryKey key() const override { return AuthoringLibraryKey::BattlePlan; }
    QString title() const override { return QStringLiteral("Battle Plans"); }
    QString placeholderText() const override { return QStringLiteral("Create a new battle plan, or select one from the library to edit a copy."); }

    std::vector<SpecLibraryRow> refreshRows(QString* errorText) override
    {
        const auto result = soasimqt2::db::SimCoreDbAuthoringService::ListBattlePlans();
        if (!result.ok) {
            if (errorText != nullptr) *errorText = QString::fromStdString(result.error.message);
            return {};
        }
        rows_ = result.value;
        std::vector<SpecLibraryRow> rows;
        rows.reserve(rows_.size());
        for (const auto& row : rows_) {
            rows.push_back(SpecLibraryRow{
                static_cast<qint64>(row.plan_id),
                QStringLiteral("#%1  %2")
                    .arg(static_cast<qint64>(row.plan_id))
                    .arg(QString::fromStdString(row.name))
            });
        }
        return rows;
    }

    QWidget* createNewEditor(QWidget* parent, SpecLibraryCallbacks callbacks) override
    {
        auto* editor = new BattlePlanEditorWindow(parent, true);
        attachCallbacks(editor, callbacks);
        return editor;
    }

    QWidget* createEditorForRow(int row, bool duplicate, QWidget* parent, SpecLibraryCallbacks callbacks) override
    {
        if (row < 0 || row >= static_cast<int>(rows_.size())) return nullptr;
        auto* editor = new BattlePlanEditorWindow(parent, true);
        attachCallbacks(editor, callbacks);
        editor->loadSnapshot(rows_[static_cast<std::size_t>(row)], duplicate);
        return editor;
    }

private:
    std::vector<simcore::db::BattlePlanSnapshot> rows_;
};

class ExplorerSettingsSpecLibraryAdapter final : public ISpecLibraryAdapter {
public:
    AuthoringLibraryKey key() const override { return AuthoringLibraryKey::ExplorerSettings; }
    QString title() const override { return QStringLiteral("Battle Explorer Settings"); }
    QString placeholderText() const override { return QStringLiteral("Create new battle explorer settings, or select one from the library to edit a copy."); }

    std::vector<SpecLibraryRow> refreshRows(QString* errorText) override
    {
        const auto result = soasimqt2::db::SimCoreDbAuthoringService::ListExplorerSettings();
        if (!result.ok) {
            if (errorText != nullptr) *errorText = QString::fromStdString(result.error.message);
            return {};
        }
        rows_ = result.value;
        std::vector<SpecLibraryRow> rows;
        rows.reserve(rows_.size());
        for (const auto& row : rows_) {
            rows.push_back(SpecLibraryRow{
                static_cast<qint64>(row.explorer_settings_id),
                QStringLiteral("#%1  %2")
                    .arg(static_cast<qint64>(row.explorer_settings_id))
                    .arg(QString::fromStdString(row.name))
            });
        }
        return rows;
    }

    QWidget* createNewEditor(QWidget* parent, SpecLibraryCallbacks callbacks) override
    {
        auto* editor = new ExplorerSettingsEditorWindow(parent, true);
        attachCallbacks(editor, callbacks);
        return editor;
    }

    QWidget* createEditorForRow(int row, bool duplicate, QWidget* parent, SpecLibraryCallbacks callbacks) override
    {
        if (row < 0 || row >= static_cast<int>(rows_.size())) return nullptr;
        auto* editor = new ExplorerSettingsEditorWindow(parent, true);
        attachCallbacks(editor, callbacks);
        editor->loadSnapshot(rows_[static_cast<std::size_t>(row)], duplicate);
        return editor;
    }

private:
    std::vector<simcore::db::ExplorerSettingsSnapshot> rows_;
};

} // namespace

QString ISpecLibraryAdapter::savedItemsLabel() const
{
    return QStringLiteral("Saved specs");
}

QString ISpecLibraryAdapter::newButtonText() const
{
    return QStringLiteral("New");
}

QString ISpecLibraryAdapter::editButtonText() const
{
    return editCreatesCopy() ? QStringLiteral("Edit Selected (copy)") : QStringLiteral("Edit Selected");
}

bool ISpecLibraryAdapter::supportsDelete() const
{
    return false;
}

bool ISpecLibraryAdapter::editCreatesCopy() const
{
    return true;
}

SpecLibraryOperationResult ISpecLibraryAdapter::deleteRow(int, QWidget*)
{
    return { false, QString(), StatusToast::Severity::Info };
}

AuthoringLibraryWidget::AuthoringLibraryWidget(QWidget* parent)
    : QWidget(parent)
{
    createAdapters();
    createWidgets();
    if (!adapters_.empty()) {
        selectLibraryIndex(0, true);
    }
}

void AuthoringLibraryWidget::selectLibrary(AuthoringLibraryKey key)
{
    for (int i = 0; i < static_cast<int>(adapters_.size()); ++i) {
        if (adapters_[static_cast<std::size_t>(i)]->key() == key) {
            if (librarySelector_ != nullptr && librarySelector_->currentRow() != i) {
                librarySelector_->setCurrentRow(i);
            } else {
                selectLibraryIndex(i, true);
            }
            return;
        }
    }
}

AuthoringLibraryKey AuthoringLibraryWidget::currentLibrary() const
{
    const auto* adapter = currentAdapter();
    return adapter != nullptr ? adapter->key() : AuthoringLibraryKey::SeedProbe;
}

void AuthoringLibraryWidget::createAdapters()
{
    adapters_.push_back(std::make_unique<TasSpecLibraryAdapter>());
    adapters_.push_back(std::make_unique<SeedProbeSpecLibraryAdapter>());
    adapters_.push_back(std::make_unique<BattleRunSpecLibraryAdapter>());
    adapters_.push_back(std::make_unique<ExplorerSettingsSpecLibraryAdapter>());
    adapters_.push_back(std::make_unique<BattlePlanSpecLibraryAdapter>());
    adapters_.push_back(std::make_unique<PredicateSpecLibraryAdapter>());
    adapters_.push_back(std::make_unique<PredicateSetSpecLibraryAdapter>());
}

void AuthoringLibraryWidget::createWidgets()
{
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto* libraryPanel = createSurfacePanel(this);
    auto* libraryLayout = new QVBoxLayout(libraryPanel);
    libraryLayout->setContentsMargins(12, 12, 12, 12);
    libraryLayout->setSpacing(8);
    auto* libraryLabel = new QLabel(QStringLiteral("Libraries"), libraryPanel);
    libraryLabel->setObjectName("sectionHeading");
    libraryLayout->addWidget(libraryLabel);
    librarySelector_ = new QListWidget(libraryPanel);
    for (const auto& adapter : adapters_) {
        auto* item = new QListWidgetItem(adapter->title(), librarySelector_);
        item->setData(Qt::UserRole, static_cast<int>(adapter->key()));
    }
    libraryLayout->addWidget(librarySelector_, 1);
    root->addWidget(libraryPanel, 0);

    auto* mainPanel = new QWidget(this);
    auto* mainLayout = new QVBoxLayout(mainPanel);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(10);

    auto* toolbar = createSurfacePanel(mainPanel);
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(12, 10, 12, 10);
    toolbarLayout->setSpacing(10);
    newButton_ = new QPushButton(QStringLiteral("New"), toolbar);
    newButton_->setObjectName("jobsPrimaryButton");
    editButton_ = new QPushButton(QStringLiteral("Edit Selected (copy)"), toolbar);
    editButton_->setObjectName("jobsSecondaryButton");
    editButton_->setEnabled(false);
    deleteButton_ = new QPushButton(QStringLiteral("Delete Selected"), toolbar);
    deleteButton_->setObjectName("jobsSecondaryButton");
    deleteButton_->setEnabled(false);
    refreshButton_ = new QPushButton(QStringLiteral("Refresh"), toolbar);
    refreshButton_->setObjectName("jobsSecondaryButton");
    toolbarLayout->addWidget(newButton_);
    toolbarLayout->addWidget(editButton_);
    toolbarLayout->addWidget(deleteButton_);
    toolbarLayout->addWidget(refreshButton_);
    toolbarLayout->addStretch();
    mainLayout->addWidget(toolbar);

    contentSplitter_ = new QSplitter(Qt::Horizontal, mainPanel);
    contentSplitter_->setChildrenCollapsible(false);
    auto* savedPanel = createSurfacePanel(contentSplitter_);
    auto* savedLayout = new QVBoxLayout(savedPanel);
    savedLayout->setContentsMargins(12, 12, 12, 12);
    savedLayout->setSpacing(8);
    savedItemsLabel_ = new QLabel(savedPanel);
    savedItemsLabel_->setObjectName("sectionHeading");
    savedLayout->addWidget(savedItemsLabel_);
    savedItemsList_ = new QListWidget(savedPanel);
    savedLayout->addWidget(savedItemsList_, 1);
    libraryStatusLabel_ = new QLabel(savedPanel);
    libraryStatusLabel_->setObjectName("sectionDescription");
    savedLayout->addWidget(libraryStatusLabel_);

    rightPane_ = createSurfacePanel(contentSplitter_);
    rightPaneLayout_ = new QVBoxLayout(rightPane_);
    rightPaneLayout_->setContentsMargins(12, 12, 12, 12);
    rightPaneLayout_->setSpacing(10);
    placeholderLabel_ = new QLabel(rightPane_);
    placeholderLabel_->setObjectName("sectionDescription");
    placeholderLabel_->setWordWrap(true);
    rightPaneLayout_->addWidget(placeholderLabel_);
    rightPaneLayout_->addStretch(1);

    contentSplitter_->addWidget(savedPanel);
    contentSplitter_->addWidget(rightPane_);
    contentSplitter_->setStretchFactor(0, 1);
    contentSplitter_->setStretchFactor(1, 2);
    mainLayout->addWidget(contentSplitter_, 1);
    root->addWidget(mainPanel, 1);

    connect(librarySelector_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0) {
            selectLibraryIndex(row, true);
        }
    });
    connect(newButton_, &QPushButton::clicked, this, [this]() {
        auto* adapter = currentAdapter();
        if (adapter == nullptr) return;
        clearRightPane();
        installEditorWidget(adapter->createNewEditor(rightPane_, SpecLibraryCallbacks{
            [this](const QString& text, StatusToast::Severity severity) { postStatusMessage(text, severity); },
            [this]() { refreshLibrary(); }
        }));
    });
    connect(editButton_, &QPushButton::clicked, this, &AuthoringLibraryWidget::showEditorForSelectedRow);
    connect(deleteButton_, &QPushButton::clicked, this, &AuthoringLibraryWidget::deleteSelectedRow);
    connect(refreshButton_, &QPushButton::clicked, this, &AuthoringLibraryWidget::refreshLibrary);
    connect(savedItemsList_, &QListWidget::currentRowChanged, this, &AuthoringLibraryWidget::handleSavedRowChanged);
    connect(savedItemsList_, &QListWidget::itemDoubleClicked, this, [this]() { showEditorForSelectedRow(); });
}

void AuthoringLibraryWidget::selectLibraryIndex(int index, bool forceRefresh)
{
    if (index < 0 || index >= static_cast<int>(adapters_.size())) {
        return;
    }

    const bool changed = currentAdapterIndex_ != index;
    currentAdapterIndex_ = index;
    auto* adapter = currentAdapter();
    if (adapter == nullptr) {
        return;
    }

    clearRightPane();
    savedItemsLabel_->setText(adapter->savedItemsLabel());
    newButton_->setText(adapter->newButtonText());
    editButton_->setText(adapter->editButtonText());
    deleteButton_->setVisible(adapter->supportsDelete());
    placeholderLabel_->setText(adapter->placeholderText());
    updateActionState();
    if (changed || forceRefresh) {
        refreshLibrary();
    }
}

void AuthoringLibraryWidget::refreshLibrary()
{
    auto* adapter = currentAdapter();
    if (adapter == nullptr || savedItemsList_ == nullptr) {
        return;
    }

    QString errorText;
    const auto rows = adapter->refreshRows(&errorText);
    if (!errorText.isEmpty()) {
        postStatusMessage(errorText, StatusToast::Severity::Error);
        return;
    }

    savedItemsList_->clear();
    for (const auto& row : rows) {
        auto* item = new QListWidgetItem(row.text, savedItemsList_);
        item->setData(Qt::UserRole, row.id);
    }
    libraryStatusLabel_->setText(QStringLiteral("Saved: %1").arg(static_cast<int>(rows.size())));
    updateActionState();
}

void AuthoringLibraryWidget::clearRightPane()
{
    if (activeEditor_ != nullptr) {
        rightPaneLayout_->removeWidget(activeEditor_);
        activeEditor_->hide();
        activeEditor_->deleteLater();
        activeEditor_ = nullptr;
    }
    if (placeholderLabel_ != nullptr && rightPaneLayout_->indexOf(placeholderLabel_) < 0) {
        rightPaneLayout_->insertWidget(0, placeholderLabel_);
        placeholderLabel_->show();
    }
}

void AuthoringLibraryWidget::installEditorWidget(QWidget* editor)
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

void AuthoringLibraryWidget::showEditorForSelectedRow()
{
    auto* adapter = currentAdapter();
    const int row = selectedSavedRow();
    if (adapter == nullptr || row < 0) {
        return;
    }

    installEditorWidget(adapter->createEditorForRow(row, adapter->editCreatesCopy(), rightPane_, SpecLibraryCallbacks{
        [this](const QString& text, StatusToast::Severity severity) { postStatusMessage(text, severity); },
        [this]() { refreshLibrary(); }
    }));
}

void AuthoringLibraryWidget::deleteSelectedRow()
{
    auto* adapter = currentAdapter();
    if (adapter == nullptr || !adapter->supportsDelete()) {
        return;
    }

    const auto result = adapter->deleteRow(selectedSavedRow(), this);
    if (!result.message.isEmpty()) {
        postStatusMessage(result.message, result.severity);
    }
    if (result.ok) {
        refreshLibrary();
        clearRightPane();
    }
}

void AuthoringLibraryWidget::handleSavedRowChanged()
{
    updateActionState();
    if (selectedSavedRow() < 0) {
        clearRightPane();
        auto* adapter = currentAdapter();
        if (adapter != nullptr && placeholderLabel_ != nullptr) {
            placeholderLabel_->setText(adapter->placeholderText());
        }
        return;
    }
    showEditorForSelectedRow();
}

void AuthoringLibraryWidget::updateActionState()
{
    const auto* adapter = currentAdapter();
    const bool hasSelection = selectedSavedRow() >= 0;
    if (editButton_ != nullptr) {
        editButton_->setEnabled(hasSelection);
    }
    if (deleteButton_ != nullptr) {
        deleteButton_->setVisible(adapter != nullptr && adapter->supportsDelete());
        deleteButton_->setEnabled(adapter != nullptr && adapter->supportsDelete() && hasSelection);
    }
}

void AuthoringLibraryWidget::postStatusMessage(const QString& text, StatusToast::Severity severity)
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

int AuthoringLibraryWidget::selectedSavedRow() const
{
    const int row = savedItemsList_ != nullptr ? savedItemsList_->currentRow() : -1;
    return (row < 0 || row >= (savedItemsList_ != nullptr ? savedItemsList_->count() : 0)) ? -1 : row;
}

ISpecLibraryAdapter* AuthoringLibraryWidget::currentAdapter() const
{
    if (currentAdapterIndex_ < 0 || currentAdapterIndex_ >= static_cast<int>(adapters_.size())) {
        return nullptr;
    }
    return adapters_[static_cast<std::size_t>(currentAdapterIndex_)].get();
}

AuthoringLibraryDialog::AuthoringLibraryDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Authoring Libraries"));
    resize(1280, 720);
    setAttribute(Qt::WA_DeleteOnClose);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    widget_ = new AuthoringLibraryWidget(this);
    connect(widget_, &AuthoringLibraryWidget::statusToastRequested, this, &AuthoringLibraryDialog::statusToastRequested);
    root->addWidget(widget_, 1);
}

void AuthoringLibraryDialog::selectLibrary(AuthoringLibraryKey key)
{
    if (widget_ != nullptr) {
        widget_->selectLibrary(key);
    }
}

AuthoringLibraryKey AuthoringLibraryDialog::currentLibrary() const
{
    return widget_ != nullptr ? widget_->currentLibrary() : AuthoringLibraryKey::SeedProbe;
}
