#include "BattlePredictor.h"

#include "BattleVisualRngModel.h"
#include "FirstBattleDataModel.h"
#include "PreAiCameraModel.h"
#include "RngCore.h"

#include <algorithm>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>

namespace savor::predict {
namespace {

struct QueuedPredictionAction {
    int actor_slot = -1;
    int target_slot = -1;
    bool attack = false;
    bool guard = false;
    bool enemy_owned = false;
    int instr_param_0x6 = 0;
    std::string source;
};

struct DropRowSimulation {
    int row_index = -1;
    std::uint16_t rand_value = 0;
    int rand_mod100 = 0;
    int chance = 0;
    int item_id = -1;
    int amount = 0;
    bool success = false;
};

struct DropTableSimulation {
    std::uint32_t end_state = 0;
    int draws_consumed = 0;
    std::optional<int> item_id;
    std::optional<int> amount;
    std::vector<DropRowSimulation> rows;
};

std::string hex_seed(std::uint32_t seed) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << seed;
    return out.str();
}

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (const char c : value) {
        switch (c) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default: out << c; break;
        }
    }
    return out.str();
}

int element_effectiveness_tenths(const soa::ElementalEffectiveness& effectiveness, int element) {
    switch (element) {
    case 0: return effectiveness.green;
    case 1: return effectiveness.red;
    case 2: return effectiveness.purple;
    case 3: return effectiveness.blue;
    case 4: return effectiveness.Yellow;
    case 5: return effectiveness.Silver;
    default: return 10;
    }
}

int first_alive_enemy_slot(const std::vector<BattlePredictionSlotState>& slots) {
    for (const auto& slot : slots) {
        if (slot.present && !slot.is_player && slot.alive) {
            return slot.slot;
        }
    }
    return -1;
}

bool has_alive_enemy(const std::vector<BattlePredictionSlotState>& slots) {
    return first_alive_enemy_slot(slots) >= 0;
}

bool has_alive_pc(const std::vector<BattlePredictionSlotState>& slots) {
    for (const auto& slot : slots) {
        if (slot.present && slot.is_player && slot.alive) {
            return true;
        }
    }
    return false;
}

int present_alive_pc_count(const std::vector<BattlePredictionSlotState>& slots) {
    int count = 0;
    for (const auto& slot : slots) {
        if (slot.present && slot.is_player && slot.alive) {
            ++count;
        }
    }
    return count;
}

BattlePredictionSlotState make_slot_state(
    const BattlePredictionProfile& profile,
    const soa::battle::ctx::BattleContext& context,
    int slot_index,
    std::vector<std::string>& warnings) {
    BattlePredictionSlotState state;
    state.slot = slot_index;
    if (slot_index < 0 || slot_index >= soa::battle::ctx::SLOT_COUNT) {
        return state;
    }

    const auto& source = context.slots_[slot_index];
    state.present = source.present != 0;
    state.is_player = source.is_player != 0;
    state.alive = state.present && source.is_alive != 0 && source.instance.Current_HP > 0;
    state.id = source.id;
    state.current_hp = static_cast<int>(source.instance.Current_HP);
    state.max_hp = static_cast<int>(source.instance.Max_HP);
    state.status_flags = source.instance.status_flags;
    state.movement_flags = source.instance.movement_flags;
    state.base_counter_chance = source.instance.base_counter_chance;
    state.current_counter_chance = source.instance.current_counter_chance;
    state.agile = source.instance.current_base_stats.Agility;
    state.attack = source.instance.current_derived_stats.Attack;
    state.defense = source.instance.current_derived_stats.Defense;
    state.hit = source.instance.current_derived_stats.HitChance;
    state.dodge = source.instance.current_derived_stats.DodgeChance;
    state.element = source.instance.current_weapon_element;
    state.attack_inputs_known =
        state.attack > 0 && state.hit > 0 && state.defense >= 0 && state.dodge >= 0;

    if (profile.name == "first-battle") {
        if (const auto first_battle = first_battle_actor_by_slot(slot_index); first_battle.has_value()) {
            state.quick = first_battle->quick;
            state.quick_known = true;
            if (state.agile == 0) {
                state.agile = first_battle->agile;
            }
            if (state.attack == 0 && first_battle->attack_known) {
                state.attack = first_battle->attack;
            }
            if (state.hit == 0 && first_battle->hit_known) {
                state.hit = first_battle->hit;
            }
            if (state.defense == 0 && first_battle->defense_known) {
                state.defense = first_battle->defense;
            }
            if (state.dodge == 0 && first_battle->dodge_known) {
                state.dodge = first_battle->dodge;
            }
            if (state.element < 0 || state.element > 5) {
                state.element = first_battle->element;
            }
            if (state.base_counter_chance == 0) {
                state.base_counter_chance = first_battle->counter_chance;
            }
            if (state.current_counter_chance == 0) {
                state.current_counter_chance = first_battle->counter_chance;
            }
            if (state.movement_flags == 0) {
                state.movement_flags = static_cast<std::uint16_t>(first_battle->movement_flags);
            }
            state.attack_inputs_known =
                state.attack > 0 && state.hit > 0 && state.defense >= 0 && state.dodge >= 0;
        }
    }

    if (state.present && !state.quick_known) {
        warnings.push_back(
            "quick stat is not materialized in BattleContext for slot "
            + std::to_string(slot_index)
            + "; non-first-battle turn-order prediction needs another data source");
    }
    return state;
}

