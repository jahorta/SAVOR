#include "WorkflowGraphEditorWindow.h"

#include "DB/SimCoreDbAuthoringService.h"
#include "DB/SimCoreDbWorkflowService.h"

#include <QtCore/QDateTime>
#include <QtGui/QCloseEvent>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <sstream>
#include <unordered_map>

namespace {

QFrame* createPanel(const QString& title, QWidget* parent, QVBoxLayout** bodyLayout = nullptr)
{
    auto* frame = new QFrame(parent);
    frame->setObjectName("jobsSurfacePanel");
    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);
    auto* titleLabel = new QLabel(title, frame);
    titleLabel->setObjectName("panelTitle");
    layout->addWidget(titleLabel);
    if (bodyLayout) {
        *bodyLayout = layout;
    }
    return frame;
}

QString portListText(const std::vector<simcore::db::execution::workflow::WorkflowPortDefinition>& ports)
{
    QStringList lines;
    for (const auto& port : ports) {
        lines << QStringLiteral("%1 (%2)")
            .arg(QString::fromStdString(port.display_name))
            .arg(QString::fromStdString(port.data_kind));
    }
    return lines.join(QStringLiteral("\n"));
}

std::string workflowGraphHash(
    const QString& name,
    const QString& description,
    const std::vector<simcore::db::execution::workflow::WorkflowCompositionNode>& nodes,
    const std::vector<simcore::db::execution::workflow::WorkflowUnitOutputBinding>& bindings)
{
    std::string content = "name:" + name.toStdString() + "\ndescription:" + description.toStdString() + "\n";
    for (const auto& node : nodes) {
        content += "node:" + node.node_key + ":" + node.unit_kind + "\n";
    }
    for (const auto& binding : bindings) {
        content += "edge:" + binding.from_node_key + "." + binding.output_key
            + ">" + binding.to_node_key + "." + binding.input_key + "\n";
    }

    std::uint64_t hash = 1469598103934665603ull;
    for (const auto ch : content) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 1099511628211ull;
    }
    std::ostringstream out;
    out << "fnv1a64-" << std::hex << hash;
    return out.str();
}

} // namespace

WorkflowGraphEditorWindow::WorkflowGraphEditorWindow(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlag(Qt::Window, true);
    setWindowTitle(QStringLiteral("Workflow Graph Editor"));
    resize(1120, 760);

    createWidgets();
    loadUnits();
    rebuildBindings();
    refreshPreview();
}

void WorkflowGraphEditorWindow::setStatusCallback(std::function<void(const QString&, StatusToast::Severity)> callback)
{
    statusCallback_ = std::move(callback);
}

void WorkflowGraphEditorWindow::setSavedCallback(std::function<void()> callback)
{
    savedCallback_ = std::move(callback);
}

void WorkflowGraphEditorWindow::loadSnapshot(const simcore::db::WorkflowGraphSnapshot& snapshot, bool duplicate)
{
    workflowGraphId_ = duplicate ? std::nullopt : std::optional<std::int64_t>{ snapshot.workflow_graph_id };
    parentRevisionId_ = duplicate ? std::nullopt : std::optional<std::int64_t>{ snapshot.workflow_graph_revision_id };
    setWindowTitle(duplicate
        ? QStringLiteral("Workflow Graph Editor - Duplicate")
        : QStringLiteral("Workflow Graph Editor - Edit"));
    nameEdit_->setText(QString::fromStdString(snapshot.name) + (duplicate ? QStringLiteral(" copy") : QString()));
    descriptionEdit_->setPlainText(QString::fromStdString(snapshot.description));

    nodes_.clear();
    authoredRefsByNode_.clear();
    outputBindings_.clear();
    nextNodeOrdinal_ = 1;

    for (const auto& node_snapshot : snapshot.nodes) {
        WorkflowCompositionNode node{};
        node.node_key = node_snapshot.node_key;
        node.unit_kind = node_snapshot.unit_kind;
        nodes_.push_back(std::move(node));
        authoredRefsByNode_[node_snapshot.node_key] = { node_snapshot.authored_ref_kind, node_snapshot.authored_ref_id };

        const auto underscore = node_snapshot.node_key.rfind('_');
        if (underscore != std::string::npos && underscore + 1 < node_snapshot.node_key.size()) {
            try {
                nextNodeOrdinal_ = std::max(nextNodeOrdinal_, std::stoi(node_snapshot.node_key.substr(underscore + 1)) + 1);
            } catch (...) {
            }
        }
    }

    for (const auto& edge : snapshot.edges) {
        outputBindings_.push_back(WorkflowUnitOutputBinding{
            .from_node_key = edge.from_node_key,
            .output_key = edge.output_key,
            .to_node_key = edge.to_node_key,
            .input_key = edge.input_key,
        });
    }

    refreshCompositionList();
    if (compositionList_ != nullptr && !nodes_.empty()) {
        compositionList_->setCurrentRow(0);
    } else {
        refreshNodeSettings();
    }
    refreshPreview();
    dirty_ = false;
}

