#pragma once

#include "AuthoringRecipe.h"

namespace savor::db::authoring {

class AuthoringRecipeMaterializer final {
public:
    explicit AuthoringRecipeMaterializer(IAuthoringDb* db) : db_(db) {}

    bool Apply(
        const AuthoringRecipe& recipe,
        AuthoringRecipeResult* result_out,
        std::string* error_out = nullptr) const;

    bool SaveSeedProbeSpec(
        const SeedProbeSpecDefinition& definition,
        std::int64_t* id_out,
        std::string* error_out = nullptr) const;
    bool SaveWorkflowGraph(
        const WorkflowGraphDefinition& definition,
        const AuthoringRecipeResult& resolved,
        SaveWorkflowGraphResult* result_out,
        std::string* error_out = nullptr) const;

private:
    IAuthoringDb* db_ = nullptr;
};

bool MaterializeWorkflowGraph(
    IAuthoringDb* db,
    const SaveWorkflowGraphCommand& draft,
    SaveWorkflowGraphResult* result_out,
    std::string* error_out = nullptr);

bool MaterializePredicateDefinitionDraft(
    IAuthoringDb* db, const CreatePredicateDefinitionDraftCommand& draft,
    PredicateAuthoringRevisionReceipt* receipt_out,
    std::string* error_out = nullptr);
bool MaterializePredicateExecutionBindingDraft(
    IAuthoringDb* db, const CreatePredicateExecutionBindingDraftCommand& draft,
    PredicateAuthoringRevisionReceipt* receipt_out,
    std::string* error_out = nullptr);
bool MaterializePredicateGroupDraft(
    IAuthoringDb* db, const CreatePredicateGroupDraftCommand& draft,
    PredicateAuthoringRevisionReceipt* receipt_out,
    std::string* error_out = nullptr);
bool MaterializeBattlePlanHeader(
    IAuthoringDb* db, const SavePlanCommand& draft,
    std::int64_t* id_out, std::string* error_out = nullptr);
bool MaterializeBattleActionPreset(
    IAuthoringDb* db, const SaveBattlePlanActionPresetCommand& draft,
    std::int64_t* id_out, std::string* error_out = nullptr);
bool MaterializeBattlePlanTurn(
    IAuthoringDb* db, const SaveBattlePlanTurnCommand& draft,
    std::int64_t* id_out, std::string* error_out = nullptr);

} // namespace savor::db::authoring