std::vector<BattlePredictionSlotState> make_initial_slots(
    const BattlePredictionProfile& profile,
    const soa::battle::ctx::BattleContext& context,
    std::vector<std::string>& warnings) {
    std::vector<BattlePredictionSlotState> slots;
    slots.reserve(soa::battle::ctx::SLOT_COUNT);
    for (int slot = 0; slot < soa::battle::ctx::SLOT_COUNT; ++slot) {
        slots.push_back(make_slot_state(profile, context, slot, warnings));
    }
    return slots;
}

BattlePredictionSlotState* find_slot(std::vector<BattlePredictionSlotState>& slots, int slot) {
    if (slot < 0 || slot >= static_cast<int>(slots.size())) {
        return nullptr;
    }
    return &slots[slot];
}

const BattlePredictionSlotState* find_slot(const std::vector<BattlePredictionSlotState>& slots, int slot) {
    if (slot < 0 || slot >= static_cast<int>(slots.size())) {
        return nullptr;
    }
    return &slots[slot];
}

const soa::battle::ctx::BattleSlot* context_slot(
    const soa::battle::ctx::BattleContext& context,
    int slot) {
    if (slot < 0 || slot >= soa::battle::ctx::SLOT_COUNT) {
        return nullptr;
    }
    return &context.slots_[slot];
}

void append_event(BattlePredictionResult& result, BattlePredictionEvent event) {
    event.sequence = static_cast<int>(result.events.size()) + 1;
    result.total_draws_consumed += event.draws_consumed;
    if (event.status == BattlePredictionEventStatus::Ambiguous) {
        result.has_ambiguous_events = true;
    }
    if (event.status == BattlePredictionEventStatus::Unsupported) {
        result.has_unsupported_events = true;
    }
    result.events.push_back(std::move(event));
}

void advance_without_rand_values(std::uint32_t& state, int draws) {
    for (int i = 0; i < draws; ++i) {
        state = advance_once(state);
    }
}

std::optional<BasicAttackInputs> basic_attack_inputs_for(
    const soa::battle::ctx::BattleContext& context,
    const std::vector<BattlePredictionSlotState>& slots,
    int attacker_slot,
    int target_slot,
    int instr_param_0x6) {
    const auto* attacker = find_slot(slots, attacker_slot);
    const auto* target = find_slot(slots, target_slot);
    const auto* target_context_slot = context_slot(context, target_slot);
    if (attacker == nullptr || target == nullptr || target_context_slot == nullptr
        || !attacker->attack_inputs_known || !target->attack_inputs_known) {
        return std::nullopt;
    }

    BasicAttackInputs inputs;
    inputs.attacker_attack = attacker->attack;
    inputs.attacker_hit = attacker->hit;
    inputs.attacker_agile = attacker->agile;
    inputs.attacker_element = attacker->element;
    inputs.target_defense = target->defense;
    inputs.target_dodge = target->dodge;
    inputs.target_element_effectiveness_tenths =
        element_effectiveness_tenths(target_context_slot->instance.current_elemental_eff, attacker->element);
    if (inputs.target_element_effectiveness_tenths == 0) {
        inputs.target_element_effectiveness_tenths = 10;
    }
    inputs.target_status_flags = static_cast<int>(target->status_flags);
    inputs.instr_param_0x6 = instr_param_0x6;
    return inputs;
}

DropTableSimulation simulate_drop_table_from_enemy_definition(
    std::uint32_t state,
    const soa::EnemyDefinition& enemy_def) {
    DropTableSimulation result;
    result.end_state = state;

    for (int row = 0; row < 4; ++row) {
        const auto& item = enemy_def.items[row];
        if (item.chance == 0 || item.amount == 0 || item.itemId <= 0) {
            continue;
        }

        const auto draw = draw_rand15(result.end_state);
        result.end_state = draw.next_state;
        ++result.draws_consumed;

        DropRowSimulation row_result;
        row_result.row_index = row;
        row_result.rand_value = draw.value;
        row_result.rand_mod100 = draw.value % 100;
        row_result.chance = item.chance;
        row_result.item_id = item.itemId;
        row_result.amount = item.amount;
        row_result.success = row_result.rand_mod100 < row_result.chance;
        result.rows.push_back(row_result);

        if (row_result.success) {
            result.item_id = row_result.item_id;
            result.amount = row_result.amount;
            break;
        }
    }
    return result;
}

