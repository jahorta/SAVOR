#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include <QtWidgets/QWidget>

#include "Authoring/IAuthoringDb.h"
#include "DB/SavorDbServiceResult.h"
#include "GUI/Common/StatusToast.h"
#include "GUI/Refresh/DatabaseProjectionController.h"

class GuidedPredicateRuleEditor;
class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QVBoxLayout;

class PredicateAuthoringEditor final : public QWidget
{
public:
    enum class ObjectKind { Definition, ExecutionBinding };

    explicit PredicateAuthoringEditor(QWidget* parent = nullptr,
                                      ObjectKind kind = ObjectKind::Definition);
    void setStatusCallback(std::function<void(const QString&, StatusToast::Severity)> callback);
    void setSavedCallback(std::function<void()> callback);
    void setOpenGroupCallback(std::function<void(std::int64_t)> callback);
    void startDefinition();
    void startExecutionBinding(std::optional<std::int64_t> definitionRevisionId = std::nullopt);
    void loadDefinition(const savor::db::PredicateDefinitionRevisionV2Snapshot& snapshot, bool duplicate);
    void loadExecutionBinding(const savor::db::PredicateExecutionBindingRevisionSnapshot& snapshot, bool duplicate);

private:
    struct CatalogRefreshData {
        savorqt::db::ServiceResult<savor::runtime::predicates::PredicateAuthoringCatalogV2> catalog;
        savorqt::db::ServiceResult<savor::db::PredicateRevisionPageV2<savor::db::PredicateDefinitionRevisionV2Summary>> definitions;
        std::vector<savor::db::PredicateDefinitionRevisionV2Snapshot> definitionSnapshots;
    };
    struct BindingSourceWidgets {
        bool automatic = false;
        QGroupBox* card = nullptr;
        QLabel* explanation = nullptr;
        QLineEdit* value = nullptr;
    };

    void createWidgets();
    void configureRefresh();
    void requestRefresh();
    void applyCatalog(const CatalogRefreshData& data);
    void setMode(ObjectKind kind);
    void rebuildBindingCards();
    void saveDefinitionDraft(bool publish);
    void saveExecutionBindingDraft(bool publish);
    void duplicateCurrent();
    void abandonCurrentDraft();
    void createBindingFromCurrentDefinition();
    void postStatus(const QString& text, StatusToast::Severity severity);

    std::function<void(const QString&, StatusToast::Severity)> statusCallback_;
    std::function<void()> savedCallback_;
    std::function<void(std::int64_t)> openGroupCallback_;
    savor::runtime::predicates::PredicateAuthoringCatalogV2 catalog_;
    std::vector<savor::db::PredicateDefinitionRevisionV2Summary> publishedDefinitions_;
    std::vector<savor::db::PredicateDefinitionRevisionV2Snapshot> publishedDefinitionSnapshots_;
    std::optional<savor::db::PredicateDefinitionRevisionV2Snapshot> selectedDefinition_;
    std::optional<savor::db::PredicateDefinitionRevisionV2Snapshot> pendingDefinitionLoad_;
    std::optional<savor::db::PredicateExecutionBindingRevisionSnapshot> pendingBindingLoad_;
    bool pendingLoadDuplicate_ = false;
    std::optional<std::int64_t> requestedDefinitionRevisionId_;
    std::optional<savor::runtime::program::composition::PredicateDefinition> originalDefinitionBody_;
    std::optional<std::int64_t> loadedRevisionId_;
    std::optional<std::int64_t> loadedParentId_;
    std::optional<std::int64_t> duplicateSourceRevisionId_;
    ObjectKind mode_ = ObjectKind::Definition;
    QString loadedRevisionState_;
    QString creationRequestKey_;