void WorkflowGraphEditorWindow::closeEvent(QCloseEvent* event)
{
    if (confirmDiscardIfDirty()) {
        event->accept();
    } else {
        event->ignore();
    }
}

void WorkflowGraphEditorWindow::createWidgets()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(12, 12, 12, 12);
    rootLayout->setSpacing(10);

    auto* metadataPanel = new QFrame(this);
    metadataPanel->setObjectName("jobsToolbarPanel");
    auto* metadataLayout = new QFormLayout(metadataPanel);
    metadataLayout->setContentsMargins(12, 10, 12, 10);
    nameEdit_ = new QLineEdit(metadataPanel);
    descriptionEdit_ = new QPlainTextEdit(metadataPanel);
    descriptionEdit_->setMaximumHeight(70);
    const auto stamp = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    nameEdit_->setText(QStringLiteral("Qt2 workflow graph %1").arg(stamp));
    metadataLayout->addRow(QStringLiteral("Name"), nameEdit_);
    metadataLayout->addRow(QStringLiteral("Description"), descriptionEdit_);
    rootLayout->addWidget(metadataPanel);

    auto* toolbar = new QFrame(this);
    toolbar->setObjectName("jobsToolbarPanel");
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(12, 10, 12, 10);
    toolbarLayout->setSpacing(10);

    addUnitButton_ = new QPushButton(QStringLiteral("Add Unit"), toolbar);
    removeNodeButton_ = new QPushButton(QStringLiteral("Remove"), toolbar);
    clearButton_ = new QPushButton(QStringLiteral("Clear"), toolbar);
    saveGraphButton_ = new QPushButton(QStringLiteral("Save Graph"), toolbar);
    addUnitButton_->setObjectName("jobsPrimaryButton");
    removeNodeButton_->setObjectName("jobsSecondaryButton");
    clearButton_->setObjectName("jobsSecondaryButton");
    saveGraphButton_->setObjectName("jobsPrimaryButton");

    toolbarLayout->addWidget(addUnitButton_);
    toolbarLayout->addWidget(removeNodeButton_);
    toolbarLayout->addWidget(clearButton_);
    toolbarLayout->addWidget(saveGraphButton_);
    toolbarLayout->addStretch();
    rootLayout->addWidget(toolbar);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);

    QVBoxLayout* unitLayout = nullptr;
    auto* unitPanel = createPanel(QStringLiteral("Workflow Units"), splitter, &unitLayout);
    unitList_ = new QListWidget(unitPanel);
    unitLayout->addWidget(unitList_, 1);

    QVBoxLayout* compositionLayout = nullptr;
    auto* compositionPanel = createPanel(QStringLiteral("Composition"), splitter, &compositionLayout);
    compositionList_ = new QListWidget(compositionPanel);
    compositionLayout->addWidget(compositionList_, 1);
    nodeSettingsLabel_ = new QLabel(QStringLiteral("Select a node to assign authored settings."), compositionPanel);
    nodeSettingsLabel_->setObjectName("sectionDescription");
    nodeSettingsLabel_->setWordWrap(true);
    compositionLayout->addWidget(nodeSettingsLabel_);
    authoredRefList_ = new QListWidget(compositionPanel);
    authoredRefList_->setMaximumHeight(130);
    compositionLayout->addWidget(authoredRefList_);
    auto* refButtonRow = new QHBoxLayout();
    refreshRefsButton_ = new QPushButton(QStringLiteral("Refresh"), compositionPanel);
    assignRefButton_ = new QPushButton(QStringLiteral("Assign"), compositionPanel);
    clearRefButton_ = new QPushButton(QStringLiteral("Clear"), compositionPanel);
    refreshRefsButton_->setObjectName("jobsSecondaryButton");
    assignRefButton_->setObjectName("jobsSecondaryButton");
    clearRefButton_->setObjectName("jobsSecondaryButton");
    refButtonRow->addWidget(refreshRefsButton_);
    refButtonRow->addWidget(assignRefButton_);
    refButtonRow->addWidget(clearRefButton_);
    compositionLayout->addLayout(refButtonRow);

    QVBoxLayout* previewLayout = nullptr;
    auto* previewPanel = createPanel(QStringLiteral("Preview"), splitter, &previewLayout);
    previewText_ = new QPlainTextEdit(previewPanel);
    previewText_->setReadOnly(true);
    previewText_->setMinimumWidth(360);
    previewLayout->addWidget(previewText_, 1);

    splitter->addWidget(unitPanel);
    splitter->addWidget(compositionPanel);
    splitter->addWidget(previewPanel);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 2);
    rootLayout->addWidget(splitter, 1);

    connect(addUnitButton_, &QPushButton::clicked, this, &WorkflowGraphEditorWindow::addSelectedUnit);
    connect(removeNodeButton_, &QPushButton::clicked, this, &WorkflowGraphEditorWindow::removeSelectedNode);
    connect(clearButton_, &QPushButton::clicked, this, &WorkflowGraphEditorWindow::clearComposition);
    connect(saveGraphButton_, &QPushButton::clicked, this, &WorkflowGraphEditorWindow::saveGraph);
    connect(compositionList_, &QListWidget::currentRowChanged, this, &WorkflowGraphEditorWindow::refreshNodeSettings);
    connect(refreshRefsButton_, &QPushButton::clicked, this, &WorkflowGraphEditorWindow::loadAuthoredRefOptionsForSelectedNode);
    connect(assignRefButton_, &QPushButton::clicked, this, &WorkflowGraphEditorWindow::applySelectedAuthoredRef);
    connect(clearRefButton_, &QPushButton::clicked, this, &WorkflowGraphEditorWindow::clearSelectedAuthoredRef);
    connect(authoredRefList_, &QListWidget::itemDoubleClicked, this, [this]() { applySelectedAuthoredRef(); });
    connect(nameEdit_, &QLineEdit::textChanged, this, [this]() { markDirty(); refreshPreview(); });
    connect(descriptionEdit_, &QPlainTextEdit::textChanged, this, [this]() { markDirty(); refreshPreview(); });
}

