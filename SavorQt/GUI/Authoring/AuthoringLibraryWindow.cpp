#include "AuthoringLibraryWindow.h"

#include "SeedProbeSpecEditor.h"
#include "BattlePlanEditor.h"
#include "PredicateAuthoringEditors.h"
#include "DB/SavorDbAuthoringService.h"

#include <QtCore/QDateTime>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QVBoxLayout>

#include <ranges>

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
    if constexpr (requires { editor->setOpenGroupCallback(callbacks.openPredicateGroup); }) {
        editor->setOpenGroupCallback(callbacks.openPredicateGroup);
    }
}

class SeedProbeSpecLibraryAdapter final : public ISpecLibraryAdapter {
public:
    AuthoringLibraryKey key() const override { return AuthoringLibraryKey::SeedProbe; }
    QString title() const override { return QStringLiteral("Seed Probe Specs"); }
    QString placeholderText() const override { return QStringLiteral("Create a new seed probe spec, or select one from the library to edit a copy."); }

    std::vector<SpecLibraryRow> refreshRows(QString* errorText) override
    {
        const auto result = savorqt::db::SavorDbAuthoringService::ListSeedProbeSpecs();
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
        auto* editor = new SeedProbeSpecEditor(parent);
        attachCallbacks(editor, callbacks);
        return editor;
    }

    QWidget* createEditorForRow(int row, bool duplicate, QWidget* parent, SpecLibraryCallbacks callbacks) override
    {
        if (row < 0 || row >= static_cast<int>(rows_.size())) return nullptr;
        auto* editor = new SeedProbeSpecEditor(parent);
        attachCallbacks(editor, callbacks);
        editor->loadSnapshot(rows_[static_cast<std::size_t>(row)], duplicate);
        return editor;
    }

private:
    std::vector<savor::db::SeedProbeSpecSnapshot> rows_;
};

class BattlePlanSpecLibraryAdapter final : public ISpecLibraryAdapter {
public:
    AuthoringLibraryKey key() const override { return AuthoringLibraryKey::BattlePlan; }
    QString title() const override { return QStringLiteral("Battle Plans"); }
    QString placeholderText() const override { return QStringLiteral("Create a new battle plan, or select one from the library to edit a copy."); }

    std::vector<SpecLibraryRow> refreshRows(QString* errorText) override
    {
        const auto result = savorqt::db::SavorDbAuthoringService::ListBattlePlans();
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
                    .arg(QString::fromStdString(row.name)),
                QString::fromStdString(row.description)
            });
        }
        return rows;
    }

    QWidget* createNewEditor(QWidget* parent, SpecLibraryCallbacks callbacks) override
    {
        auto* editor = new BattlePlanEditor(parent);
        attachCallbacks(editor, callbacks);
        return editor;
    }

    QWidget* createEditorForRow(int row, bool duplicate, QWidget* parent, SpecLibraryCallbacks callbacks) override
    {
        if (row < 0 || row >= static_cast<int>(rows_.size())) return nullptr;
        auto* editor = new BattlePlanEditor(parent);
        attachCallbacks(editor, callbacks);
        editor->loadSnapshot(rows_[static_cast<std::size_t>(row)], duplicate);
        return editor;
    }

private:
    std::vector<savor::db::BattlePlanSnapshot> rows_;
};


class PredicateLibraryAdapter final : public ISpecLibraryAdapter {
public:
    AuthoringLibraryKey key() const override { return AuthoringLibraryKey::Predicates; }
    QString title() const override { return QStringLiteral("Predicates"); }
    QString placeholderText() const override { return QStringLiteral("Author pure Predicate Definitions and their concrete Execution Bindings."); }
    QString savedItemsLabel() const override { return QStringLiteral("Definitions and Execution Bindings"); }
    bool supportsDelete() const override { return true; }
    bool editCreatesCopy() const override { return false; }
    QStringList newActionLabels() const override
    {
        return {QStringLiteral("New Predicate Definition"),
                QStringLiteral("New Execution Binding")};
    }

