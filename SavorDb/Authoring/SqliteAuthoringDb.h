#pragma once

#include <sqlite3.h>

#include "IAuthoringDb.h"

namespace savor::db {

class SqliteAuthoringDb final : public IAuthoringDb {
public:
    bool CreatePredicateDefinitionDraft(
        const CreatePredicateDefinitionDraftCommand& command,
        PredicateAuthoringRevisionReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    bool SavePredicateDefinitionDraft(
        const SavePredicateDefinitionDraftCommand& command,
        PredicateAuthoringRevisionReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    bool DuplicatePredicateDefinition(
        const DuplicatePredicateDefinitionCommand& command,
        PredicateAuthoringRevisionReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    bool PublishPredicateDefinitionRevisionV2(
        std::int64_t revision_id,
        types::UtcTimePoint published_at_utc,
        PredicateAuthoringRevisionReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    std::optional<PredicateDefinitionRevisionV2Snapshot>
    GetPredicateDefinitionRevisionV2(std::int64_t revision_id) const override;
    PredicateRevisionPageV2<PredicateDefinitionRevisionV2Summary>
    ListPredicateDefinitionRevisionsV2(const PredicateRevisionListQueryV2& query) const override;
    bool AbandonPredicateDefinitionDraftV2(
        std::int64_t revision_id, std::string* error_out = nullptr) override;
    bool CreatePredicateExecutionBindingDraft(
        const CreatePredicateExecutionBindingDraftCommand& command,
        PredicateAuthoringRevisionReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    bool SavePredicateExecutionBindingDraft(
        const SavePredicateExecutionBindingDraftCommand& command,
        PredicateAuthoringRevisionReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    bool DuplicatePredicateExecutionBinding(
        const DuplicatePredicateExecutionBindingCommand& command,
        PredicateAuthoringRevisionReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    bool PublishPredicateExecutionBindingRevision(
        std::int64_t revision_id,
        types::UtcTimePoint published_at_utc,
        PredicateAuthoringRevisionReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    std::optional<PredicateExecutionBindingRevisionSnapshot>
    GetPredicateExecutionBindingRevision(std::int64_t revision_id) const override;
    PredicateRevisionPageV2<PredicateExecutionBindingRevisionSummary>
    ListPredicateExecutionBindingRevisions(const PredicateRevisionListQueryV2& query) const override;
    bool AbandonPredicateExecutionBindingDraft(
        std::int64_t revision_id, std::string* error_out = nullptr) override;
    bool CreatePredicateGroupDraft(
        const CreatePredicateGroupDraftCommand& command,
        PredicateAuthoringRevisionReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    bool SavePredicateGroupDraft(
        const SavePredicateGroupDraftCommand& command,
        PredicateAuthoringRevisionReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    bool DuplicatePredicateGroup(
        const DuplicatePredicateGroupCommand& command,
        PredicateAuthoringRevisionReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    bool PublishPredicateGroupRevision(
        std::int64_t revision_id, types::UtcTimePoint published_at_utc,
        PredicateAuthoringRevisionReceipt* receipt_out = nullptr,
        std::string* error_out = nullptr) override;
    std::optional<PredicateGroupRevisionSnapshot>
    GetPredicateGroupRevision(std::int64_t revision_id) const override;
    PredicateRevisionPageV2<PredicateGroupRevisionSummary>
    ListPredicateGroupRevisions(const PredicateRevisionListQueryV2& query) const override;
    bool AbandonPredicateGroupDraft(
        std::int64_t revision_id, std::string* error_out = nullptr) override;
    bool UpdatePredicateAuthoringMetadata(
        const UpdatePredicateAuthoringMetadataCommand& command,
        bool* changed_out = nullptr,
        std::string* error_out = nullptr) override;
    explicit SqliteAuthoringDb(sqlite3* db);

    bool SaveSeedProbeSpec(
        const SaveSeedProbeSpecCommand& command,
        std::int64_t* seed_probe_spec_id_out = nullptr,
        std::string* error_out = nullptr) override;

    std::optional<SeedProbeSpecSnapshot> GetSeedProbeSpec(
        std::int64_t seed_probe_spec_id) const override;
    std::vector<SeedProbeSpecSnapshot> ListSeedProbeSpecs(
        int max_count) const override;
    bool EnsureAuthoringInputSet(
        const EnsureAuthoringInputSetCommand& command,
        std::int64_t* input_set_id_out = nullptr,
        std::string* error_out = nullptr) override;
    std::vector<AuthoringInputSetFrameSnapshot> ListAuthoringInputSetFrames(
        std::int64_t input_set_id) const override;

    bool SavePlan(
        const SavePlanCommand& command,
        std::int64_t* plan_id_out = nullptr,
        std::string* error_out = nullptr) override;

    bool SaveBattlePlanActionPreset(
        const SaveBattlePlanActionPresetCommand& command,
        std::int64_t* action_preset_id_out = nullptr,
        std::string* error_out = nullptr) override;

    bool RenameBattlePlanActionPreset(
        const RenameBattlePlanActionPresetCommand& command,
        std::string* error_out = nullptr) override;

    std::optional<BattlePlanActionPresetSnapshot> GetBattlePlanActionPreset(
        std::int64_t action_preset_id) const override;

    std::vector<BattlePlanActionPresetSnapshot> ListBattlePlanActionPresets(
        int max_count) const override;

    bool SaveBattlePlanTurn(
        const SaveBattlePlanTurnCommand& command,
        std::int64_t* plan_turn_id_out = nullptr,
        std::string* error_out = nullptr) override;

    std::optional<BattlePlanSnapshot> GetBattlePlan(
        std::int64_t plan_id) const override;

    std::vector<BattlePlanSnapshot> ListBattlePlans(
        int max_count) const override;

    bool SaveWorkflowGraph(
        const SaveWorkflowGraphCommand& command,
        SaveWorkflowGraphResult* result_out = nullptr,
        std::string* error_out = nullptr) override;

    bool SetWorkflowGraphHidden(
        std::int64_t workflow_graph_id,
        bool hidden,
        std::string* error_out = nullptr) override;

    std::optional<WorkflowGraphSnapshot> GetWorkflowGraph(
        std::int64_t workflow_graph_id) const override;

    std::optional<WorkflowGraphSnapshot> GetWorkflowGraphRevision(
        std::int64_t workflow_graph_revision_id) const override;

    std::vector<WorkflowGraphSnapshot> ListWorkflowGraphs(
        int max_count,
        bool include_hidden = false) const override;

    std::vector<events::EventEnvelope> ReadUnpublishedOutboxBatch(
        std::int64_t after_outbox_id,
        int max_batch_size) override;

    bool MarkOutboxPublished(
        std::int64_t outbox_id,
        types::UtcTimePoint published_at_utc) override;

    bool MarkOutboxPublishFailure(
        std::int64_t outbox_id,
        std::string_view last_error) override;
    retention::OutboxRetentionPreview PreviewOutboxRetention(
        const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
        types::UtcTimePoint now_utc,
        const retention::OutboxRetentionPolicy& policy) const override;
    bool PurgeOutboxThroughRetentionFloor(
        const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
        types::UtcTimePoint now_utc,
        const retention::OutboxRetentionPolicy& policy,
        int max_rows,
        int* rows_deleted_out = nullptr,
        std::string* error_out = nullptr) override;

    std::optional<AuthoringPayloadRecord> ResolveAuthoringPayload(
        const events::EventEnvelope& envelope) const override;

    std::optional<AuthoringPayloadRecord> ResolveAuthoringPayload(
        std::string_view event_type,
        int event_version,
        std::string_view payload_ref_kind,
        std::int64_t payload_ref_id) const override;

private:
    sqlite3* db_ = nullptr;
};

} // namespace savor::db
