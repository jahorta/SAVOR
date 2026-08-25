#pragma once

#include <functional>
#include <optional>
#include <vector>

#include <QtWidgets/QWidget>

#include "Runner/Runtime/Predicates/PredicateExecution.h"

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;

class GuidedPredicateRuleEditor final : public QWidget
{
public:
    explicit GuidedPredicateRuleEditor(QWidget* parent = nullptr);

    void setCatalog(const savor::runtime::predicates::PredicateAuthoringCatalogV2& catalog);
    void setRule(const savor::runtime::predicates::PredicateGuidedNodeV1& root);
    [[nodiscard]] const savor::runtime::predicates::PredicateGuidedNodeV1& rule() const noexcept;
    [[nodiscard]] savor::runtime::predicates::PredicateGuidedCompileResultV1 compile() const;
    void setChangedCallback(std::function<void()> callback);
    void resetDirty() noexcept;
    [[nodiscard]] bool isDirty() const noexcept;

private:
    using GuidedNode = savor::runtime::predicates::PredicateGuidedNodeV1;
    using GuidedKind = savor::runtime::predicates::PredicateGuidedNodeKindV1;
    using TypeRef = savor::runtime::program::TypeRef;

    void createWidgets();
    void rebuildTree(std::optional<std::vector<int>> preferred_path = std::nullopt,
                     std::optional<int> preferred_role = std::nullopt);
    void rebuildDetails();
    void buildConditionTree(QTreeWidgetItem* parent,
                            const std::vector<int>& path,
                            const QString& prefix = {});
    void buildValueTree(QTreeWidgetItem* parent,
                        const std::vector<int>& path,
                        const QString& prefix);
    void buildConditionDetails(const std::vector<int>& path);
    void buildComparisonDetails(const std::vector<int>& path);
    void buildValueDetails(const std::vector<int>& path);
    void normalizeCondition(GuidedNode& node);
    void normalizeValue(GuidedNode& node);
    void normalizeRecipe(GuidedNode& node);
    void setConditionShape(const std::vector<int>& path, int shape);
    void setNegated(const std::vector<int>& path, bool negated);
    void addCondition(const std::vector<int>& path);
    void removeCondition(const std::vector<int>& path);
    void moveCondition(const std::vector<int>& path, int delta);
    void markChanged(bool rebuild_tree = true,
                     std::optional<std::vector<int>> preferred_path = std::nullopt,
                     std::optional<int> preferred_role = std::nullopt);
    void refreshValidation();
    void refreshTreeActions();
    [[nodiscard]] GuidedNode* nodeAt(const std::vector<int>& path);
    [[nodiscard]] const GuidedNode* nodeAt(const std::vector<int>& path) const;
    [[nodiscard]] std::optional<TypeRef> inferredType(const GuidedNode& node) const;
    [[nodiscard]] QString typeLabel(const TypeRef& type) const;
    [[nodiscard]] QString conditionSummary(const GuidedNode& node) const;
    [[nodiscard]] QString valueSummary(const GuidedNode& node) const;
    [[nodiscard]] QString comparisonLabel(GuidedKind kind) const;
    [[nodiscard]] QString calculationLabel(GuidedKind kind) const;
    [[nodiscard]] std::optional<TypeRef> expectedTypeForValue(
        const std::vector<int>& path) const;
    [[nodiscard]] std::vector<int> selectedPath() const;
    [[nodiscard]] int selectedRole() const;
    [[nodiscard]] std::optional<savor::runtime::program::LiteralValue>
        parseLiteral(const QString& text, const TypeRef& type, QString* error) const;

    savor::runtime::predicates::PredicateAuthoringCatalogV2 catalog_;
    GuidedNode root_;
    std::function<void()> changedCallback_;
    bool dirty_ = false;
    bool rebuilding_ = false;

    QTreeWidget* ruleTree_ = nullptr;
    QWidget* detailHost_ = nullptr;
    QVBoxLayout* detailLayout_ = nullptr;
    QLabel* detailHeading_ = nullptr;
    QPushButton* moveUpButton_ = nullptr;
    QPushButton* moveDownButton_ = nullptr;
    QPushButton* removeButton_ = nullptr;
    QLabel* validationLabel_ = nullptr;
    QPlainTextEdit* technicalDetails_ = nullptr;
};