std::string drop_detail(const DropTableSimulation& drop) {
    std::ostringstream out;
    out << "drop table rows evaluated=" << drop.rows.size();
    if (drop.item_id.has_value()) {
        out << "; item_id=" << *drop.item_id << " amount=" << drop.amount.value_or(0);
    } else {
        out << "; no drop";
    }
    return out.str();
}

void append_pre_ai_events(
    BattlePredictionResult& result,
    std::uint32_t& state,
    int fake_attacks,
    int pc_count) {
    const auto model = model_pre_ai_camera_draws(fake_attacks, pc_count);

    if (model.fake_attack_draws > 0) {
        const auto before = state;
        advance_without_rand_values(state, model.fake_attack_draws);
        append_event(result, {
            .phase = "pre_ai",
            .label = "fake_attack_draws",
            .status = BattlePredictionEventStatus::Exact,
            .rng_seed_before = before,
            .rng_seed_after = state,
            .draws_consumed = model.fake_attack_draws,
            .detail = "v1 assumes every fake attack consumes one RNG draw",
        });
    }

    if (model.expected_camera_draws > 0) {
        const auto before = state;
        advance_without_rand_values(state, model.expected_camera_draws);
        append_event(result, {
            .phase = "pre_ai",
            .label = "camera_draws",
            .status = BattlePredictionEventStatus::Exact,
            .rng_seed_before = before,
            .rng_seed_after = state,
            .draws_consumed = model.expected_camera_draws,
            .detail = "v1 assumes battle-start camera plus one targeting camera draw per PC",
        });
    }
}

void append_player_commands(
    BattlePredictionResult& result,
    const soa::battle::actions::TurnPlan& turn_plan,
    const std::vector<BattlePredictionSlotState>& slots,
    std::vector<QueuedPredictionAction>& actions) {
    for (const auto& command : turn_plan.commands) {
        const int actor_slot = static_cast<int>(command.actor_slot);
        const auto* actor = find_slot(slots, actor_slot);
        if (actor == nullptr || !actor->present || !actor->alive) {
            append_event(result, {
                .phase = "player_command",
                .label = "skipped_missing_actor",
                .status = BattlePredictionEventStatus::Skipped,
                .actor_slot = actor_slot,
                .detail = "player command actor is not present and alive",
            });
            continue;
        }

        if (command.macro == soa::battle::actions::BattleAction::Attack) {
            int target_slot = soa::battle::actions::resolveTargetIndex(command.params.target_slot);
            if (target_slot < 0) {
                target_slot = first_alive_enemy_slot(slots);
            }
            if (target_slot < 0) {
                append_event(result, {
                    .phase = "player_command",
                    .label = "unsupported_missing_attack_target",
                    .status = BattlePredictionEventStatus::Unsupported,
                    .actor_slot = actor_slot,
                    .detail = "attack has no concrete live target",
                });
                continue;
            }
            actions.push_back({
                .actor_slot = actor_slot,
                .target_slot = target_slot,
                .attack = true,
                .enemy_owned = false,
                .instr_param_0x6 = 0,
                .source = "turn_plan",
            });
            continue;
        }

        if (command.macro == soa::battle::actions::BattleAction::Defend) {
            append_event(result, {
                .phase = "player_command",
                .label = "guard_no_turn",
                .status = BattlePredictionEventStatus::Exact,
                .actor_slot = actor_slot,
                .detail = "guarding characters do not enter turn-order execution",
            });
            continue;
        }

        append_event(result, {
            .phase = "player_command",
            .label = "unsupported_player_action",
            .status = BattlePredictionEventStatus::Unsupported,
            .actor_slot = actor_slot,
            .detail = "v1 BattlePredictor supports Attack and Guard only",
        });
    }
}

void append_enemy_ai(
    BattlePredictionResult& result,
    std::uint32_t& state,
    const BattlePredictionProfile& profile,
    const std::vector<BattlePredictionSlotState>& slots,
    std::vector<QueuedPredictionAction>& actions) {
    for (int slot = 4; slot < soa::battle::ctx::SLOT_COUNT; ++slot) {
        const auto* actor = find_slot(slots, slot);
        if (actor == nullptr || !actor->present || !actor->alive) {
            continue;
        }
        if (profile.name == "first-battle" && slot != 4 && slot != 5) {
            append_event(result, {
                .phase = "enemy_ai",
                .label = "unsupported_enemy_slot",
                .status = BattlePredictionEventStatus::Unsupported,
                .actor_slot = slot,
                .detail = "first-battle profile only supports Soldier AI in slots 4 and 5",
            });
            continue;
        }

        const auto before = state;
        const auto decision = resolve_soldier_ai(slot, state);
        const int draws = decision.attacks ? 3 : 1;
        std::ostringstream detail;
        detail << (decision.attacks ? "attack" : "guard")
               << "; action_rand=" << decision.action_rand;
        if (decision.target_pc_slot.has_value()) {
            detail << "; target_slot=" << *decision.target_pc_slot;
        }
        if (decision.attack_param_rand.has_value()) {
            detail << "; instr_param_0x6="
                   << soldier_attack_param_from_rand(*decision.attack_param_rand);
        }

        append_event(result, {
            .phase = "enemy_ai",
            .label = "soldier_ai",
            .status = BattlePredictionEventStatus::Exact,
            .actor_slot = slot,
            .target_slot = decision.target_pc_slot.value_or(-1),
            .rng_seed_before = before,
            .rng_seed_after = state,
            .draws_consumed = draws,
            .rand_value = decision.action_rand,
            .detail = detail.str(),
        });

        if (decision.attacks && decision.target_pc_slot.has_value() && decision.attack_param_rand.has_value()) {
            actions.push_back({
                .actor_slot = slot,
                .target_slot = *decision.target_pc_slot,
                .attack = true,
                .enemy_owned = true,
                .instr_param_0x6 = soldier_attack_param_from_rand(*decision.attack_param_rand),
                .source = "soldier_ai",
            });
        }
    }
}