void WorkflowGraphEditorWindow::loadUnits()
{
    const auto result = soasimqt2::db::SimCoreDbWorkflowService::ListWorkflowUnits();
    if (!result.ok) {
        postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
        return;
    }
    units_ = result.value;
    refreshUnitList();
}

void WorkflowGraphEditorWindow::addSelectedUnit()
{
    const auto* item = unitList_ != nullptr ? unitList_->currentItem() : nullptr;
    if (item == nullptr) {
        return;
    }

    const auto unitKind = item->data(Qt::UserRole).toString().toStdString();
    if (findUnit(unitKind) == nullptr) {
        return;
    }

    WorkflowCompositionNode node{};
    node.unit_kind = unitKind;
    node.node_key = unitKind + "_" + std::to_string(nextNodeOrdinal_++);
    nodes_.push_back(std::move(node));

    rebuildBindings();
    refreshCompositionList();
    if (compositionList_ != nullptr) {
        compositionList_->setCurrentRow(static_cast<int>(nodes_.size()) - 1);
    }
    refreshPreview();
    markDirty();
}

void WorkflowGraphEditorWindow::removeSelectedNode()
{
    const int row = compositionList_ != nullptr ? compositionList_->currentRow() : -1;
    if (row < 0 || row >= static_cast<int>(nodes_.size())) {
        return;
    }
    authoredRefsByNode_.erase(nodes_[static_cast<std::size_t>(row)].node_key);
    nodes_.erase(nodes_.begin() + row);
    rebuildBindings();
    refreshCompositionList();
    refreshNodeSettings();
    refreshPreview();
    markDirty();
}

void WorkflowGraphEditorWindow::clearComposition()
{
    nodes_.clear();
    outputBindings_.clear();
    authoredRefsByNode_.clear();
    rebuildBindings();
    refreshCompositionList();
    refreshNodeSettings();
    refreshPreview();
    markDirty();
}

