#include "GuidedPredicateRuleEditor.h"

#include <algorithm>
#include <limits>
#include <ranges>
#include <set>

#include <QtCore/QSignalBlocker>
#include <QtCore/QStringList>
#include <QtCore/QVariant>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QTreeWidgetItemIterator>
#include <QtWidgets/QVBoxLayout>

namespace {

using namespace savor::runtime::predicates;
using namespace savor::runtime::program;

constexpr int kPathRole = Qt::UserRole;
constexpr int kEditorRole = Qt::UserRole + 1;
constexpr std::string_view kPendingComparisonKey = "ui.pending.comparison";

enum class TreeEditorRole : int
{
    Condition,
    Comparison,
    Value,
};

bool IsComparison(PredicateGuidedNodeKindV1 kind)
{
    return kind >= PredicateGuidedNodeKindV1::Equal &&
           kind <= PredicateGuidedNodeKindV1::GreaterEqual;
}

bool IsCalculation(PredicateGuidedNodeKindV1 kind)
{
    return kind >= PredicateGuidedNodeKindV1::Add &&
           kind <= PredicateGuidedNodeKindV1::Remainder;
}

bool IsNumeric(const TypeRef& type)
{
    if (type.is_named()) return false;
    return type.builtin >= BuiltinType::U8 && type.builtin <= BuiltinType::F64;
}

PredicateGuidedNodeV1 EmptyValueNode()
{
    return {.kind = PredicateGuidedNodeKindV1::CatalogValue};
}

PredicateGuidedNodeV1 LiteralNode(BuiltinType type = BuiltinType::U32)
{
    const auto ref = TypeRef::Builtin(type);
    LiteralValue value{.type = ref};
    switch (type) {
    case BuiltinType::Bool: value.payload = false; break;
    case BuiltinType::U8: value.payload = std::uint8_t{}; break;
    case BuiltinType::U16: value.payload = std::uint16_t{}; break;
    case BuiltinType::U32: value.payload = std::uint32_t{}; break;
    case BuiltinType::U64: value.payload = std::uint64_t{}; break;
    case BuiltinType::I32: value.payload = std::int32_t{}; break;
    case BuiltinType::I64: value.payload = std::int64_t{}; break;
    case BuiltinType::F32: value.payload = float{}; break;
    case BuiltinType::F64: value.payload = double{}; break;
    default: value.payload = UnitValue{}; break;
    }
    return {.kind = PredicateGuidedNodeKindV1::Literal,
            .declared_type = ref,
            .literal = value};
}

PredicateGuidedNodeV1 LiteralNode(const TypeRef& type)
{
    if (type.is_named()) return LiteralNode();
    return LiteralNode(type.builtin);
}

PredicateGuidedNodeV1 ComparisonNode()
{
    return {.kind = PredicateGuidedNodeKindV1::Equal,
            .key = std::string(kPendingComparisonKey),
            .children = {EmptyValueNode(), EmptyValueNode()}};
}

bool HasPendingComparison(const PredicateGuidedNodeV1& node)
{
    if (IsComparison(node.kind) && node.key == kPendingComparisonKey) return true;
    return std::ranges::any_of(node.children, HasPendingComparison);
}

bool HasUnselectedValue(const PredicateGuidedNodeV1& node)
{
    if (node.kind == PredicateGuidedNodeKindV1::CatalogValue && node.key.empty()) return true;
    return std::ranges::any_of(node.children, HasUnselectedValue);
}

QString LiteralText(const LiteralValue& value)
{
    return std::visit([](const auto& item) -> QString {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, UnitValue>) return {};
        else if constexpr (std::is_same_v<T, bool>)
            return item ? QStringLiteral("true") : QStringLiteral("false");
        else if constexpr (std::is_arithmetic_v<T>) return QString::number(item);
        else return QStringLiteral("<typed value>");
    }, value.payload);
}

QVariant EncodePath(const std::vector<int>& path)
{
    QVariantList values;
    values.reserve(static_cast<qsizetype>(path.size()));
    for (const int part : path) values.push_back(part);
    return values;
}

std::vector<int> DecodePath(const QVariant& value)
{
    std::vector<int> path;
    const auto values = value.toList();
    path.reserve(static_cast<std::size_t>(values.size()));
    for (const auto& part : values) path.push_back(part.toInt());
    return path;
}

void ClearLayout(QLayout* layout)
{
    while (layout && layout->count() > 0) {
        auto* item = layout->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        if (item->layout()) ClearLayout(item->layout());
        delete item;
    }
}

} // namespace

GuidedPredicateRuleEditor::GuidedPredicateRuleEditor(QWidget* parent)
    : QWidget(parent), root_(ComparisonNode())
{
    createWidgets();
}

