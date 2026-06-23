#pragma once

#include "RngCore.h"

#include <cstdint>
#include <optional>

namespace savor::predict {

enum class CounterResultReason {
    Counter,
    StatusSuppressed,
    SameSide,
    CriticalSuppressed,
    ZeroBaseChance,
    RandomFailed,
    MovementFlagSuppressed,
};

struct CounterInputs {
    int attacker_slot = 0;
    int target_slot = 0;
    int target_status_flags = 0;
    int target_movement_flags = 0;
    int target_base_counter_chance = 0;
    int target_current_counter_chance = 0;
    int attacker_action_marker = 0;
    bool attack_was_critical = false;
};

struct CounterSimulation {
    std::uint32_t end_state = 0;
    int draws_consumed = 0;
    std::optional<std::uint16_t> counter_rand;
    bool counter = false;
    std::optional<int> queued_field7_0xc;
    int updated_current_counter_chance = 0;
    CounterResultReason reason = CounterResultReason::RandomFailed;
};

struct CounterChanceIncrementInputs {
    int hit_check = 0;
    int current_counter_chance = 0;
    int counter_chance_increment = 0;
};

struct CounterChanceIncrementSimulation {
    bool incremented = false;
    int updated_current_counter_chance = 0;
};

CounterSimulation simulate_counter_check(std::uint32_t state, const CounterInputs& inputs);
CounterSimulation simulate_counter_check_from_rand(
    const CounterInputs& inputs,
    std::uint16_t counter_rand);
CounterChanceIncrementSimulation simulate_counter_chance_increment_after_damage(
    const CounterChanceIncrementInputs& inputs);
const char* counter_result_reason_name(CounterResultReason reason);

} // namespace savor::predict