void WorkflowGraphEditorWindow::saveGraph()
{
    if (nameEdit_->text().trimmed().isEmpty()) {
        postStatusMessage(QStringLiteral("Workflow graph name is required."), StatusToast::Severity::Warn);
        return;
    }
    if (nodes_.empty()) {
        postStatusMessage(QStringLiteral("Add at least one workflow unit before saving."), StatusToast::Severity::Warn);
        return;
    }

    rebuildBindings();

    soasimqt2::db::WorkflowGraphDraft draft{};
    draft.workflow_graph_id = workflowGraphId_;
    draft.parent_revision_id = parentRevisionId_;
    draft.name = nameEdit_->text().trimmed().toStdString();
    draft.description = descriptionEdit_->toPlainText().trimmed().toStdString();
    draft.graph_version = workflowGraphId_.has_value() ? 0 : 1;
    draft.graph_hash = workflowGraphHash(nameEdit_->text().trimmed(), descriptionEdit_->toPlainText().trimmed(), nodes_, outputBindings_);

    for (const auto& node : nodes_) {
        const auto* unit = findUnit(node.unit_kind);
        if (unit == nullptr) {
            postStatusMessage(QStringLiteral("Cannot save graph with unknown workflow unit."), StatusToast::Severity::Error);
            return;
        }
        if (const auto required_ref_kind = requiredAuthoredRefKindForUnit(node.unit_kind); required_ref_kind.has_value()) {
            const auto refs = authoredRefsByNode_.find(node.node_key);
            if (refs == authoredRefsByNode_.end()
                || refs->second.first.value_or("") != *required_ref_kind
                || !refs->second.second.has_value()
                || *refs->second.second <= 0) {
                postStatusMessage(
                    QStringLiteral("Node %1 requires authored settings (%2).")
                        .arg(QString::fromStdString(node.node_key))
                        .arg(QString::fromStdString(*required_ref_kind)),
                    StatusToast::Severity::Warn);
                return;
            }
        }

        simcore::db::SaveWorkflowGraphNodeCommand node_command{};
        node_command.node_key = node.node_key;
        node_command.unit_kind = node.unit_kind;
        node_command.display_name = unit->display_name;
        if (const auto refs = authoredRefsByNode_.find(node.node_key); refs != authoredRefsByNode_.end()) {
            node_command.authored_ref_kind = refs->second.first;
            node_command.authored_ref_id = refs->second.second;
        }
        for (const auto& input : unit->required_inputs) {
            node_command.inputs.push_back(simcore::db::SaveWorkflowGraphNodeInputCommand{
                .input_key = input.key,
                .data_kind = input.data_kind,
                .display_name = input.display_name,
                .required = input.required,
            });
        }
        for (const auto& output : unit->possible_outputs) {
            node_command.possible_outputs.push_back(simcore::db::SaveWorkflowGraphNodeOutputCommand{
                .output_key = output.key,
                .data_kind = output.data_kind,
                .display_name = output.display_name,
            });
        }
        draft.nodes.push_back(std::move(node_command));
    }

    for (const auto& binding : outputBindings_) {
        draft.edges.push_back(simcore::db::SaveWorkflowGraphEdgeCommand{
            .from_node_key = binding.from_node_key,
            .output_key = binding.output_key,
            .to_node_key = binding.to_node_key,
            .input_key = binding.input_key,
        });
    }

    saveGraphButton_->setEnabled(false);
    const auto result = soasimqt2::db::SimCoreDbAuthoringService::SaveWorkflowGraph(draft);
    saveGraphButton_->setEnabled(true);
    if (!result.ok) {
        postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
        return;
    }

    workflowGraphId_ = result.value.workflow_graph_id;
    parentRevisionId_ = result.value.workflow_graph_revision_id;
    dirty_ = false;
    if (savedCallback_) {
        savedCallback_();
    }
    postStatusMessage(
        QStringLiteral("Saved workflow graph %1 revision %2")
            .arg(static_cast<qint64>(result.value.workflow_graph_id))
            .arg(static_cast<qint64>(result.value.workflow_graph_revision_id)),
        StatusToast::Severity::Info);
}