void GuidedPredicateRuleEditor::createWidgets()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto* intro = new QLabel(QStringLiteral(
        "Build the rule in game terms. Select an item in the rule to edit it."), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* splitter = new QSplitter(Qt::Horizontal, this);

    auto* treePane = new QWidget(splitter);
    auto* treeLayout = new QVBoxLayout(treePane);
    treeLayout->setContentsMargins(0, 0, 0, 0);
    ruleTree_ = new QTreeWidget(treePane);
    ruleTree_->setHeaderHidden(true);
    ruleTree_->setAlternatingRowColors(true);
    ruleTree_->setUniformRowHeights(true);
    treeLayout->addWidget(ruleTree_, 1);

    auto* treeActions = new QHBoxLayout();
    moveUpButton_ = new QPushButton(QStringLiteral("Up"), treePane);
    moveDownButton_ = new QPushButton(QStringLiteral("Down"), treePane);
    removeButton_ = new QPushButton(QStringLiteral("Remove"), treePane);
    treeActions->addWidget(moveUpButton_);
    treeActions->addWidget(moveDownButton_);
    treeActions->addWidget(removeButton_);
    treeActions->addStretch();
    treeLayout->addLayout(treeActions);

    auto* detailFrame = new QFrame(splitter);
    detailFrame->setObjectName(QStringLiteral("jobsSurfacePanel"));
    detailLayout_ = new QVBoxLayout(detailFrame);
    detailLayout_->setContentsMargins(12, 12, 12, 12);
    detailHost_ = detailFrame;

    splitter->addWidget(treePane);
    splitter->addWidget(detailFrame);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes({620, 380});
    layout->addWidget(splitter, 1);

    validationLabel_ = new QLabel(this);
    validationLabel_->setWordWrap(true);
    layout->addWidget(validationLabel_);

    auto* technical = new QGroupBox(QStringLiteral("Technical details"), this);
    technical->setCheckable(true);
    technical->setChecked(false);
    auto* technicalLayout = new QVBoxLayout(technical);
    technicalDetails_ = new QPlainTextEdit(technical);
    technicalDetails_->setReadOnly(true);
    technicalDetails_->setMaximumBlockCount(4096);
    technicalDetails_->setVisible(false);
    technicalLayout->addWidget(technicalDetails_);
    connect(technical, &QGroupBox::toggled, technicalDetails_, &QWidget::setVisible);
    layout->addWidget(technical);

    connect(ruleTree_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem*, QTreeWidgetItem*) {
                rebuildDetails();
                refreshTreeActions();
            });
    connect(moveUpButton_, &QPushButton::clicked, this,
            [this] { moveCondition(selectedPath(), -1); });
    connect(moveDownButton_, &QPushButton::clicked, this,
            [this] { moveCondition(selectedPath(), 1); });
    connect(removeButton_, &QPushButton::clicked, this,
            [this] { removeCondition(selectedPath()); });

    rebuildTree();
}

void GuidedPredicateRuleEditor::setCatalog(const PredicateAuthoringCatalogV2& catalog)
{
    catalog_ = catalog;
    rebuildTree();
}

void GuidedPredicateRuleEditor::setRule(const GuidedNode& root)
{
    root_ = root;
    dirty_ = false;
    rebuildTree(std::vector<int>{}, static_cast<int>(TreeEditorRole::Condition));
}

const GuidedPredicateRuleEditor::GuidedNode& GuidedPredicateRuleEditor::rule() const noexcept
{
    return root_;
}

PredicateGuidedCompileResultV1 GuidedPredicateRuleEditor::compile() const
{
    if (HasPendingComparison(root_)) {
        PredicateGuidedCompileResultV1 result;
        result.diagnostics.push_back({
            .path = "root",
            .code = "predicate.comparison_required",
            .message = "Choose a comparison."});
        return result;
    }
    return CompileGuidedPredicateDefinitionV1(root_, catalog_);
}

void GuidedPredicateRuleEditor::setChangedCallback(std::function<void()> callback)
{
    changedCallback_ = std::move(callback);
}

void GuidedPredicateRuleEditor::resetDirty() noexcept { dirty_ = false; }
bool GuidedPredicateRuleEditor::isDirty() const noexcept { return dirty_; }

GuidedPredicateRuleEditor::GuidedNode*
GuidedPredicateRuleEditor::nodeAt(const std::vector<int>& path)
{
    auto* node = &root_;
    for (const int part : path) {
        if (part < 0 || part >= static_cast<int>(node->children.size())) return nullptr;
        node = &node->children[static_cast<std::size_t>(part)];
    }
    return node;
}

const GuidedPredicateRuleEditor::GuidedNode*
GuidedPredicateRuleEditor::nodeAt(const std::vector<int>& path) const
{
    const auto* node = &root_;
    for (const int part : path) {
        if (part < 0 || part >= static_cast<int>(node->children.size())) return nullptr;
        node = &node->children[static_cast<std::size_t>(part)];
    }
    return node;
}

QString GuidedPredicateRuleEditor::typeLabel(const TypeRef& type) const
{
    if (type.is_named()) return QString::fromStdString(type.named->canonical_id);
    static constexpr const char* labels[] = {
        "Unit", "True / false", "U8", "U16", "Nonnegative whole number",
        "U64", "Signed whole number", "I64", "Decimal", "F64"};
    const auto index = static_cast<std::size_t>(type.builtin);
    return index < std::size(labels) ? QString::fromLatin1(labels[index])
                                     : QStringLiteral("Unknown");
}

std::optional<GuidedPredicateRuleEditor::TypeRef>
GuidedPredicateRuleEditor::inferredType(const GuidedNode& node) const
{
    if (node.kind == GuidedKind::Literal && node.literal) return node.literal->type;
    if (node.kind == GuidedKind::Parameter && node.declared_type) return node.declared_type;
    if (node.kind == GuidedKind::SemanticInput) {
        const auto found = std::ranges::find(catalog_.semantic_inputs, node.key,
            &PredicateAuthoringSemanticInputV2::role_id);
        if (found != catalog_.semantic_inputs.end()) return found->value_type;
    }
    if (node.kind == GuidedKind::CatalogValue) {
        const auto found = std::ranges::find(catalog_.value_recipes, node.key,
            &PredicateAuthoringValueRecipeV2::recipe_id);
        if (found != catalog_.value_recipes.end()) return found->result_type;
    }
    if (IsCalculation(node.kind) && !node.children.empty()) return inferredType(node.children.front());
    if (IsComparison(node.kind) || node.kind == GuidedKind::All ||
        node.kind == GuidedKind::Any || node.kind == GuidedKind::Not)
        return TypeRef::Builtin(BuiltinType::Bool);
    return std::nullopt;
}

