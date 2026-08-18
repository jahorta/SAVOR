#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "SavorDbRuntime.h"
#include "DB/SavorDbServiceResult.h"
#include "Authoring/IAuthoringDb.h"
#include "Common/Types/UtcTimestamp.h"

namespace savorqt::db {

struct SeedProbeSpecDraft {
    std::string name;
    int priority = 0;
    std::int64_t min_value = -128;
    std::int64_t max_value = 127;
    bool cap_trigger_top = false;
    bool ignore_trigger_minmax = false;
    int combo_attempts_per_target = 3;
    int combo_sampler_tries = 32;
};

struct TasSpecDraft {
    std::string base_name;
    int priority = 0;
    bool progress_enable = true;
    std::int64_t base_dtm_artifact_id = 0;
};

struct BattlePlanActionDraft {
    int actor_slot = 0;
    std::optional<std::int64_t> action_preset_id;
    savor::db::BattlePlanActionMacro macro = savor::db::BattlePlanActionMacro::Attack;
    savor::db::BattlePlanTargetKind target_kind = savor::db::BattlePlanTargetKind::SingleEnemy;
    std::optional<int> target_slot;
    std::optional<int> target_mask_bits;
    std::optional<int> target_single_slot;
    std::optional<int> target_same_as_actor_slot;
    std::optional<int> item_id;
    int ordinal = 0;
};

struct BattlePlanActionPresetDraft {
    std::string name;
    savor::db::BattlePlanActionMacro macro = savor::db::BattlePlanActionMacro::Attack;
    savor::db::BattlePlanTargetKind target_kind = savor::db::BattlePlanTargetKind::SingleEnemy;
    std::optional<int> target_mask_bits;
    std::optional<int> target_single_slot;
    std::optional<int> target_same_as_actor_slot;
    std::optional<int> item_id;
    int flags = 0;
};

struct BattlePlanTurnDraft {
    int turn_index = 0;
    std::optional<std::int64_t> predicate_group_revision_id;
    std::vector<BattlePlanActionDraft> actions;
};

struct BattlePlanDraft {
    std::string name;
    std::string fingerprint;
    std::vector<BattlePlanTurnDraft> turns;
};

struct WorkflowGraphDraft {
    std::optional<std::int64_t> workflow_graph_id;
    std::optional<std::int64_t> parent_revision_id;
    std::string name;
    std::string description;
    std::optional<bool> hidden;
    int graph_version = 1;
    std::string graph_hash;
    std::vector<savor::db::SaveWorkflowGraphNodeCommand> nodes;
    std::vector<savor::db::SaveWorkflowGraphEdgeCommand> edges;
};

class SavorDbAuthoringService {
public:
    static ServiceResult<savor::db::PredicateRevisionPageV2<savor::db::PredicateGroupRevisionSummary>>
    ListPredicateGroupRevisions(
        std::optional<std::string> revision_state = std::string("PUBLISHED"),
        std::string search_text = {},
        std::optional<std::int64_t> before_revision_id = std::nullopt,
        int limit = 100) {
        auto* db = AuthoringDb();
        if (db == nullptr) {
            return Unavailable<savor::db::PredicateRevisionPageV2<savor::db::PredicateGroupRevisionSummary>>(
                kSavorDbRuntimeUnavailableMessage);
        }
        return ServiceResult<savor::db::PredicateRevisionPageV2<savor::db::PredicateGroupRevisionSummary>>::Ok(
            db->ListPredicateGroupRevisions({
                .revision_state = std::move(revision_state),
                .search_text = std::move(search_text),
                .before_revision_id = before_revision_id,
                .limit = limit,
            }));
    }

    static ServiceResult<savor::db::PredicateGroupRevisionSnapshot>
    GetPredicateGroupRevision(std::int64_t revision_id) {
        auto* db = AuthoringDb();
        if (db == nullptr) return Unavailable<savor::db::PredicateGroupRevisionSnapshot>(kSavorDbRuntimeUnavailableMessage);
        const auto value = db->GetPredicateGroupRevision(revision_id);
        if (!value) return NotFound<savor::db::PredicateGroupRevisionSnapshot>("predicate group revision not found");
        return ServiceResult<savor::db::PredicateGroupRevisionSnapshot>::Ok(*value);
    }

    static ServiceResult<savor::runtime::predicates::PredicateAuthoringCatalogV2>
    GetPredicateAuthoringCatalog() {
        return ServiceResult<savor::runtime::predicates::PredicateAuthoringCatalogV2>::Ok(
            savor::runtime::predicates::BattlePredicateAuthoringCatalogV2());
    }

