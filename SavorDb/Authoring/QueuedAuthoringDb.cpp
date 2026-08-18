#include "QueuedAuthoringDb.h"

#include <utility>

namespace savor::db {
namespace {

void SetError(std::string* error_out, std::string message) {
    if (error_out != nullptr) {
        *error_out = std::move(message);
    }
}

core::QueuedDbTelemetrySnapshot BuildTelemetrySnapshot(
    const core::QueuedDbLane* write_lane,
    const core::QueuedDbLane* read_lane) {
    return core::BuildQueuedDbTelemetrySnapshot(write_lane, read_lane);
}

} // namespace

QueuedAuthoringDb::QueuedAuthoringDb(
    IAuthoringDb* inner,
    core::QueuedDbConfig config)
    : inner_(inner)
    , config_(config)
    , read_lane_(std::make_unique<core::QueuedDbLane>("authoring-read", config_.read_capacity))
    , write_lane_(std::make_unique<core::QueuedDbLane>("authoring-write", config_.write_capacity)) {
}

QueuedAuthoringDb::~QueuedAuthoringDb() {
    Stop();
}

bool QueuedAuthoringDb::Start(std::string* error_out) {
    if (inner_ == nullptr) {
        SetError(error_out, "authoring db inner database is null");
        return false;
    }
    if (IsRunning()) {
        return true;
    }
    if (!write_lane_->Start(error_out)) {
        return false;
    }
    if (!read_lane_->Start(error_out)) {
        write_lane_->Stop();
        return false;
    }
    return true;
}

void QueuedAuthoringDb::Stop() {
    if (read_lane_) {
        read_lane_->Stop();
    }
    if (write_lane_) {
        write_lane_->Stop();
    }
}

bool QueuedAuthoringDb::IsRunning() const {
    return read_lane_ != nullptr
        && write_lane_ != nullptr
        && read_lane_->IsRunning()
        && write_lane_->IsRunning();
}

core::QueuedDbTelemetrySnapshot QueuedAuthoringDb::GetTelemetrySnapshot() const {
    return BuildTelemetrySnapshot(write_lane_.get(), read_lane_.get());
}

bool QueuedAuthoringDb::CreatePredicateDefinitionDraft(
    const CreatePredicateDefinitionDraftCommand& command,
    PredicateAuthoringRevisionReceipt* receipt_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, receipt_out, error_out]() {
            return inner_ && inner_->CreatePredicateDefinitionDraft(
                command, receipt_out, error_out);
        }, false, error_out);
}

bool QueuedAuthoringDb::SavePredicateDefinitionDraft(
    const SavePredicateDefinitionDraftCommand& command,
    PredicateAuthoringRevisionReceipt* receipt_out,
    std::string* error_out) {
    return ExecuteWrite<bool>([this, command, receipt_out, error_out]() {
        return inner_ && inner_->SavePredicateDefinitionDraft(command, receipt_out, error_out);
    }, false, error_out);
}

bool QueuedAuthoringDb::DuplicatePredicateDefinition(
    const DuplicatePredicateDefinitionCommand& command,
    PredicateAuthoringRevisionReceipt* receipt_out,
    std::string* error_out) {
    return ExecuteWrite<bool>([this, command, receipt_out, error_out]() {
        return inner_ && inner_->DuplicatePredicateDefinition(command, receipt_out, error_out);
    }, false, error_out);
}

bool QueuedAuthoringDb::PublishPredicateDefinitionRevisionV2(
    std::int64_t revision_id,
    types::UtcTimePoint published_at_utc,
    PredicateAuthoringRevisionReceipt* receipt_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, revision_id, published_at_utc, receipt_out, error_out]() {
            return inner_ && inner_->PublishPredicateDefinitionRevisionV2(
                revision_id, published_at_utc, receipt_out, error_out);
        }, false, error_out);
}