QString GuidedPredicateRuleEditor::comparisonLabel(GuidedKind kind) const
{
    const auto found = std::ranges::find(catalog_.comparison_operators, kind,
        &PredicateAuthoringOperatorV2::node_kind);
    if (found != catalog_.comparison_operators.end()) return QString::fromStdString(found->display_name);
    switch (kind) {
    case GuidedKind::Equal: return QStringLiteral("equals");
    case GuidedKind::NotEqual: return QStringLiteral("does not equal");
    case GuidedKind::Less: return QStringLiteral("is before / less than");
    case GuidedKind::LessEqual: return QStringLiteral("is at most");
    case GuidedKind::Greater: return QStringLiteral("is after / greater than");
    case GuidedKind::GreaterEqual: return QStringLiteral("is at least");
    default: return QStringLiteral("choose a comparison");
    }
}

QString GuidedPredicateRuleEditor::calculationLabel(GuidedKind kind) const
{
    const auto found = std::ranges::find(catalog_.calculation_operators, kind,
        &PredicateAuthoringOperatorV2::node_kind);
    if (found != catalog_.calculation_operators.end()) return QString::fromStdString(found->display_name);
    switch (kind) {
    case GuidedKind::Add: return QStringLiteral("Add");
    case GuidedKind::Subtract: return QStringLiteral("Subtract");
    case GuidedKind::Multiply: return QStringLiteral("Multiply");
    case GuidedKind::Divide: return QStringLiteral("Divide");
    case GuidedKind::Remainder: return QStringLiteral("Remainder");
    default: return QStringLiteral("Calculation");
    }
}

QString GuidedPredicateRuleEditor::valueSummary(const GuidedNode& node) const
{
    if (node.kind == GuidedKind::CatalogValue) {
        if (node.key.empty()) return QStringLiteral("Choose a value");
        const auto found = std::ranges::find(catalog_.value_recipes, node.key,
            &PredicateAuthoringValueRecipeV2::recipe_id);
        return found == catalog_.value_recipes.end()
            ? QStringLiteral("Unavailable battle value")
            : QString::fromStdString(found->display_name);
    }
    if (node.kind == GuidedKind::Literal && node.literal)
        return QStringLiteral("Fixed value %1").arg(LiteralText(*node.literal));
    if (IsCalculation(node.kind)) {
        const auto left = node.children.empty() ? QStringLiteral("Choose a value")
                                                : valueSummary(node.children[0]);
        const auto right = node.children.size() < 2 ? QStringLiteral("Choose a value")
                                                    : valueSummary(node.children[1]);
        return QStringLiteral("%1 (%2, %3)").arg(calculationLabel(node.kind), left, right);
    }
    if (node.kind == GuidedKind::Parameter)
        return QStringLiteral("%1 supplied by Execution Binding").arg(QString::fromStdString(node.key));
    if (node.kind == GuidedKind::SemanticInput) return QStringLiteral("Current battle data");
    return QStringLiteral("Unavailable value");
}

QString GuidedPredicateRuleEditor::conditionSummary(const GuidedNode& outer) const
{
    if (outer.kind == GuidedKind::Not && outer.children.size() == 1)
        return QStringLiteral("Not: %1").arg(conditionSummary(outer.children.front()));
    if (IsComparison(outer.kind) && outer.children.size() >= 2)
        return QStringLiteral("%1 %2 %3")
            .arg(valueSummary(outer.children[0]),
                 outer.key == kPendingComparisonKey
                    ? QStringLiteral("choose a comparison")
                    : comparisonLabel(outer.kind),
                 valueSummary(outer.children[1]));
    if (outer.kind == GuidedKind::All)
        return QStringLiteral("All conditions (%1)").arg(outer.children.size());
    if (outer.kind == GuidedKind::Any)
        return QStringLiteral("Any condition (%1)").arg(outer.children.size());
    return QStringLiteral("Unavailable condition");
}

void GuidedPredicateRuleEditor::normalizeRecipe(GuidedNode& node)
{
    const auto recipe = std::ranges::find(catalog_.value_recipes, node.key,
        &PredicateAuthoringValueRecipeV2::recipe_id);
    if (recipe == catalog_.value_recipes.end()) {
        node.children.clear();
        return;
    }
    std::vector<GuidedNode> generated;
    for (const auto& argument : recipe->arguments) {
        if (argument.automatic_semantic_input_role || !argument.suggested_parameter_name) continue;
        generated.push_back({.kind = GuidedKind::Parameter,
            .key = *argument.suggested_parameter_name,
            .declared_type = argument.value_type});
    }
    node.children = std::move(generated);
}

void GuidedPredicateRuleEditor::normalizeValue(GuidedNode& node)
{
    if (node.kind == GuidedKind::CatalogValue) {
        normalizeRecipe(node);
    } else if (node.kind == GuidedKind::Literal) {
        if (!node.literal) node = LiteralNode();
        node.children.clear();
    } else if (IsCalculation(node.kind)) {
        while (node.children.size() < 2) node.children.push_back(EmptyValueNode());
        node.children.resize(2);
    }
}

