// AUTO-GENERATED. DO NOT EDIT.
#pragma once
#include <tuple>
#include "SoaStructs.h"

namespace soa_reflect {
template <class T, class = void> struct reflect_has : std::false_type {};
template <class T> struct reflect_has<T, std::void_t<decltype(T::members)>> : std::true_type {};
}

template <class T> struct reflect; // primary template

template <> struct reflect<soa::Vec2> {
  using type = soa::Vec2;
  static constexpr auto members = std::make_tuple(
    &type::x,
    &type::y
  );
};

template <> struct reflect<soa::Vec3Float> {
  using type = soa::Vec3Float;
  static constexpr auto members = std::make_tuple(
    &type::x,
    &type::y,
    &type::z
  );
};

template <> struct reflect<soa::Instruction> {
  using type = soa::Instruction;
  static constexpr auto members = std::make_tuple(
    &type::inst,
    &type::target,
    &type::targetMethod,
    &type::instParam,
    &type::atkResult,
    &type::_pad
  );
};

template <> struct reflect<soa::InstructionSet> {
  using type = soa::InstructionSet;
  static constexpr auto members = std::make_tuple(
    &type::current,
    &type::previous
  );
};

template <> struct reflect<soa::Thread> {
  using type = soa::Thread;
  static constexpr auto members = std::make_tuple(
    &type::fxn,
    &type::next,
    &type::parent,
    &type::id,
    &type::onQueue,
    &type::fn,
    &type::priority,
    &type::phase,
    &type::name
  );
};

template <> struct reflect<soa::AllCombatInstances> {
  using type = soa::AllCombatInstances;
  static constexpr auto members = std::make_tuple(
    &type::PC_1,
    &type::PC_2,
    &type::PC_3,
    &type::PC_4,
    &type::EC_1,
    &type::EC_2,
    &type::EC_3,
    &type::EC_4,
    &type::EC_5,
    &type::EC_6,
    &type::EC_7,
    &type::EC_8
  );
};

template <> struct reflect<soa::ElementalEffectiveness> {
  using type = soa::ElementalEffectiveness;
  static constexpr auto members = std::make_tuple(
    &type::green,
    &type::red,
    &type::purple,
    &type::blue,
    &type::Yellow,
    &type::Silver
  );
};

template <> struct reflect<soa::DerivedStats> {
  using type = soa::DerivedStats;
  static constexpr auto members = std::make_tuple(
    &type::Attack,
    &type::Defense,
    &type::MagicDefense,
    &type::HitChance,
    &type::DodgeChance,
    &type::_pad
  );
};

template <> struct reflect<soa::StatusEffectiveness> {
  using type = soa::StatusEffectiveness;
  static constexpr auto members = std::make_tuple(
    &type::Poison,
    &type::Death,
    &type::Stone,
    &type::Sleep,
    &type::Confusion,
    &type::Silence,
    &type::Fatigue,
    &type::Revive,
    &type::Weak,
    &type::_pad0,
    &type::Danger
  );
};

template <> struct reflect<soa::ItemDrop> {
  using type = soa::ItemDrop;
  static constexpr auto members = std::make_tuple(
    &type::chance,
    &type::amount,
    &type::itemId
  );
};

template <> struct reflect<soa::AIInstruction> {
  using type = soa::AIInstruction;
  static constexpr auto members = std::make_tuple(
    &type::type,
    &type::instruction,
    &type::param
  );
};

template <> struct reflect<soa::Wksht> {
  using type = soa::Wksht;
  static constexpr auto members = std::make_tuple(
    &type::vtable,
    &type::size,
    &type::capacity,
    &type::data
  );
};

template <> struct reflect<soa::GridRow> {
  using type = soa::GridRow;
  static constexpr auto members = std::make_tuple(
    &type::cell
  );
};

template <> struct reflect<soa::Grid> {
  using type = soa::Grid;
  static constexpr auto members = std::make_tuple(
    &type::row
  );
};

template <> struct reflect<soa::GridCoord> {
  using type = soa::GridCoord;
  static constexpr auto members = std::make_tuple(
    &type::x,
    &type::z
  );
};

template <> struct reflect<soa::Magic_Ranks> {
  using type = soa::Magic_Ranks;
  static constexpr auto members = std::make_tuple(
    &type::Green,
    &type::Red,
    &type::Purple,
    &type::Blue,
    &type::Yellow,
    &type::Silver
  );
};

template <> struct reflect<soa::Character_Stats> {
  using type = soa::Character_Stats;
  static constexpr auto members = std::make_tuple(
    &type::Strength,
    &type::Will,
    &type::Vigor,
    &type::Agility,
    &type::Magic
  );
};

