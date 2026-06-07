#include "QueuedAuthoringDb.h"

#include <utility>

namespace simcore::db {
namespace {

void SetError(std::string* error_out, std::string message) {
    if (error_out != nullptr) {
        *error_out = std::move(message);
    }
}

core::QueuedDbTelemetrySnapshot BuildTelemetrySnapshot(
    const core::QueuedDbLane* write_lane,
    const core::QueuedDbLane* read_lane) {
    core::QueuedDbTelemetrySnapshot snapshot{};
    if (write_lane != nullptr) {
        const auto lane = write_lane->GetTelemetrySnapshot();
        snapshot.write_depth = lane.depth;
        snapshot.write_enqueued = lane.enqueued;
        snapshot.write_rejected = lane.rejected;
        snapshot.write_completed = lane.completed;
        snapshot.write_failed = lane.failed;
    }
    if (read_lane != nullptr) {
        const auto lane = read_lane->GetTelemetrySnapshot();
        snapshot.read_depth = lane.depth;
        snapshot.read_enqueued = lane.enqueued;
        snapshot.read_rejected = lane.rejected;
        snapshot.read_completed = lane.completed;
        snapshot.read_failed = lane.failed;
    }
    return snapshot;
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

bool QueuedAuthoringDb::SaveBattleRunSpec(
    const SaveBattleRunSpecCommand& command,
    std::int64_t* battle_run_spec_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, battle_run_spec_id_out, error_out]() {
            return inner_ != nullptr ? inner_->SaveBattleRunSpec(command, battle_run_spec_id_out, error_out) : false;
        },
        false,
        error_out);
}

std::optional<BattleRunSpecSnapshot> QueuedAuthoringDb::GetBattleRunSpec(
    std::int64_t battle_run_spec_id) const {
    return ExecuteRead<std::optional<BattleRunSpecSnapshot>>(
        [this, battle_run_spec_id]() {
            return inner_ != nullptr ? inner_->GetBattleRunSpec(battle_run_spec_id) : std::nullopt;
        },
        std::nullopt);
}

std::vector<BattleRunSpecSnapshot> QueuedAuthoringDb::ListBattleRunSpecs(
    int max_count) const {
    return ExecuteRead<std::vector<BattleRunSpecSnapshot>>(
        [this, max_count]() {
            return inner_ != nullptr ? inner_->ListBattleRunSpecs(max_count) : std::vector<BattleRunSpecSnapshot>{};
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

bool QueuedAuthoringDb::EnsureAddressProgram(
    const EnsureAddressProgramCommand& command,
    std::int64_t* address_program_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, address_program_id_out, error_out]() {
            return inner_ != nullptr ? inner_->EnsureAddressProgram(command, address_program_id_out, error_out) : false;
        },
        false,
        error_out);
}

std::optional<AddressProgramSnapshot> QueuedAuthoringDb::GetAddressProgram(
    std::int64_t address_program_id) const {
    return ExecuteRead<std::optional<AddressProgramSnapshot>>(
        [this, address_program_id]() {
            return inner_ != nullptr ? inner_->GetAddressProgram(address_program_id) : std::nullopt;
        },
        std::nullopt);
}

bool QueuedAuthoringDb::SavePredicateSpec(
    const SavePredicateSpecCommand& command,
    std::int64_t* predicate_spec_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, predicate_spec_id_out, error_out]() {
            return inner_ != nullptr ? inner_->SavePredicateSpec(command, predicate_spec_id_out, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAuthoringDb::UpdatePredicateSpec(
    std::int64_t predicate_spec_id,
    const SavePredicateSpecCommand& command,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, predicate_spec_id, command, error_out]() {
            return inner_ != nullptr ? inner_->UpdatePredicateSpec(predicate_spec_id, command, error_out) : false;
        },
        false,
        error_out);
}

bool QueuedAuthoringDb::DeletePredicateSpec(
    const DeletePredicateSpecCommand& command,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, error_out]() {
            return inner_ != nullptr ? inner_->DeletePredicateSpec(command, error_out) : false;
        },
        false,
        error_out);
}

std::optional<PredicateSpecSnapshot> QueuedAuthoringDb::GetPredicateSpec(
    std::int64_t predicate_spec_id) const {
    return ExecuteRead<std::optional<PredicateSpecSnapshot>>(
        [this, predicate_spec_id]() {
            return inner_ != nullptr ? inner_->GetPredicateSpec(predicate_spec_id) : std::nullopt;
        },
        std::nullopt);
}

PredicateSpecUsageSnapshot QueuedAuthoringDb::GetPredicateSpecUsage(
    std::int64_t predicate_spec_id) const {
    return ExecuteRead<PredicateSpecUsageSnapshot>(
        [this, predicate_spec_id]() {
            return inner_ != nullptr ? inner_->GetPredicateSpecUsage(predicate_spec_id) : PredicateSpecUsageSnapshot{};
        },
        PredicateSpecUsageSnapshot{ .predicate_spec_id = predicate_spec_id });
}