void GuidedPredicateRuleEditor::normalizeCondition(GuidedNode& node)
{
    if (IsComparison(node.kind)) {
        while (node.children.size() < 2) node.children.push_back(EmptyValueNode());
        node.children.resize(2);
    } else if (node.kind == GuidedKind::All || node.kind == GuidedKind::Any) {
        while (node.children.size() < 2) node.children.push_back(ComparisonNode());
    } else if (node.kind == GuidedKind::Not) {
        if (node.children.empty()) node.children.push_back(ComparisonNode());
        node.children.resize(1);
        normalizeCondition(node.children.front());
    }
}

void GuidedPredicateRuleEditor::setConditionShape(const std::vector<int>& path, int shape)
{
    auto* outer = nodeAt(path);
    if (!outer) return;
    auto* node = outer->kind == GuidedKind::Not && outer->children.size() == 1
        ? &outer->children.front() : outer;
    const auto next = shape == 1 ? GuidedKind::All
        : shape == 2 ? GuidedKind::Any : GuidedKind::Equal;
    if (node->kind == next || (shape == 0 && IsComparison(node->kind))) return;
    *node = next == GuidedKind::All || next == GuidedKind::Any
        ? GuidedNode{.kind = next, .children = {ComparisonNode(), ComparisonNode()}}
        : ComparisonNode();
    markChanged(true, path, static_cast<int>(TreeEditorRole::Condition));
}

void GuidedPredicateRuleEditor::setNegated(const std::vector<int>& path, bool negated)
{
    auto* node = nodeAt(path);
    if (!node) return;
    if (negated && node->kind != GuidedKind::Not) {
        GuidedNode child = std::move(*node);
        *node = {.kind = GuidedKind::Not, .children = {std::move(child)}};
    } else if (!negated && node->kind == GuidedKind::Not && node->children.size() == 1) {
        *node = std::move(node->children.front());
    }
    markChanged(true, path, static_cast<int>(TreeEditorRole::Condition));
}

void GuidedPredicateRuleEditor::addCondition(const std::vector<int>& path)
{
    auto* node = nodeAt(path);
    auto childPath = path;
    if (node && node->kind == GuidedKind::Not && node->children.size() == 1) {
        node = &node->children.front();
        childPath.push_back(0);
    }
    if (!node || (node->kind != GuidedKind::All && node->kind != GuidedKind::Any)) return;
    node->children.push_back(ComparisonNode());
    childPath.push_back(static_cast<int>(node->children.size() - 1));
    markChanged(true, childPath, static_cast<int>(TreeEditorRole::Condition));
}

void GuidedPredicateRuleEditor::removeCondition(const std::vector<int>& path)
{
    if (path.empty()) return;
    auto parentPath = path;
    const int index = parentPath.back();
    parentPath.pop_back();
    auto* parent = nodeAt(parentPath);
    if (!parent || (parent->kind != GuidedKind::All && parent->kind != GuidedKind::Any) ||
        parent->children.size() <= 2 || index < 0 ||
        index >= static_cast<int>(parent->children.size())) return;
    parent->children.erase(parent->children.begin() + index);
    markChanged(true, parentPath, static_cast<int>(TreeEditorRole::Condition));
}

void GuidedPredicateRuleEditor::moveCondition(const std::vector<int>& path, int delta)
{
    if (path.empty()) return;
    auto parentPath = path;
    const int index = parentPath.back();
    const int target = index + delta;
    parentPath.pop_back();
    auto* parent = nodeAt(parentPath);
    if (!parent || (parent->kind != GuidedKind::All && parent->kind != GuidedKind::Any) ||
        target < 0 || target >= static_cast<int>(parent->children.size())) return;
    std::swap(parent->children[static_cast<std::size_t>(index)],
              parent->children[static_cast<std::size_t>(target)]);
    auto selected = parentPath;
    selected.push_back(target);
    markChanged(true, selected, static_cast<int>(TreeEditorRole::Condition));
}

void GuidedPredicateRuleEditor::buildValueTree(
    QTreeWidgetItem* parent, const std::vector<int>& path, const QString& prefix)
{
    const auto* node = nodeAt(path);
    if (!node) return;
    auto* item = new QTreeWidgetItem(parent);
    item->setText(0, QStringLiteral("%1: %2").arg(prefix, valueSummary(*node)));
    item->setData(0, kPathRole, EncodePath(path));
    item->setData(0, kEditorRole, static_cast<int>(TreeEditorRole::Value));
    if (IsCalculation(node->kind) && node->children.size() >= 2) {
        auto first = path;
        first.push_back(0);
        buildValueTree(item, first, QStringLiteral("First value"));
        auto second = path;
        second.push_back(1);
        buildValueTree(item, second, QStringLiteral("Second value"));
    }
}