std::optional<PredicateDefinitionRevisionV2Snapshot>
QueuedAuthoringDb::GetPredicateDefinitionRevisionV2(
    std::int64_t revision_id) const {
    return ExecuteRead<std::optional<PredicateDefinitionRevisionV2Snapshot>>(
        [this, revision_id]() {
            return inner_ ? inner_->GetPredicateDefinitionRevisionV2(revision_id)
                          : std::nullopt;
        }, std::nullopt);
}

PredicateRevisionPageV2<PredicateDefinitionRevisionV2Summary>
QueuedAuthoringDb::ListPredicateDefinitionRevisionsV2(
    const PredicateRevisionListQueryV2& query) const {
    return ExecuteRead<PredicateRevisionPageV2<PredicateDefinitionRevisionV2Summary>>(
        [this, query]() { return inner_ ? inner_->ListPredicateDefinitionRevisionsV2(query)
                                      : PredicateRevisionPageV2<PredicateDefinitionRevisionV2Summary>{}; }, {});
}

bool QueuedAuthoringDb::AbandonPredicateDefinitionDraftV2(
    std::int64_t revision_id, std::string* error_out) {
    return ExecuteWrite<bool>([this,revision_id,error_out]() {
        return inner_ && inner_->AbandonPredicateDefinitionDraftV2(
            revision_id,error_out);},false,error_out);
}

bool QueuedAuthoringDb::CreatePredicateExecutionBindingDraft(
    const CreatePredicateExecutionBindingDraftCommand& command,
    PredicateAuthoringRevisionReceipt* receipt_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, receipt_out, error_out]() {
            return inner_ && inner_->CreatePredicateExecutionBindingDraft(
                command, receipt_out, error_out);
        }, false, error_out);
}

bool QueuedAuthoringDb::SavePredicateExecutionBindingDraft(
    const SavePredicateExecutionBindingDraftCommand& command,
    PredicateAuthoringRevisionReceipt* receipt_out,
    std::string* error_out) {
    return ExecuteWrite<bool>([this, command, receipt_out, error_out]() {
        return inner_ && inner_->SavePredicateExecutionBindingDraft(command, receipt_out, error_out);
    }, false, error_out);
}

bool QueuedAuthoringDb::DuplicatePredicateExecutionBinding(
    const DuplicatePredicateExecutionBindingCommand& command,
    PredicateAuthoringRevisionReceipt* receipt_out,
    std::string* error_out) {
    return ExecuteWrite<bool>([this, command, receipt_out, error_out]() {
        return inner_ && inner_->DuplicatePredicateExecutionBinding(command, receipt_out, error_out);
    }, false, error_out);
}

bool QueuedAuthoringDb::PublishPredicateExecutionBindingRevision(
    std::int64_t revision_id,
    types::UtcTimePoint published_at_utc,
    PredicateAuthoringRevisionReceipt* receipt_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, revision_id, published_at_utc, receipt_out, error_out]() {
            return inner_ && inner_->PublishPredicateExecutionBindingRevision(
                revision_id, published_at_utc, receipt_out, error_out);
        }, false, error_out);
}

std::optional<PredicateExecutionBindingRevisionSnapshot>
QueuedAuthoringDb::GetPredicateExecutionBindingRevision(
    std::int64_t revision_id) const {
    return ExecuteRead<std::optional<PredicateExecutionBindingRevisionSnapshot>>(
        [this, revision_id]() {
            return inner_ ? inner_->GetPredicateExecutionBindingRevision(revision_id)
                          : std::nullopt;
        }, std::nullopt);
}

PredicateRevisionPageV2<PredicateExecutionBindingRevisionSummary>
QueuedAuthoringDb::ListPredicateExecutionBindingRevisions(
    const PredicateRevisionListQueryV2& query) const {
    return ExecuteRead<PredicateRevisionPageV2<PredicateExecutionBindingRevisionSummary>>(
        [this, query]() { return inner_ ? inner_->ListPredicateExecutionBindingRevisions(query)
                                      : PredicateRevisionPageV2<PredicateExecutionBindingRevisionSummary>{}; }, {});
}

bool QueuedAuthoringDb::AbandonPredicateExecutionBindingDraft(
    std::int64_t revision_id, std::string* error_out) {
    return ExecuteWrite<bool>([this,revision_id,error_out]() {
        return inner_ && inner_->AbandonPredicateExecutionBindingDraft(
            revision_id,error_out);},false,error_out);
}