    static ServiceResult<savor::db::PredicateRevisionPageV2<savor::db::PredicateDefinitionRevisionV2Summary>>
    ListPredicateDefinitionRevisions(
        std::optional<std::string> revision_state = std::nullopt,
        std::string search_text = {}, int limit = 100) {
        auto* db = AuthoringDb();
        if (!db) return Unavailable<savor::db::PredicateRevisionPageV2<savor::db::PredicateDefinitionRevisionV2Summary>>(kSavorDbRuntimeUnavailableMessage);
        return ServiceResult<savor::db::PredicateRevisionPageV2<savor::db::PredicateDefinitionRevisionV2Summary>>::Ok(
            db->ListPredicateDefinitionRevisionsV2({
                .revision_state = std::move(revision_state),
                .search_text = std::move(search_text),
                .limit = limit,
            }));
    }

    static ServiceResult<savor::db::PredicateRevisionPageV2<savor::db::PredicateExecutionBindingRevisionSummary>>
    ListPredicateExecutionBindingRevisions(
        std::optional<std::string> revision_state = std::nullopt,
        std::string search_text = {}, int limit = 100) {
        auto* db = AuthoringDb();
        if (!db) return Unavailable<savor::db::PredicateRevisionPageV2<savor::db::PredicateExecutionBindingRevisionSummary>>(kSavorDbRuntimeUnavailableMessage);
        return ServiceResult<savor::db::PredicateRevisionPageV2<savor::db::PredicateExecutionBindingRevisionSummary>>::Ok(
            db->ListPredicateExecutionBindingRevisions({
                .revision_state = std::move(revision_state),
                .search_text = std::move(search_text),
                .limit = limit,
            }));
    }

    static ServiceResult<savor::db::PredicateDefinitionRevisionV2Snapshot>
    GetPredicateDefinitionRevision(std::int64_t revision_id) {
        auto* db = AuthoringDb();
        if (!db) return Unavailable<savor::db::PredicateDefinitionRevisionV2Snapshot>(kSavorDbRuntimeUnavailableMessage);
        const auto value = db->GetPredicateDefinitionRevisionV2(revision_id);
        if (!value) return NotFound<savor::db::PredicateDefinitionRevisionV2Snapshot>("predicate definition revision not found");
        return ServiceResult<savor::db::PredicateDefinitionRevisionV2Snapshot>::Ok(*value);
    }

    static ServiceResult<savor::db::PredicateExecutionBindingRevisionSnapshot>
    GetPredicateExecutionBindingRevision(std::int64_t revision_id) {
        auto* db = AuthoringDb();
        if (!db) return Unavailable<savor::db::PredicateExecutionBindingRevisionSnapshot>(kSavorDbRuntimeUnavailableMessage);
        const auto value = db->GetPredicateExecutionBindingRevision(revision_id);
        if (!value) return NotFound<savor::db::PredicateExecutionBindingRevisionSnapshot>("predicate execution binding revision not found");
        return ServiceResult<savor::db::PredicateExecutionBindingRevisionSnapshot>::Ok(*value);
    }

