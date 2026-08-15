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
#include "../../SavorCore/Runner/Runtime/Predicates/PredicateBundle.h"

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
    std::int64_t battle_chain_spec_id = 0;
    std::int64_t seed_probe_spec_id = 0;
    std::int64_t tas_spec_id = 0;
    std::int64_t battle_run_spec_id = 0;
    std::int64_t explorer_settings_id = 0;
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

struct SaveBattleRunSpecCommand {
    std::string name;
    int priority = 0;
    bool progress_enable = true;
    bool use_single_turn_runner = false;
    bool auto_wave_trigger_enable = false;
    types::UtcTimePoint created_at_utc{};
    std::string correlation_id;
    std::string causation_id;
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
    std::optional<std::int64_t> default_predicate_bundle_revision_id;
    std::vector<SaveBattlePlanActionCommand> actions;
    bool replace_existing_actions = true;
    types::UtcTimePoint created_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct SaveExplorerSettingsCommand {
    std::string name;
    std::string description;
    std::optional<std::int64_t> default_plan_id;
    types::UtcTimePoint created_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct SaveBattleChainSpecCommand {
    std::string name;
    std::string description;
    std::int64_t battle_run_spec_id = 0;
    std::int64_t explorer_settings_id = 0;
    types::UtcTimePoint created_at_utc{};
    std::string correlation_id;
    std::string causation_id;
};

struct BattleChainSpecSnapshot {
    std::int64_t battle_chain_spec_id = 0;
    std::string name;
    std::string description;
    std::int64_t battle_run_spec_id = 0;
    std::int64_t explorer_settings_id = 0;
};

struct SaveWorkflowGraphNodeInputCommand {
    std::string input_key;
    std::string data_kind;
    std::string display_name;
    bool required = true;
};

struct SaveWorkflowGraphNodeOutputCommand {
    std::string output_key;
    std::string data_kind;
    std::string display_name;
};

struct SaveWorkflowGraphNodeCommand {
    std::string node_key;
    std::string unit_kind;
    std::string display_name;
    std::optional<std::string> authored_ref_kind;
    std::optional<std::int64_t> authored_ref_id;
    std::vector<SaveWorkflowGraphNodeInputCommand> inputs;
    std::vector<SaveWorkflowGraphNodeOutputCommand> possible_outputs;
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

struct BattleRunSpecSnapshot {
    std::int64_t battle_run_spec_id = 0;
    std::string name;
    int priority = 0;
    bool progress_enable = true;
    bool use_single_turn_runner = false;
    bool auto_wave_trigger_enable = false;
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
    std::optional<std::int64_t> default_predicate_bundle_revision_id;
    std::vector<BattlePlanActionSnapshot> actions;
};

struct BattlePlanSnapshot {
    std::int64_t plan_id = 0;
    std::string name;
    std::string fingerprint;
    std::vector<BattlePlanTurnSnapshot> turns;
};

struct ExplorerSettingsSnapshot {
    std::int64_t explorer_settings_id = 0;
    std::string name;
    std::string description;
    std::optional<std::int64_t> default_plan_id;
};

struct WorkflowGraphNodeInputSnapshot {
    std::string input_key;
    std::string data_kind;
    std::string display_name;
    bool required = true;
};

struct WorkflowGraphNodeOutputSnapshot {
    std::string output_key;
    std::string data_kind;
    std::string display_name;
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
struct SavePredicateDefinitionDraftV2Command {
    std::string stable_key;
    std::string name;
    std::string description;
    savor::runtime::program::composition::PredicateDefinition definition;
    types::UtcTimePoint created_at_utc{};
};

struct PredicateDefinitionRevisionV2Snapshot {
    std::int64_t predicate_definition_id = 0;
    std::int64_t predicate_definition_revision_id = 0;
    std::string stable_key;
    std::string name;
    std::string description;
    std::string revision_state;
    std::string content_sha256;
    savor::runtime::program::composition::PredicateDefinition definition;
};

struct SavePredicateBundleDraftV2Command {
    std::string stable_key;
    std::string name;
    std::string description;
    savor::runtime::predicates::ResolvedPredicateBundleV1 bundle;
    types::UtcTimePoint created_at_utc{};
};

struct PredicateBundleRevisionV2Snapshot {
    std::int64_t predicate_bundle_id = 0;
    std::string stable_key;
    std::string name;
    std::string description;
    std::string revision_state;
    savor::runtime::predicates::ResolvedPredicateBundleV1 bundle;
};

struct IAuthoringDb {
    virtual ~IAuthoringDb() = default;

    virtual bool SavePredicateDefinitionDraftV2(
        const SavePredicateDefinitionDraftV2Command&,
        std::int64_t* = nullptr,
        std::string* error_out = nullptr) {
        if (error_out) *error_out = "predicate-v2 authoring is unavailable";
        return false;
    }
    virtual bool PublishPredicateDefinitionRevisionV2(
        std::int64_t,
        types::UtcTimePoint,
        std::string* error_out = nullptr) {
        if (error_out) *error_out = "predicate-v2 authoring is unavailable";
        return false;
    }
    virtual std::optional<PredicateDefinitionRevisionV2Snapshot>
    GetPredicateDefinitionRevisionV2(std::int64_t) const { return std::nullopt; }

    virtual bool SavePredicateBundleDraftV2(
        const SavePredicateBundleDraftV2Command&,
        std::int64_t* = nullptr,
        std::string* error_out = nullptr) {
        if (error_out) *error_out = "predicate-v2 authoring is unavailable";
        return false;
    }
    virtual bool PublishPredicateBundleRevisionV2(
        std::int64_t,
        types::UtcTimePoint,
        std::string* error_out = nullptr) {
        if (error_out) *error_out = "predicate-v2 authoring is unavailable";
        return false;
    }
    virtual std::optional<PredicateBundleRevisionV2Snapshot>
    GetPredicateBundleRevisionV2(std::int64_t) const { return std::nullopt; }

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

    virtual bool SaveBattleRunSpec(
        const SaveBattleRunSpecCommand& command,
        std::int64_t* battle_run_spec_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual std::optional<BattleRunSpecSnapshot> GetBattleRunSpec(
        std::int64_t battle_run_spec_id) const = 0;

    virtual std::vector<BattleRunSpecSnapshot> ListBattleRunSpecs(
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

    virtual bool SaveExplorerSettings(
        const SaveExplorerSettingsCommand& command,
        std::int64_t* explorer_settings_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual std::optional<ExplorerSettingsSnapshot> GetExplorerSettings(
        std::int64_t explorer_settings_id) const = 0;

    virtual std::vector<ExplorerSettingsSnapshot> ListExplorerSettings(
        int max_count) const = 0;

    virtual bool SaveBattleChainSpec(
        const SaveBattleChainSpecCommand& command,
        std::int64_t* battle_chain_spec_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual std::optional<BattleChainSpecSnapshot> GetBattleChainSpec(
        std::int64_t battle_chain_spec_id) const = 0;

    virtual std::vector<BattleChainSpecSnapshot> ListBattleChainSpecs(
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