bool QueuedAuthoringDb::CreatePredicateGroupDraft(
    const CreatePredicateGroupDraftCommand& command,
    PredicateAuthoringRevisionReceipt* receipt_out, std::string* error_out) {
    return ExecuteWrite<bool>([this,command,receipt_out,error_out]() {
        return inner_ && inner_->CreatePredicateGroupDraft(
            command,receipt_out,error_out);},false,error_out);
}

bool QueuedAuthoringDb::SavePredicateGroupDraft(
    const SavePredicateGroupDraftCommand& command,
    PredicateAuthoringRevisionReceipt* receipt_out, std::string* error_out) {
    return ExecuteWrite<bool>([this,command,receipt_out,error_out]() {
        return inner_ && inner_->SavePredicateGroupDraft(
            command,receipt_out,error_out);},false,error_out);
}

bool QueuedAuthoringDb::DuplicatePredicateGroup(
    const DuplicatePredicateGroupCommand& command,
    PredicateAuthoringRevisionReceipt* receipt_out, std::string* error_out) {
    return ExecuteWrite<bool>([this,command,receipt_out,error_out]() {
        return inner_ && inner_->DuplicatePredicateGroup(
            command,receipt_out,error_out);},false,error_out);
}

bool QueuedAuthoringDb::PublishPredicateGroupRevision(
    std::int64_t revision_id, types::UtcTimePoint published_at_utc,
    PredicateAuthoringRevisionReceipt* receipt_out,
    std::string* error_out) {
    return ExecuteWrite<bool>([this,revision_id,published_at_utc,receipt_out,error_out]() {
        return inner_ && inner_->PublishPredicateGroupRevision(
            revision_id,published_at_utc,receipt_out,error_out);},false,error_out);
}

std::optional<PredicateGroupRevisionSnapshot>
QueuedAuthoringDb::GetPredicateGroupRevision(std::int64_t revision_id) const {
    return ExecuteRead<std::optional<PredicateGroupRevisionSnapshot>>(
        [this,revision_id]() { return inner_ ? inner_->GetPredicateGroupRevision(revision_id)
                                             : std::nullopt; },std::nullopt);
}

PredicateRevisionPageV2<PredicateGroupRevisionSummary>
QueuedAuthoringDb::ListPredicateGroupRevisions(
    const PredicateRevisionListQueryV2& query) const {
    return ExecuteRead<PredicateRevisionPageV2<PredicateGroupRevisionSummary>>(
        [this,query]() { return inner_ ? inner_->ListPredicateGroupRevisions(query)
                                      : PredicateRevisionPageV2<PredicateGroupRevisionSummary>{}; },{});
}

bool QueuedAuthoringDb::AbandonPredicateGroupDraft(
    std::int64_t revision_id, std::string* error_out) {
    return ExecuteWrite<bool>([this,revision_id,error_out]() {
        return inner_ && inner_->AbandonPredicateGroupDraft(revision_id,error_out);},false,error_out);
}

bool QueuedAuthoringDb::UpdatePredicateAuthoringMetadata(
    const UpdatePredicateAuthoringMetadataCommand& command,
    bool* changed_out, std::string* error_out) {
    return ExecuteWrite<bool>([this,command,changed_out,error_out]() {
        return inner_ && inner_->UpdatePredicateAuthoringMetadata(
            command,changed_out,error_out);},false,error_out);
}

bool QueuedAuthoringDb::SaveSeedProbeSpec(
    const SaveSeedProbeSpecCommand& command,
    std::int64_t* seed_probe_spec_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, seed_probe_spec_id_out, error_out]() {
            return inner_ != nullptr ? inner_->SaveSeedProbeSpec(command, seed_probe_spec_id_out, error_out) : false;
        },
        false,
        error_out);
}