void WorkflowGraphEditorWindow::rebuildBindings()
{
    outputBindings_.clear();

    std::unordered_map<std::string, std::pair<std::string, std::string>> latestOutputByKind;
    for (const auto& node : nodes_) {
        const auto* unit = findUnit(node.unit_kind);
        if (unit == nullptr) {
            continue;
        }

        for (const auto& input : unit->required_inputs) {
            const auto outputIt = latestOutputByKind.find(input.data_kind);
            if (outputIt != latestOutputByKind.end()) {
                outputBindings_.push_back(WorkflowUnitOutputBinding{
                    .from_node_key = outputIt->second.first,
                    .output_key = outputIt->second.second,
                    .to_node_key = node.node_key,
                    .input_key = input.key,
                });
            }
        }

        for (const auto& output : unit->possible_outputs) {
            latestOutputByKind[output.data_kind] = { node.node_key, output.key };
        }
    }
}

void WorkflowGraphEditorWindow::refreshUnitList()
{
    if (unitList_ == nullptr) {
        return;
    }
    unitList_->clear();
    for (const auto& unit : units_) {
        auto* item = new QListWidgetItem(describeUnit(unit), unitList_);
        item->setData(Qt::UserRole, QString::fromStdString(unit.unit_kind));
    }
    if (unitList_->count() > 0) {
        unitList_->setCurrentRow(0);
    }
}

void WorkflowGraphEditorWindow::refreshCompositionList()
{
    if (compositionList_ == nullptr) {
        return;
    }
    compositionList_->clear();
    for (const auto& node : nodes_) {
        compositionList_->addItem(describeNode(node));
    }
}

void WorkflowGraphEditorWindow::refreshNodeSettings()
{
    if (nodeSettingsLabel_ == nullptr || authoredRefList_ == nullptr) {
        return;
    }
    const int row = compositionList_ != nullptr ? compositionList_->currentRow() : -1;
    if (row < 0 || row >= static_cast<int>(nodes_.size())) {
        nodeSettingsLabel_->setText(QStringLiteral("Select a node to assign authored settings."));
        authoredRefOptions_.clear();
        authoredRefList_->clear();
        return;
    }

    const auto& node = nodes_[static_cast<std::size_t>(row)];
    const auto requiredKind = requiredAuthoredRefKindForUnit(node.unit_kind);
    if (!requiredKind.has_value()) {
        nodeSettingsLabel_->setText(QStringLiteral("%1 does not require authored node settings.")
            .arg(QString::fromStdString(node.node_key)));
        authoredRefOptions_.clear();
        authoredRefList_->clear();
        return;
    }

    QString current = QStringLiteral("none");
    if (const auto refs = authoredRefsByNode_.find(node.node_key); refs != authoredRefsByNode_.end()
        && refs->second.first.has_value()
        && refs->second.second.has_value()) {
        current = QStringLiteral("%1 #%2")
            .arg(QString::fromStdString(*refs->second.first))
            .arg(static_cast<qint64>(*refs->second.second));
    }
    nodeSettingsLabel_->setText(QStringLiteral("%1 requires %2. Current: %3")
        .arg(QString::fromStdString(node.node_key))
        .arg(QString::fromStdString(*requiredKind))
        .arg(current));
    loadAuthoredRefOptionsForSelectedNode();
}