std::vector<TurnOrderEntryInput> turn_order_entries_for_actions(
    const std::vector<BattlePredictionSlotState>& slots,
    const std::vector<QueuedPredictionAction>& actions,
    BattlePredictionResult& result) {
    std::vector<TurnOrderEntryInput> entries;
    entries.reserve(actions.size());
    for (const auto& action : actions) {
        if (!action.attack) {
            continue;
        }
        const auto* actor = find_slot(slots, action.actor_slot);
        if (actor == nullptr || !actor->alive) {
            continue;
        }
        if (!actor->quick_known) {
            append_event(result, {
                .phase = "turn_order",
                .label = "unsupported_missing_quick",
                .status = BattlePredictionEventStatus::Unsupported,
                .actor_slot = action.actor_slot,
                .detail = "turn-order prediction needs quick for every queued actor",
            });
            continue;
        }
        entries.push_back({
            .slot = action.actor_slot,
            .quick = actor->quick,
            .queued_instruction = 3,
        });
    }
    return entries;
}

const QueuedPredictionAction* find_action_for_slot(
    const std::vector<QueuedPredictionAction>& actions,
    int slot) {
    for (const auto& action : actions) {
        if (action.actor_slot == slot && action.attack) {
            return &action;
        }
    }
    return nullptr;
}

void append_turn_order(
    BattlePredictionResult& result,
    std::uint32_t& state,
    const std::vector<BattlePredictionSlotState>& slots,
    const std::vector<QueuedPredictionAction>& actions,
    std::vector<int>& execution_slots) {
    const auto entries = turn_order_entries_for_actions(slots, actions, result);
    const auto before = state;
    const auto turn_order = simulate_turn_order(state, entries);
    state = turn_order.end_state;
    execution_slots = turn_order.execution_slots;

    std::ostringstream detail;
    detail << "queued=" << turn_order.queued_count
           << "; order=";
    for (std::size_t i = 0; i < execution_slots.size(); ++i) {
        if (i != 0) {
            detail << ",";
        }
        detail << execution_slots[i];
    }
    if (!turn_order.execution_order_exact) {
        detail << "; priority ties or unresolved priorities make order ambiguous";
    }

    append_event(result, {
        .phase = "turn_order",
        .label = "resolve_turn_order",
        .status = turn_order.execution_order_exact
            ? BattlePredictionEventStatus::Exact
            : BattlePredictionEventStatus::Ambiguous,
        .rng_seed_before = before,
        .rng_seed_after = state,
        .draws_consumed = turn_order.draws_consumed,
        .detail = detail.str(),
    });

    result.exact_through_turn_order =
        !result.has_unsupported_events && !result.has_ambiguous_events && turn_order.execution_order_exact;
    result.exact_draws_through_turn_order = result.total_draws_consumed;
}

void append_enemy_attack_setup(
    BattlePredictionResult& result,
    std::uint32_t& state,
    const BattlePredictionSlotState& actor) {
    const auto before = state;
    const auto setup = simulate_enemy_attack_setup_gate(
        state,
        EnemyAttackSetupInputs{
            .queued_instruction = 3,
            .movement_flags = actor.movement_flags,
        });
    state = setup.end_state;
    append_event(result, {
        .phase = "action_setup",
        .label = "enemy_attack_setup_gate",
        .status = BattlePredictionEventStatus::Exact,
        .actor_slot = actor.slot,
        .rng_seed_before = before,
        .rng_seed_after = state,
        .draws_consumed = setup.draws_consumed,
        .rand_value = setup.setup_rand,
        .detail = enemy_attack_setup_path_name(setup.path),
    });
}

BattlePredictionEventStatus battle_event_status_from_visual_status(BattleVisualRngStepStatus status) {
    switch (status) {
    case BattleVisualRngStepStatus::Exact:
        return BattlePredictionEventStatus::Exact;
    case BattleVisualRngStepStatus::Provisional:
        return BattlePredictionEventStatus::Provisional;
    case BattleVisualRngStepStatus::Ambiguous:
        return BattlePredictionEventStatus::Ambiguous;
    case BattleVisualRngStepStatus::Unsupported:
        return BattlePredictionEventStatus::Unsupported;
    }
    return BattlePredictionEventStatus::Unsupported;
}