std::optional<SeedProbeSpecSnapshot> QueuedAuthoringDb::GetSeedProbeSpec(
    std::int64_t seed_probe_spec_id) const {
    return ExecuteRead<std::optional<SeedProbeSpecSnapshot>>(
        [this, seed_probe_spec_id]() {
            return inner_ != nullptr ? inner_->GetSeedProbeSpec(seed_probe_spec_id) : std::nullopt;
        },
        std::nullopt);
}

std::vector<SeedProbeSpecSnapshot> QueuedAuthoringDb::ListSeedProbeSpecs(
    int max_count) const {
    return ExecuteRead<std::vector<SeedProbeSpecSnapshot>>(
        [this, max_count]() {
            return inner_ != nullptr ? inner_->ListSeedProbeSpecs(max_count) : std::vector<SeedProbeSpecSnapshot>{};
        },
        {});
}

bool QueuedAuthoringDb::EnsureAuthoringInputSet(
    const EnsureAuthoringInputSetCommand& command,
    std::int64_t* input_set_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, input_set_id_out, error_out]() {
            return inner_ != nullptr ? inner_->EnsureAuthoringInputSet(command, input_set_id_out, error_out) : false;
        },
        false,
        error_out);
}

std::vector<AuthoringInputSetFrameSnapshot> QueuedAuthoringDb::ListAuthoringInputSetFrames(
    std::int64_t input_set_id) const {
    return ExecuteRead<std::vector<AuthoringInputSetFrameSnapshot>>(
        [this, input_set_id]() {
            return inner_ != nullptr ? inner_->ListAuthoringInputSetFrames(input_set_id) : std::vector<AuthoringInputSetFrameSnapshot>{};
        },
        {});
}

bool QueuedAuthoringDb::SaveTasSpec(
    const SaveTasSpecCommand& command,
    std::int64_t* tas_spec_id_out,
    std::int64_t* tas_spec_base_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, tas_spec_id_out, tas_spec_base_id_out, error_out]() {
            return inner_ != nullptr ? inner_->SaveTasSpec(command, tas_spec_id_out, tas_spec_base_id_out, error_out) : false;
        },
        false,
        error_out);
}

std::optional<TasSpecSnapshot> QueuedAuthoringDb::GetTasSpec(
    std::int64_t tas_spec_id) const {
    return ExecuteRead<std::optional<TasSpecSnapshot>>(
        [this, tas_spec_id]() {
            return inner_ != nullptr ? inner_->GetTasSpec(tas_spec_id) : std::nullopt;
        },
        std::nullopt);
}

std::vector<TasSpecSnapshot> QueuedAuthoringDb::ListTasSpecs(
    int max_count) const {
    return ExecuteRead<std::vector<TasSpecSnapshot>>(
        [this, max_count]() {
            return inner_ != nullptr ? inner_->ListTasSpecs(max_count) : std::vector<TasSpecSnapshot>{};
        },
        {});
}

bool QueuedAuthoringDb::SavePlan(
    const SavePlanCommand& command,
    std::int64_t* plan_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, plan_id_out, error_out]() {
            return inner_ != nullptr ? inner_->SavePlan(command, plan_id_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAuthoringDb::SaveBattlePlanActionPreset(
    const SaveBattlePlanActionPresetCommand& command,
    std::int64_t* action_preset_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, action_preset_id_out, error_out]() {
            return inner_ != nullptr ? inner_->SaveBattlePlanActionPreset(command, action_preset_id_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAuthoringDb::RenameBattlePlanActionPreset(
    const RenameBattlePlanActionPresetCommand& command,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, error_out]() {
            return inner_ != nullptr ? inner_->RenameBattlePlanActionPreset(command, error_out) : false;
        },
        false,
        error_out);
}

std::optional<BattlePlanActionPresetSnapshot> QueuedAuthoringDb::GetBattlePlanActionPreset(
    std::int64_t action_preset_id) const {
    return ExecuteRead<std::optional<BattlePlanActionPresetSnapshot>>(
        [this, action_preset_id]() {
            return inner_ != nullptr ? inner_->GetBattlePlanActionPreset(action_preset_id) : std::nullopt;
        },
        std::nullopt);
}

std::vector<BattlePlanActionPresetSnapshot> QueuedAuthoringDb::ListBattlePlanActionPresets(
    int max_count) const {
    return ExecuteRead<std::vector<BattlePlanActionPresetSnapshot>>(
        [this, max_count]() {
            return inner_ != nullptr ? inner_->ListBattlePlanActionPresets(max_count) : std::vector<BattlePlanActionPresetSnapshot>{};
        },
        {});
}

bool QueuedAuthoringDb::SaveBattlePlanTurn(
    const SaveBattlePlanTurnCommand& command,
    std::int64_t* plan_turn_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, plan_turn_id_out, error_out]() {
            return inner_ != nullptr ? inner_->SaveBattlePlanTurn(command, plan_turn_id_out, error_out) : false;
        },
        false,
        error_out);
}

