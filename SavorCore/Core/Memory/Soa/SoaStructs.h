#pragma once

#include <cstdint>
#include <cstddef>

namespace soa {

#pragma pack(push,1)

    struct Vec2 {
        float x;
        float y;
    };
    static_assert(sizeof(Vec2) == 8, "size");

    struct Vec3Float {
        float x;
        float y;
        float z;
    };
    static_assert(sizeof(Vec3Float) == 12, "size");

    struct Instruction {
        uint32_t inst;
        uint8_t target;
        uint8_t targetMethod;
        uint16_t instParam;
        uint8_t atkResult;
        uint8_t _pad[7];
    };
    static_assert(sizeof(Instruction) == 16, "size");

    struct InstructionSet {
        Instruction current;
        Instruction previous;
    };
    static_assert(sizeof(InstructionSet) == 32, "size");

    // TODO: Correct these fields
    struct Thread {
        uint32_t fxn;
        uint32_t next; // ptr -> Thread
        uint32_t parent;
        uint32_t id;
        int32_t onQueue;
        int32_t fn;
        int32_t priority;
        int32_t phase;
        int32_t name;
    };
    static_assert(sizeof(Thread) == 36, "size");

    struct AllCombatInstances {
        uint32_t PC_1; // ptr -> CombatantInstance
        uint32_t PC_2; // ptr -> CombatantInstance
        uint32_t PC_3; // ptr -> CombatantInstance
        uint32_t PC_4; // ptr -> CombatantInstance
        uint32_t EC_1; // ptr -> CombatantInstance
        uint32_t EC_2; // ptr -> CombatantInstance
        uint32_t EC_3; // ptr -> CombatantInstance
        uint32_t EC_4; // ptr -> CombatantInstance
        uint32_t EC_5; // ptr -> CombatantInstance
        uint32_t EC_6; // ptr -> CombatantInstance
        uint32_t EC_7; // ptr -> CombatantInstance
        uint32_t EC_8; // ptr -> CombatantInstance
    };
    static_assert(sizeof(AllCombatInstances) == 48, "size");

    struct ElementalEffectiveness {
        uint16_t green;
        uint16_t red;
        uint16_t purple;
        uint16_t blue;
        uint16_t Yellow;
        uint16_t Silver;
    };
    static_assert(sizeof(ElementalEffectiveness) == 12, "size");

    struct DerivedStats {
        uint16_t Attack;
        uint16_t Defense;
        uint16_t MagicDefense;
        uint16_t HitChance;
        uint16_t DodgeChance;
        uint8_t _pad[2];
    };
    static_assert(sizeof(DerivedStats) == 12, "size");

    struct StatusEffectiveness {
        uint16_t Poison;
        uint16_t Death;
        uint16_t Stone;
        uint16_t Sleep;
        uint16_t Confusion;
        uint16_t Silence;
        uint16_t Fatigue;
        uint16_t Revive;
        uint16_t Weak;
        uint8_t _pad0[12];
        uint16_t Danger;
    };
    static_assert(sizeof(StatusEffectiveness) == 32, "size");

    struct ItemDrop {
        uint16_t chance;
        uint16_t amount;
        int16_t itemId;
    };
    static_assert(sizeof(ItemDrop) == 6, "size");

    struct AIInstruction {
        uint16_t type;             // 0 = Strategic, 1 = Action
        uint16_t instruction;
        uint16_t param;
    };
    static_assert(sizeof(AIInstruction) == 6, "size");

    struct Wksht {
        int32_t vtable;
        int32_t size;
        int32_t capacity;
        int32_t data;
    };
    static_assert(sizeof(Wksht) == 16, "size");

    struct GridRow {
        uint8_t cell[11];
    };
    static_assert(sizeof(GridRow) == 11, "size");

    struct Grid {
        GridRow row[11];
    };
    static_assert(sizeof(Grid) == 0x79, "size");

    struct GridCoord {
        int8_t x;
        int8_t z;
    };
    static_assert(sizeof(GridCoord) == 2, "size");

    struct Magic_Ranks {
        int8_t Green;
        int8_t Red;
        int8_t Purple;
        int8_t Blue;
        int8_t Yellow;
        int8_t Silver;
    };
    static_assert(sizeof(Magic_Ranks) == 6, "size");

    struct Character_Stats {
        int16_t Strength;
        int16_t Will;
        int16_t Vigor;
        int16_t Agility;
        int16_t Magic;
    };
    static_assert(sizeof(Character_Stats) == 10, "size");

    struct Color_XP {
        int32_t Green;
        int32_t Red;
        int32_t Purple;
        int32_t Blue;
        int32_t Yellow;
        int32_t Silver;
    };
    static_assert(sizeof(Color_XP) == 24, "size");