void GuidedPredicateRuleEditor::buildConditionTree(
    QTreeWidgetItem* parent, const std::vector<int>& path, const QString& prefix)
{
    const auto* outer = nodeAt(path);
    if (!outer) return;
    auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(ruleTree_);
    item->setText(0, prefix.isEmpty() ? conditionSummary(*outer)
                                     : QStringLiteral("%1: %2").arg(prefix, conditionSummary(*outer)));
    item->setData(0, kPathRole, EncodePath(path));
    item->setData(0, kEditorRole, static_cast<int>(TreeEditorRole::Condition));

    const bool negated = outer->kind == GuidedKind::Not && outer->children.size() == 1;
    const auto* node = negated ? &outer->children.front() : outer;
    auto innerPath = path;
    if (negated) innerPath.push_back(0);
    if (IsComparison(node->kind) && node->children.size() >= 2) {
        auto left = innerPath;
        left.push_back(0);
        buildValueTree(item, left, QStringLiteral("Left"));
        auto* comparison = new QTreeWidgetItem(item);
        comparison->setText(0, QStringLiteral("Comparison: %1").arg(
            node->key == kPendingComparisonKey
                ? QStringLiteral("Choose a comparison")
                : comparisonLabel(node->kind)));
        comparison->setData(0, kPathRole, EncodePath(path));
        comparison->setData(0, kEditorRole, static_cast<int>(TreeEditorRole::Comparison));
        auto right = innerPath;
        right.push_back(1);
        buildValueTree(item, right, QStringLiteral("Right"));
    } else if (node->kind == GuidedKind::All || node->kind == GuidedKind::Any) {
        for (std::size_t index = 0; index < node->children.size(); ++index) {
            auto child = innerPath;
            child.push_back(static_cast<int>(index));
            buildConditionTree(item, child);
        }
    }
}

std::vector<int> GuidedPredicateRuleEditor::selectedPath() const
{
    return ruleTree_ && ruleTree_->currentItem()
        ? DecodePath(ruleTree_->currentItem()->data(0, kPathRole))
        : std::vector<int>{};
}

int GuidedPredicateRuleEditor::selectedRole() const
{
    return ruleTree_ && ruleTree_->currentItem()
        ? ruleTree_->currentItem()->data(0, kEditorRole).toInt()
        : static_cast<int>(TreeEditorRole::Condition);
}

void GuidedPredicateRuleEditor::rebuildTree(
    std::optional<std::vector<int>> preferredPath, std::optional<int> preferredRole)
{
    const auto oldPath = preferredPath.value_or(selectedPath());
    const int oldRole = preferredRole.value_or(selectedRole());
    const int oldScroll = ruleTree_->verticalScrollBar()->value();
    const QSignalBlocker blocker(ruleTree_);
    rebuilding_ = true;
    ruleTree_->clear();
    buildConditionTree(nullptr, {});
    ruleTree_->expandAll();

    QTreeWidgetItem* selected = nullptr;
    for (QTreeWidgetItemIterator it(ruleTree_); *it; ++it) {
        if (DecodePath((*it)->data(0, kPathRole)) == oldPath &&
            (*it)->data(0, kEditorRole).toInt() == oldRole) {
            selected = *it;
            break;
        }
    }
    if (!selected && ruleTree_->topLevelItemCount() > 0) selected = ruleTree_->topLevelItem(0);
    ruleTree_->setCurrentItem(selected);
    ruleTree_->verticalScrollBar()->setValue(oldScroll);
    rebuilding_ = false;
    rebuildDetails();
    refreshTreeActions();
    refreshValidation();
}

std::optional<GuidedPredicateRuleEditor::TypeRef>
GuidedPredicateRuleEditor::expectedTypeForValue(const std::vector<int>& path) const
{
    if (path.empty()) return std::nullopt;
    auto parentPath = path;
    const int index = parentPath.back();
    parentPath.pop_back();
    const auto* parent = nodeAt(parentPath);
    if (!parent) return std::nullopt;
    if ((IsComparison(parent->kind) || IsCalculation(parent->kind)) &&
        parent->children.size() >= 2 && index >= 0 && index <= 1) {
        const auto sibling = inferredType(parent->children[static_cast<std::size_t>(1 - index)]);
        if (sibling) return sibling;
        if (IsCalculation(parent->kind)) return expectedTypeForValue(parentPath);
    }
    return std::nullopt;
}

void GuidedPredicateRuleEditor::buildConditionDetails(const std::vector<int>& path)
{
    auto* outer = nodeAt(path);
    if (!outer) return;
    const bool negated = outer->kind == GuidedKind::Not && outer->children.size() == 1;
    auto* node = negated ? &outer->children.front() : outer;

    detailHeading_ = new QLabel(QStringLiteral("Edit condition"), detailHost_);
    detailHeading_->setObjectName(QStringLiteral("sectionHeading"));
    detailLayout_->addWidget(detailHeading_);
    auto* summary = new QLabel(conditionSummary(*outer), detailHost_);
    summary->setWordWrap(true);
    detailLayout_->addWidget(summary);

    auto* form = new QFormLayout();
    auto* shape = new QComboBox(detailHost_);
    shape->addItem(QStringLiteral("Comparison"), 0);
    shape->addItem(QStringLiteral("All conditions"), 1);
    shape->addItem(QStringLiteral("Any condition"), 2);
    shape->setCurrentIndex(node->kind == GuidedKind::All ? 1 : node->kind == GuidedKind::Any ? 2 : 0);
    form->addRow(QStringLiteral("Condition"), shape);
    auto* notCheck = new QCheckBox(QStringLiteral("Negate this condition"), detailHost_);
    notCheck->setChecked(negated);
    form->addRow(QString(), notCheck);
    detailLayout_->addLayout(form);

    if (node->kind == GuidedKind::All || node->kind == GuidedKind::Any) {
        auto* add = new QPushButton(QStringLiteral("Add condition"), detailHost_);
        detailLayout_->addWidget(add);
        connect(add, &QPushButton::clicked, this, [this, path] { addCondition(path); });
    } else {
        auto* guidance = new QLabel(QStringLiteral(
            "Select Left, Comparison, or Right in the rule tree to edit that part."), detailHost_);
        guidance->setWordWrap(true);
        detailLayout_->addWidget(guidance);
    }

    connect(shape, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this, path, shape](int) {
                if (!rebuilding_) setConditionShape(path, shape->currentData().toInt());
            });
    connect(notCheck, &QCheckBox::toggled, this,
            [this, path](bool checked) {
                if (!rebuilding_) setNegated(path, checked);
            });
}

