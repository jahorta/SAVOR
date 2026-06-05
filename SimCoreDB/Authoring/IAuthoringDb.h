#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../Common/Events/EventEnvelope.h"
#include "../Common/Types/UtcTimestamp.h"
#include "../Common/Retention/OutboxRetention.h"
#include "../../SimCore/Core/Input/SoaBattle/ActionTypes.h"
#include "../../SimCore/Runner/Breakpoints/BPRegistry.h"
#include "../../SimCore/Runner/Breakpoints/Predicate.h"

namespace simcore::db {

enum class BattlePlanTargetKind : int {
    SingleEnemy = 0,
    MultipleEnemies = 1,
    AnyEnemy = 2,
    SameAsOtherPC = 3,
};

using BattlePlanActionMacro = soa::battle::actions::BattleAction;

enum class PredicateOperandKind {
    Unknown = 0,
    Absolute,
    Delta,
    Literal,
    Memory,
};

using PredicateComparisonOp = simcore::pred::CmpOp;

inline std::string_view ToDbString(PredicateOperandKind value) {
    switch (value) {
    case PredicateOperandKind::Absolute: return "ABS";
    case PredicateOperandKind::Delta: return "DELTA";
    case PredicateOperandKind::Literal: return "literal";
    case PredicateOperandKind::Memory: return "mem";
    default: return "";
    }
}

inline PredicateOperandKind ParsePredicateOperandKind(std::string_view value) {
    if (value == "ABS" || value == "abs") return PredicateOperandKind::Absolute;
    if (value == "DELTA" || value == "delta") return PredicateOperandKind::Delta;
    if (value == "literal" || value == "LITERAL") return PredicateOperandKind::Literal;
    if (value == "mem" || value == "MEM" || value == "memory" || value == "MEMORY") return PredicateOperandKind::Memory;
    return PredicateOperandKind::Unknown;
}

inline simcore::pred::PredKind ToRuntimePredicateKind(PredicateOperandKind value) {
    return value == PredicateOperandKind::Delta ? simcore::pred::PredKind::DELTA : simcore::pred::PredKind::ABS;
}

inline std::string_view ToDbString(PredicateComparisonOp value) {
    switch (value) {
    case PredicateComparisonOp::EQ: return "EQ";
    case PredicateComparisonOp::NE: return "NE";
    case PredicateComparisonOp::LT: return "LT";
    case PredicateComparisonOp::LE: return "LE";
    case PredicateComparisonOp::GT: return "GT";
    case PredicateComparisonOp::GE: return "GE";
    default: return "";
    }
}

inline PredicateComparisonOp ParsePredicateComparisonOp(std::string_view value) {
    if (value == "NE" || value == "!=") return PredicateComparisonOp::NE;
    if (value == "LT" || value == "<") return PredicateComparisonOp::LT;
    if (value == "LE" || value == "<=") return PredicateComparisonOp::LE;
    if (value == "GT" || value == ">") return PredicateComparisonOp::GT;
    if (value == "GE" || value == ">=") return PredicateComparisonOp::GE;
    return PredicateComparisonOp::EQ;
}

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

struct SeedProbeSpecSnapshot {
    std::int64_t seed_probe_spec_id = 0;
    std::string name;
    int priority = 0;
    std::int64_t run_ms = 0;
    std::int64_t vi_stall_ms = 0;
    int samples_per_axis = 0;
    std::int64_t min_value = 0;
    std::int64_t max_value = 0;
    bool cap_trigger_top = false;
    bool ignore_trigger_minmax = false;
    int combo_attempts_per_target = 0;
    int combo_sampler_tries = 0;
    bool auto_schedule_battle_run = false;
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

struct SaveBattlePlanActionCommand {
    int actor_slot = 0;
    BattlePlanActionMacro macro = BattlePlanActionMacro::Attack;
    BattlePlanTargetKind target_kind = BattlePlanTargetKind::SingleEnemy;
    std::optional<int> target_slot;
    std::optional<int> target_mask_bits;
    std::optional<int> target_single_slot;
    std::optional<int> target_same_as_actor_slot;
    std::optional<std::string> target_expr_ini;
    std::optional<int> item_id;
    int ordinal = 0;
};

struct SaveBattlePlanTurnCommand {
    std::int64_t plan_id = 0;
    int turn_index = 0;
    std::vector<SaveBattlePlanActionCommand> actions;
    bool replace_existing_actions = true;
    types::UtcTimePoint created_at_utc{};
    std::string event_id;
    std::string correlation_id;
    std::string causation_id;
};

struct EnsureAddressProgramCommand {
    int program_version = 0;
    std::vector<std::uint8_t> prog_bytes;
    std::optional<int> derived_buffer_version;
    std::optional<std::string> derived_buffer_schema_hash;
    std::optional<std::string> soa_structs_hash;
    std::string description;
};

struct SavePredicateSpecCommand {
    std::string name;
    BPKey breakpoint_id = 0;
    PredicateOperandKind lhs_kind = PredicateOperandKind::Unknown;
    std::int64_t lhs_value = 0;
    PredicateOperandKind rhs_kind = PredicateOperandKind::Unknown;
    std::int64_t rhs_value = 0;
    PredicateComparisonOp cmp_op = PredicateComparisonOp::EQ;
    int width = 0;
    std::optional<std::int64_t> flag_mask;
    std::optional<std::int64_t> value_mask;
    std::optional<std::int64_t> lhs_address_program_id;
    std::optional<std::int64_t> rhs_address_program_id;
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

struct SavePredicateSetCommand {
    std::vector<std::int64_t> predicate_spec_ids;
    types::UtcTimePoint created_at_utc{};
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

struct BattleRunSpecSnapshot {
    std::int64_t battle_run_spec_id = 0;
    std::string name;
    int priority = 0;
    std::int64_t run_ms = 0;
    std::int64_t vi_stall_ms = 0;
    bool progress_enable = false;
    bool use_single_turn_runner = false;
    bool auto_wave_trigger_enable = false;
    int min_fake_attacks = 0;
    int max_fake_attacks = 0;
};

struct BattlePlanActionSnapshot {
    std::int64_t plan_action_id = 0;
    std::int64_t plan_turn_id = 0;
    int actor_slot = 0;
    BattlePlanActionMacro macro = BattlePlanActionMacro::Attack;
    BattlePlanTargetKind target_kind = BattlePlanTargetKind::SingleEnemy;
    std::optional<int> target_slot;
    std::optional<int> target_mask_bits;
    std::optional<int> target_single_slot;
    std::optional<int> target_same_as_actor_slot;
    std::optional<std::string> target_expr_ini;
    std::optional<int> item_id;
    int ordinal = 0;
};

struct BattlePlanTurnSnapshot {
    std::int64_t plan_turn_id = 0;
    std::int64_t plan_id = 0;
    int turn_index = 0;
    std::vector<BattlePlanActionSnapshot> actions;
};

struct BattlePlanSnapshot {
    std::int64_t plan_id = 0;
    std::string name;
    std::string fingerprint;
    int num_turns = 0;
    std::vector<BattlePlanTurnSnapshot> turns;
};

struct AddressProgramSnapshot {
    std::int64_t address_program_id = 0;
    int program_version = 0;
    std::vector<std::uint8_t> prog_bytes;
    std::optional<int> derived_buffer_version;
    std::optional<std::string> derived_buffer_schema_hash;
    std::optional<std::string> soa_structs_hash;
    std::string description;
};

struct PredicateSpecSnapshot {
    std::int64_t predicate_spec_id = 0;
    std::string name;
    BPKey breakpoint_id = 0;
    PredicateOperandKind lhs_kind = PredicateOperandKind::Unknown;
    std::int64_t lhs_value = 0;
    PredicateOperandKind rhs_kind = PredicateOperandKind::Unknown;
    std::int64_t rhs_value = 0;
    PredicateComparisonOp cmp_op = PredicateComparisonOp::EQ;
    int width = 0;
    std::optional<std::int64_t> flag_mask;
    std::optional<std::int64_t> value_mask;
    std::optional<std::int64_t> lhs_address_program_id;
    std::optional<std::int64_t> rhs_address_program_id;
    bool abort_on_fail = false;
};

struct PredicateSetSnapshot {
    std::int64_t predicate_set_id = 0;
    std::vector<PredicateSpecSnapshot> predicates;
};

struct ExplorerSettingsSnapshot {
    std::int64_t explorer_settings_id = 0;
    std::string name;
    std::string description;
    std::optional<std::int64_t> default_plan_id;
    std::optional<std::int64_t> default_predicate_set_id;
};

struct IAuthoringDb {
    virtual ~IAuthoringDb() = default;