    static ServiceResult<savor::db::PredicateAuthoringRevisionReceipt> CreatePredicateDefinitionDraft(
        const savor::db::CreatePredicateDefinitionDraftCommand& command) {
        auto* db = AuthoringDb();
        if (!db) return Unavailable<savor::db::PredicateAuthoringRevisionReceipt>(kSavorDbRuntimeUnavailableMessage);
        savor::db::PredicateAuthoringRevisionReceipt receipt{}; std::string error;
        if (!db->CreatePredicateDefinitionDraft(command, &receipt, &error)) return Failed<savor::db::PredicateAuthoringRevisionReceipt>(error);
        return ServiceResult<savor::db::PredicateAuthoringRevisionReceipt>::Ok(receipt);
    }
    static ServiceResult<savor::db::PredicateAuthoringRevisionReceipt> SavePredicateDefinitionDraft(
        const savor::db::SavePredicateDefinitionDraftCommand& command) {
        auto* db = AuthoringDb(); if (!db) return Unavailable<savor::db::PredicateAuthoringRevisionReceipt>(kSavorDbRuntimeUnavailableMessage);
        savor::db::PredicateAuthoringRevisionReceipt receipt{}; std::string error;
        if (!db->SavePredicateDefinitionDraft(command, &receipt, &error)) return Failed<savor::db::PredicateAuthoringRevisionReceipt>(error);
        return ServiceResult<savor::db::PredicateAuthoringRevisionReceipt>::Ok(receipt);
    }
    static ServiceResult<savor::db::PredicateAuthoringRevisionReceipt> CreatePredicateExecutionBindingDraft(
        const savor::db::CreatePredicateExecutionBindingDraftCommand& command) {
        auto* db = AuthoringDb(); if (!db) return Unavailable<savor::db::PredicateAuthoringRevisionReceipt>(kSavorDbRuntimeUnavailableMessage);
        savor::db::PredicateAuthoringRevisionReceipt receipt{}; std::string error;
        if (!db->CreatePredicateExecutionBindingDraft(command, &receipt, &error)) return Failed<savor::db::PredicateAuthoringRevisionReceipt>(error);
        return ServiceResult<savor::db::PredicateAuthoringRevisionReceipt>::Ok(receipt);
    }
    static ServiceResult<savor::db::PredicateAuthoringRevisionReceipt> SavePredicateExecutionBindingDraft(
        const savor::db::SavePredicateExecutionBindingDraftCommand& command) {
        auto* db = AuthoringDb();
        if (!db) return Unavailable<savor::db::PredicateAuthoringRevisionReceipt>(kSavorDbRuntimeUnavailableMessage);
        savor::db::PredicateAuthoringRevisionReceipt receipt{}; std::string error;
        if (!db->SavePredicateExecutionBindingDraft(command, &receipt, &error)) return Failed<savor::db::PredicateAuthoringRevisionReceipt>(error);
        return ServiceResult<savor::db::PredicateAuthoringRevisionReceipt>::Ok(receipt);
    }
    static ServiceResult<savor::db::PredicateAuthoringRevisionReceipt> CreatePredicateGroupDraft(
        const savor::db::CreatePredicateGroupDraftCommand& command) {
        auto* db = AuthoringDb(); if (!db) return Unavailable<savor::db::PredicateAuthoringRevisionReceipt>(kSavorDbRuntimeUnavailableMessage);
        savor::db::PredicateAuthoringRevisionReceipt receipt{}; std::string error;
        if (!db->CreatePredicateGroupDraft(command, &receipt, &error)) return Failed<savor::db::PredicateAuthoringRevisionReceipt>(error);
        return ServiceResult<savor::db::PredicateAuthoringRevisionReceipt>::Ok(receipt);
    }
    static ServiceResult<savor::db::PredicateAuthoringRevisionReceipt> SavePredicateGroupDraft(
        const savor::db::SavePredicateGroupDraftCommand& command) {
        auto* db = AuthoringDb();
        if (!db) return Unavailable<savor::db::PredicateAuthoringRevisionReceipt>(kSavorDbRuntimeUnavailableMessage);
        savor::db::PredicateAuthoringRevisionReceipt receipt{}; std::string error;
        if (!db->SavePredicateGroupDraft(command, &receipt, &error)) return Failed<savor::db::PredicateAuthoringRevisionReceipt>(error);
        return ServiceResult<savor::db::PredicateAuthoringRevisionReceipt>::Ok(receipt);
    }
    static ServiceResult<savor::db::PredicateAuthoringRevisionReceipt> DuplicatePredicateDefinition(
        const savor::db::DuplicatePredicateDefinitionCommand& command) {
        auto* db = AuthoringDb(); if (!db) return Unavailable<savor::db::PredicateAuthoringRevisionReceipt>(kSavorDbRuntimeUnavailableMessage);
        savor::db::PredicateAuthoringRevisionReceipt receipt{}; std::string error;
        if (!db->DuplicatePredicateDefinition(command, &receipt, &error)) return Failed<savor::db::PredicateAuthoringRevisionReceipt>(error);
        return ServiceResult<savor::db::PredicateAuthoringRevisionReceipt>::Ok(receipt);
    }
    static ServiceResult<savor::db::PredicateAuthoringRevisionReceipt> DuplicatePredicateExecutionBinding(
        const savor::db::DuplicatePredicateExecutionBindingCommand& command) {
        auto* db = AuthoringDb(); if (!db) return Unavailable<savor::db::PredicateAuthoringRevisionReceipt>(kSavorDbRuntimeUnavailableMessage);
        savor::db::PredicateAuthoringRevisionReceipt receipt{}; std::string error;
        if (!db->DuplicatePredicateExecutionBinding(command, &receipt, &error)) return Failed<savor::db::PredicateAuthoringRevisionReceipt>(error);
        return ServiceResult<savor::db::PredicateAuthoringRevisionReceipt>::Ok(receipt);
    }
    static ServiceResult<savor::db::PredicateAuthoringRevisionReceipt> DuplicatePredicateGroup(
        const savor::db::DuplicatePredicateGroupCommand& command) {
        auto* db = AuthoringDb(); if (!db) return Unavailable<savor::db::PredicateAuthoringRevisionReceipt>(kSavorDbRuntimeUnavailableMessage);
        savor::db::PredicateAuthoringRevisionReceipt receipt{}; std::string error;
        if (!db->DuplicatePredicateGroup(command, &receipt, &error)) return Failed<savor::db::PredicateAuthoringRevisionReceipt>(error);
        return ServiceResult<savor::db::PredicateAuthoringRevisionReceipt>::Ok(receipt);
    }
    static ServiceResult<bool> UpdatePredicateMetadata(
        const savor::db::UpdatePredicateAuthoringMetadataCommand& command) {
        auto* db = AuthoringDb(); if (!db) return Unavailable<bool>(kSavorDbRuntimeUnavailableMessage);
        bool changed = false; std::string error;
        if (!db->UpdatePredicateAuthoringMetadata(command, &changed, &error)) return Failed<bool>(error);
        return ServiceResult<bool>::Ok(changed);
    }