std::optional<BattlePlanSnapshot> QueuedAuthoringDb::GetBattlePlan(
    std::int64_t plan_id) const {
    return ExecuteRead<std::optional<BattlePlanSnapshot>>(
        [this, plan_id]() {
            return inner_ != nullptr ? inner_->GetBattlePlan(plan_id) : std::nullopt;
        },
        std::nullopt);
}

std::vector<BattlePlanSnapshot> QueuedAuthoringDb::ListBattlePlans(
    int max_count) const {
    return ExecuteRead<std::vector<BattlePlanSnapshot>>(
        [this, max_count]() {
            return inner_ != nullptr ? inner_->ListBattlePlans(max_count) : std::vector<BattlePlanSnapshot>{};
        },
        {});
}

bool QueuedAuthoringDb::SaveWorkflowGraph(
    const SaveWorkflowGraphCommand& command,
    SaveWorkflowGraphResult* result_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, result_out, error_out]() {
            return inner_ != nullptr ? inner_->SaveWorkflowGraph(command, result_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAuthoringDb::SetWorkflowGraphHidden(
    std::int64_t workflow_graph_id,
    bool hidden,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, workflow_graph_id, hidden, error_out]() {
            return inner_ != nullptr ? inner_->SetWorkflowGraphHidden(workflow_graph_id, hidden, error_out) : false;
        },
        false,
        error_out);
}

std::optional<WorkflowGraphSnapshot> QueuedAuthoringDb::GetWorkflowGraph(
    std::int64_t workflow_graph_id) const {
    return ExecuteRead<std::optional<WorkflowGraphSnapshot>>(
        [this, workflow_graph_id]() {
            return inner_ != nullptr ? inner_->GetWorkflowGraph(workflow_graph_id) : std::nullopt;
        },
        std::nullopt);
}

std::optional<WorkflowGraphSnapshot> QueuedAuthoringDb::GetWorkflowGraphRevision(
    std::int64_t workflow_graph_revision_id) const {
    return ExecuteRead<std::optional<WorkflowGraphSnapshot>>(
        [this, workflow_graph_revision_id]() {
            return inner_ != nullptr ? inner_->GetWorkflowGraphRevision(workflow_graph_revision_id) : std::nullopt;
        },
        std::nullopt);
}

std::vector<WorkflowGraphSnapshot> QueuedAuthoringDb::ListWorkflowGraphs(
    int max_count,
    bool include_hidden) const {
    return ExecuteRead<std::vector<WorkflowGraphSnapshot>>(
        [this, max_count, include_hidden]() {
            return inner_ != nullptr ? inner_->ListWorkflowGraphs(max_count, include_hidden) : std::vector<WorkflowGraphSnapshot>{};
        },
        {});
}

std::vector<events::EventEnvelope> QueuedAuthoringDb::ReadUnpublishedOutboxBatch(
    std::int64_t after_outbox_id,
    int max_batch_size) {
    return ExecuteRead<std::vector<events::EventEnvelope>>(
        [this, after_outbox_id, max_batch_size]() {
            return inner_ != nullptr
                ? inner_->ReadUnpublishedOutboxBatch(after_outbox_id, max_batch_size)
                : std::vector<events::EventEnvelope>{};
        },
        {});
}