    virtual bool SaveSeedProbeSpec(
        const SaveSeedProbeSpecCommand& command,
        std::int64_t* seed_probe_spec_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual std::optional<SeedProbeSpecSnapshot> GetSeedProbeSpec(
        std::int64_t seed_probe_spec_id) const = 0;

    virtual bool SaveTasSpec(
        const SaveTasSpecCommand& command,
        std::int64_t* tas_spec_id_out = nullptr,
        std::int64_t* tas_spec_base_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool SaveBattleRunSpec(
        const SaveBattleRunSpecCommand& command,
        std::int64_t* battle_run_spec_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual std::optional<BattleRunSpecSnapshot> GetBattleRunSpec(
        std::int64_t battle_run_spec_id) const = 0;

    virtual bool SavePlan(
        const SavePlanCommand& command,
        std::int64_t* plan_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual bool SaveBattlePlanTurn(
        const SaveBattlePlanTurnCommand& command,
        std::int64_t* plan_turn_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual std::optional<BattlePlanSnapshot> GetBattlePlan(
        std::int64_t plan_id) const = 0;

    virtual bool EnsureAddressProgram(
        const EnsureAddressProgramCommand& command,
        std::int64_t* address_program_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual std::optional<AddressProgramSnapshot> GetAddressProgram(
        std::int64_t address_program_id) const = 0;

    virtual bool SavePredicateSpec(
        const SavePredicateSpecCommand& command,
        std::int64_t* predicate_spec_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual std::optional<PredicateSpecSnapshot> GetPredicateSpec(
        std::int64_t predicate_spec_id) const = 0;

    virtual bool SavePredicateSet(
        const SavePredicateSetCommand& command,
        std::int64_t* predicate_set_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual std::optional<PredicateSetSnapshot> GetPredicateSet(
        std::int64_t predicate_set_id) const = 0;

    virtual bool SaveExplorerSettings(
        const SaveExplorerSettingsCommand& command,
        std::int64_t* explorer_settings_id_out = nullptr,
        std::string* error_out = nullptr) = 0;

    virtual std::optional<ExplorerSettingsSnapshot> GetExplorerSettings(
        std::int64_t explorer_settings_id) const = 0;

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

} // namespace simcore::db