    static ServiceResult<void> PublishPredicateDefinition(std::int64_t id) {
        auto* db = AuthoringDb(); if (!db) return ServiceResult<void>::Err({ServiceErrorKind::Unavailable, kSavorDbRuntimeUnavailableMessage});
        std::string error; if (!db->PublishPredicateDefinitionRevisionV2(id, savor::db::types::UtcNow(), nullptr, &error)) return ServiceResult<void>::Err({ServiceErrorKind::Failed, error});
        return ServiceResult<void>::Ok();
    }

    static ServiceResult<void> PublishPredicateExecutionBinding(std::int64_t id) {
        auto* db = AuthoringDb(); if (!db) return ServiceResult<void>::Err({ServiceErrorKind::Unavailable, kSavorDbRuntimeUnavailableMessage});
        std::string error; if (!db->PublishPredicateExecutionBindingRevision(id, savor::db::types::UtcNow(), nullptr, &error)) return ServiceResult<void>::Err({ServiceErrorKind::Failed, error});
        return ServiceResult<void>::Ok();
    }

    static ServiceResult<void> PublishPredicateGroup(std::int64_t id) {
        auto* db = AuthoringDb(); if (!db) return ServiceResult<void>::Err({ServiceErrorKind::Unavailable, kSavorDbRuntimeUnavailableMessage});
        std::string error; if (!db->PublishPredicateGroupRevision(id, savor::db::types::UtcNow(), nullptr, &error)) return ServiceResult<void>::Err({ServiceErrorKind::Failed, error});
        return ServiceResult<void>::Ok();
    }

    static ServiceResult<void> AbandonPredicateDefinitionDraft(std::int64_t id) {
        auto* db = AuthoringDb(); if (!db) return ServiceResult<void>::Err({ServiceErrorKind::Unavailable, kSavorDbRuntimeUnavailableMessage});
        std::string error; if (!db->AbandonPredicateDefinitionDraftV2(id, &error)) return ServiceResult<void>::Err({ServiceErrorKind::Failed, error});
        return ServiceResult<void>::Ok();
    }

    static ServiceResult<void> AbandonPredicateExecutionBindingDraft(std::int64_t id) {
        auto* db = AuthoringDb(); if (!db) return ServiceResult<void>::Err({ServiceErrorKind::Unavailable, kSavorDbRuntimeUnavailableMessage});
        std::string error; if (!db->AbandonPredicateExecutionBindingDraft(id, &error)) return ServiceResult<void>::Err({ServiceErrorKind::Failed, error});
        return ServiceResult<void>::Ok();
    }

    static ServiceResult<void> AbandonPredicateGroupDraft(std::int64_t id) {
        auto* db = AuthoringDb(); if (!db) return ServiceResult<void>::Err({ServiceErrorKind::Unavailable, kSavorDbRuntimeUnavailableMessage});
        std::string error; if (!db->AbandonPredicateGroupDraft(id, &error)) return ServiceResult<void>::Err({ServiceErrorKind::Failed, error});
        return ServiceResult<void>::Ok();
    }