    std::vector<SpecLibraryRow> refreshRows(QString* errorText) override
    {
        const auto definitions = savorqt::db::SavorDbAuthoringService::ListPredicateDefinitionRevisions(std::nullopt, {}, 500);
        const auto bindings = savorqt::db::SavorDbAuthoringService::ListPredicateExecutionBindingRevisions(std::nullopt, {}, 1000);
        if (!definitions.ok || !bindings.ok) {
            if (errorText) *errorText = QString::fromStdString((!definitions.ok ? definitions.error : bindings.error).message);
            return {};
        }
        definitionSnapshots_.clear(); bindingSnapshots_.clear(); entries_.clear();
        struct PreparedRow { QString name; int kind = 0; int revision = 0; SpecLibraryRow row; Entry entry; };
        std::vector<PreparedRow> prepared;
        for (const auto& summary : definitions.value.items) {
            const auto detail = savorqt::db::SavorDbAuthoringService::GetPredicateDefinitionRevision(summary.predicate_definition_revision_id);
            if (detail.ok) {
                const auto index=definitionSnapshots_.size();definitionSnapshots_.push_back(detail.value);
                const auto secondary=QStringLiteral("Predicate Definition — %1 — revision %2 — %3\n%4 inputs; %5 expression steps")
                    .arg(QString::fromStdString(summary.description)).arg(summary.revision_number)
                    .arg(QString::fromStdString(summary.revision_state)).arg(summary.witness_count).arg(summary.expression_node_count);
                prepared.push_back({QString::fromStdString(summary.name),0,summary.revision_number,
                    {summary.predicate_definition_revision_id,QStringLiteral("%1\n%2").arg(QString::fromStdString(summary.name),secondary),secondary,true},
                    {false,index,summary.revision_state,true}});
            } else {
                const auto message=QString::fromStdString(detail.error.message);
                const auto secondary=QStringLiteral("Predicate Definition — revision %1 — %2 — unavailable: %3")
                    .arg(summary.revision_number).arg(QString::fromStdString(summary.revision_state),message);
                prepared.push_back({QString::fromStdString(summary.name),0,summary.revision_number,
                    {summary.predicate_definition_revision_id,QStringLiteral("%1\n%2").arg(QString::fromStdString(summary.name),secondary),secondary,false},
                    {false,0,summary.revision_state,false}});
            }
        }
        for (const auto& summary : bindings.value.items) {
            const auto detail = savorqt::db::SavorDbAuthoringService::GetPredicateExecutionBindingRevision(summary.predicate_execution_binding_revision_id);
            if (detail.ok) {
                const auto index=bindingSnapshots_.size();bindingSnapshots_.push_back(detail.value);
                const auto secondary=QStringLiteral("Execution Binding — %1 — revision %2 — %3\n%4 bound inputs")
                    .arg(QString::fromStdString(summary.description)).arg(summary.revision_number)
                    .arg(QString::fromStdString(summary.revision_state)).arg(summary.witness_source_count);
                prepared.push_back({QString::fromStdString(summary.name),1,summary.revision_number,
                    {summary.predicate_execution_binding_revision_id,QStringLiteral("%1\n%2").arg(QString::fromStdString(summary.name),secondary),secondary,true},
                    {true,index,summary.revision_state,true}});
            } else {
                const auto message=QString::fromStdString(detail.error.message);
                const auto secondary=QStringLiteral("Execution Binding — revision %1 — %2 — unavailable: %3")
                    .arg(summary.revision_number).arg(QString::fromStdString(summary.revision_state),message);
                prepared.push_back({QString::fromStdString(summary.name),1,summary.revision_number,
                    {summary.predicate_execution_binding_revision_id,QStringLiteral("%1\n%2").arg(QString::fromStdString(summary.name),secondary),secondary,false},
                    {true,0,summary.revision_state,false}});
            }
        }
        std::ranges::sort(prepared, [](const auto& a, const auto& b) {
            const auto byName=QString::compare(a.name,b.name,Qt::CaseInsensitive);
            if(byName!=0)return byName<0;if(a.kind!=b.kind)return a.kind<b.kind;return a.revision<b.revision;
        });
        std::vector<SpecLibraryRow> rows;
        for(auto&item:prepared){rows.push_back(std::move(item.row));entries_.push_back(std::move(item.entry));}
        return rows;
    }