    QLabel* objectHeading_ = nullptr;
    QStackedWidget* editorStack_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QPlainTextEdit* descriptionEdit_ = nullptr;
    GuidedPredicateRuleEditor* ruleEditor_ = nullptr;
    QPushButton* createBindingButton_ = nullptr;
    QComboBox* definitionCombo_ = nullptr;
    QWidget* bindingCardsHost_ = nullptr;
    QScrollArea* bindingScroll_ = nullptr;
    QVBoxLayout* bindingCardsLayout_ = nullptr;
    std::vector<BindingSourceWidgets> bindingCards_;
    QLabel* bindingSummaryLabel_ = nullptr;
    QLabel* validationLabel_ = nullptr;
    QPushButton* addToGroupButton_ = nullptr;
    QPushButton* saveButton_ = nullptr;
    QPushButton* publishButton_ = nullptr;
    QPushButton* duplicateButton_ = nullptr;
    QPushButton* abandonButton_ = nullptr;
    savorqt::gui::DatabaseProjectionController<int, CatalogRefreshData>* refreshPipeline_ = nullptr;
};

class PredicateGroupEditor final : public QWidget
{
public:
    explicit PredicateGroupEditor(QWidget* parent = nullptr);
    void setStatusCallback(std::function<void(const QString&, StatusToast::Severity)> callback);
    void setSavedCallback(std::function<void()> callback);
    void loadSnapshot(const savor::db::PredicateGroupRevisionSnapshot& snapshot, bool duplicate);
    void preselectBinding(std::int64_t bindingRevisionId);

private:
    struct RefreshData {
        savorqt::db::ServiceResult<savor::runtime::predicates::PredicateAuthoringCatalogV2> catalog;
        savorqt::db::ServiceResult<savor::db::PredicateRevisionPageV2<savor::db::PredicateExecutionBindingRevisionSummary>> bindings;
    };
    struct MemberDraft {
        std::int64_t bindingRevisionId = 0;
        std::vector<std::string> hooks;
        savor::runtime::predicates::PredicateOccurrencePolicyV1 occurrence = savor::runtime::predicates::PredicateOccurrencePolicyV1::First;
        std::optional<std::uint32_t> occurrenceOrdinal;
        std::optional<std::int64_t> guardBindingRevisionId;
        savor::runtime::program::composition::PredicateReaction reaction = savor::runtime::program::composition::PredicateReaction::RecordAndContinue;
        bool participatesInAggregation = true;
        bool emitEvidence = true;
    };

    void createWidgets();
    void configureRefresh();
    void requestRefresh();
    void applyRefresh(const RefreshData& data);
    void addMember(std::optional<std::int64_t> bindingRevisionId = std::nullopt);
    void removeMember(std::size_t index);
    void moveMember(std::size_t index, int delta);
    void rebuildMemberCards();
    void saveDraft(bool publish);
    void duplicateCurrent();
    void abandonCurrentDraft();
    void postStatus(const QString& text, StatusToast::Severity severity);
    [[nodiscard]] QString hookLabel(std::string_view hook) const;
    [[nodiscard]] bool guardsCompatible(std::int64_t memberBinding, std::int64_t guardBinding) const;

    std::function<void(const QString&, StatusToast::Severity)> statusCallback_;
    std::function<void()> savedCallback_;
    savor::runtime::predicates::PredicateAuthoringCatalogV2 catalog_;
    std::vector<savor::db::PredicateExecutionBindingRevisionSummary> publishedBindings_;
    std::vector<savor::db::PredicateExecutionBindingRevisionSnapshot> publishedBindingSnapshots_;
    std::vector<MemberDraft> members_;
    std::optional<std::int64_t> pendingPreselectedBinding_;
    std::optional<std::int64_t> loadedRevisionId_;
    std::optional<std::int64_t> loadedParentId_;
    std::optional<std::int64_t> duplicateSourceRevisionId_;
    QString loadedRevisionState_;
    QString creationRequestKey_;
    QLineEdit* nameEdit_ = nullptr;
    QPlainTextEdit* descriptionEdit_ = nullptr;
    QWidget* memberCardsHost_ = nullptr;
    QScrollArea* memberScroll_ = nullptr;
    QVBoxLayout* memberCardsLayout_ = nullptr;
    QLabel* validationLabel_ = nullptr;
    QPushButton* abandonButton_ = nullptr;
    savorqt::gui::DatabaseProjectionController<int, RefreshData>* refreshPipeline_ = nullptr;
};