    static ServiceResult<std::int64_t> SaveSeedProbeSpec(const SeedProbeSpecDraft& draft) {
        auto* db = AuthoringDb();
        if (db == nullptr) {
            return Unavailable<std::int64_t>(kSavorDbRuntimeUnavailableMessage);
        }
        if (draft.name.empty()) {
            return Invalid<std::int64_t>("seed probe spec name is required");
        }
        const auto now = savor::db::types::UtcNow();
        savor::db::SaveSeedProbeSpecCommand command{};
        command.name = draft.name;
        command.priority = draft.priority;
        command.min_value = draft.min_value;
        command.max_value = draft.max_value;
        command.cap_trigger_top = draft.cap_trigger_top;
        command.ignore_trigger_minmax = draft.ignore_trigger_minmax;
        command.combo_attempts_per_target = draft.combo_attempts_per_target;
        command.combo_sampler_tries = draft.combo_sampler_tries;
        command.created_at_utc = now;
        command.correlation_id = NextEventId("Authoring.SeedProbeSpecSaved");

        std::int64_t id = 0;
        std::string error;
        if (!db->SaveSeedProbeSpec(command, &id, &error)) {
            return Failed<std::int64_t>(error);
        }
        return ServiceResult<std::int64_t>::Ok(id);
    }

    static ServiceResult<savor::db::SeedProbeSpecSnapshot> GetSeedProbeSpec(std::int64_t seed_probe_spec_id) {
        auto* db = AuthoringDb();
        if (db == nullptr) {
            return Unavailable<savor::db::SeedProbeSpecSnapshot>(kSavorDbRuntimeUnavailableMessage);
        }
        const auto snapshot = db->GetSeedProbeSpec(seed_probe_spec_id);
        if (!snapshot.has_value()) {
            return NotFound<savor::db::SeedProbeSpecSnapshot>("seed probe spec not found");
        }
        return ServiceResult<savor::db::SeedProbeSpecSnapshot>::Ok(*snapshot);
    }

    static ServiceResult<std::vector<savor::db::SeedProbeSpecSnapshot>> ListSeedProbeSpecs(int max_count = 100) {
        auto* db = AuthoringDb();
        if (db == nullptr) {
            return Unavailable<std::vector<savor::db::SeedProbeSpecSnapshot>>(kSavorDbRuntimeUnavailableMessage);
        }
        return ServiceResult<std::vector<savor::db::SeedProbeSpecSnapshot>>::Ok(db->ListSeedProbeSpecs(max_count));
    }

    static ServiceResult<std::int64_t> SaveTasSpec(const TasSpecDraft& draft) {
        auto* db = AuthoringDb();
        if (db == nullptr) {
            return Unavailable<std::int64_t>(kSavorDbRuntimeUnavailableMessage);
        }
        if (draft.base_name.empty()) {
            return Invalid<std::int64_t>("TAS spec name is required");
        }

        const auto now = savor::db::types::UtcNow();
        savor::db::SaveTasSpecCommand command{};
        command.base_name = draft.base_name;
        command.priority = draft.priority;
        command.progress_enable = draft.progress_enable;
        command.base_dtm_artifact_id = draft.base_dtm_artifact_id;
        command.created_at_utc = now;
        command.correlation_id = NextEventId("Authoring.TasSpecSaved");

        std::int64_t id = 0;
        std::int64_t base_id = 0;
        std::string error;
        if (!db->SaveTasSpec(command, &id, &base_id, &error)) {
            return Failed<std::int64_t>(error);
        }
        return ServiceResult<std::int64_t>::Ok(id);
    }

    static ServiceResult<savor::db::TasSpecSnapshot> GetTasSpec(std::int64_t tas_spec_id) {
        auto* db = AuthoringDb();
        if (db == nullptr) {
            return Unavailable<savor::db::TasSpecSnapshot>(kSavorDbRuntimeUnavailableMessage);
        }
        const auto snapshot = db->GetTasSpec(tas_spec_id);
        if (!snapshot.has_value()) {
            return NotFound<savor::db::TasSpecSnapshot>("TAS spec not found");
        }
        return ServiceResult<savor::db::TasSpecSnapshot>::Ok(*snapshot);
    }

    static ServiceResult<std::vector<savor::db::TasSpecSnapshot>> ListTasSpecs(int max_count = 100) {
        auto* db = AuthoringDb();
        if (db == nullptr) {
            return Unavailable<std::vector<savor::db::TasSpecSnapshot>>(kSavorDbRuntimeUnavailableMessage);
        }
        return ServiceResult<std::vector<savor::db::TasSpecSnapshot>>::Ok(db->ListTasSpecs(max_count));
    }