void WorkflowGraphEditorWindow::loadAuthoredRefOptionsForSelectedNode()
{
    if (authoredRefList_ == nullptr) {
        return;
    }
    authoredRefOptions_.clear();
    authoredRefList_->clear();

    const int row = compositionList_ != nullptr ? compositionList_->currentRow() : -1;
    if (row < 0 || row >= static_cast<int>(nodes_.size())) {
        return;
    }
    const auto& node = nodes_[static_cast<std::size_t>(row)];
    const auto requiredKind = requiredAuthoredRefKindForUnit(node.unit_kind);
    if (!requiredKind.has_value()) {
        return;
    }

    if (*requiredKind == "tas_spec") {
        const auto result = soasimqt2::db::SimCoreDbAuthoringService::ListTasSpecs();
        if (!result.ok) {
            postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
            return;
        }
        for (const auto& spec : result.value) {
            authoredRefOptions_.push_back(AuthoredRefOption{
                .label = QStringLiteral("#%1 %2 rtc %3-%4")
                    .arg(static_cast<qint64>(spec.tas_spec_id))
                    .arg(QString::fromStdString(spec.base_name))
                    .arg(static_cast<qint64>(spec.rtc_low))
                    .arg(static_cast<qint64>(spec.rtc_high)),
                .ref_kind = "tas_spec",
                .ref_id = spec.tas_spec_id,
            });
        }
    } else if (*requiredKind == "seed_probe_spec") {
        const auto result = soasimqt2::db::SimCoreDbAuthoringService::ListSeedProbeSpecs();
        if (!result.ok) {
            postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
            return;
        }
        for (const auto& spec : result.value) {
            authoredRefOptions_.push_back(AuthoredRefOption{
                .label = QStringLiteral("#%1 %2 (%3/axis)")
                    .arg(static_cast<qint64>(spec.seed_probe_spec_id))
                    .arg(QString::fromStdString(spec.name))
                    .arg(spec.samples_per_axis),
                .ref_kind = "seed_probe_spec",
                .ref_id = spec.seed_probe_spec_id,
            });
        }
    } else if (*requiredKind == "authoring.template") {
        const auto result = soasimqt2::db::SimCoreDbAuthoringService::ListTemplates();
        if (!result.ok) {
            postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
            return;
        }
        for (const auto& template_row : result.value) {
            authoredRefOptions_.push_back(AuthoredRefOption{
                .label = QStringLiteral("#%1 %2 battle=%3 explorer=%4")
                    .arg(static_cast<qint64>(template_row.template_id))
                    .arg(QString::fromStdString(template_row.name))
                    .arg(template_row.battle_run_spec_id.has_value() ? QString::number(*template_row.battle_run_spec_id) : QStringLiteral("-"))
                    .arg(template_row.explorer_settings_id.has_value() ? QString::number(*template_row.explorer_settings_id) : QStringLiteral("-")),
                .ref_kind = "authoring.template",
                .ref_id = template_row.template_id,
            });
        }
    }

    for (const auto& option : authoredRefOptions_) {
        auto* item = new QListWidgetItem(option.label, authoredRefList_);
        item->setData(Qt::UserRole, static_cast<qint64>(option.ref_id));
    }
    if (authoredRefList_->count() > 0) {
        authoredRefList_->setCurrentRow(0);
    }
}

void WorkflowGraphEditorWindow::applySelectedAuthoredRef()
{
    const int nodeRow = compositionList_ != nullptr ? compositionList_->currentRow() : -1;
    const int refRow = authoredRefList_ != nullptr ? authoredRefList_->currentRow() : -1;
    if (nodeRow < 0 || nodeRow >= static_cast<int>(nodes_.size())
        || refRow < 0 || refRow >= static_cast<int>(authoredRefOptions_.size())) {
        postStatusMessage(QStringLiteral("Select a node and authored setting first."), StatusToast::Severity::Warn);
        return;
    }

    const auto& node = nodes_[static_cast<std::size_t>(nodeRow)];
    const auto& option = authoredRefOptions_[static_cast<std::size_t>(refRow)];
    authoredRefsByNode_[node.node_key] = { option.ref_kind, option.ref_id };
    refreshCompositionList();
    compositionList_->setCurrentRow(nodeRow);
    refreshPreview();
    markDirty();
}

void WorkflowGraphEditorWindow::clearSelectedAuthoredRef()
{
    const int nodeRow = compositionList_ != nullptr ? compositionList_->currentRow() : -1;
    if (nodeRow < 0 || nodeRow >= static_cast<int>(nodes_.size())) {
        return;
    }
    authoredRefsByNode_.erase(nodes_[static_cast<std::size_t>(nodeRow)].node_key);
    refreshCompositionList();
    compositionList_->setCurrentRow(nodeRow);
    refreshPreview();
    markDirty();
}