std::vector<PredicateSpecSnapshot> QueuedAuthoringDb::ListPredicateSpecs(
    int max_count) const {
    return ExecuteRead<std::vector<PredicateSpecSnapshot>>(
        [this, max_count]() {
            return inner_ != nullptr ? inner_->ListPredicateSpecs(max_count) : std::vector<PredicateSpecSnapshot>{};
        },
        {});
}

bool QueuedAuthoringDb::SavePredicateSet(
    const SavePredicateSetCommand& command,
    std::int64_t* predicate_set_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, predicate_set_id_out, error_out]() {
            return inner_ != nullptr ? inner_->SavePredicateSet(command, predicate_set_id_out, error_out) : false;
        },
        false,
        error_out);
}

std::optional<PredicateSetSnapshot> QueuedAuthoringDb::GetPredicateSet(
    std::int64_t predicate_set_id) const {
    return ExecuteRead<std::optional<PredicateSetSnapshot>>(
        [this, predicate_set_id]() {
            return inner_ != nullptr ? inner_->GetPredicateSet(predicate_set_id) : std::nullopt;
        },
        std::nullopt);
}

std::vector<PredicateSetSnapshot> QueuedAuthoringDb::ListPredicateSets(
    int max_count) const {
    return ExecuteRead<std::vector<PredicateSetSnapshot>>(
        [this, max_count]() {
            return inner_ != nullptr ? inner_->ListPredicateSets(max_count) : std::vector<PredicateSetSnapshot>{};
        },
        {});
}

bool QueuedAuthoringDb::SaveExplorerSettings(
    const SaveExplorerSettingsCommand& command,
    std::int64_t* explorer_settings_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, explorer_settings_id_out, error_out]() {
            return inner_ != nullptr ? inner_->SaveExplorerSettings(command, explorer_settings_id_out, error_out) : false;
        },
        false,
        error_out);
}

std::optional<ExplorerSettingsSnapshot> QueuedAuthoringDb::GetExplorerSettings(
    std::int64_t explorer_settings_id) const {
    return ExecuteRead<std::optional<ExplorerSettingsSnapshot>>(
        [this, explorer_settings_id]() {
            return inner_ != nullptr ? inner_->GetExplorerSettings(explorer_settings_id) : std::nullopt;
        },
        std::nullopt);
}

std::vector<ExplorerSettingsSnapshot> QueuedAuthoringDb::ListExplorerSettings(
    int max_count) const {
    return ExecuteRead<std::vector<ExplorerSettingsSnapshot>>(
        [this, max_count]() {
            return inner_ != nullptr ? inner_->ListExplorerSettings(max_count) : std::vector<ExplorerSettingsSnapshot>{};
        },
        {});
}

bool QueuedAuthoringDb::SaveBattleChainSpec(
    const SaveBattleChainSpecCommand& command,
    std::int64_t* battle_chain_spec_id_out,
    std::string* error_out) {
    return ExecuteWrite<bool>(
        [this, command, battle_chain_spec_id_out, error_out]() {
            return inner_ != nullptr ? inner_->SaveBattleChainSpec(command, battle_chain_spec_id_out, error_out) : false;
        },
        false,
        error_out);
}

std::optional<BattleChainSpecSnapshot> QueuedAuthoringDb::GetBattleChainSpec(
    std::int64_t battle_chain_spec_id) const {
    return ExecuteRead<std::optional<BattleChainSpecSnapshot>>(
        [this, battle_chain_spec_id]() {
            return inner_ != nullptr ? inner_->GetBattleChainSpec(battle_chain_spec_id) : std::nullopt;
        },
        std::nullopt);
}

std::vector<BattleChainSpecSnapshot> QueuedAuthoringDb::ListBattleChainSpecs(
    int max_count) const {
    return ExecuteRead<std::vector<BattleChainSpecSnapshot>>(
        [this, max_count]() {
            return inner_ != nullptr ? inner_->ListBattleChainSpecs(max_count) : std::vector<BattleChainSpecSnapshot>{};
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
Result QueuedAuthoringDb::ExecuteRead(Fn&& fn, Result fallback, std::string* error_out) const {
    return core::QueuedDbExecutor::ExecuteQueued<Result>(
        *read_lane_,
        sqlite_call_mtx_,
        std::forward<Fn>(fn),
        std::move(fallback),
        error_out);
}

template <typename Result, typename Fn>
Result QueuedAuthoringDb::ExecuteWrite(Fn&& fn, Result fallback, std::string* error_out) const {
    return core::QueuedDbExecutor::ExecuteQueued<Result>(
        *write_lane_,
        sqlite_call_mtx_,
        std::forward<Fn>(fn),
        std::move(fallback),
        error_out);
}

} // namespace simcore::db