    static ServiceResult<std::int64_t> SaveBattlePlan(const BattlePlanDraft& draft) {
        auto* db = AuthoringDb();
        if (db == nullptr) {
            return Unavailable<std::int64_t>(kSavorDbRuntimeUnavailableMessage);
        }
        if (draft.name.empty() || draft.fingerprint.empty()) {
            return Invalid<std::int64_t>("battle plan name and fingerprint are required");
        }

        const auto now = savor::db::types::UtcNow();
        savor::db::SavePlanCommand plan{};
        plan.name = draft.name;
        plan.fingerprint = draft.fingerprint;
        plan.created_at_utc = now;
        plan.correlation_id = NextEventId("Authoring.BattlePlanSaved");

        std::int64_t plan_id = 0;
        std::string error;
        if (!db->SavePlan(plan, &plan_id, &error)) {
            return Failed<std::int64_t>(error);
        }

        for (const auto& turn : draft.turns) {
            savor::db::SaveBattlePlanTurnCommand turn_command{};
            turn_command.plan_id = plan_id;
            turn_command.turn_index = turn.turn_index;
            turn_command.default_predicate_group_revision_id =
                turn.predicate_group_revision_id;
            turn_command.created_at_utc = now;
            turn_command.correlation_id = plan.correlation_id;
            for (const auto& action : turn.actions) {
                if (action.action_preset_id.has_value() && action.action_preset_id.value() > 0) {
                    turn_command.actions.push_back(savor::db::SaveBattlePlanActionCommand{
                        .actor_slot = action.actor_slot,
                        .action_preset_id = action.action_preset_id.value(),
                        .ordinal = action.ordinal,
                    });
                } else {
                    savor::db::SaveBattlePlanActionPresetCommand preset_command{};
                    preset_command.name = BuildGeneratedActionPresetName(draft.name, turn.turn_index, action.ordinal);
                    preset_command.macro = action.macro;
                    preset_command.target_kind = action.target_kind;
                    preset_command.target_mask_bits = action.target_mask_bits;
                    preset_command.target_single_slot = action.target_single_slot.has_value()
                        ? action.target_single_slot
                        : action.target_slot;
                    preset_command.target_same_as_actor_slot = action.target_same_as_actor_slot;
                    preset_command.item_id = action.item_id;
                    preset_command.created_at_utc = now;
                    preset_command.correlation_id = plan.correlation_id;

                    std::int64_t action_preset_id = 0;
                    if (!db->SaveBattlePlanActionPreset(preset_command, &action_preset_id, &error)) {
                        return Failed<std::int64_t>(error);
                    }

                    turn_command.actions.push_back(savor::db::SaveBattlePlanActionCommand{
                        .actor_slot = action.actor_slot,
                        .action_preset_id = action_preset_id,
                        .ordinal = action.ordinal,
                    });
                }

            }

            std::int64_t turn_id = 0;
            if (!db->SaveBattlePlanTurn(turn_command, &turn_id, &error)) {
                return Failed<std::int64_t>(error);
            }
        }

        return ServiceResult<std::int64_t>::Ok(plan_id);
    }

    static ServiceResult<std::int64_t> SaveBattlePlanActionPreset(const BattlePlanActionPresetDraft& draft) {
        auto* db = AuthoringDb();
        if (db == nullptr) {
            return Unavailable<std::int64_t>(kSavorDbRuntimeUnavailableMessage);
        }
        if (draft.name.empty()) {
            return Invalid<std::int64_t>("battle plan action preset name is required");
        }

        const auto now = savor::db::types::UtcNow();
        savor::db::SaveBattlePlanActionPresetCommand command{};
        command.name = draft.name;
        command.macro = draft.macro;
        command.target_kind = draft.target_kind;
        command.target_mask_bits = draft.target_mask_bits;
        command.target_single_slot = draft.target_single_slot;
        command.target_same_as_actor_slot = draft.target_same_as_actor_slot;
        command.item_id = draft.item_id;
        command.flags = draft.flags;
        command.created_at_utc = now;
        command.correlation_id = NextEventId("Authoring.BattlePlanActionPresetSaved");

        std::int64_t id = 0;
        std::string error;
        if (!db->SaveBattlePlanActionPreset(command, &id, &error)) {
            return Failed<std::int64_t>(error);
        }
        return ServiceResult<std::int64_t>::Ok(id);
    }