    struct CombatantInstance {
        uint16_t regen_amount;
        uint8_t _pad0[6];
        uint16_t counter_chance;
        uint8_t _pad1[4];
        uint16_t battleState_flags_1;
        uint32_t battleState_flags_2;
        uint32_t Current_HP;
        uint32_t Max_HP;
        uint32_t status_flags;
        uint32_t new_status_flags;
        uint16_t destruction_recharge_1;
        uint16_t destruction_recharge_2;
        ElementalEffectiveness current_elemental_eff; // embed ElementalEffectiveness
        ElementalEffectiveness change_elemental_eff; // embed ElementalEffectiveness
        StatusEffectiveness current_status_eff; // embed StatusEffectiveness
        StatusEffectiveness change_status_eff; // embed StatusEffectiveness
        Character_Stats current_base_stats; // embed Character_Stats
        Character_Stats change_base_stats; // embed Character_Stats
        DerivedStats current_derived_stats; // embed DerivedStats
        DerivedStats change_derived_stats; // embed DerivedStats
        uint8_t width;
        uint8_t depth;
        uint16_t movement_flags;
        uint8_t _pad2[4];
        uint16_t current_counter_chance;
        uint16_t equipped_weapon;
        uint8_t _pad3[2];
        uint16_t base_counter_chance;
        ElementalEffectiveness base_elemental_eff;
        StatusEffectiveness base_status_eff;
        Character_Stats base_base_stats;
        DerivedStats base_derived_stats;
        uint8_t spells_cast;
        uint8_t _pad4;
        uint8_t current_weapon_element;
        uint8_t _pad5;
        uint16_t equipped_armor;
        uint16_t equipped_accessory;
        uint8_t death_count;
        int8_t _pad6;
        int16_t kill_count;
        int16_t new_regen_amount;
        uint8_t _pad7[4];
        uint32_t Enemy_Definition; // ptr -> EnemyDefinition
    };
    static_assert(sizeof(CombatantInstance) == 0x114, "size");

    struct PC_Data {
        char Name[11];
        uint8_t Level;
        uint8_t Deaths;
        uint8_t Current_MP;
        uint8_t Max_MP;
        uint8_t Weapon_Element;
        uint16_t Equipped_Weapon;
        uint16_t Equipped_Armor;
        uint16_t Equipped_Accessory;
        uint16_t Moonberries_Used;
        uint16_t Current_HP;
        uint16_t Max_HP;
        uint16_t Spirit;
        uint16_t Max_Spirit;
        uint16_t Current_Counter_Chance;
        uint16_t Enemies_Killed;
        uint32_t Experience;
        float Max_MP_fractional;
        uint8_t Ability_Flags[8];
        Magic_Ranks Magic_Levels; // embed Magic_Ranks
        Character_Stats Base_Stats; // embed Character_Stats
        Color_XP Magic_XP; // embed Color_XP
    };
    static_assert(sizeof(PC_Data) == 92, "size");

    struct EnemyDefinition {
        char japaneseName[21];
        int8_t width;
        int8_t depth;
        int8_t elemental_alignment;
        uint8_t _pad0[2];
        int16_t movment_flags;
        int16_t counter_pcnt;
        int16_t experience;
        int16_t gold;
        uint8_t _pad1[2];
        uint32_t max_HP;
        int8_t _pad2[4];
        ElementalEffectiveness elemental_eff; // embed ElementalEffectiveness
        StatusEffectiveness status_eff; // embed StatusEffectiveness
        int8_t atk_effect_id;
        int8_t atk_state_id;
        int8_t atd_state_miss_chance;
        uint8_t _pad3[1];
        Character_Stats base_stats;
        DerivedStats derive_stats; // embed DerivedStats
        ItemDrop items[4]; // embed ItemDropArray
        AIInstruction inst[64];
    };
    static_assert(sizeof(EnemyDefinition) == 0x20a, "size");

    struct All_PC_Data {
        PC_Data Vyse; // embed PC_Data
        PC_Data Aika; // embed PC_Data
        PC_Data Fina; // embed PC_Data
        PC_Data Drachma; // embed PC_Data
        PC_Data Enrique; // embed PC_Data
        PC_Data Gilder; // embed PC_Data
    };
    static_assert(sizeof(All_PC_Data) == 552, "size");

    struct BattleItemDropSlot {
        int16_t count;
        int16_t item_id;
    };
    static_assert(sizeof(BattleItemDropSlot) == 4, "size");

    struct ItemSlot {
        uint16_t item_id;
        uint8_t count;
        uint8_t _pad0[1];
    };
    static_assert(sizeof(ItemSlot) == 4, "size");

    struct BattleState {
        uint8_t  initiative;
        uint8_t  _pad0[1];
        uint8_t  PC_escape_chance;
        uint8_t  EC_escape_chance;
        uint8_t  _pad1[2];
        uint16_t maxSP;
        uint16_t curSP;
        uint16_t sp_after_instructions;
        uint16_t enemies_killed;
        BattleItemDropSlot item_drops[8];
        uint8_t  _pad2[2];
        uint32_t experience_earned;
        uint32_t gold_earned;
        ItemSlot useable_items[80];
    };
    static_assert(sizeof(BattleState) == 0x178, "size");

#pragma pack(pop)

} // namespace soa
