#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../Common/QueuedDb.h"
#include "IAuthoringDb.h"

namespace savor::db {

class QueuedAuthoringDb final : public IAuthoringDb, private core::QueuedDbExecutor {
public:
    explicit QueuedAuthoringDb(
        IAuthoringDb* inner,
        core::QueuedDbConfig config = {});
    ~QueuedAuthoringDb() override;

    QueuedAuthoringDb(const QueuedAuthoringDb&) = delete;
    QueuedAuthoringDb& operator=(const QueuedAuthoringDb&) = delete;

    bool Start(std::string* error_out = nullptr);
    void Stop();
    [[nodiscard]] bool IsRunning() const;
    [[nodiscard]] core::QueuedDbTelemetrySnapshot GetTelemetrySnapshot() const;

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
    bool SaveTasSpec(
        const SaveTasSpecCommand& command,
        std::int64_t* tas_spec_id_out = nullptr,
        std::int64_t* tas_spec_base_id_out = nullptr,
        std::string* error_out = nullptr) override;
    std::optional<TasSpecSnapshot> GetTasSpec(
        std::int64_t tas_spec_id) const override;
    std::vector<TasSpecSnapshot> ListTasSpecs(
        int max_count) const override;
    bool SaveBattleRunSpec(
        const SaveBattleRunSpecCommand& command,
        std::int64_t* battle_run_spec_id_out = nullptr,
        std::string* error_out = nullptr) override;
    std::optional<BattleRunSpecSnapshot> GetBattleRunSpec(
        std::int64_t battle_run_spec_id) const override;
    std::vector<BattleRunSpecSnapshot> ListBattleRunSpecs(
        int max_count) const override;
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
    bool EnsureAddressProgram(
        const EnsureAddressProgramCommand& command,
        std::int64_t* address_program_id_out = nullptr,
        std::string* error_out = nullptr) override;
    std::optional<AddressProgramSnapshot> GetAddressProgram(
        std::int64_t address_program_id) const override;
    bool SavePredicateSpec(
        const SavePredicateSpecCommand& command,
        std::int64_t* predicate_spec_id_out = nullptr,
        std::string* error_out = nullptr) override;
    bool UpdatePredicateSpec(
        std::int64_t predicate_spec_id,
        const SavePredicateSpecCommand& command,
        std::string* error_out = nullptr) override;
    bool DeletePredicateSpec(
        const DeletePredicateSpecCommand& command,
        std::string* error_out = nullptr) override;
    std::optional<PredicateSpecSnapshot> GetPredicateSpec(
        std::int64_t predicate_spec_id) const override;
    PredicateSpecUsageSnapshot GetPredicateSpecUsage(
        std::int64_t predicate_spec_id) const override;
    std::vector<PredicateSpecSnapshot> ListPredicateSpecs(
        int max_count) const override;
    bool SavePredicateSet(
        const SavePredicateSetCommand& command,
        std::int64_t* predicate_set_id_out = nullptr,
        std::string* error_out = nullptr) override;
    std::optional<PredicateSetSnapshot> GetPredicateSet(
        std::int64_t predicate_set_id) const override;
    std::vector<PredicateSetSnapshot> ListPredicateSets(
        int max_count) const override;
    bool SaveExplorerSettings(
        const SaveExplorerSettingsCommand& command,
        std::int64_t* explorer_settings_id_out = nullptr,
        std::string* error_out = nullptr) override;
    std::optional<ExplorerSettingsSnapshot> GetExplorerSettings(
        std::int64_t explorer_settings_id) const override;
    std::vector<ExplorerSettingsSnapshot> ListExplorerSettings(
        int max_count) const override;
    bool SaveBattleChainSpec(
        const SaveBattleChainSpecCommand& command,
        std::int64_t* battle_chain_spec_id_out = nullptr,
        std::string* error_out = nullptr) override;
    std::optional<BattleChainSpecSnapshot> GetBattleChainSpec(
        std::int64_t battle_chain_spec_id) const override;
    std::vector<BattleChainSpecSnapshot> ListBattleChainSpecs(
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
    template <typename Result, typename Fn>
    Result ExecuteRead(Fn&& fn, Result fallback, std::string* error_out = nullptr) const;

    template <typename Result, typename Fn>
    Result ExecuteWrite(Fn&& fn, Result fallback, std::string* error_out = nullptr) const;

    IAuthoringDb* inner_ = nullptr;
    core::QueuedDbConfig config_{};
    mutable std::mutex sqlite_call_mtx_;
    std::unique_ptr<core::QueuedDbLane> read_lane_;
    std::unique_ptr<core::QueuedDbLane> write_lane_;
};

} // namespace savor::db