    static ServiceResult<void> RenameBattlePlanActionPreset(std::int64_t action_preset_id, const std::string& name) {
        auto* db = AuthoringDb();
        if (db == nullptr) {
            return ServiceResult<void>::Err({ ServiceErrorKind::Unavailable, kSavorDbRuntimeUnavailableMessage });
        }
        if (action_preset_id <= 0) {
            return ServiceResult<void>::Err({ ServiceErrorKind::InvalidInput, "battle plan action preset id is required" });
        }
        if (name.empty()) {
            return ServiceResult<void>::Err({ ServiceErrorKind::InvalidInput, "battle plan action preset name is required" });
        }

        savor::db::RenameBattlePlanActionPresetCommand command{};
        command.action_preset_id = action_preset_id;
        command.name = name;
        command.updated_at_utc = savor::db::types::UtcNow();
        command.correlation_id = NextEventId("Authoring.BattlePlanActionPresetRenamed");

        std::string error;
        if (!db->RenameBattlePlanActionPreset(command, &error)) {
            return ServiceResult<void>::Err({ ServiceErrorKind::Failed, std::move(error) });
        }
        return ServiceResult<void>::Ok();
    }

    static ServiceResult<savor::db::BattlePlanActionPresetSnapshot> GetBattlePlanActionPreset(std::int64_t action_preset_id) {
        auto* db = AuthoringDb();
        if (db == nullptr) {
            return Unavailable<savor::db::BattlePlanActionPresetSnapshot>(kSavorDbRuntimeUnavailableMessage);
        }
        const auto snapshot = db->GetBattlePlanActionPreset(action_preset_id);
        if (!snapshot.has_value()) {
            return NotFound<savor::db::BattlePlanActionPresetSnapshot>("battle plan action preset not found");
        }
        return ServiceResult<savor::db::BattlePlanActionPresetSnapshot>::Ok(*snapshot);
    }

    static ServiceResult<std::vector<savor::db::BattlePlanActionPresetSnapshot>> ListBattlePlanActionPresets(int max_count = 100) {
        auto* db = AuthoringDb();
        if (db == nullptr) {
            return Unavailable<std::vector<savor::db::BattlePlanActionPresetSnapshot>>(kSavorDbRuntimeUnavailableMessage);
        }
        return ServiceResult<std::vector<savor::db::BattlePlanActionPresetSnapshot>>::Ok(db->ListBattlePlanActionPresets(max_count));
    }

    static ServiceResult<savor::db::BattlePlanSnapshot> GetBattlePlan(std::int64_t plan_id) {
        auto* db = AuthoringDb();
        if (db == nullptr) {
            return Unavailable<savor::db::BattlePlanSnapshot>(kSavorDbRuntimeUnavailableMessage);
        }
        const auto snapshot = db->GetBattlePlan(plan_id);
        if (!snapshot.has_value()) {
            return NotFound<savor::db::BattlePlanSnapshot>("battle plan not found");
        }
        return ServiceResult<savor::db::BattlePlanSnapshot>::Ok(*snapshot);
    }

    static ServiceResult<std::vector<savor::db::BattlePlanSnapshot>> ListBattlePlans(int max_count = 100) {
        auto* db = AuthoringDb();
        if (db == nullptr) {
            return Unavailable<std::vector<savor::db::BattlePlanSnapshot>>(kSavorDbRuntimeUnavailableMessage);
        }
        return ServiceResult<std::vector<savor::db::BattlePlanSnapshot>>::Ok(db->ListBattlePlans(max_count));
    }

    static ServiceResult<savor::db::SaveWorkflowGraphResult> SaveWorkflowGraph(const WorkflowGraphDraft& draft) {
        auto* db = AuthoringDb();
        if (db == nullptr) {
            return Unavailable<savor::db::SaveWorkflowGraphResult>(kSavorDbRuntimeUnavailableMessage);
        }
        if (draft.name.empty() || draft.graph_hash.empty()) {
            return Invalid<savor::db::SaveWorkflowGraphResult>("workflow graph name and hash are required");
        }
        if (draft.nodes.empty()) {
            return Invalid<savor::db::SaveWorkflowGraphResult>("workflow graph must contain at least one node");
        }

        const auto now = savor::db::types::UtcNow();
        savor::db::SaveWorkflowGraphCommand command{};
        command.workflow_graph_id = draft.workflow_graph_id;
        command.parent_revision_id = draft.parent_revision_id;
        command.name = draft.name;
        command.description = draft.description;
        command.hidden = draft.hidden;
        command.graph_version = draft.graph_version;
        command.graph_hash = draft.graph_hash;
        command.nodes = draft.nodes;
        command.edges = draft.edges;
        command.created_at_utc = now;
        command.correlation_id = NextEventId("Authoring.WorkflowGraphSaved");

        savor::db::SaveWorkflowGraphResult result{};
        std::string error;
        if (!db->SaveWorkflowGraph(command, &result, &error)) {
            return Failed<savor::db::SaveWorkflowGraphResult>(error);
        }
        return ServiceResult<savor::db::SaveWorkflowGraphResult>::Ok(result);
    }

