#include "BattleContextTree.h"
#include "TreeView.h"
#include "imgui.h"
#include <vector>
#include <format>
#include <unordered_set>

Node decode_context(soa::battle::ctx::BattleContext bc) {
    Node root{ .name = "Battle Context" };

    // 1) Structured BattleContext (placeholder one-liners)
    Node pm = { .name = "Party Members" };
    for (int i = 0; i < 4; i++) {
        if (!bc.slots_[i].present) continue;
        auto element = soa::text::get_element_name(bc.slots_[i].instance.current_weapon_element);
        auto name = soa::text::PCNames[bc.slots_[i].id];
        Node m{ std::format("[{}] {} (element={})", i, name, element) };
        pm.children.push_back(m);
    }
    root.children.push_back(pm);

    Node em = { "Enemies" };
    for (int i = 4; i < 12; i++) {
        if (!bc.slots_[i].present) continue;
        auto name = soa::text::get_enemy_name(bc.slots_[i].id);
        Node m{ std::format("[{}] {}", i, name) };
        em.children.push_back(m);
    }
    root.children.push_back(em);


    std::unordered_set<uint8_t> unique_enemy_types;
    std::unordered_set<uint8_t> unique_slot_by_enemy_types;
    for (int i = 4; i < 12; i++) {
        if (!bc.slots_[i].present) continue;
        if (unique_enemy_types.contains((uint8_t)bc.slots_[i].id)) continue;
        unique_enemy_types.emplace(bc.slots_[i].id);
        unique_slot_by_enemy_types.emplace(i);
    }

    Node items{ "Item Drops" };
    for (auto i : unique_slot_by_enemy_types) {
        auto name = soa::text::get_enemy_name(bc.slots_[i].id);
        Node e{ .name = name.data()};
        for (auto item : bc.slots_[i].enemy_def.items)
        {
            if (item.itemId < 0) continue;
            auto item_name = soa::text::get_item_name((size_t)item.itemId);
            auto amt = (int)item.amount;
            auto chance = (int)item.chance;
            Node i{ std::format("({}%) [{}]{} x{}", chance, item.itemId, item_name, amt) };
            e.children.push_back(i);
        }
        items.children.push_back(e);
    }
    root.children.push_back(items);

    return root;
}

void BattleContextTree::DrawTree(soa::battle::ctx::BattleContext bc) {
    Node root = decode_context(bc);

    TreeView::Draw(&root);
}

void recursive_imgui_lines(const Node n, int level = 0) {
    std::string ind(level * 2, ' ');
    ImGui::Text(n.name.c_str());
    for (auto c : n.children) {
        recursive_imgui_lines(c, level + 1);
    }
}

void BattleContextTree::DrawLines(soa::battle::ctx::BattleContext bc)
{
    Node root = decode_context(bc);
}