bool append_visual_rng_steps(
    BattlePredictionResult& result,
    std::uint32_t& state,
    const QueuedPredictionAction& action,
    const BattleVisualRngModelResult& visual) {
    for (const auto& step : visual.steps) {
        BattlePredictionEvent event;
        event.phase = step.phase;
        event.label = step.label;
        event.status = battle_event_status_from_visual_status(step.status);
        event.actor_slot = action.actor_slot;
        event.target_slot = action.target_slot;
        event.draws_consumed = step.draws_consumed;
        event.effect_source_key = step.effect_source_key;
        event.detail = step.detail;
        if (step.draws_consumed > 0) {
            event.rng_seed_before = state;
            advance_without_rand_values(state, step.draws_consumed);
            event.rng_seed_after = state;
        }
        append_event(result, std::move(event));
    }
    return !visual.has_ambiguous_steps && !visual.has_unsupported_steps;
}

bool append_pre_attack_visual_rng(
    BattlePredictionResult& result,
    std::uint32_t& state,
    const BattlePredictionProfile& profile,
    const QueuedPredictionAction& action) {
    const auto visual = model_first_battle_basic_attack_visual_rng({
        .profile_name = profile.name,
        .actor_slot = action.actor_slot,
        .target_slot = action.target_slot,
        .include_action_view_camera = true,
        .include_effect_bursts = false,
    });
    return append_visual_rng_steps(result, state, action, visual);
}

bool append_post_attack_visual_rng(
    BattlePredictionResult& result,
    std::uint32_t& state,
    const BattlePredictionProfile& profile,
    const QueuedPredictionAction& action,
    bool attack_landed,
    bool attack_was_critical) {
    const auto visual = model_first_battle_basic_attack_visual_rng({
        .profile_name = profile.name,
        .actor_slot = action.actor_slot,
        .target_slot = action.target_slot,
        .attack_landed = attack_landed,
        .attack_was_critical = attack_was_critical,
        .include_action_view_camera = false,
        .include_effect_bursts = true,
    });
    return append_visual_rng_steps(result, state, action, visual);
}

void append_counter_check(
    BattlePredictionResult& result,
    std::uint32_t& state,
    std::vector<BattlePredictionSlotState>& slots,
    const QueuedPredictionAction& action,
    bool critical) {
    auto* attacker = find_slot(slots, action.actor_slot);
    auto* target = find_slot(slots, action.target_slot);
    if (attacker == nullptr || target == nullptr || !target->alive) {
        return;
    }

    const auto before = state;
    const auto counter = simulate_counter_check(
        state,
        CounterInputs{
            .attacker_slot = action.actor_slot,
            .target_slot = action.target_slot,
            .target_status_flags = static_cast<int>(target->status_flags),
            .target_movement_flags = target->movement_flags,
            .target_base_counter_chance = target->base_counter_chance,
            .target_current_counter_chance = target->current_counter_chance,
            .attacker_action_marker = 0,
            .attack_was_critical = critical,
        });
    state = counter.end_state;
    target->current_counter_chance = counter.updated_current_counter_chance;

    append_event(result, {
        .phase = "counter",
        .label = counter.counter ? "counter_queued" : "counter_not_queued",
        .status = counter.counter
            ? BattlePredictionEventStatus::Ambiguous
            : BattlePredictionEventStatus::Exact,
        .actor_slot = action.target_slot,
        .target_slot = action.actor_slot,
        .rng_seed_before = before,
        .rng_seed_after = state,
        .draws_consumed = counter.draws_consumed,
        .rand_value = counter.counter_rand,
        .detail = counter.counter
            ? "counter follow-up identity/insertion remains a live-validation gap"
            : counter_result_reason_name(counter.reason),
    });
}

void append_death_and_drop(
    BattlePredictionResult& result,
    std::uint32_t& state,
    const soa::battle::ctx::BattleContext& context,
    const QueuedPredictionAction& action) {
    const auto* target_context = context_slot(context, action.target_slot);
    if (target_context == nullptr || target_context->is_player || !target_context->has_enemy_def) {
        return;
    }

    append_event(result, {
        .phase = "death_drop",
        .label = "death_handler",
        .status = BattlePredictionEventStatus::Exact,
        .actor_slot = action.actor_slot,
        .target_slot = action.target_slot,
        .detail = "lethal enemy damage enters death/drop path",
    });

    const auto before = state;
    const auto drop = simulate_drop_table_from_enemy_definition(state, target_context->enemy_def);
    state = drop.end_state;
    BattlePredictionEvent event;
    event.phase = "death_drop";
    event.label = drop.item_id.has_value() ? "enemy_drop" : "enemy_no_drop";
    event.status = BattlePredictionEventStatus::Exact;
    event.actor_slot = action.actor_slot;
    event.target_slot = action.target_slot;
    event.rng_seed_before = before;
    event.rng_seed_after = state;
    event.draws_consumed = drop.draws_consumed;
    event.item_id = drop.item_id;
    event.amount = drop.amount;
    event.detail = drop_detail(drop);
    append_event(result, std::move(event));
}

