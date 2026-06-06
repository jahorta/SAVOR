#include "JobBuilderPage.h"

#include "DB/SimCoreDbAuthoringService.h"
#include "DB/SimCoreDbWorkflowService.h"

#include <QtCore/QDateTime>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cstdint>
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
    const std::vector<simcore::db::execution::workflow::WorkflowCompositionNode>& nodes,
    const std::vector<simcore::db::execution::workflow::WorkflowUnitOutputBinding>& bindings)
{
    std::string content;
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

JobBuilderPage::JobBuilderPage(QWidget* parent)
    : QWidget(parent)
{
    createWidgets();
    loadUnits();
    rebuildBindings();
    refreshPreview();
}

void JobBuilderPage::createWidgets()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(10);

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

    connect(addUnitButton_, &QPushButton::clicked, this, &JobBuilderPage::addSelectedUnit);
    connect(removeNodeButton_, &QPushButton::clicked, this, &JobBuilderPage::removeSelectedNode);
    connect(clearButton_, &QPushButton::clicked, this, &JobBuilderPage::clearComposition);
    connect(saveGraphButton_, &QPushButton::clicked, this, &JobBuilderPage::saveGraph);
}

void JobBuilderPage::loadUnits()
{
    const auto result = soasimqt2::db::SimCoreDbWorkflowService::ListWorkflowUnits();
    if (!result.ok) {
        postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
        return;
    }
    units_ = result.value;
    refreshUnitList();
}

void JobBuilderPage::addSelectedUnit()
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
    refreshPreview();
}

void JobBuilderPage::removeSelectedNode()
{
    const int row = compositionList_ != nullptr ? compositionList_->currentRow() : -1;
    if (row < 0 || row >= static_cast<int>(nodes_.size())) {
        return;
    }
    nodes_.erase(nodes_.begin() + row);
    rebuildBindings();
    refreshCompositionList();
    refreshPreview();
}

void JobBuilderPage::clearComposition()
{
    nodes_.clear();
    outputBindings_.clear();
    rebuildBindings();
    refreshCompositionList();
    refreshPreview();
}

void JobBuilderPage::saveGraph()
{
    if (nodes_.empty()) {
        postStatusMessage(QStringLiteral("Add at least one workflow unit before saving."), StatusToast::Severity::Warn);
        return;
    }

    rebuildBindings();

    soasimqt2::db::WorkflowGraphDraft draft{};
    const auto stamp = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    draft.name = "Qt2 workflow graph " + std::to_string(stamp);
    draft.description = "Authored from Qt2 Workflow Builder";
    draft.graph_version = 1;
    draft.graph_hash = workflowGraphHash(nodes_, outputBindings_);

    for (const auto& node : nodes_) {
        const auto* unit = findUnit(node.unit_kind);
        if (unit == nullptr) {
            postStatusMessage(QStringLiteral("Cannot save graph with unknown workflow unit."), StatusToast::Severity::Error);
            return;
        }

        simcore::db::SaveWorkflowGraphNodeCommand node_command{};
        node_command.node_key = node.node_key;
        node_command.unit_kind = node.unit_kind;
        node_command.display_name = unit->display_name;
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

    const auto result = soasimqt2::db::SimCoreDbAuthoringService::SaveWorkflowGraph(draft);
    if (!result.ok) {
        postStatusMessage(QString::fromStdString(result.error.message), StatusToast::Severity::Error);
        return;
    }

    postStatusMessage(
        QStringLiteral("Saved workflow graph %1 revision %2")
            .arg(static_cast<qint64>(result.value.workflow_graph_id))
            .arg(static_cast<qint64>(result.value.workflow_graph_revision_id)),
        StatusToast::Severity::Info);
}

void JobBuilderPage::rebuildBindings()
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
                continue;
            }
        }

        for (const auto& output : unit->possible_outputs) {
            latestOutputByKind[output.data_kind] = { node.node_key, output.key };
        }
    }
}

void JobBuilderPage::refreshUnitList()
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

void JobBuilderPage::refreshCompositionList()
{
    if (compositionList_ == nullptr) {
        return;
    }
    compositionList_->clear();
    for (const auto& node : nodes_) {
        compositionList_->addItem(describeNode(node));
    }
}

void JobBuilderPage::refreshPreview()
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
    lines << QStringLiteral("Status: %1").arg(result.value.valid ? QStringLiteral("ready") : QStringLiteral("needs inputs"));
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

void JobBuilderPage::postStatusMessage(const QString& text, StatusToast::Severity severity)
{
    if (text.isEmpty()) {
        return;
    }
    emit statusToastRequested(StatusToast{ severity, text, {}, 1, QDateTime{}, 4000 });
}

const JobBuilderPage::WorkflowUnitDefinition* JobBuilderPage::findUnit(const std::string& unit_kind) const
{
    const auto it = std::find_if(units_.begin(), units_.end(), [&](const auto& unit) {
        return unit.unit_kind == unit_kind;
    });
    return it == units_.end() ? nullptr : &*it;
}

QString JobBuilderPage::describeUnit(const WorkflowUnitDefinition& unit) const
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

QString JobBuilderPage::describeNode(const WorkflowCompositionNode& node) const
{
    const auto* unit = findUnit(node.unit_kind);
    return QStringLiteral("%1  %2")
        .arg(QString::fromStdString(node.node_key))
        .arg(unit != nullptr ? QString::fromStdString(unit->display_name) : QString::fromStdString(node.unit_kind));
}
