#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "SimCoreDbRuntime.h"
#include "DB/SimCoreDbServiceResult.h"
#include "Authoring/IAuthoringDb.h"
#include "Common/Types/UtcTimestamp.h"

namespace soasimqt2::db {

struct AddressProgramDraft {
    int program_version = 1;
    std::vector<std::uint8_t> prog_bytes;
    std::optional<int> derived_buffer_version;
    std::optional<std::string> derived_buffer_schema_hash;
    std::optional<std::string> soa_structs_hash;
    std::string description;
};

struct PredicateSpecDraft {
    std::string name;
    int breakpoint_id = 0;
    simcore::db::PredicateOperandKind lhs_kind = simcore::db::PredicateOperandKind::Unknown;
    std::int64_t lhs_value = 0;
    simcore::db::PredicateOperandKind rhs_kind = simcore::db::PredicateOperandKind::Unknown;
    std::int64_t rhs_value = 0;
    simcore::db::PredicateComparisonOp cmp_op = simcore::db::PredicateComparisonOp::EQ;
    int width = 4;
    std::optional<std::int64_t> flag_mask;
    std::optional<std::int64_t> value_mask;
    std::optional<std::int64_t> lhs_address_program_id;
    std::optional<std::int64_t> rhs_address_program_id;
    bool abort_on_fail = false;
};

struct BattlePlanActionDraft {
    int actor_slot = 0;
    simcore::db::BattlePlanActionMacro macro = simcore::db::BattlePlanActionMacro::Attack;
    simcore::db::BattlePlanTargetKind target_kind = simcore::db::BattlePlanTargetKind::SingleEnemy;
    std::optional<int> target_slot;
    std::optional<int> target_mask_bits;
    std::optional<int> target_single_slot;
    std::optional<int> target_same_as_actor_slot;
    std::optional<std::string> target_expr_ini;
    std::optional<int> item_id;
    int ordinal = 0;
};

struct BattlePlanTurnDraft {
    int turn_index = 0;
    std::vector<BattlePlanActionDraft> actions;
};

struct BattlePlanDraft {
    std::string name;
    std::string fingerprint;
    int num_turns = 0;
    std::vector<BattlePlanTurnDraft> turns;
};

struct BattleRunSpecDraft {
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

struct TemplateDraft {
    std::string name;
    std::string description;
    std::optional<std::int64_t> seed_probe_spec_id;
    std::optional<std::int64_t> tas_spec_id;
    std::optional<std::int64_t> battle_run_spec_id;
    std::optional<std::int64_t> explorer_settings_id;
};

class SimCoreDbAuthoringService {
public:
    static ServiceResult<std::int64_t> EnsureAddressProgram(const AddressProgramDraft& draft) {
        auto* db = AuthoringDb();
        if (db == nullptr) {
            return Unavailable<std::int64_t>("SimCoreDB authoring database is not running");
        }

        simcore::db::EnsureAddressProgramCommand command{};
        command.program_version = draft.program_version;
        command.prog_bytes = draft.prog_bytes;
        command.derived_buffer_version = draft.derived_buffer_version;
        command.derived_buffer_schema_hash = draft.derived_buffer_schema_hash;
        command.soa_structs_hash = draft.soa_structs_hash;
        command.description = draft.description;

        std::int64_t id = 0;
        std::string error;
        if (!db->EnsureAddressProgram(command, &id, &error)) {
            return Failed<std::int64_t>(error);
        }
        return ServiceResult<std::int64_t>::Ok(id);
    }

    static ServiceResult<simcore::db::AddressProgramSnapshot> GetAddressProgram(std::int64_t address_program_id) {
        auto* db = AuthoringDb();
        if (db == nullptr) {
            return Unavailable<simcore::db::AddressProgramSnapshot>("SimCoreDB authoring database is not running");
        }
        const auto snapshot = db->GetAddressProgram(address_program_id);
        if (!snapshot.has_value()) {
            return NotFound<simcore::db::AddressProgramSnapshot>("address program not found");
        }
        return ServiceResult<simcore::db::AddressProgramSnapshot>::Ok(*snapshot);
    }