void append_attack_resolution(
    BattlePredictionResult& result,
    std::uint32_t& state,
    const soa::battle::ctx::BattleContext& context,
    std::vector<BattlePredictionSlotState>& slots,
    const BattlePredictionProfile& profile,
    const QueuedPredictionAction& action) {
    auto* actor = find_slot(slots, action.actor_slot);
    auto* target = find_slot(slots, action.target_slot);
    if (actor == nullptr || target == nullptr || !actor->alive || !target->alive) {
        append_event(result, {
            .phase = "attack_resolution",
            .label = "skipped_dead_or_missing_actor",
            .status = BattlePredictionEventStatus::Skipped,
            .actor_slot = action.actor_slot,
            .target_slot = action.target_slot,
            .detail = "actor or target is not alive at execution time",
        });
        return;
    }

    const auto inputs = basic_attack_inputs_for(
        context,
        slots,
        action.actor_slot,
        action.target_slot,
        action.instr_param_0x6);
    if (!inputs.has_value()) {
        append_event(result, {
            .phase = "attack_resolution",
            .label = "unsupported_missing_attack_inputs",
            .status = BattlePredictionEventStatus::Unsupported,
            .actor_slot = action.actor_slot,
            .target_slot = action.target_slot,
            .detail = "attack, hit, defense, dodge, or elemental effectiveness data is unavailable",
        });
        return;
    }

    const auto before = state;
    const int hp_before = target->current_hp;
    const auto attack = simulate_basic_attack_burst(state, *inputs);
    state = attack.end_state;
    int hp_after = hp_before;
    if (attack.hit_check != 0) {
        hp_after = std::max(0, hp_before - attack.damage);
        target->current_hp = hp_after;
        if (hp_after == 0) {
            target->alive = false;
        }
    }

    BattlePredictionEvent event;
    event.phase = "attack_resolution";
    event.label = attack.attack_result == 0
        ? "attack_miss"
        : (attack.attack_result == 2 ? "attack_crit" : "attack_hit");
    event.status = result.has_ambiguous_events
        ? BattlePredictionEventStatus::Provisional
        : BattlePredictionEventStatus::Exact;
    event.actor_slot = action.actor_slot;
    event.target_slot = action.target_slot;
    event.rng_seed_before = before;
    event.rng_seed_after = state;
    event.draws_consumed = attack.draws_consumed;
    event.rand_value = attack.hit_rand;
    event.attack_result = attack.attack_result;
    event.damage = attack.damage;
    event.hp_before = hp_before;
    event.hp_after = hp_after;
    event.detail = "basic attack helper; instr_param_0x6=" + std::to_string(action.instr_param_0x6);
    append_event(result, std::move(event));

    if (attack.attack_result != 0) {
        append_post_attack_visual_rng(
            result,
            state,
            profile,
            action,
            true,
            attack.attack_result == 2);
    }

    if (!target->alive) {
        append_death_and_drop(result, state, context, action);
        return;
    }

    if (attack.attack_result != 0) {
        append_counter_check(result, state, slots, action, attack.attack_result == 2);
    }
}

void append_action_execution(
    BattlePredictionResult& result,
    std::uint32_t& state,
    const soa::battle::ctx::BattleContext& context,
    std::vector<BattlePredictionSlotState>& slots,
    const std::vector<QueuedPredictionAction>& actions,
    const std::vector<int>& execution_slots,
    const BattlePredictionProfile& profile,
    const BattlePredictionOptions& options) {
    for (const int slot : execution_slots) {
        const auto* action = find_action_for_slot(actions, slot);
        if (action == nullptr) {
            append_event(result, {
                .phase = "action_execution",
                .label = "unsupported_missing_action",
                .status = BattlePredictionEventStatus::Unsupported,
                .actor_slot = slot,
                .detail = "turn-order slot had no queued action in predictor state",
            });
            continue;
        }

        auto* actor = find_slot(slots, action->actor_slot);
        if (actor == nullptr || !actor->alive) {
            append_event(result, {
                .phase = "action_execution",
                .label = "skipped_dead_actor",
                .status = BattlePredictionEventStatus::Skipped,
                .actor_slot = action->actor_slot,
                .target_slot = action->target_slot,
                .detail = "actor died before its queued action",
            });
            continue;
        }

        if (action->enemy_owned) {
            append_enemy_attack_setup(result, state, *actor);
        }

        if (options.include_visual_rng_gap_events) {
            const bool visual_exact = append_pre_attack_visual_rng(result, state, profile, *action);
            if (!visual_exact && !options.continue_after_visual_rng_gap) {
                continue;
            }
        }

        append_attack_resolution(result, state, context, slots, profile, *action);
        if (!has_alive_enemy(slots) || !has_alive_pc(slots)) {
            break;
        }
    }
}

} // namespace