void GuidedPredicateRuleEditor::buildComparisonDetails(const std::vector<int>& path)
{
    auto* outer = nodeAt(path);
    if (!outer) return;
    const bool negated = outer->kind == GuidedKind::Not && outer->children.size() == 1;
    auto* node = negated ? &outer->children.front() : outer;
    if (!IsComparison(node->kind)) return;

    detailHeading_ = new QLabel(QStringLiteral("Edit comparison"), detailHost_);
    detailHeading_->setObjectName(QStringLiteral("sectionHeading"));
    detailLayout_->addWidget(detailHeading_);
    auto* form = new QFormLayout();
    auto* comparison = new QComboBox(detailHost_);
    comparison->addItem(QStringLiteral("Choose a comparison..."), QVariant());
    const auto leftType = node->children.empty() ? std::nullopt : inferredType(node->children[0]);
    const auto rightType = node->children.size() < 2 ? std::nullopt : inferredType(node->children[1]);
    const auto common = leftType ? leftType : rightType;
    for (const auto& operation : catalog_.comparison_operators) {
        if (operation.ordered_only && common && !IsNumeric(*common)) continue;
        comparison->addItem(QString::fromStdString(operation.display_name),
                            static_cast<int>(operation.node_kind));
        comparison->setItemData(comparison->count() - 1,
                                QString::fromStdString(operation.description), Qt::ToolTipRole);
    }
    comparison->setCurrentIndex(node->key == kPendingComparisonKey
        ? 0 : comparison->findData(static_cast<int>(node->kind)));
    form->addRow(QStringLiteral("Comparison"), comparison);
    detailLayout_->addLayout(form);
    connect(comparison, qOverload<int>(&QComboBox::activated), this,
            [this, path, comparison](int) {
                if (rebuilding_ || comparison->currentIndex() <= 0 ||
                    !comparison->currentData().isValid()) return;
                auto* selected = nodeAt(path);
                if (!selected) return;
                if (selected->kind == GuidedKind::Not && selected->children.size() == 1)
                    selected = &selected->children.front();
                selected->kind = static_cast<GuidedKind>(comparison->currentData().toInt());
                selected->key.clear();
                markChanged(true, path, static_cast<int>(TreeEditorRole::Comparison));
            });
}