void WorkflowGraphEditorWindow::refreshPreview()
{
    if (previewText_ == nullptr) {
        return;
    }

    simcore::db::execution::workflow::WorkflowCompositionSpec spec{};
    spec.nodes = nodes_;
    spec.output_bindings = outputBindings_;

    const auto result = soasimqt2::db::SimCoreDbWorkflowService::PreviewComposition(spec);
    if (!result.ok) {
        previewText_->setPlainText(QString::fromStdString(result.error.message));
        return;
    }

    QStringList lines;
    lines << QStringLiteral("Name: %1").arg(nameEdit_->text().trimmed());
    lines << QStringLiteral("Status: %1").arg(result.value.valid ? QStringLiteral("ready") : QStringLiteral("needs inputs"));
    lines << QStringLiteral("Hash: %1").arg(QString::fromStdString(
        workflowGraphHash(nameEdit_->text().trimmed(), descriptionEdit_->toPlainText().trimmed(), nodes_, outputBindings_)));
    lines << QString();

    if (result.value.nodes.empty()) {
        lines << QStringLiteral("No workflow units selected.");
    }

    for (const auto& node : result.value.nodes) {
        const auto* unit = findUnit(node.unit_kind);
        lines << QStringLiteral("[%1] %2")
            .arg(QString::fromStdString(node.node_key))
            .arg(unit != nullptr ? QString::fromStdString(unit->display_name) : QString::fromStdString(node.unit_kind));

        if (!node.resolved_inputs.empty()) {
            lines << QStringLiteral("  Inputs");
            for (const auto& input : node.resolved_inputs) {
                lines << QStringLiteral("    %1").arg(QString::fromStdString(input));
            }
        }

        if (!node.possible_outputs.empty()) {
            lines << QStringLiteral("  Possible outputs");
            for (const auto& output : node.possible_outputs) {
                lines << QStringLiteral("    %1").arg(QString::fromStdString(output));
            }
        }
        lines << QString();
    }

    if (!result.value.issues.empty()) {
        lines << QStringLiteral("Issues");
        for (const auto& issue : result.value.issues) {
            lines << QStringLiteral("  %1.%2: %3")
                .arg(QString::fromStdString(issue.node_key))
                .arg(QString::fromStdString(issue.input_key))
                .arg(QString::fromStdString(issue.message));
        }
    }

    previewText_->setPlainText(lines.join(QStringLiteral("\n")));
}

void WorkflowGraphEditorWindow::markDirty()
{
    dirty_ = true;
}

bool WorkflowGraphEditorWindow::confirmDiscardIfDirty()
{
    if (!dirty_) {
        return true;
    }
    const auto result = QMessageBox::warning(
        this,
        QStringLiteral("Discard workflow graph changes?"),
        QStringLiteral("This workflow graph has unsaved changes."),
        QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Cancel);
    return result == QMessageBox::Discard;
}

void WorkflowGraphEditorWindow::postStatusMessage(const QString& text, StatusToast::Severity severity)
{
    if (text.isEmpty()) {
        return;
    }
    if (statusCallback_) {
        statusCallback_(text, severity);
    }
}

const WorkflowGraphEditorWindow::WorkflowUnitDefinition* WorkflowGraphEditorWindow::findUnit(const std::string& unit_kind) const
{
    const auto it = std::find_if(units_.begin(), units_.end(), [&](const auto& unit) {
        return unit.unit_kind == unit_kind;
    });
    return it == units_.end() ? nullptr : &*it;
}

QString WorkflowGraphEditorWindow::describeUnit(const WorkflowUnitDefinition& unit) const
{
    QString text = QString::fromStdString(unit.display_name);
    if (!unit.required_inputs.empty()) {
        text += QStringLiteral("\nInputs:\n%1").arg(portListText(unit.required_inputs));
    }
    if (!unit.possible_outputs.empty()) {
        text += QStringLiteral("\nPossible outputs:\n%1").arg(portListText(unit.possible_outputs));
    }
    return text;
}

QString WorkflowGraphEditorWindow::describeNode(const WorkflowCompositionNode& node) const
{
    const auto* unit = findUnit(node.unit_kind);
    QString text = QStringLiteral("%1  %2")
        .arg(QString::fromStdString(node.node_key))
        .arg(unit != nullptr ? QString::fromStdString(unit->display_name) : QString::fromStdString(node.unit_kind));
    if (const auto refs = authoredRefsByNode_.find(node.node_key); refs != authoredRefsByNode_.end()
        && refs->second.first.has_value()
        && refs->second.second.has_value()) {
        text += QStringLiteral("\nSettings: %1 #%2")
            .arg(QString::fromStdString(*refs->second.first))
            .arg(static_cast<qint64>(*refs->second.second));
    }
    return text;
}

std::optional<std::string> WorkflowGraphEditorWindow::requiredAuthoredRefKindForUnit(
    const std::string& unit_kind) const
{
    if (unit_kind == "tas_movie") {
        return std::string("tas_spec");
    }
    if (unit_kind == "seed_probe_chain") {
        return std::string("seed_probe_spec");
    }
    if (unit_kind == "battle_chain") {
        return std::string("authoring.template");
    }
    return std::nullopt;
}