BattlePredictionProfile first_battle_prediction_profile() {
    return BattlePredictionProfile{
        .name = "first-battle",
        .supported_turn_index = 1,
        .supports_status_effects = false,
        .supports_non_soldier_ai = false,
    };
}

std::optional<BattlePredictionProfile> battle_prediction_profile_by_name(std::string_view name) {
    if (name == "first-battle") {
        return first_battle_prediction_profile();
    }
    return std::nullopt;
}

BattlePredictionResult predict_battle(const BattlePredictionInput& input) {
    BattlePredictionResult result;
    result.profile = input.profile;
    result.starting_rng_seed = input.starting_rng_seed;
    result.final_rng_seed = input.starting_rng_seed;

    if (input.profile.name != "first-battle") {
        result.outcome = BattlePredictionOutcome::Unsupported;
        result.errors.push_back("unsupported battle prediction profile: " + input.profile.name);
        return result;
    }

    auto slots = make_initial_slots(input.profile, input.context, result.warnings);
    auto state = input.starting_rng_seed;

    const int pc_count = std::max(1, present_alive_pc_count(slots));
    append_pre_ai_events(
        result,
        state,
        static_cast<int>(input.turn_plan.fake_attack_count),
        pc_count);

    std::vector<QueuedPredictionAction> actions;
    append_player_commands(result, input.turn_plan, slots, actions);
    append_enemy_ai(result, state, input.profile, slots, actions);

    std::vector<int> execution_slots;
    append_turn_order(result, state, slots, actions, execution_slots);
    append_action_execution(
        result,
        state,
        input.context,
        slots,
        actions,
        execution_slots,
        input.profile,
        input.options);

    result.final_rng_seed = state;
    result.final_slots = std::move(slots);

    if (result.has_unsupported_events || !result.errors.empty()) {
        result.outcome = BattlePredictionOutcome::Unsupported;
    } else if (result.has_ambiguous_events) {
        result.outcome = BattlePredictionOutcome::Ambiguous;
    } else if (!has_alive_pc(result.final_slots)) {
        result.outcome = BattlePredictionOutcome::Defeat;
    } else if (!has_alive_enemy(result.final_slots)) {
        result.outcome = BattlePredictionOutcome::Victory;
    } else {
        result.outcome = BattlePredictionOutcome::ReachedNextTurn;
    }

    return result;
}

const char* battle_prediction_outcome_name(BattlePredictionOutcome outcome) {
    switch (outcome) {
    case BattlePredictionOutcome::ReachedNextTurn: return "ReachedNextTurn";
    case BattlePredictionOutcome::Victory: return "Victory";
    case BattlePredictionOutcome::Defeat: return "Defeat";
    case BattlePredictionOutcome::Unsupported: return "Unsupported";
    case BattlePredictionOutcome::Ambiguous: return "Ambiguous";
    }
    return "Unsupported";
}

const char* battle_prediction_event_status_name(BattlePredictionEventStatus status) {
    switch (status) {
    case BattlePredictionEventStatus::Exact: return "Exact";
    case BattlePredictionEventStatus::Provisional: return "Provisional";
    case BattlePredictionEventStatus::Skipped: return "Skipped";
    case BattlePredictionEventStatus::Unsupported: return "Unsupported";
    case BattlePredictionEventStatus::Ambiguous: return "Ambiguous";
    }
    return "Unsupported";
}

void write_battle_prediction_text(const BattlePredictionResult& result, std::ostream& out) {
    out << "SavorPredict predict-battle\n";
    out << "  profile: " << result.profile.name << "\n";
    out << "  outcome: " << battle_prediction_outcome_name(result.outcome) << "\n";
    out << "  start_seed: " << result.starting_rng_seed << " ("
        << hex_seed(result.starting_rng_seed) << ")\n";
    out << "  final_seed: " << result.final_rng_seed << " ("
        << hex_seed(result.final_rng_seed) << ")\n";
    out << "  total_draws_consumed: " << result.total_draws_consumed << "\n";
    out << "  exact_through_turn_order: " << (result.exact_through_turn_order ? "true" : "false") << "\n";
    out << "  exact_draws_through_turn_order: " << result.exact_draws_through_turn_order << "\n";
    if (!result.warnings.empty()) {
        out << "  warnings:\n";
        for (const auto& warning : result.warnings) {
            out << "    - " << warning << "\n";
        }
    }
    if (!result.errors.empty()) {
        out << "  errors:\n";
        for (const auto& error : result.errors) {
            out << "    - " << error << "\n";
        }
    }

    out << "\nEvents:\n";
    for (const auto& event : result.events) {
        out << "  [" << event.sequence << "] "
            << event.phase << "." << event.label
            << " status=" << battle_prediction_event_status_name(event.status)
            << " draws=" << event.draws_consumed;
        if (event.actor_slot >= 0) {
            out << " actor=" << event.actor_slot;
        }
        if (event.target_slot >= 0) {
            out << " target=" << event.target_slot;
        }
        if (event.rng_seed_before.has_value()) {
            out << " rng_before=" << hex_seed(*event.rng_seed_before);
        }
        if (event.rng_seed_after.has_value()) {
            out << " rng_after=" << hex_seed(*event.rng_seed_after);
        }
        if (event.damage.has_value()) {
            out << " damage=" << *event.damage;
        }
        if (event.hp_before.has_value() && event.hp_after.has_value()) {
            out << " hp=" << *event.hp_before << "->" << *event.hp_after;
        }
        if (event.effect_source_key.has_value()) {
            out << " effect_source_key=" << *event.effect_source_key;
        }
        if (event.item_id.has_value()) {
            out << " item_id=" << *event.item_id
                << " amount=" << event.amount.value_or(0);
        }
        if (!event.detail.empty()) {
            out << " detail=\"" << event.detail << "\"";
        }
        out << "\n";
    }
}