template <> struct reflect<soa::Color_XP> {
  using type = soa::Color_XP;
  static constexpr auto members = std::make_tuple(
    &type::Green,
    &type::Red,
    &type::Purple,
    &type::Blue,
    &type::Yellow,
    &type::Silver
  );
};

template <> struct reflect<soa::CombatantInstance> {
  using type = soa::CombatantInstance;
  static constexpr auto members = std::make_tuple(
    &type::regen_amount,
    &type::_pad0,
    &type::counter_chance,
    &type::_pad1,
    &type::battleState_flags_1,
    &type::battleState_flags_2,
    &type::Current_HP,
    &type::Max_HP,
    &type::status_flags,
    &type::new_status_flags,
    &type::destruction_recharge_1,
    &type::destruction_recharge_2,
    &type::current_elemental_eff,
    &type::change_elemental_eff,
    &type::current_status_eff,
    &type::change_status_eff,
    &type::current_base_stats,
    &type::change_base_stats,
    &type::current_derived_stats,
    &type::change_derived_stats,
    &type::width,
    &type::depth,
    &type::movement_flags,
    &type::_pad2,
    &type::current_counter_chance,
    &type::equipped_weapon,
    &type::_pad3,
    &type::base_counter_chance,
    &type::base_elemental_eff,
    &type::base_status_eff,
    &type::base_base_stats,
    &type::base_derived_stats,
    &type::spells_cast,
    &type::_pad4,
    &type::current_weapon_element,
    &type::_pad5,
    &type::equipped_armor,
    &type::equipped_accessory,
    &type::death_count,
    &type::_pad6,
    &type::kill_count,
    &type::new_regen_amount,
    &type::_pad7,
    &type::Enemy_Definition
  );
};

template <> struct reflect<soa::PC_Data> {
  using type = soa::PC_Data;
  static constexpr auto members = std::make_tuple(
    &type::Name,
    &type::Level,
    &type::Deaths,
    &type::Current_MP,
    &type::Max_MP,
    &type::Weapon_Element,
    &type::Equipped_Weapon,
    &type::Equipped_Armor,
    &type::Equipped_Accessory,
    &type::Moonberries_Used,
    &type::Current_HP,
    &type::Max_HP,
    &type::Spirit,
    &type::Max_Spirit,
    &type::Current_Counter_Chance,
    &type::Enemies_Killed,
    &type::Experience,
    &type::Max_MP_fractional,
    &type::Ability_Flags,
    &type::Magic_Levels,
    &type::Base_Stats,
    &type::Magic_XP
  );
};

template <> struct reflect<soa::EnemyDefinition> {
  using type = soa::EnemyDefinition;
  static constexpr auto members = std::make_tuple(
    &type::japaneseName,
    &type::width,
    &type::depth,
    &type::elemental_alignment,
    &type::_pad0,
    &type::movment_flags,
    &type::counter_pcnt,
    &type::experience,
    &type::gold,
    &type::_pad1,
    &type::max_HP,
    &type::_pad2,
    &type::elemental_eff,
    &type::status_eff,
    &type::atk_effect_id,
    &type::atk_state_id,
    &type::atd_state_miss_chance,
    &type::_pad3,
    &type::base_stats,
    &type::derive_stats,
    &type::items,
    &type::inst
  );
};

template <> struct reflect<soa::All_PC_Data> {
  using type = soa::All_PC_Data;
  static constexpr auto members = std::make_tuple(
    &type::Vyse,
    &type::Aika,
    &type::Fina,
    &type::Drachma,
    &type::Enrique,
    &type::Gilder
  );
};

template <> struct reflect<soa::BattleItemDropSlot> {
  using type = soa::BattleItemDropSlot;
  static constexpr auto members = std::make_tuple(
    &type::count,
    &type::item_id
  );
};

template <> struct reflect<soa::ItemSlot> {
  using type = soa::ItemSlot;
  static constexpr auto members = std::make_tuple(
    &type::item_id,
    &type::count,
    &type::_pad0
  );
};

template <> struct reflect<soa::BattleState> {
  using type = soa::BattleState;
  static constexpr auto members = std::make_tuple(
    &type::initiative,
    &type::_pad0,
    &type::PC_escape_chance,
    &type::EC_escape_chance,
    &type::_pad1,
    &type::maxSP,
    &type::curSP,
    &type::sp_after_instructions,
    &type::enemies_killed,
    &type::item_drops,
    &type::_pad2,
    &type::experience_earned,
    &type::gold_earned,
    &type::useable_items
  );
};