    static ServiceResult<std::int64_t> SavePredicateSpec(const PredicateSpecDraft& draft) {
        auto* db = AuthoringDb();
        if (db == nullptr) {
            return Unavailable<std::int64_t>("SimCoreDB authoring database is not running");
        }
        if (draft.name.empty()) {
            return Invalid<std::int64_t>("predicate name is required");
        }

        const auto now = simcore::db::types::UtcNow();
        simcore::db::SavePredicateSpecCommand command{};
        command.name = draft.name;
        command.breakpoint_id = static_cast<BPKey>(draft.breakpoint_id);
        command.lhs_kind = draft.lhs_kind;
        command.lhs_value = draft.lhs_value;
        command.rhs_kind = draft.rhs_kind;
        command.rhs_value = draft.rhs_value;
        command.cmp_op = draft.cmp_op;
        command.width = draft.width;
        command.flag_mask = draft.flag_mask;
        command.value_mask = draft.value_mask;
        command.lhs_address_program_id = draft.lhs_address_program_id;
        command.rhs_address_program_id = draft.rhs_address_program_id;
        command.abort_on_fail = draft.abort_on_fail;
        command.created_at_utc = now;
        command.event_id = NextEventId("Authoring.PredicateSpecSaved");
        command.correlation_id = command.event_id;

        std::int64_t id = 0;
        std::string error;
        if (!db->SavePredicateSpec(command, &id, &error)) {
            return Failed<std::int64_t>(error);
        }
        return ServiceResult<std::int64_t>::Ok(id);
    }

    static ServiceResult<simcore::db::PredicateSpecSnapshot> GetPredicateSpec(std::int64_t predicate_spec_id) {
        auto* db = AuthoringDb();
        if (db == nullptr) {
            return Unavailable<simcore::db::PredicateSpecSnapshot>("SimCoreDB authoring database is not running");
        }
        const auto snapshot = db->GetPredicateSpec(predicate_spec_id);
        if (!snapshot.has_value()) {
            return NotFound<simcore::db::PredicateSpecSnapshot>("predicate spec not found");
        }
        return ServiceResult<simcore::db::PredicateSpecSnapshot>::Ok(*snapshot);
    }

    static ServiceResult<std::int64_t> SaveBattleRunSpec(const BattleRunSpecDraft& draft) {
        auto* db = AuthoringDb();
        if (db == nullptr) {
            return Unavailable<std::int64_t>("SimCoreDB authoring database is not running");
        }
        if (draft.name.empty()) {
            return Invalid<std::int64_t>("battle run spec name is required");
        }

        const auto now = simcore::db::types::UtcNow();
        simcore::db::SaveBattleRunSpecCommand command{};
        command.name = draft.name;
        command.priority = draft.priority;
        command.run_ms = draft.run_ms;
        command.vi_stall_ms = draft.vi_stall_ms;
        command.progress_enable = draft.progress_enable;
        command.use_single_turn_runner = draft.use_single_turn_runner;
        command.auto_wave_trigger_enable = draft.auto_wave_trigger_enable;
        command.min_fake_attacks = draft.min_fake_attacks;
        command.max_fake_attacks = draft.max_fake_attacks;
        command.created_at_utc = now;
        command.event_id = NextEventId("Authoring.BattleRunSpecSaved");
        command.correlation_id = command.event_id;

        std::int64_t id = 0;
        std::string error;
        if (!db->SaveBattleRunSpec(command, &id, &error)) {
            return Failed<std::int64_t>(error);
        }
        return ServiceResult<std::int64_t>::Ok(id);
    }

