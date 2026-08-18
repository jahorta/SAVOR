#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../Common/Events/EventEnvelope.h"
#include "../Common/Types/UtcTimestamp.h"
#include "../Common/Retention/OutboxRetention.h"
#include "../../SavorCore/Core/Input/SoaBattle/ActionTypes.h"
#include "../../SavorCore/Runner/Runtime/Predicates/PredicateExecution.h"

namespace savor::db {

inline constexpr std::string_view kWorkflowOutputPresentGuard =
    "output_present";

enum class BattlePlanTargetKind : int {
    SingleEnemy = 0,
    MultipleEnemies = 1,
    AnyEnemy = 2,
    SameAsOtherPC = 3,
};

using BattlePlanActionMacro = soa::battle::actions::BattleAction;

struct AuthoringPayloadRecord {
    std::int64_t seed_probe_spec_id = 0;
    std::int64_t tas_spec_id = 0;
    std::int64_t battle_plan_action_preset_id = 0;
    std::int64_t workflow_graph_id = 0;
    std::int64_t workflow_graph_revision_id = 0;
};

struct SaveSeedProbeSpecCommand {
    std::string name;
    int priority = 0;
    std::int64_t min_value = 0;
    std::int64_t max_value = 0;
    bool cap_trigger_top = false;
    bool ignore_trigger_minmax = false;
    int combo_attempts_per_target = 0;
    int combo_sampler_tries = 0;
    bool auto_schedule_battle_run = false;
    types::UtcTimePoint created_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct SeedProbeSpecSnapshot {
    std::int64_t seed_probe_spec_id = 0;
    std::string name;
    int priority = 0;
    std::int64_t min_value = 0;
    std::int64_t max_value = 0;
    bool cap_trigger_top = false;
    bool ignore_trigger_minmax = false;
    int combo_attempts_per_target = 0;
    int combo_sampler_tries = 0;
    bool auto_schedule_battle_run = false;
};

struct AuthoringInputSetFrameCommand {
    std::int32_t main_x = 0;
    std::int32_t main_y = 0;
    std::int32_t cstick_x = 0;
    std::int32_t cstick_y = 0;
    std::int32_t trigger_x = 0;
    std::int32_t trigger_y = 0;
};

struct EnsureAuthoringInputSetCommand {
    std::string name;
    std::vector<AuthoringInputSetFrameCommand> frames;
    types::UtcTimePoint created_at_utc{};
};

struct AuthoringInputSetFrameSnapshot {
    int ordinal = 0;
    std::int32_t main_x = 0;
    std::int32_t main_y = 0;
    std::int32_t cstick_x = 0;
    std::int32_t cstick_y = 0;
    std::int32_t trigger_x = 0;
    std::int32_t trigger_y = 0;
};

struct SaveTasSpecCommand {
    std::string base_name;
    int priority = 0;
    bool progress_enable = true;
    bool auto_queue_seeds = false;
    std::int64_t base_dtm_artifact_id = 0;
    types::UtcTimePoint created_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct TasSpecSnapshot {
    std::int64_t tas_spec_id = 0;
    std::int64_t tas_spec_base_id = 0;
    std::string base_name;
    int priority = 0;
    bool progress_enable = true;
    bool auto_queue_seeds = false;
    std::int64_t base_dtm_artifact_id = 0;
};

struct SavePlanCommand {
    std::string name;
    std::string fingerprint;
    types::UtcTimePoint created_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct SaveBattlePlanActionPresetCommand {
    std::string name;
    BattlePlanActionMacro macro = BattlePlanActionMacro::Attack;
    BattlePlanTargetKind target_kind = BattlePlanTargetKind::SingleEnemy;
    std::optional<int> target_mask_bits;
    std::optional<int> target_single_slot;
    std::optional<int> target_same_as_actor_slot;
    std::optional<int> item_id;
    int flags = 0;
    types::UtcTimePoint created_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct RenameBattlePlanActionPresetCommand {
    std::int64_t action_preset_id = 0;
    std::string name;
    types::UtcTimePoint updated_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct SaveBattlePlanActionCommand {
    int actor_slot = 0;
    std::int64_t action_preset_id = 0;
    int ordinal = 0;
};

struct SaveBattlePlanTurnCommand {
    std::int64_t plan_id = 0;
    int turn_index = 0;
    std::optional<std::int64_t> default_predicate_group_revision_id;
    std::vector<SaveBattlePlanActionCommand> actions;
    bool replace_existing_actions = true;
    types::UtcTimePoint created_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct SaveWorkflowGraphNodeInputCommand {
    std::string input_key;
    std::string data_kind;
    std::string ref_kind;
    std::string display_name;
    bool required = true;
};

struct SaveWorkflowGraphNodeOutputCommand {
    std::string output_key;
    std::string data_kind;
    std::string ref_kind;
    std::string display_name;
};

struct SaveWorkflowGraphNodeArgumentCommand {
    std::string argument_key;
    std::string display_name;
    std::string value_type;
    bool required = false;
    std::optional<std::string> default_value;
    std::optional<std::int64_t> minimum_integer;
    std::optional<std::uint64_t> maximum_integer;
    struct Choice {
        std::string value;
        std::string display_name;
    };
    std::vector<Choice> choices;
};

struct SaveWorkflowGraphNodeArgumentConstraintCommand {
    std::string lesser_or_equal_key;
    std::string greater_or_equal_key;
    std::string message;
};

struct SaveWorkflowGraphNodeCommand {
    std::string node_key;
    std::string unit_kind;
    std::string display_name;
    std::optional<std::string> authored_ref_kind;
    std::optional<std::int64_t> authored_ref_id;
    std::vector<SaveWorkflowGraphNodeInputCommand> inputs;
    std::vector<SaveWorkflowGraphNodeOutputCommand> possible_outputs;
    std::vector<SaveWorkflowGraphNodeArgumentCommand> arguments;
    std::vector<SaveWorkflowGraphNodeArgumentConstraintCommand> argument_constraints;
};

struct SaveWorkflowGraphEdgeCommand {
    std::string from_node_key;
    std::string output_key;
    std::string to_node_key;
    std::string input_key;
    std::optional<std::string> guard_kind;
    std::optional<std::string> guard_value;
};

struct SaveWorkflowGraphCommand {
    std::optional<std::int64_t> workflow_graph_id;
    std::optional<std::int64_t> parent_revision_id;
    std::string name;
    std::string description;
    std::optional<bool> hidden;
    int graph_version = 1;
    std::string graph_hash;
    bool make_active = true;
    std::vector<SaveWorkflowGraphNodeCommand> nodes;
    std::vector<SaveWorkflowGraphEdgeCommand> edges;
    types::UtcTimePoint created_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct SaveWorkflowGraphResult {
    std::int64_t workflow_graph_id = 0;
    std::int64_t workflow_graph_revision_id = 0;
};

struct BattlePlanActionPresetSnapshot {
    std::int64_t action_preset_id = 0;
    std::string name;
    BattlePlanActionMacro macro = BattlePlanActionMacro::Attack;
    BattlePlanTargetKind target_kind = BattlePlanTargetKind::SingleEnemy;
    std::optional<int> target_mask_bits;
    std::optional<int> target_single_slot;
    std::optional<int> target_same_as_actor_slot;
    std::optional<int> item_id;
    int flags = 0;
    types::UtcTimePoint created_at_utc{};
    std::optional<types::UtcTimePoint> updated_at_utc;
};

struct BattlePlanActionSnapshot {
    std::int64_t plan_action_id = 0;
    std::int64_t plan_turn_id = 0;
    int actor_slot = 0;
    std::int64_t action_preset_id = 0;
    BattlePlanActionPresetSnapshot action_preset;
    int ordinal = 0;
};

struct BattlePlanTurnSnapshot {
    std::int64_t plan_turn_id = 0;
    std::int64_t plan_id = 0;
    int turn_index = 0;
    std::optional<std::int64_t> default_predicate_group_revision_id;
    std::vector<BattlePlanActionSnapshot> actions;
};

struct BattlePlanSnapshot {
    std::int64_t plan_id = 0;
    std::string name;
    std::string fingerprint;
    std::vector<BattlePlanTurnSnapshot> turns;
};

struct WorkflowGraphNodeInputSnapshot {
    std::string input_key;
    std::string data_kind;
    std::string ref_kind;
    std::string display_name;
    bool required = true;
};

struct WorkflowGraphNodeOutputSnapshot {
    std::string output_key;
    std::string data_kind;
    std::string ref_kind;
    std::string display_name;
};

struct WorkflowGraphNodeArgumentSnapshot {
    std::string argument_key;
    std::string display_name;
    std::string value_type;
    bool required = false;
    std::optional<std::string> default_value;
    std::optional<std::int64_t> minimum_integer;
    std::optional<std::uint64_t> maximum_integer;
    struct Choice {
        std::string value;
        std::string display_name;
    };
    std::vector<Choice> choices;
};

struct WorkflowGraphNodeArgumentConstraintSnapshot {
    std::string lesser_or_equal_key;
    std::string greater_or_equal_key;
    std::string message;
};

struct WorkflowGraphNodeSnapshot {
    std::int64_t workflow_graph_revision_node_id = 0;
    std::string node_key;
    std::string unit_kind;
    std::string display_name;
    std::optional<std::string> authored_ref_kind;
    std::optional<std::int64_t> authored_ref_id;
    std::vector<WorkflowGraphNodeInputSnapshot> inputs;
    std::vector<WorkflowGraphNodeOutputSnapshot> possible_outputs;
    std::vector<WorkflowGraphNodeArgumentSnapshot> arguments;
    std::vector<WorkflowGraphNodeArgumentConstraintSnapshot> argument_constraints;
};

struct WorkflowGraphEdgeSnapshot {
    std::int64_t workflow_graph_revision_edge_id = 0;
    std::string from_node_key;
    std::string output_key;
    std::string to_node_key;
    std::string input_key;
    std::optional<std::string> guard_kind;
    std::optional<std::string> guard_value;
};

struct WorkflowGraphSnapshot {
    std::int64_t workflow_graph_id = 0;
    std::int64_t workflow_graph_revision_id = 0;
    std::optional<std::int64_t> parent_revision_id;
    std::string name;
    std::string description;
    bool hidden = false;
    int graph_version = 1;
    std::string graph_hash;
    std::string status;
    std::vector<WorkflowGraphNodeSnapshot> nodes;
    std::vector<WorkflowGraphEdgeSnapshot> edges;
};

// Clean predicate-v2 authoring surface. These records intentionally expose
// the same DB-independent resolved model that coordination places in a
// workset. Draft writes are relational; published snapshots are immutable.
struct PredicateAuthoringRevisionReceipt {
    std::int64_t parent_id = 0;
    std::string stable_key;
    std::int64_t revision_id = 0;
    int revision_number = 0;
    std::string revision_state;
    std::string semantic_sha256;
    bool identity_created = false;
    bool metadata_changed = false;
    bool semantic_changed = false;
};

struct PredicateDefinitionDraftBody {
    std::vector<savor::runtime::program::composition::PredicateWitness> witnesses;
    std::vector<savor::runtime::program::composition::PredicateExpressionNode> expression;
    std::size_t root_expression = 0;
};

struct CreatePredicateDefinitionDraftCommand {
    std::string creation_request_key;
    std::string name;
    std::string description;
    PredicateDefinitionDraftBody body;
    types::UtcTimePoint created_at_utc{};
};

struct SavePredicateDefinitionDraftCommand {
    std::int64_t predicate_definition_id = 0;
    PredicateDefinitionDraftBody body;
    types::UtcTimePoint saved_at_utc{};
};

struct DuplicatePredicateDefinitionCommand {
    std::int64_t source_revision_id = 0;
    std::string creation_request_key;
    std::string name;
    std::string description;
    types::UtcTimePoint created_at_utc{};
};

struct PredicateDefinitionRevisionV2Snapshot {
    std::int64_t predicate_definition_id = 0;
    std::int64_t predicate_definition_revision_id = 0;
    std::string stable_key;
    std::string name;
    std::string description;
    std::string revision_state;
    std::string semantic_sha256;
    std::string content_sha256;
    savor::runtime::program::composition::PredicateDefinition definition;
};

struct PredicateExecutionBindingDraftBody {
    std::int64_t predicate_definition_revision_id = 0;
    std::vector<savor::runtime::predicates::PredicateWitnessSourceBindingV1> witnesses;
};

struct CreatePredicateExecutionBindingDraftCommand {
    std::string creation_request_key;
    std::string name;
    std::string description;
    PredicateExecutionBindingDraftBody body;
    types::UtcTimePoint created_at_utc{};
};

struct SavePredicateExecutionBindingDraftCommand {
    std::int64_t predicate_execution_binding_id = 0;
    PredicateExecutionBindingDraftBody body;
    types::UtcTimePoint saved_at_utc{};
};

struct DuplicatePredicateExecutionBindingCommand {
    std::int64_t source_revision_id = 0;
    std::string creation_request_key;
    std::string name;
    std::string description;
    types::UtcTimePoint created_at_utc{};
};

struct PredicateExecutionBindingRevisionSnapshot {
    std::int64_t predicate_execution_binding_id = 0;
    std::string stable_key;
    std::string name;
    std::string description;
    std::string revision_state;
    std::string semantic_sha256;
    savor::runtime::predicates::PredicateExecutionBindingV1 binding;
};

struct PredicateGroupDraftBody {
    std::vector<savor::runtime::predicates::PredicateGroupMemberV1> members;
};

struct CreatePredicateGroupDraftCommand {
    std::string creation_request_key;
    std::string name;
    std::string description;
    PredicateGroupDraftBody body;
    types::UtcTimePoint created_at_utc{};
};

struct SavePredicateGroupDraftCommand {
    std::int64_t predicate_group_id = 0;
    PredicateGroupDraftBody body;
    types::UtcTimePoint saved_at_utc{};
};

struct DuplicatePredicateGroupCommand {
    std::int64_t source_revision_id = 0;
    std::string creation_request_key;
    std::string name;
    std::string description;
    types::UtcTimePoint created_at_utc{};
};

enum class PredicateAuthoringObjectKind : std::uint8_t {
    Definition,
    ExecutionBinding,
    Group,
};

struct UpdatePredicateAuthoringMetadataCommand {
    PredicateAuthoringObjectKind object_kind = PredicateAuthoringObjectKind::Definition;
    std::int64_t parent_id = 0;
    std::string name;
    std::string description;
    types::UtcTimePoint updated_at_utc{};
};

struct PredicateGroupRevisionSnapshot {
    std::int64_t predicate_group_id = 0;
    std::string stable_key;
    std::string name;
    std::string description;
    std::string revision_state;
    std::string semantic_sha256;
    savor::runtime::predicates::ResolvedPredicateGroupV1 group;
};

struct PredicateRevisionListQueryV2 {
    std::optional<std::string> revision_state;
    std::string search_text;
    std::optional<std::int64_t> before_revision_id;
    int limit = 100;
};

struct PredicateDefinitionRevisionV2Summary {
    std::int64_t predicate_definition_revision_id = 0;
    int revision_number = 0;
    std::string stable_key;
    std::string name;
    std::string description;
    std::string revision_state;
    std::string semantic_sha256;
    std::string content_sha256;
    int witness_count = 0;
    int expression_node_count = 0;
};

struct PredicateExecutionBindingRevisionSummary {
    std::int64_t predicate_execution_binding_revision_id = 0;
    int revision_number = 0;
    std::string stable_key;
    std::string name;
    std::string description;
    std::string revision_state;
    std::string semantic_sha256;
    std::string content_sha256;
    std::int64_t predicate_definition_revision_id = 0;
    int witness_source_count = 0;
};

struct PredicateGroupRevisionSummary {
    std::int64_t predicate_group_revision_id = 0;
    int revision_number = 0;
    std::string stable_key;
    std::string name;
    std::string description;
    std::string revision_state;
    std::string semantic_sha256;
    std::string content_sha256;
    int member_count = 0;
    int hook_count = 0;
};

template <typename T>
struct PredicateRevisionPageV2 {
    std::vector<T> items;
    std::optional<std::int64_t> next_before_revision_id;
};

struct IAuthoringDb {
    virtual ~IAuthoringDb() = default;

    virtual bool CreatePredicateDefinitionDraft(
        const CreatePredicateDefinitionDraftCommand&,
        PredicateAuthoringRevisionReceipt* = nullptr,
        std::string* error_out = nullptr) {
        if (error_out) *error_out = "predicate definition authoring is unavailable";
        return false;
    }
    virtual bool SavePredicateDefinitionDraft(
        const SavePredicateDefinitionDraftCommand&,
        PredicateAuthoringRevisionReceipt* = nullptr,
        std::string* error_out = nullptr) {
        if (error_out) *error_out = "predicate definition authoring is unavailable";
        return false;
    }
    virtual bool DuplicatePredicateDefinition(
        const DuplicatePredicateDefinitionCommand&,
        PredicateAuthoringRevisionReceipt* = nullptr,
        std::string* error_out = nullptr) {
        if (error_out) *error_out = "predicate definition authoring is unavailable";
        return false;
    }
    virtual bool PublishPredicateDefinitionRevisionV2(
        std::int64_t,
        types::UtcTimePoint,
        PredicateAuthoringRevisionReceipt* = nullptr,
        std::string* error_out = nullptr) {
        if (error_out) *error_out = "predicate-v2 authoring is unavailable";
        return false;
    }
    virtual std::optional<PredicateDefinitionRevisionV2Snapshot>
    GetPredicateDefinitionRevisionV2(std::int64_t) const { return std::nullopt; }
    virtual PredicateRevisionPageV2<PredicateDefinitionRevisionV2Summary>
    ListPredicateDefinitionRevisionsV2(const PredicateRevisionListQueryV2&) const { return {}; }
    virtual bool AbandonPredicateDefinitionDraftV2(
        std::int64_t, std::string* error_out = nullptr) {
        if (error_out) *error_out = "predicate authoring is unavailable";
        return false;
    }

    virtual bool CreatePredicateExecutionBindingDraft(
        const CreatePredicateExecutionBindingDraftCommand&,
        PredicateAuthoringRevisionReceipt* = nullptr,
        std::string* error_out = nullptr) {
        if (error_out) *error_out = "predicate execution-binding authoring is unavailable";
        return false;
    }
    virtual bool SavePredicateExecutionBindingDraft(
        const SavePredicateExecutionBindingDraftCommand&,
        PredicateAuthoringRevisionReceipt* = nullptr,
        std::string* error_out = nullptr) {
        if (error_out) *error_out = "predicate execution-binding authoring is unavailable";
        return false;
    }
    virtual bool DuplicatePredicateExecutionBinding(
        const DuplicatePredicateExecutionBindingCommand&,
        PredicateAuthoringRevisionReceipt* = nullptr,
        std::string* error_out = nullptr) {
        if (error_out) *error_out = "predicate execution-binding authoring is unavailable";
        return false;
    }
    virtual bool PublishPredicateExecutionBindingRevision(
        std::int64_t,
        types::UtcTimePoint,
        PredicateAuthoringRevisionReceipt* = nullptr,
        std::string* error_out = nullptr) {
        if (error_out) *error_out = "predicate execution-binding authoring is unavailable";
        return false;
    }
    virtual std::optional<PredicateExecutionBindingRevisionSnapshot>
    GetPredicateExecutionBindingRevision(std::int64_t) const { return std::nullopt; }
    virtual PredicateRevisionPageV2<PredicateExecutionBindingRevisionSummary>
    ListPredicateExecutionBindingRevisions(const PredicateRevisionListQueryV2&) const { return {}; }
    virtual bool AbandonPredicateExecutionBindingDraft(
        std::int64_t, std::string* error_out = nullptr) {
        if (error_out) *error_out = "predicate execution-binding authoring is unavailable";
        return false;
    }

    virtual bool CreatePredicateGroupDraft(
        const CreatePredicateGroupDraftCommand&,
        PredicateAuthoringRevisionReceipt* = nullptr,
        std::string* error_out = nullptr) {
        if (error_out) *error_out = "predicate-group authoring is unavailable";
        return false;
    }
    virtual bool SavePredicateGroupDraft(
        const SavePredicateGroupDraftCommand&,
        PredicateAuthoringRevisionReceipt* = nullptr,
        std::string* error_out = nullptr) {
        if (error_out) *error_out = "predicate-group authoring is unavailable";
        return false;
    }
    virtual bool DuplicatePredicateGroup(
        const DuplicatePredicateGroupCommand&,
        PredicateAuthoringRevisionReceipt* = nullptr,
        std::string* error_out = nullptr) {
        if (error_out) *error_out = "predicate-group authoring is unavailable";
        return false;
    }
    virtual bool PublishPredicateGroupRevision(
        std::int64_t, types::UtcTimePoint,
        PredicateAuthoringRevisionReceipt* = nullptr,
        std::string* error_out = nullptr) {
        if (error_out) *error_out = "predicate-group authoring is unavailable";
        return false;
    }
    virtual std::optional<PredicateGroupRevisionSnapshot>
    GetPredicateGroupRevision(std::int64_t) const { return std::nullopt; }
    virtual PredicateRevisionPageV2<PredicateGroupRevisionSummary>
    ListPredicateGroupRevisions(const PredicateRevisionListQueryV2&) const { return {}; }
    virtual bool AbandonPredicateGroupDraft(
        std::int64_t, std::string* error_out = nullptr) {
        if (error_out) *error_out = "predicate-group authoring is unavailable";
        return false;
    }

    virtual bool UpdatePredicateAuthoringMetadata(
        const UpdatePredicateAuthoringMetadataCommand&,
        bool* changed_out = nullptr,
        std::string* error_out = nullptr) {
        if (error_out) *error_out = "predicate metadata authoring is unavailable";
        return false;
    }

    virtual bool SaveSeedProbeSpec(
        const SaveSeedProbeSpecCommand& command,
        std::int64_t* seed_probe_spec_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual std::optional<SeedProbeSpecSnapshot> GetSeedProbeSpec(
        std::int64_t seed_probe_spec_id) const = 0;

    virtual std::vector<SeedProbeSpecSnapshot> ListSeedProbeSpecs(
        int max_count) const = 0;

    virtual bool EnsureAuthoringInputSet(
        const EnsureAuthoringInputSetCommand& command,
        std::int64_t* input_set_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual std::vector<AuthoringInputSetFrameSnapshot> ListAuthoringInputSetFrames(
        std::int64_t input_set_id) const = 0;

    virtual bool SaveTasSpec(
        const SaveTasSpecCommand& command,
        std::int64_t* tas_spec_id_out = nullptr,
        std::int64_t* tas_spec_base_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual std::optional<TasSpecSnapshot> GetTasSpec(
        std::int64_t tas_spec_id) const = 0;

    virtual std::vector<TasSpecSnapshot> ListTasSpecs(
        int max_count) const = 0;

    virtual bool SavePlan(
        const SavePlanCommand& command,
        std::int64_t* plan_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool SaveBattlePlanActionPreset(
        const SaveBattlePlanActionPresetCommand& command,
        std::int64_t* action_preset_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool RenameBattlePlanActionPreset(
        const RenameBattlePlanActionPresetCommand& command,
        std::string* error_out = nullptr) = 0;

    virtual std::optional<BattlePlanActionPresetSnapshot> GetBattlePlanActionPreset(
        std::int64_t action_preset_id) const = 0;

    virtual std::vector<BattlePlanActionPresetSnapshot> ListBattlePlanActionPresets(
        int max_count) const = 0;

    virtual bool SaveBattlePlanTurn(
        const SaveBattlePlanTurnCommand& command,
        std::int64_t* plan_turn_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual std::optional<BattlePlanSnapshot> GetBattlePlan(
        std::int64_t plan_id) const = 0;

    virtual std::vector<BattlePlanSnapshot> ListBattlePlans(
        int max_count) const = 0;

    virtual bool SaveWorkflowGraph(
        const SaveWorkflowGraphCommand& command,
        SaveWorkflowGraphResult* result_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool SetWorkflowGraphHidden(
        std::int64_t workflow_graph_id,
        bool hidden,
        std::string* error_out = nullptr) = 0;

    virtual std::optional<WorkflowGraphSnapshot> GetWorkflowGraph(
        std::int64_t workflow_graph_id) const = 0;

    virtual std::optional<WorkflowGraphSnapshot> GetWorkflowGraphRevision(
        std::int64_t workflow_graph_revision_id) const = 0;

    virtual std::vector<WorkflowGraphSnapshot> ListWorkflowGraphs(
        int max_count,
        bool include_hidden = false) const = 0;

    virtual std::vector<events::EventEnvelope> ReadUnpublishedOutboxBatch(
        std::int64_t after_outbox_id,
        int max_batch_size) = 0;

    virtual bool MarkOutboxPublished(
        std::int64_t outbox_id,
        types::UtcTimePoint published_at_utc) = 0;

    virtual bool MarkOutboxPublishFailure(
        std::int64_t outbox_id,
        std::string_view last_error) = 0;

    virtual retention::OutboxRetentionPreview PreviewOutboxRetention(
        const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
        types::UtcTimePoint now_utc,
        const retention::OutboxRetentionPolicy& policy) const = 0;

    virtual bool PurgeOutboxThroughRetentionFloor(
        const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
        types::UtcTimePoint now_utc,
        const retention::OutboxRetentionPolicy& policy,
        int max_rows,
        int* rows_deleted_out = nullptr,
        std::string* error_out = nullptr) = 0;

    // Resolves authoring payload references from a canonical outbox/event envelope.
    virtual std::optional<AuthoringPayloadRecord> ResolveAuthoringPayload(
        const events::EventEnvelope& envelope) const = 0;

    // Dispatch-key variant for projector/consumer code paths that already split key fields.
    virtual std::optional<AuthoringPayloadRecord> ResolveAuthoringPayload(
        std::string_view event_type,
        int event_version,
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const = 0;
};

} // namespace savor::db
