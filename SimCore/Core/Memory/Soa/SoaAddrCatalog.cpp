#include "SoaAddrCatalog.h"
#include "../Soa/SoaAddrRegistry.h"
#include "SoaStructs.h"

namespace addrprog::catalog {

    template<class FieldT>
    uint32_t battle_treasure_slot(addrprog::Builder& b, uint16_t slot_index, FieldT soa::BattleItemDropSlot::* field, std::string& description)
    {
        description = "battle treasure slot";
        b.op_base_key(addr::battle::MainInstancePtr);
        b.op_field_of(&soa::BattleState::item_drops); 
        b.op_index_elems<soa::BattleItemDropSlot>(slot_index);
        if constexpr (!std::is_same_v<FieldT, void>) b.op_field_of(field);
        b.op_end();
        return b.current_offset();
    }

    template<class FieldT>
    uint32_t enemy_item_field(addrprog::Builder& b, uint16_t combatant_slot, uint16_t item_index, FieldT soa::ItemDrop::* field, std::string& description)
    {
        description = "enemy item field";
        b.op_base_key(addr::battle::CombatantInstancesTable);
        b.op_index_elems<uint32_t>(combatant_slot); // table of u32 pointers
        b.op_load_ptr32();                           // *(u32) -> instance
        b.op_field_of(&soa::CombatantInstance::Enemy_Definition);  // no magic 0x110
        b.op_index_elems<soa::ItemDrop>(item_index);               // items[j]
        if constexpr (!std::is_same_v<FieldT, void>) b.op_field_of(field);
        b.op_end();
        return b.current_offset();
    }

    uint32_t turn_order_idx(addrprog::Builder& b, uint16_t logical_id_index, std::string& description)
    {
        description = "turn order index";
        b.op_base_key(addr::derived::battle::TurnOrderIdx_base);
        b.op_index(logical_id_index, /*stride*/1);
        b.op_end();
        return b.current_offset();
    }

    uint32_t item_drop_amt(addrprog::Builder& b, uint16_t item_id, std::string& description)
    {
        description = "item drop amount";
        b.op_base_key(addr::derived::battle::DropsByItem_base);
        b.op_index(item_id, 1);
        b.op_end();
        return b.current_offset();
    }

    // Explicit instantiations for the common types you already use
    template uint32_t battle_treasure_slot<>(addrprog::Builder&, uint16_t, int16_t soa::BattleItemDropSlot::*, std::string& description);
    template uint32_t enemy_item_field<uint16_t>(addrprog::Builder&, uint16_t, uint16_t, uint16_t soa::ItemDrop::*, std::string& description);

}