    static ServiceResult<std::int64_t> SaveBattlePlan(const BattlePlanDraft& draft) {
        auto* db = AuthoringDb();
        if (db == nullptr) {
            return Unavailable<std::int64_t>("SimCoreDB authoring database is not running");
        }
        if (draft.name.empty() || draft.fingerprint.empty()) {
            return Invalid<std::int64_t>("battle plan name and fingerprint are required");
        }

        const auto now = simcore::db::types::UtcNow();
        simcore::db::SavePlanCommand plan{};
        plan.name = draft.name;
        plan.fingerprint = draft.fingerprint;
        plan.num_turns = draft.num_turns;
        plan.created_at_utc = now;
        plan.event_id = NextEventId("Authoring.BattlePlanSaved");
        plan.correlation_id = plan.event_id;

        std::int64_t plan_id = 0;
        std::string error;
        if (!db->SavePlan(plan, &plan_id, &error)) {
            return Failed<std::int64_t>(error);
        }

        for (const auto& turn : draft.turns) {
            simcore::db::SaveBattlePlanTurnCommand turn_command{};
            turn_command.plan_id = plan_id;
            turn_command.turn_index = turn.turn_index;
            turn_command.created_at_utc = now;
            turn_command.event_id = NextEventId("Authoring.BattlePlanTurnSaved");
            turn_command.correlation_id = plan.event_id;
            for (const auto& action : turn.actions) {
                turn_command.actions.push_back(simcore::db::SaveBattlePlanActionCommand{
                    .actor_slot = action.actor_slot,
                    .macro = action.macro,
                    .target_kind = action.target_kind,
                    .target_slot = action.target_slot,
                    .target_mask_bits = action.target_mask_bits,
                    .target_single_slot = action.target_single_slot,
                    .target_same_as_actor_slot = action.target_same_as_actor_slot,
                    .target_expr_ini = action.target_expr_ini,
                    .item_id = action.item_id,
                    .ordinal = action.ordinal,
                });
            }

            std::int64_t turn_id = 0;
            if (!db->SaveBattlePlanTurn(turn_command, &turn_id, &error)) {
                return Failed<std::int64_t>(error);
            }
        }

        return ServiceResult<std::int64_t>::Ok(plan_id);
    }

    static ServiceResult<simcore::db::BattlePlanSnapshot> GetBattlePlan(std::int64_t plan_id) {
        auto* db = AuthoringDb();
        if (db == nullptr) {
            return Unavailable<simcore::db::BattlePlanSnapshot>("SimCoreDB authoring database is not running");
        }
        const auto snapshot = db->GetBattlePlan(plan_id);
        if (!snapshot.has_value()) {
            return NotFound<simcore::db::BattlePlanSnapshot>("battle plan not found");
        }
        return ServiceResult<simcore::db::BattlePlanSnapshot>::Ok(*snapshot);
    }

    static ServiceResult<std::int64_t> SaveTemplate(const TemplateDraft& draft) {
        auto* db = AuthoringDb();
        if (db == nullptr) {
            return Unavailable<std::int64_t>("SimCoreDB authoring database is not running");
        }
        if (draft.name.empty()) {
            return Invalid<std::int64_t>("template name is required");
        }

        const auto now = simcore::db::types::UtcNow();
        simcore::db::SaveTemplateCommand command{};
        command.name = draft.name;
        command.description = draft.description;
        command.seed_probe_spec_id = draft.seed_probe_spec_id;
        command.tas_spec_id = draft.tas_spec_id;
        command.battle_run_spec_id = draft.battle_run_spec_id;
        command.explorer_settings_id = draft.explorer_settings_id;
        command.created_at_utc = now;
        command.event_id = NextEventId("Authoring.TemplateSaved");
        command.correlation_id = command.event_id;

        std::int64_t id = 0;
        std::string error;
        if (!db->SaveTemplate(command, &id, &error)) {
            return Failed<std::int64_t>(error);
        }
        return ServiceResult<std::int64_t>::Ok(id);
    }

private:
    static simcore::db::IAuthoringDb* AuthoringDb() {
        return soasimqt2::SimCoreDbRuntime::instance().authoringDb();
    }

    static std::string NextEventId(const char* prefix) {
        static std::atomic<std::uint64_t> counter{ 1 };
        const auto now = simcore::db::types::UtcNow().time_since_epoch().count();
        return std::string(prefix) + "." + std::to_string(now) + "." + std::to_string(counter.fetch_add(1));
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

} // namespace soasimqt2::db
