#include "FirstBattleDataModel.h"

namespace savor::predict {

namespace {

constexpr FirstBattleActorStats kVyse{
    .kind = FirstBattleActorKind::Vyse,
    .slot = 0,
    .name = "Vyse",
    .max_hp = 420,
    .element = 0,
    .movement_flags = 0x0FC7,
    .counter_chance = 15,
    .power = 23,
    .will = 16,
    .vigor = 19,
    .agile = 11,
    .quick = 22,
    .attack = 43,
    .hit = 90,
    .weapon_id = 0,
    .weapon_attack = 20,
    .weapon_hit = 90,
    .effect_id = -1,
    .motion_base_speed = 2.55f,
    .motion_alt_speed = 0.45f,
    .motion_speeds_known = true,
    .attack_known = true,
    .hit_known = true,
};

constexpr FirstBattleActorStats kAika{
    .kind = FirstBattleActorKind::Aika,
    .slot = 1,
    .name = "Aika",
    .max_hp = 360,
    .element = 1,
    .movement_flags = 0x0FF7,
    .counter_chance = 6,
    .power = 17,
    .will = 21,
    .vigor = 18,
    .agile = 22,
    .quick = 26,
    .attack = 36,
    .hit = 110,
    .weapon_id = 16,
    .weapon_attack = 19,
    .weapon_hit = 110,
    .effect_id = -1,
    .motion_base_speed = 2.25f,
    .motion_alt_speed = 0.6f,
    .motion_speeds_known = true,
    .attack_known = true,
    .hit_known = true,
};

constexpr FirstBattleActorStats kSoldier{
    .kind = FirstBattleActorKind::Soldier,
    .slot = 4,
    .name = "Soldier",
    .max_hp = 58,
    .element = 4,
    .movement_flags = 0x0FC7,
    .counter_chance = 10,
    .agile = 10,
    .quick = 18,
    .attack = 43,
    .defense = 42,
    .mag_def = 40,
    .hit = 95,
    .dodge = 15,
    .effect_id = -1,
    .motion_base_speed = 2.399997f,
    .motion_alt_speed = 2.399997f,
    .motion_speeds_known = true,
    .attack_known = true,
    .hit_known = true,
    .defense_known = true,
    .dodge_known = true,
};

} // namespace

std::optional<FirstBattleActorStats> first_battle_actor_by_slot(int slot) {
    switch (slot) {
    case 0:
        return kVyse;
    case 1:
        return kAika;
    case 4:
    case 5: {
        auto soldier = kSoldier;
        soldier.slot = slot;
        return soldier;
    }
    default:
        return std::nullopt;
    }
}

std::optional<int> first_battle_soldier_slot_from_progress_name(const std::string& actor) {
    if (actor.size() < 3 || actor[0] != '[') {
        return std::nullopt;
    }

    int slot = 0;
    bool saw_digit = false;
    for (std::size_t i = 1; i < actor.size(); ++i) {
        const char c = actor[i];
        if (c == ']') {
            if (!saw_digit || actor.find("]Soldier", i) == std::string::npos) {
                return std::nullopt;
            }
            return slot;
        }
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        saw_digit = true;
        slot = slot * 10 + (c - '0');
    }
    return std::nullopt;
}

std::optional<FirstBattleActorStats> first_battle_actor_by_progress_name(const std::string& actor) {
    if (actor == "Vyse") {
        return kVyse;
    }
    if (actor == "Aika") {
        return kAika;
    }

    const auto soldier_slot = first_battle_soldier_slot_from_progress_name(actor);
    if (soldier_slot.has_value()) {
        return first_battle_actor_by_slot(*soldier_slot);
    }
    return std::nullopt;
}

bool first_battle_is_pc_actor_name(const std::string& actor) {
    return actor == "Vyse" || actor == "Aika";
}

bool first_battle_is_soldier_actor_name(const std::string& actor) {
    return first_battle_soldier_slot_from_progress_name(actor).has_value();
}

std::optional<int> first_battle_base_counter_chance_for_actor_name(const std::string& actor) {
    const auto stats = first_battle_actor_by_progress_name(actor);
    if (!stats.has_value()) {
        return std::nullopt;
    }
    return stats->counter_chance;
}

std::optional<int> first_battle_base_agile_for_actor_name(const std::string& actor) {
    const auto stats = first_battle_actor_by_progress_name(actor);
    if (!stats.has_value()) {
        return std::nullopt;
    }
    return stats->agile;
}

std::optional<BasicAttackInputs> first_battle_static_basic_attack_inputs(
    const std::string& attacker,
    const std::string& target,
    int instr_param_0x6) {
    const auto attacker_stats = first_battle_actor_by_progress_name(attacker);
    const auto target_stats = first_battle_actor_by_progress_name(target);
    if (!attacker_stats.has_value() || !target_stats.has_value()) {
        return std::nullopt;
    }
    if (!attacker_stats->attack_known || !attacker_stats->hit_known
        || !target_stats->defense_known || !target_stats->dodge_known) {
        return std::nullopt;
    }

    BasicAttackInputs inputs;
    inputs.attacker_attack = attacker_stats->attack;
    inputs.attacker_hit = attacker_stats->hit;
    inputs.attacker_agile = attacker_stats->agile;
    inputs.attacker_element = attacker_stats->element;
    inputs.target_defense = target_stats->defense;
    inputs.target_dodge = target_stats->dodge;
    inputs.target_element_effectiveness_tenths = 10;
    inputs.target_status_flags = 0;
    inputs.instr_param_0x6 = instr_param_0x6;
    return inputs;
}

} // namespace savor::predict