void GuidedPredicateRuleEditor::buildValueDetails(const std::vector<int>& path)
{
    auto* node = nodeAt(path);
    if (!node) return;
    const auto expectedType = expectedTypeForValue(path);

    detailHeading_ = new QLabel(QStringLiteral("Edit value"), detailHost_);
    detailHeading_->setObjectName(QStringLiteral("sectionHeading"));
    detailLayout_->addWidget(detailHeading_);

    auto* sourceForm = new QFormLayout();
    auto* sourceKind = new QComboBox(detailHost_);
    sourceKind->addItem(QStringLiteral("Battle value"), static_cast<int>(GuidedKind::CatalogValue));
    if (!expectedType || !expectedType->is_named())
        sourceKind->addItem(QStringLiteral("Fixed value"), static_cast<int>(GuidedKind::Literal));
    if (!expectedType || IsNumeric(*expectedType))
        sourceKind->addItem(QStringLiteral("Calculation"), static_cast<int>(GuidedKind::Add));
    const int currentKind = node->kind == GuidedKind::CatalogValue
        ? static_cast<int>(GuidedKind::CatalogValue)
        : IsCalculation(node->kind) ? static_cast<int>(GuidedKind::Add)
                                    : static_cast<int>(GuidedKind::Literal);
    sourceKind->setCurrentIndex(sourceKind->findData(currentKind));
    sourceForm->addRow(QStringLiteral("Value source"), sourceKind);
    detailLayout_->addLayout(sourceForm);

    if (node->kind == GuidedKind::CatalogValue) {
        auto* form = new QFormLayout();
        auto* battleValue = new QComboBox(detailHost_);
        battleValue->addItem(QStringLiteral("Choose a battle value..."), QString());
        for (const auto& recipe : catalog_.value_recipes) {
            if (expectedType && recipe.result_type != *expectedType) continue;
            battleValue->addItem(
                QString::fromStdString(recipe.display_name),
                QString::fromStdString(recipe.recipe_id));
            battleValue->setItemData(battleValue->count() - 1,
                                     QString::fromStdString(recipe.description), Qt::ToolTipRole);
        }
        int selected = battleValue->findData(QString::fromStdString(node->key));
        if (selected < 0 && !node->key.empty()) {
            battleValue->addItem(QStringLiteral("Unavailable battle value"),
                                 QString::fromStdString(node->key));
            selected = battleValue->count() - 1;
        }
        battleValue->setCurrentIndex(std::max(0, selected));
        form->addRow(QStringLiteral("Battle value"), battleValue);
        detailLayout_->addLayout(form);

        const auto recipe = std::ranges::find(catalog_.value_recipes, node->key,
            &PredicateAuthoringValueRecipeV2::recipe_id);
        if (recipe != catalog_.value_recipes.end()) {
            auto* explanation = new QLabel(QString::fromStdString(recipe->description), detailHost_);
            explanation->setWordWrap(true);
            detailLayout_->addWidget(explanation);
            for (const auto& argument : recipe->arguments) {
                if (!argument.suggested_parameter_name) continue;
                auto* supplied = new QLabel(
                    QStringLiteral("%1 will be supplied by the Execution Binding.")
                        .arg(QString::fromStdString(argument.display_name)), detailHost_);
                supplied->setWordWrap(true);
                detailLayout_->addWidget(supplied);
            }
        }

        connect(battleValue, qOverload<int>(&QComboBox::activated), this,
                [this, path, battleValue](int) {
                    auto* value = nodeAt(path);
                    if (!value) return;
                    value->kind = GuidedKind::CatalogValue;
                    value->key = battleValue->currentData().toString().toStdString();
                    normalizeRecipe(*value);
                    markChanged(true, path, static_cast<int>(TreeEditorRole::Value));
                });
    } else if (node->kind == GuidedKind::Literal) {
        auto* form = new QFormLayout();
        auto* format = new QComboBox(detailHost_);
        format->addItem(QStringLiteral("True / false"), static_cast<int>(BuiltinType::Bool));
        format->addItem(QStringLiteral("Nonnegative whole number"), static_cast<int>(BuiltinType::U32));
        format->addItem(QStringLiteral("Signed whole number"), static_cast<int>(BuiltinType::I32));
        format->addItem(QStringLiteral("Decimal"), static_cast<int>(BuiltinType::F64));
        const auto type = expectedType.value_or(
            node->literal ? node->literal->type : TypeRef::Builtin(BuiltinType::U32));
        if (!type.is_named()) format->setCurrentIndex(format->findData(static_cast<int>(type.builtin)));
        if (expectedType) form->addRow(QStringLiteral("Format"), new QLabel(typeLabel(type), detailHost_));
        else form->addRow(QStringLiteral("Format"), format);
        auto* edit = new QLineEdit(node->literal ? LiteralText(*node->literal) : QString(), detailHost_);
        form->addRow(QStringLiteral("Value"), edit);
        detailLayout_->addLayout(form);
        const auto apply = [this, path, edit, format, expectedType] {
            auto* value = nodeAt(path);
            if (!value) return;
            const auto type = expectedType.value_or(
                TypeRef::Builtin(static_cast<BuiltinType>(format->currentData().toInt())));
            QString error;
            const auto literal = parseLiteral(edit->text().trimmed(), type, &error);
            if (!literal) {
                validationLabel_->setText(error);
                return;
            }
            value->kind = GuidedKind::Literal;
            value->literal = *literal;
            value->declared_type = type;
            value->children.clear();
            markChanged(true, path, static_cast<int>(TreeEditorRole::Value));
        };
        connect(edit, &QLineEdit::editingFinished, this, apply);
        connect(format, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [apply](int) { apply(); });
    } else if (IsCalculation(node->kind)) {
        auto* form = new QFormLayout();
        auto* operation = new QComboBox(detailHost_);
        for (const auto& candidate : catalog_.calculation_operators) {
            operation->addItem(QString::fromStdString(candidate.display_name),
                               static_cast<int>(candidate.node_kind));
        }
        operation->setCurrentIndex(operation->findData(static_cast<int>(node->kind)));
        form->addRow(QStringLiteral("Calculation"), operation);
        detailLayout_->addLayout(form);
        auto* guidance = new QLabel(QStringLiteral(
            "Select First value or Second value beneath this calculation in the rule tree to edit it."),
            detailHost_);
        guidance->setWordWrap(true);
        detailLayout_->addWidget(guidance);
        connect(operation, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this, path, operation](int) {
                    if (rebuilding_ || operation->currentIndex() < 0) return;
                    auto* value = nodeAt(path);
                    if (!value) return;
                    value->kind = static_cast<GuidedKind>(operation->currentData().toInt());
                    normalizeValue(*value);
                    markChanged(true, path, static_cast<int>(TreeEditorRole::Value));
                });
    } else {
        auto* unresolved = new QLabel(QStringLiteral(
            "This stored value is unavailable. Choose a replacement before saving."), detailHost_);
        unresolved->setWordWrap(true);
        detailLayout_->addWidget(unresolved);
    }

    connect(sourceKind, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this, path, sourceKind, expectedType](int) {
                if (rebuilding_ || sourceKind->currentIndex() < 0) return;
                auto* value = nodeAt(path);
                if (!value) return;
                const auto selected = static_cast<GuidedKind>(sourceKind->currentData().toInt());
                if (selected == GuidedKind::CatalogValue) {
                    *value = EmptyValueNode();
                } else if (selected == GuidedKind::Literal) {
                    *value = expectedType ? LiteralNode(*expectedType) : LiteralNode();
                } else {
                    const auto operand = EmptyValueNode();
                    *value = {.kind = GuidedKind::Add, .children = {operand, operand}};
                }
                normalizeValue(*value);
                markChanged(true, path, static_cast<int>(TreeEditorRole::Value));
            });
}

void GuidedPredicateRuleEditor::rebuildDetails()
{
    if (!detailLayout_) return;
    ClearLayout(detailLayout_);
    const auto path = selectedPath();
    switch (static_cast<TreeEditorRole>(selectedRole())) {
    case TreeEditorRole::Condition: buildConditionDetails(path); break;
    case TreeEditorRole::Comparison: buildComparisonDetails(path); break;
    case TreeEditorRole::Value: buildValueDetails(path); break;
    }
    detailLayout_->addStretch();
}

