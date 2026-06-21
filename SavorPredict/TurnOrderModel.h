#pragma once

#include "RngCore.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace savor::predict {

enum class TurnOrderPriorityPath {
    ExistingPriority,
    QuickOnlyNoJitter,
    RandomizedQuick,
    FixedPriority,
    FixedPriorityUnresolved,
};

struct TurnOrderEntryInput {
    int slot = 0;
    int quick = 0;
    int queued_instruction = 3;
    int initial_priority = -1;
    int fixed_priority_result = 0;
    std::optional<int> fixed_priority_value;
};

struct TurnOrderEntrySimulation {
    TurnOrderEntryInput input;
    std::optional<std::uint16_t> priority_rand;
    std::optional<int> assigned_priority;
    TurnOrderPriorityPath path = TurnOrderPriorityPath::RandomizedQuick;
};

struct TurnOrderSimulation {
    std::uint32_t end_state = 0;
    int draws_consumed = 0;
    int queued_count = 0;
    int sum_quick = 0;
    int jitter_modulus = 0;
    bool priorities_complete = true;
    bool priority_ties_ambiguous = false;
    bool execution_order_exact = true;
    std::vector<TurnOrderEntrySimulation> entries;
    std::vector<int> execution_slots;
};

std::vector<TurnOrderEntryInput> first_battle_basic_turn_order_entries(bool soldier4_attacks, bool soldier5_attacks);
TurnOrderSimulation simulate_turn_order(std::uint32_t state, const std::vector<TurnOrderEntryInput>& queued_entries);
const char* turn_order_priority_path_name(TurnOrderPriorityPath path);

} // namespace savor::predict
