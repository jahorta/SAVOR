#pragma once

#include "AttackResolutionModel.h"

#include <cstdint>
#include <optional>
#include <string>

namespace savor::predict {

enum class FirstBattleActorKind {
    Unknown,
    Vyse,
    Aika,
    Soldier,
};

struct FirstBattleActorStats {
    FirstBattleActorKind kind = FirstBattleActorKind::Unknown;
    int slot = -1;
    const char* name = "";
    int max_hp = 0;
    int element = -1;
    int movement_flags = 0;
    int counter_chance = 0;
    int power = 0;
    int will = 0;
    int vigor = 0;
    int agile = 0;
    int quick = 0;
    int attack = 0;
    int defense = 0;
    int mag_def = 0;
    int hit = 0;
    int dodge = 0;
    int weapon_id = -1;
    int weapon_attack = 0;
    int weapon_hit = 0;
    int effect_id = -1;
    float motion_base_speed = 0.0f;
    float motion_alt_speed = 0.0f;
    bool motion_speeds_known = false;
    float motion_turn_speed = 0.0f;
    std::uint32_t motion_turn_speed_bits = 0;
    bool motion_turn_speed_known = false;
    bool attack_known = false;
    bool hit_known = false;
    bool defense_known = false;
    bool dodge_known = false;
};

std::optional<FirstBattleActorStats> first_battle_actor_by_slot(int slot);
std::optional<FirstBattleActorStats> first_battle_actor_by_progress_name(const std::string& actor);
std::optional<int> first_battle_soldier_slot_from_progress_name(const std::string& actor);
bool first_battle_is_pc_actor_name(const std::string& actor);
bool first_battle_is_soldier_actor_name(const std::string& actor);
std::optional<int> first_battle_base_counter_chance_for_actor_name(const std::string& actor);
std::optional<int> first_battle_base_agile_for_actor_name(const std::string& actor);
std::optional<BasicAttackInputs> first_battle_static_basic_attack_inputs(
    const std::string& attacker,
    const std::string& target,
    int instr_param_0x6);

} // namespace savor::predict