void GuidedPredicateRuleEditor::refreshTreeActions()
{
    bool canMoveUp = false;
    bool canMoveDown = false;
    bool canRemove = false;
    const auto path = selectedPath();
    if (selectedRole() == static_cast<int>(TreeEditorRole::Condition) && !path.empty()) {
        auto parentPath = path;
        const int index = parentPath.back();
        parentPath.pop_back();
        const auto* parent = nodeAt(parentPath);
        if (parent && (parent->kind == GuidedKind::All || parent->kind == GuidedKind::Any)) {
            canMoveUp = index > 0;
            canMoveDown = index + 1 < static_cast<int>(parent->children.size());
            canRemove = parent->children.size() > 2;
        }
    }
    moveUpButton_->setEnabled(canMoveUp);
    moveDownButton_->setEnabled(canMoveDown);
    removeButton_->setEnabled(canRemove);
}

void GuidedPredicateRuleEditor::markChanged(
    bool rebuildTreeNow, std::optional<std::vector<int>> preferredPath,
    std::optional<int> preferredRole)
{
    dirty_ = true;
    if (rebuildTreeNow) rebuildTree(std::move(preferredPath), preferredRole);
    else refreshValidation();
    if (changedCallback_) changedCallback_();
}

std::optional<LiteralValue> GuidedPredicateRuleEditor::parseLiteral(
    const QString& text, const TypeRef& type, QString* error) const
{
    if (type.is_named()) {
        if (error) *error = QStringLiteral("This value needs a scalar format.");
        return std::nullopt;
    }
    LiteralValue value{.type = type};
    bool ok = false;
    switch (type.builtin) {
    case BuiltinType::Bool:
        if (text.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0 || text == QStringLiteral("1")) value.payload = true;
        else if (text.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0 || text == QStringLiteral("0")) value.payload = false;
        else break;
        return value;
    case BuiltinType::U8: { const auto n=text.toUInt(&ok,0); if(ok&&n<=UINT8_MAX){value.payload=static_cast<std::uint8_t>(n);return value;} break; }
    case BuiltinType::U16: { const auto n=text.toUInt(&ok,0); if(ok&&n<=UINT16_MAX){value.payload=static_cast<std::uint16_t>(n);return value;} break; }
    case BuiltinType::U32: { const auto n=text.toULongLong(&ok,0); if(ok&&n<=UINT32_MAX){value.payload=static_cast<std::uint32_t>(n);return value;} break; }
    case BuiltinType::U64: { const auto n=text.toULongLong(&ok,0); if(ok){value.payload=static_cast<std::uint64_t>(n);return value;} break; }
    case BuiltinType::I32: { const auto n=text.toLongLong(&ok,0); if(ok&&n>=INT32_MIN&&n<=INT32_MAX){value.payload=static_cast<std::int32_t>(n);return value;} break; }
    case BuiltinType::I64: { const auto n=text.toLongLong(&ok,0); if(ok){value.payload=static_cast<std::int64_t>(n);return value;} break; }
    case BuiltinType::F32: { const auto n=text.toFloat(&ok); if(ok){value.payload=n;return value;} break; }
    case BuiltinType::F64: { const auto n=text.toDouble(&ok); if(ok){value.payload=n;return value;} break; }
    default: break;
    }
    if (error) *error = QStringLiteral("The fixed value does not fit the inferred format.");
    return std::nullopt;
}

void GuidedPredicateRuleEditor::refreshValidation()
{
    if (catalog_.revision == 0) {
        validationLabel_->setText(QStringLiteral("Loading the Predicate authoring catalog..."));
        technicalDetails_->clear();
        return;
    }
    if (HasPendingComparison(root_) || HasUnselectedValue(root_)) {
        QStringList missing;
        if (HasUnselectedValue(root_)) missing << QStringLiteral("choose every value");
        if (HasPendingComparison(root_)) missing << QStringLiteral("choose every comparison");
        validationLabel_->setText(QStringLiteral("Complete the rule: %1.")
                                      .arg(missing.join(QStringLiteral(" and "))));
        technicalDetails_->clear();
        return;
    }
    const auto result = compile();
    if (!result) {
        QStringList errors;
        for (const auto& diagnostic : result.diagnostics)
            errors << QStringLiteral("%1: %2")
                .arg(QString::fromStdString(diagnostic.path),
                     QString::fromStdString(diagnostic.message));
        validationLabel_->setText(QStringLiteral("Rule needs attention: %1")
                                      .arg(errors.join(QStringLiteral("; "))));
        technicalDetails_->setPlainText(errors.join('\n'));
        return;
    }
    validationLabel_->setText(QStringLiteral("Rule is valid and returns true or false."));
    QStringList lines{QStringLiteral("Generated inputs:")};
    for (const auto& witness : result.definition.witnesses)
        lines << QStringLiteral("  %1 : %2")
            .arg(QString::fromStdString(witness.name), typeLabel(witness.value_type));
    lines << QStringLiteral("\nCompiled expression:");
    for (const auto& node : result.definition.expression) {
        QStringList operands;
        for (const auto operand : node.operands) operands << QString::number(operand);
        lines << QStringLiteral("  %1 — %2; inputs [%3]")
            .arg(QString::fromStdString(node.source_label), typeLabel(node.result_type),
                 operands.join(QStringLiteral(", ")));
    }
    technicalDetails_->setPlainText(lines.join('\n'));
}