    QWidget* createNewEditor(QWidget* parent, SpecLibraryCallbacks callbacks) override
    {
        return createNewEditorForAction(0, parent, std::move(callbacks));
    }
    QWidget* createNewEditorForAction(int action, QWidget* parent,
                                      SpecLibraryCallbacks callbacks) override
    {
        auto* editor = new PredicateAuthoringEditor(parent,
            action == 1 ? PredicateAuthoringEditor::ObjectKind::ExecutionBinding
                        : PredicateAuthoringEditor::ObjectKind::Definition);
        attachCallbacks(editor, callbacks);
        return editor;
    }
    QWidget* createEditorForRow(int row, bool duplicate, QWidget* parent, SpecLibraryCallbacks callbacks) override
    {
        if (row < 0 || row >= static_cast<int>(entries_.size()) || !entries_[static_cast<std::size_t>(row)].resolved) return nullptr;
        auto* editor = new PredicateAuthoringEditor(parent); attachCallbacks(editor, callbacks);
        const auto& entry = entries_[static_cast<std::size_t>(row)];
        if (entry.binding) editor->loadExecutionBinding(bindingSnapshots_[entry.index], duplicate);
        else editor->loadDefinition(definitionSnapshots_[entry.index], duplicate);
        return editor;
    }
    SpecLibraryOperationResult deleteRow(int row, QWidget*) override
    {
        if (row < 0 || row >= static_cast<int>(entries_.size()) || !entries_[static_cast<std::size_t>(row)].resolved) return {false,QStringLiteral("This revision could not be loaded and cannot be changed."),StatusToast::Severity::Warn};
        const auto& entry = entries_[static_cast<std::size_t>(row)];
        if (entry.state != "DRAFT") return {false, QStringLiteral("Published Predicate revisions are immutable."), StatusToast::Severity::Warn};
        const auto result = entry.binding
            ? savorqt::db::SavorDbAuthoringService::AbandonPredicateExecutionBindingDraft(bindingSnapshots_[entry.index].binding.execution_binding_revision_id)
            : savorqt::db::SavorDbAuthoringService::AbandonPredicateDefinitionDraft(definitionSnapshots_[entry.index].predicate_definition_revision_id);
        return result.ok
            ? SpecLibraryOperationResult{true, QStringLiteral("Predicate draft abandoned."), StatusToast::Severity::Info}
            : SpecLibraryOperationResult{false, QString::fromStdString(result.error.message), StatusToast::Severity::Error};
    }
private:
    struct Entry { bool binding = false; std::size_t index = 0; std::string state; bool resolved = true; };
    std::vector<Entry> entries_;
    std::vector<savor::db::PredicateDefinitionRevisionV2Snapshot> definitionSnapshots_;
    std::vector<savor::db::PredicateExecutionBindingRevisionSnapshot> bindingSnapshots_;
};