    static ServiceResult<savor::db::WorkflowGraphSnapshot> GetWorkflowGraph(std::int64_t workflow_graph_id) {
        auto* db = AuthoringDb();
        if (db == nullptr) {
            return Unavailable<savor::db::WorkflowGraphSnapshot>(kSavorDbRuntimeUnavailableMessage);
        }
        const auto snapshot = db->GetWorkflowGraph(workflow_graph_id);
        if (!snapshot.has_value()) {
            return NotFound<savor::db::WorkflowGraphSnapshot>("workflow graph not found");
        }
        return ServiceResult<savor::db::WorkflowGraphSnapshot>::Ok(*snapshot);
    }

    static ServiceResult<savor::db::WorkflowGraphSnapshot> GetWorkflowGraphRevision(std::int64_t workflow_graph_revision_id) {
        auto* db = AuthoringDb();
        if (db == nullptr) {
            return Unavailable<savor::db::WorkflowGraphSnapshot>(kSavorDbRuntimeUnavailableMessage);
        }
        const auto snapshot = db->GetWorkflowGraphRevision(workflow_graph_revision_id);
        if (!snapshot.has_value()) {
            return NotFound<savor::db::WorkflowGraphSnapshot>("workflow graph revision not found");
        }
        return ServiceResult<savor::db::WorkflowGraphSnapshot>::Ok(*snapshot);
    }

    static ServiceResult<void> SetWorkflowGraphHidden(std::int64_t workflow_graph_id, bool hidden) {
        auto* db = AuthoringDb();
        if (db == nullptr) {
            return ServiceResult<void>::Err({ ServiceErrorKind::Unavailable, kSavorDbRuntimeUnavailableMessage });
        }
        if (workflow_graph_id <= 0) {
            return ServiceResult<void>::Err({ ServiceErrorKind::InvalidInput, "workflow graph id is required" });
        }

        std::string error;
        if (!db->SetWorkflowGraphHidden(workflow_graph_id, hidden, &error)) {
            return ServiceResult<void>::Err({ ServiceErrorKind::Failed, std::move(error) });
        }
        return ServiceResult<void>::Ok();
    }

    static ServiceResult<std::vector<savor::db::WorkflowGraphSnapshot>> ListWorkflowGraphs(int max_count = 100, bool include_hidden = false) {
        auto* db = AuthoringDb();
        if (db == nullptr) {
            return Unavailable<std::vector<savor::db::WorkflowGraphSnapshot>>(kSavorDbRuntimeUnavailableMessage);
        }
        return ServiceResult<std::vector<savor::db::WorkflowGraphSnapshot>>::Ok(db->ListWorkflowGraphs(max_count, include_hidden));
    }

private:
    static savor::db::IAuthoringDb* AuthoringDb() {
        return savorqt::SavorDbRuntime::instance().authoringDb();
    }

    static std::string NextEventId(const char* prefix) {
        static std::atomic<std::uint64_t> counter{ 1 };
        const auto now = savor::db::types::UtcNow().time_since_epoch().count();
        return std::string(prefix) + "." + std::to_string(now) + "." + std::to_string(counter.fetch_add(1));
    }

    static std::string BuildGeneratedActionPresetName(const std::string& plan_name, int turn_index, int ordinal) {
        static std::atomic<std::uint64_t> counter{ 1 };
        const auto now = savor::db::types::UtcNow().time_since_epoch().count();
        return plan_name
            + ".turn" + std::to_string(turn_index)
            + ".action" + std::to_string(ordinal)
            + "." + std::to_string(now)
            + "." + std::to_string(counter.fetch_add(1));
    }

    template <typename T>
    static ServiceResult<T> Unavailable(std::string message) {
        return ServiceResult<T>::Err({ ServiceErrorKind::Unavailable, std::move(message) });
    }

    template <typename T>
    static ServiceResult<T> NotFound(std::string message) {
        return ServiceResult<T>::Err({ ServiceErrorKind::NotFound, std::move(message) });
    }

    template <typename T>
    static ServiceResult<T> Invalid(std::string message) {
        return ServiceResult<T>::Err({ ServiceErrorKind::InvalidInput, std::move(message) });
    }

    template <typename T>
    static ServiceResult<T> Failed(std::string message) {
        return ServiceResult<T>::Err({ ServiceErrorKind::Failed, std::move(message) });
    }
};

} // namespace savorqt::db

