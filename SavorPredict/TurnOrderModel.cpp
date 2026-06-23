#include "TurnOrderModel.h"

#include "FirstBattleDataModel.h"
#include "SoaQSortModel.h"

#include <numeric>

namespace savor::predict {

std::vector<TurnOrderEntryInput> first_battle_basic_turn_order_entries(bool soldier4_attacks, bool soldier5_attacks) {
    std::vector<TurnOrderEntryInput> entries;
    const auto vyse = first_battle_actor_by_slot(0);
    const auto aika = first_battle_actor_by_slot(1);
    entries.push_back({.slot = 0, .quick = vyse.has_value() ? vyse->quick : 22});
    entries.push_back({.slot = 1, .quick = aika.has_value() ? aika->quick : 24});
    if (soldier4_attacks) {
        const auto soldier = first_battle_actor_by_slot(4);
        entries.push_back({.slot = 4, .quick = soldier.has_value() ? soldier->quick : 18});
    }
    if (soldier5_attacks) {
        const auto soldier = first_battle_actor_by_slot(5);
        entries.push_back({.slot = 5, .quick = soldier.has_value() ? soldier->quick : 18});
    }
    return entries;
}

TurnOrderSimulation simulate_turn_order(std::uint32_t state, const std::vector<TurnOrderEntryInput>& queued_entries) {
    TurnOrderSimulation result;
    result.end_state = state;
    result.queued_count = static_cast<int>(queued_entries.size());
    result.sum_quick = std::accumulate(
        queued_entries.begin(),
        queued_entries.end(),
        0,
        [](int sum, const TurnOrderEntryInput& entry) {
            return sum + entry.quick;
        });
    if (result.queued_count > 0) {
        result.jitter_modulus = (result.sum_quick / result.queued_count) / 2;
    }

    result.entries.reserve(queued_entries.size());
    for (const auto& input : queued_entries) {
        TurnOrderEntrySimulation entry;
        entry.input = input;

        if (input.initial_priority != -1) {
            entry.assigned_priority = input.initial_priority;
            entry.path = TurnOrderPriorityPath::ExistingPriority;
        } else if (input.fixed_priority_result != 0) {
            if (input.fixed_priority_value.has_value()) {
                entry.assigned_priority = *input.fixed_priority_value;
                entry.path = TurnOrderPriorityPath::FixedPriority;
            } else {
                result.priorities_complete = false;
                entry.path = TurnOrderPriorityPath::FixedPriorityUnresolved;
            }
        } else if (result.jitter_modulus == 0) {
            entry.assigned_priority = input.quick;
            entry.path = TurnOrderPriorityPath::QuickOnlyNoJitter;
        } else {
            const auto draw = draw_rand15(result.end_state);
            result.end_state = draw.next_state;
            entry.priority_rand = draw.value;
            entry.assigned_priority = input.quick + (draw.value % result.jitter_modulus);
            entry.path = TurnOrderPriorityPath::RandomizedQuick;
            ++result.draws_consumed;
        }

        result.entries.push_back(entry);
    }

    if (!result.priorities_complete) {
        result.execution_order_exact = false;
        return result;
    }

    std::vector<int> priority_keys;
    priority_keys.reserve(result.entries.size());
    for (const auto& entry : result.entries) {
        priority_keys.push_back(*entry.assigned_priority);
    }
    const auto sorted_indices = soa_qsort_indices_by_key_ascending(priority_keys);
    result.qsort_sorted_indices = sorted_indices;
    result.qsort_sorted_slots.reserve(sorted_indices.size());
    for (const auto index : sorted_indices) {
        result.qsort_sorted_slots.push_back(result.entries[index].input.slot);
    }
    for (std::size_t i = 1; i < sorted_indices.size(); ++i) {
        if (result.entries[sorted_indices[i - 1]].assigned_priority
            == result.entries[sorted_indices[i]].assigned_priority) {
            result.priority_ties_ambiguous = true;
            break;
        }
    }

    result.execution_slots.reserve(sorted_indices.size());
    for (auto it = sorted_indices.rbegin(); it != sorted_indices.rend(); ++it) {
        result.execution_slots.push_back(result.entries[*it].input.slot);
    }
    return result;
}

const char* turn_order_priority_path_name(TurnOrderPriorityPath path) {
    switch (path) {
    case TurnOrderPriorityPath::ExistingPriority:
        return "ExistingPriority";
    case TurnOrderPriorityPath::QuickOnlyNoJitter:
        return "QuickOnlyNoJitter";
    case TurnOrderPriorityPath::RandomizedQuick:
        return "RandomizedQuick";
    case TurnOrderPriorityPath::FixedPriority:
        return "FixedPriority";
    case TurnOrderPriorityPath::FixedPriorityUnresolved:
        return "FixedPriorityUnresolved";
    }
    return "Unknown";
}

} // namespace savor::predict