class PredicateGroupLibraryAdapter final : public ISpecLibraryAdapter {
public:
    AuthoringLibraryKey key() const override { return AuthoringLibraryKey::PredicateGroups; }
    QString title() const override { return QStringLiteral("Predicate Groups"); }
    QString placeholderText() const override { return QStringLiteral("Order published Execution Bindings and assign their hooks and evaluation policy."); }
    QString savedItemsLabel() const override { return QStringLiteral("Predicate Group revisions"); }
    bool supportsDelete() const override { return true; }
    bool editCreatesCopy() const override { return false; }
    std::vector<SpecLibraryRow> refreshRows(QString* errorText) override
    {
        const auto result = savorqt::db::SavorDbAuthoringService::ListPredicateGroupRevisions(std::nullopt, {}, std::nullopt, 500);
        if (!result.ok) { if (errorText) *errorText = QString::fromStdString(result.error.message); return {}; }
        snapshots_.clear();entries_.clear();
        auto summaries=result.value.items;
        std::ranges::sort(summaries,[](const auto&a,const auto&b){const auto byName=QString::compare(QString::fromStdString(a.name),QString::fromStdString(b.name),Qt::CaseInsensitive);return byName!=0?byName<0:a.revision_number<b.revision_number;});
        std::vector<SpecLibraryRow> rows;
        for (const auto& summary : summaries) {
            const auto detail = savorqt::db::SavorDbAuthoringService::GetPredicateGroupRevision(summary.predicate_group_revision_id);
            const auto secondary=detail.ok
                ?QStringLiteral("Predicate Group — %1 — revision %2 — %3\n%4 predicates; %5 hooks").arg(QString::fromStdString(summary.description)).arg(summary.revision_number).arg(QString::fromStdString(summary.revision_state)).arg(summary.member_count).arg(summary.hook_count)
                :QStringLiteral("Predicate Group — revision %1 — %2 — unavailable: %3").arg(summary.revision_number).arg(QString::fromStdString(summary.revision_state),QString::fromStdString(detail.error.message));
            if(detail.ok){entries_.push_back(snapshots_.size());snapshots_.push_back(detail.value);}else entries_.push_back(std::nullopt);
            rows.push_back({summary.predicate_group_revision_id,QStringLiteral("%1\n%2").arg(QString::fromStdString(summary.name),secondary),secondary,detail.ok});
        }
        return rows;
    }
    QWidget* createNewEditor(QWidget* parent, SpecLibraryCallbacks callbacks) override
    { auto* editor = new PredicateGroupEditor(parent); attachCallbacks(editor, callbacks); return editor; }
    QWidget* createEditorForRow(int row, bool duplicate, QWidget* parent, SpecLibraryCallbacks callbacks) override
    { if (row < 0 || row >= static_cast<int>(entries_.size()) || !entries_[static_cast<std::size_t>(row)]) return nullptr; auto* editor = new PredicateGroupEditor(parent); attachCallbacks(editor, callbacks); editor->loadSnapshot(snapshots_[*entries_[static_cast<std::size_t>(row)]], duplicate); return editor; }
    SpecLibraryOperationResult deleteRow(int row, QWidget*) override
    {
        if (row < 0 || row >= static_cast<int>(entries_.size()) || !entries_[static_cast<std::size_t>(row)]) return {false,QStringLiteral("This revision could not be loaded and cannot be changed."),StatusToast::Severity::Warn};
        const auto& snapshot=snapshots_[*entries_[static_cast<std::size_t>(row)]];
        if(snapshot.revision_state!="DRAFT")return{false,QStringLiteral("Published Predicate Group revisions are immutable."),StatusToast::Severity::Warn};
        const auto result=savorqt::db::SavorDbAuthoringService::AbandonPredicateGroupDraft(snapshot.group.predicate_group_revision_id);
        return result.ok?SpecLibraryOperationResult{true,QStringLiteral("Predicate Group draft abandoned."),StatusToast::Severity::Info}:SpecLibraryOperationResult{false,QString::fromStdString(result.error.message),StatusToast::Severity::Error};
    }
private:
    std::vector<std::optional<std::size_t>> entries_;
    std::vector<savor::db::PredicateGroupRevisionSnapshot> snapshots_;
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

QStringList ISpecLibraryAdapter::newActionLabels() const
{
    return {newButtonText()};
}

QWidget* ISpecLibraryAdapter::createNewEditorForAction(
    int, QWidget* parent, SpecLibraryCallbacks callbacks)
{
    return createNewEditor(parent, std::move(callbacks));
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
    adapters_.push_back(std::make_unique<SeedProbeSpecLibraryAdapter>());
    adapters_.push_back(std::make_unique<PredicateLibraryAdapter>());
    adapters_.push_back(std::make_unique<PredicateGroupLibraryAdapter>());
    adapters_.push_back(std::make_unique<BattlePlanSpecLibraryAdapter>());
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
        if (adapter->newActionLabels().size() != 1) return;
        clearRightPane();
        installEditorWidget(adapter->createNewEditorForAction(0, rightPane_, SpecLibraryCallbacks{
            [this](const QString& text, StatusToast::Severity severity) { postStatusMessage(text, severity); },
            [this]() { refreshLibrary(); },
            [this](qint64 binding) { openPredicateGroup(binding); }
        }));
    });
    connect(editButton_, &QPushButton::clicked, this, &AuthoringLibraryWidget::showEditorForSelectedRow);
    connect(deleteButton_, &QPushButton::clicked, this, &AuthoringLibraryWidget::deleteSelectedRow);
    connect(refreshButton_, &QPushButton::clicked, this, &AuthoringLibraryWidget::refreshLibrary);
    connect(savedItemsList_, &QListWidget::currentRowChanged, this, &AuthoringLibraryWidget::handleSavedRowChanged);
    connect(savedItemsList_, &QListWidget::itemDoubleClicked, this, [this]() { showEditorForSelectedRow(); });

    refreshPipeline_ = new savorqt::gui::AsyncRefreshPipeline<LibraryRefreshRequest, LibraryRefreshData>(this);
    refreshPipeline_->setAutoRefreshEnabled(false);
    refreshPipeline_->setRequestBuilder([this](savorqt::gui::RefreshReason) -> std::optional<LibraryRefreshRequest> {
        auto* adapter = currentAdapter();
        if (adapter == nullptr) {
            return std::nullopt;
        }
        return LibraryRefreshRequest{ currentAdapterIndex_, adapter };
    });
    refreshPipeline_->setLoadAndPrepare([](LibraryRefreshRequest request) {
        LibraryRefreshData data;
        data.adapterIndex = request.adapterIndex;
        if (request.adapter != nullptr) {
            data.rows = request.adapter->refreshRows(&data.errorText);
        }
        return savorqt::gui::AsyncRefreshResult<LibraryRefreshData>::Ok(std::move(data));
    });
    refreshPipeline_->setApply([this](const LibraryRefreshData& data, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        if (data.adapterIndex != currentAdapterIndex_) {
            return;
        }
        if (!data.errorText.isEmpty()) {
            postStatusMessage(data.errorText, StatusToast::Severity::Error);
            return;
        }

        savedItemsList_->clear();
        for (const auto& row : data.rows) {
            auto* item = new QListWidgetItem(row.text, savedItemsList_);
            item->setData(Qt::UserRole, row.id);
            item->setToolTip(row.details);
            if (!row.enabled) item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
        }
        libraryStatusLabel_->setText(QStringLiteral("Saved: %1").arg(static_cast<int>(data.rows.size())));
        updateActionState();
    });
    refreshPipeline_->setApplyError([this](const QString& error, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        postStatusMessage(error, StatusToast::Severity::Error);
    });
    refreshPipeline_->setActive(true);
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
    installNewActionMenu();
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

    refreshPipeline_->requestRefresh(savorqt::gui::RefreshReason::Manual);
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
    activeEditor_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
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
        [this]() { refreshLibrary(); },
        [this](qint64 binding) { openPredicateGroup(binding); }
    }));
}

