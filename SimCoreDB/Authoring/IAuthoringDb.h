#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../Common/Events/EventEnvelope.h"
#include "../Common/Types/UtcTimestamp.h"

namespace simcore::db {

struct AuthoringPayloadRecord {
    std::int64_t template_id = 0;
    std::int64_t seed_probe_spec_id = 0;
    std::int64_t battle_run_spec_id = 0;
};

struct SaveSeedProbeSpecCommand {
    std::string name;
    int priority = 0;
    std::int64_t run_ms = 0;
    std::int64_t vi_stall_ms = 0;
    bool clear_result_winners = false;
    int samples_per_axis = 0;
    std::int64_t min_value = 0;
    std::int64_t max_value = 0;
    bool cap_trigger_top = false;
    bool ignore_trigger_minmax = false;
    int combo_attempts_per_target = 0;
    int combo_sampler_tries = 0;
    bool auto_schedule_battle_run = false;
    types::UtcTimePoint created_at_utc{};
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct SaveTasSpecCommand {
    std::string base_name;
    int priority = 0;
    std::int64_t run_ms = 0;
    std::int64_t vi_stall_ms = 0;
    int headroom_x10 = 0;
    bool progress_enable = false;
    bool auto_queue_seeds = false;
    std::int64_t base_dtm_artifact_id = 0;
    std::int64_t rtc_low = 0;
    std::int64_t rtc_high = 0;
    types::UtcTimePoint created_at_utc{};
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct SaveBattleRunSpecCommand {
    std::string name;
    int priority = 0;
    std::int64_t run_ms = 0;
    std::int64_t vi_stall_ms = 0;
    bool progress_enable = false;
    bool use_single_turn_runner = false;
    bool auto_wave_trigger_enable = false;
    int min_fake_attacks = 0;
    int max_fake_attacks = 0;
    types::UtcTimePoint created_at_utc{};
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct SavePlanCommand {
    std::string name;
    std::string fingerprint;
    int num_turns = 0;
    types::UtcTimePoint created_at_utc{};
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct SavePredicateSpecCommand {
    std::string name;
    std::string breakpoint_name;
    std::string lhs_kind;
    std::int64_t lhs_value = 0;
    std::string rhs_kind;
    std::int64_t rhs_value = 0;
    std::string cmp_op;
    std::optional<std::int64_t> flag_mask;
    std::optional<std::int64_t> value_mask;
    bool abort_on_fail = false;
    types::UtcTimePoint created_at_utc{};
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct SaveExplorerSettingsCommand {
    std::string name;
    std::string description;
    std::optional<std::int64_t> default_plan_id;
    std::optional<std::int64_t> default_predicate_set_id;
    types::UtcTimePoint created_at_utc{};
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct SaveTemplateCommand {
    std::string name;
    std::string description;
    std::optional<std::int64_t> seed_probe_spec_id;
    std::optional<std::int64_t> tas_spec_id;
    std::optional<std::int64_t> battle_run_spec_id;
    std::optional<std::int64_t> explorer_settings_id;
    types::UtcTimePoint created_at_utc{};
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct IAuthoringDb {
    virtual ~IAuthoringDb() = default;

    virtual bool SaveSeedProbeSpec(
        const SaveSeedProbeSpecCommand& command,
        std::int64_t* seed_probe_spec_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool SaveTasSpec(
        const SaveTasSpecCommand& command,
        std::int64_t* tas_spec_id_out = nullptr,
        std::int64_t* tas_spec_base_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool SaveBattleRunSpec(
        const SaveBattleRunSpecCommand& command,
        std::int64_t* battle_run_spec_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool SavePlan(
        const SavePlanCommand& command,
        std::int64_t* plan_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool SavePredicateSpec(
        const SavePredicateSpecCommand& command,
        std::int64_t* predicate_spec_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool SaveExplorerSettings(
        const SaveExplorerSettingsCommand& command,
        std::int64_t* explorer_settings_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool SaveTemplate(
        const SaveTemplateCommand& command,
        std::int64_t* template_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual std::vector<events::EventEnvelope> ReadUnpublishedOutboxBatch(
        std::int64_t after_outbox_id,
        int max_batch_size) = 0;

    virtual bool MarkOutboxPublished(
        std::int64_t outbox_id,
        types::UtcTimePoint published_at_utc) = 0;

    virtual bool MarkOutboxPublishFailure(
        std::int64_t outbox_id,
        std::string_view last_error) = 0;

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

} // namespace simcore::db