bool QueuedAuthoringDb::MarkOutboxPublished(
    std::int64_t outbox_id,
    types::UtcTimePoint published_at_utc) {
    return ExecuteWrite<bool>(
        [this, outbox_id, published_at_utc]() {
            return inner_ != nullptr ? inner_->MarkOutboxPublished(outbox_id, published_at_utc) : false;
        },
        false);
}

bool QueuedAuthoringDb::MarkOutboxPublishFailure(
    std::int64_t outbox_id,
    std::string_view last_error) {
    const auto last_error_copy = std::string(last_error);
    return ExecuteWrite<bool>(
        [this, outbox_id, last_error_copy]() {
            return inner_ != nullptr ? inner_->MarkOutboxPublishFailure(outbox_id, last_error_copy) : false;
        },
        false);
}

retention::OutboxRetentionPreview QueuedAuthoringDb::PreviewOutboxRetention(
    const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
    types::UtcTimePoint now_utc,
    const retention::OutboxRetentionPolicy& policy) const {
    return ExecuteRead<retention::OutboxRetentionPreview>(
        [this, subscriptions, now_utc, policy]() {
            return inner_ != nullptr ? inner_->PreviewOutboxRetention(subscriptions, now_utc, policy) : retention::OutboxRetentionPreview{};
        },
        {});
}

bool QueuedAuthoringDb::PurgeOutboxThroughRetentionFloor(
    const std::vector<retention::OutboxSubscriptionSnapshot>& subscriptions,
    types::UtcTimePoint now_utc,
    const retention::OutboxRetentionPolicy& policy,
    int max_rows,
    int* rows_deleted_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, subscriptions, now_utc, policy, max_rows, rows_deleted_out, error_out]() {
            return inner_ != nullptr
                ? inner_->PurgeOutboxThroughRetentionFloor(subscriptions, now_utc, policy, max_rows, rows_deleted_out, error_out)
                : false;
        },
        false,
        error_out);
}

std::optional<AuthoringPayloadRecord> QueuedAuthoringDb::ResolveAuthoringPayload(
    const events::EventEnvelope& envelope) const {
    return ExecuteRead<std::optional<AuthoringPayloadRecord>>(
        [this, envelope]() {
            return inner_ != nullptr ? inner_->ResolveAuthoringPayload(envelope) : std::nullopt;
        },
        std::nullopt);
}

std::optional<AuthoringPayloadRecord> QueuedAuthoringDb::ResolveAuthoringPayload(
    std::string_view event_type,
    int event_version,
    std::string_view payload_ref_kind,
    std::int64_t payload_ref_id) const {
    const auto event_type_copy = std::string(event_type);
    const auto payload_ref_kind_copy = std::string(payload_ref_kind);
    return ExecuteRead<std::optional<AuthoringPayloadRecord>>(
        [this, event_type_copy, event_version, payload_ref_kind_copy, payload_ref_id]() {
            return inner_ != nullptr
                ? inner_->ResolveAuthoringPayload(event_type_copy, event_version, payload_ref_kind_copy, payload_ref_id)
                : std::nullopt;
        },
        std::nullopt);
}

template <typename Result, typename Fn>
Result QueuedAuthoringDb::ExecuteRead(
    Fn&& fn,
    Result fallback,
    std::string* error_out,
    const std::source_location& location) const {
    return core::QueuedDbExecutor::ExecuteQueued<Result>(
        *read_lane_,
        sqlite_call_mtx_,
        core::MakeQueuedDbOperationName("Authoring", location),
        std::forward<Fn>(fn),
        std::move(fallback),
        error_out);
}

template <typename Result, typename Fn>
Result QueuedAuthoringDb::ExecuteWrite(
    Fn&& fn,
    Result fallback,
    std::string* error_out,
    const std::source_location& location) const {
    return core::QueuedDbExecutor::ExecuteQueued<Result>(
        *write_lane_,
        sqlite_call_mtx_,
        core::MakeQueuedDbOperationName("Authoring", location),
        std::forward<Fn>(fn),
        std::move(fallback),
        error_out);
}

} // namespace savor::db