void AuthoringLibraryWidget::installNewActionMenu()
{
    if (newButton_ == nullptr) return;
    newButton_->setMenu(nullptr);
    auto* adapter = currentAdapter();
    if (adapter == nullptr) return;
    const auto labels = adapter->newActionLabels();
    if (labels.size() <= 1) {
        newButton_->setText(labels.isEmpty() ? adapter->newButtonText() : labels.front());
        return;
    }
    newButton_->setText(QStringLiteral("New..."));
    auto* menu = new QMenu(newButton_);
    for (int index = 0; index < labels.size(); ++index) {
        auto* action = menu->addAction(labels[index]);
        connect(action, &QAction::triggered, this, [this, index] {
            auto* selectedAdapter = currentAdapter();
            if (selectedAdapter == nullptr) return;
            clearRightPane();
            installEditorWidget(selectedAdapter->createNewEditorForAction(
                index, rightPane_, SpecLibraryCallbacks{
                    [this](const QString& text, StatusToast::Severity severity) {
                        postStatusMessage(text, severity);
                    },
                    [this]() { refreshLibrary(); },
                    [this](qint64 binding) { openPredicateGroup(binding); }
                }));
        });
    }
    newButton_->setMenu(menu);
}

void AuthoringLibraryWidget::openPredicateGroup(qint64 bindingRevisionId)
{
    int groupIndex = -1;
    for (int index = 0; index < static_cast<int>(adapters_.size()); ++index) {
        if (adapters_[static_cast<std::size_t>(index)]->key()
            == AuthoringLibraryKey::PredicateGroups) {
            groupIndex = index;
            break;
        }
    }
    if (groupIndex < 0) return;
    librarySelector_->setCurrentRow(groupIndex);
    auto* editor = new PredicateGroupEditor(rightPane_);
    attachCallbacks(editor, SpecLibraryCallbacks{
        [this](const QString& text, StatusToast::Severity severity) {
            postStatusMessage(text, severity);
        },
        [this]() { refreshLibrary(); },
        [this](qint64 binding) { openPredicateGroup(binding); }
    });
    editor->preselectBinding(bindingRevisionId);
    installEditorWidget(editor);
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
    const auto* selected = savedItemsList_ ? savedItemsList_->currentItem() : nullptr;
    const bool hasSelection = selectedSavedRow() >= 0 && selected &&
                              (selected->flags() & Qt::ItemIsEnabled);
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

AuthoringLibraryWindow::AuthoringLibraryWindow(QWidget* parent)
    : PersistentToolWindow(parent)
{
    setWindowTitle(QStringLiteral("Authoring Libraries"));
    resize(1280, 720);
    setAttribute(Qt::WA_DeleteOnClose);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    widget_ = new AuthoringLibraryWidget(this);
    connect(widget_, &AuthoringLibraryWidget::statusToastRequested, this, &AuthoringLibraryWindow::statusToastRequested);
    root->addWidget(widget_, 1);
}

void AuthoringLibraryWindow::selectLibrary(AuthoringLibraryKey key)
{
    if (widget_ != nullptr) {
        widget_->selectLibrary(key);
    }
}

AuthoringLibraryKey AuthoringLibraryWindow::currentLibrary() const
{
    return widget_ != nullptr ? widget_->currentLibrary() : AuthoringLibraryKey::SeedProbe;
}
