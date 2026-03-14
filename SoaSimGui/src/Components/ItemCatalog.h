#pragma once
#include <vector>
#include <string_view>
#include <cstdint>
#include <algorithm>
#include "Core/Memory/Soa/SoaConstants.h"

namespace soasim::ui {

    enum class ItemType : uint8_t {
        Weapon,
        Armor,
        Accessory,
        Consumable,
        Special,
        ShipWeapon,
        ShipAccessory,
        ShipConsumable,
        All,
    };

    static inline const char* ItemTypeLabel(ItemType t) {
        switch (t) {
        case ItemType::All:        return "All";
        case ItemType::Weapon:     return "Weapons";
        case ItemType::Armor:      return "Armor";
        case ItemType::Accessory:  return "Accessories";
        case ItemType::Consumable: return "Consumables";
        case ItemType::Special:    return "Special";
        case ItemType::ShipWeapon:    return "ShipWeapon";
        case ItemType::ShipAccessory:    return "ShipAccessory";
        case ItemType::ShipConsumable:    return "ShipConsumable";
        }
        return "All";
    }

    static inline ItemType Classify(uint16_t id) {
        if (id < 80) return ItemType::Weapon;
        if (id < 160) return ItemType::Armor;
        if (id < 240) return ItemType::Accessory;
        if (id < 320) return ItemType::Consumable;
        if (id < 400) return ItemType::Special;
        if (id < 440) return ItemType::ShipWeapon;
        if (id < 480) return ItemType::ShipAccessory;
        return ItemType::ShipConsumable;
    }

    static inline ItemType Classify(soa::itemid::ItemId iid) {
        return Classify((uint16_t)iid);
    }

    static inline void ListByCategory(ItemType type, std::vector<soa::itemid::ItemId>& out_ids) {
        out_ids.clear();
        if ((size_t)type < (size_t)ItemType::ShipWeapon) out_ids.reserve(80);
        else if (type == ItemType::All) out_ids.reserve(510);
        else out_ids.reserve(40);

        switch (type) {
        case ItemType::Weapon: 
            for (int i = 0; i < 74; i++) 
                out_ids.push_back(soa::itemid::ItemId(i));
            break;

        case ItemType::Armor: 
            for (int i = 80; i < 139; i++) 
                out_ids.push_back(soa::itemid::ItemId(i));
            break;

        case ItemType::Accessory: 
            for (int i = 160; i < 223; i++) 
                out_ids.push_back(soa::itemid::ItemId(i));
            break;

        case ItemType::Consumable: 
            for (int i = 240; i < 320; i++) 
                if (!(301 < i < 305)) 
                    out_ids.push_back(soa::itemid::ItemId(i));
            break;

        case ItemType::Special:
            for (int i = 320; i < 365; i++)
                if (!(324 < i < 326) || !(337 < i < 339) || !(342 < i < 344) || !(346 < i < 348) || !(352 < i < 355))
                    out_ids.push_back(soa::itemid::ItemId(i));
            break;

        case ItemType::ShipWeapon: 
            for (int i = 400; i < 440; i++) 
                out_ids.push_back(soa::itemid::ItemId(i));
            break;

        case ItemType::ShipAccessory: 
            for (int i = 440; i < 480; i++) 
                out_ids.push_back(soa::itemid::ItemId(i));
            break;

        case ItemType::ShipConsumable: 
            for (int i = 480; i < 510; i++) 
                out_ids.push_back(soa::itemid::ItemId(i));
            break;

        case ItemType::All:
            int i = 0;
            while (i < (int)ItemType::All)
            {
                std::vector<soa::itemid::ItemId> next_type;
                ListByCategory(ItemType(i++), next_type);
                out_ids.insert(out_ids.end(), next_type.begin(), next_type.end());
            }   
            break;
        }
    }

} // namespace soasim::ui