void write_battle_prediction_json(const BattlePredictionResult& result, std::ostream& out) {
    out << "{\n";
    out << "  \"profile\": \"" << json_escape(result.profile.name) << "\",\n";
    out << "  \"outcome\": \"" << battle_prediction_outcome_name(result.outcome) << "\",\n";
    out << "  \"starting_rng_seed\": " << result.starting_rng_seed << ",\n";
    out << "  \"starting_rng_seed_hex\": \"" << hex_seed(result.starting_rng_seed) << "\",\n";
    out << "  \"final_rng_seed\": " << result.final_rng_seed << ",\n";
    out << "  \"final_rng_seed_hex\": \"" << hex_seed(result.final_rng_seed) << "\",\n";
    out << "  \"total_draws_consumed\": " << result.total_draws_consumed << ",\n";
    out << "  \"exact_through_turn_order\": " << (result.exact_through_turn_order ? "true" : "false") << ",\n";
    out << "  \"exact_draws_through_turn_order\": " << result.exact_draws_through_turn_order << ",\n";
    out << "  \"has_ambiguous_events\": " << (result.has_ambiguous_events ? "true" : "false") << ",\n";
    out << "  \"has_unsupported_events\": " << (result.has_unsupported_events ? "true" : "false") << ",\n";
    out << "  \"warnings\": [";
    for (std::size_t i = 0; i < result.warnings.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << "\"" << json_escape(result.warnings[i]) << "\"";
    }
    out << "],\n";
    out << "  \"errors\": [";
    for (std::size_t i = 0; i < result.errors.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << "\"" << json_escape(result.errors[i]) << "\"";
    }
    out << "],\n";
    out << "  \"events\": [\n";
    for (std::size_t i = 0; i < result.events.size(); ++i) {
        const auto& event = result.events[i];
        out << "    {";
        out << "\"sequence\": " << event.sequence;
        out << ", \"phase\": \"" << json_escape(event.phase) << "\"";
        out << ", \"label\": \"" << json_escape(event.label) << "\"";
        out << ", \"status\": \"" << battle_prediction_event_status_name(event.status) << "\"";
        out << ", \"actor_slot\": " << event.actor_slot;
        out << ", \"target_slot\": " << event.target_slot;
        out << ", \"draws_consumed\": " << event.draws_consumed;
        if (event.rng_seed_before.has_value()) {
            out << ", \"rng_seed_before\": " << *event.rng_seed_before;
            out << ", \"rng_seed_before_hex\": \"" << hex_seed(*event.rng_seed_before) << "\"";
        }
        if (event.rng_seed_after.has_value()) {
            out << ", \"rng_seed_after\": " << *event.rng_seed_after;
            out << ", \"rng_seed_after_hex\": \"" << hex_seed(*event.rng_seed_after) << "\"";
        }
        if (event.rand_value.has_value()) {
            out << ", \"rand_value\": " << *event.rand_value;
        }
        if (event.attack_result.has_value()) {
            out << ", \"attack_result\": " << *event.attack_result;
        }
        if (event.damage.has_value()) {
            out << ", \"damage\": " << *event.damage;
        }
        if (event.hp_before.has_value()) {
            out << ", \"hp_before\": " << *event.hp_before;
        }
        if (event.hp_after.has_value()) {
            out << ", \"hp_after\": " << *event.hp_after;
        }
        if (event.effect_source_key.has_value()) {
            out << ", \"effect_source_key\": " << *event.effect_source_key;
        }
        if (event.item_id.has_value()) {
            out << ", \"item_id\": " << *event.item_id;
        }
        if (event.amount.has_value()) {
            out << ", \"amount\": " << *event.amount;
        }
        out << ", \"detail\": \"" << json_escape(event.detail) << "\"";
        out << "}";
        if (i + 1 != result.events.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
}

} // namespace savor::predict
