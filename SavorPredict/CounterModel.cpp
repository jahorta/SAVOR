#include "CounterModel.h"

namespace savor::predict {

namespace {

CounterSimulation simulate_counter_check_after_draw(
    const CounterInputs& inputs,
    std::optional<std::uint16_t> counter_rand) {
    CounterSimulation result;
    result.updated_current_counter_chance = inputs.target_current_counter_chance;

    if ((inputs.target_status_flags & 0x6D00) != 0) {
        result.reason = CounterResultReason::StatusSuppressed;
        return result;
    }

    const bool attacker_is_pc = inputs.attacker_slot < 4;
    const bool target_is_pc = inputs.target_slot < 4;
    if (attacker_is_pc == target_is_pc) {
        result.reason = CounterResultReason::SameSide;
        return result;
    }

    if (inputs.attack_was_critical) {
        result.reason = CounterResultReason::CriticalSuppressed;
        return result;
    }

    const bool force_counter = (inputs.target_status_flags & 0x2) != 0
        || (inputs.target_status_flags & 0x800000) != 0;
    if (!force_counter) {
        if (inputs.target_base_counter_chance == 0) {
            result.reason = CounterResultReason::ZeroBaseChance;
            return result;
        }

        if (!counter_rand.has_value()) {
            result.reason = CounterResultReason::RandomFailed;
            return result;
        }
        result.counter_rand = *counter_rand;
        result.draws_consumed = 1;
        if ((*counter_rand % 100) >= inputs.target_current_counter_chance) {
            result.reason = CounterResultReason::RandomFailed;
            return result;
        }
    }

    if (inputs.attacker_action_marker == 7) {
        if ((inputs.target_movement_flags & 0x80) == 0) {
            result.reason = CounterResultReason::MovementFlagSuppressed;
            return result;
        }
        result.queued_field7_0xc = 1;
    } else if ((inputs.target_movement_flags & 0x40) == 0) {
        result.queued_field7_0xc = 1;
    } else {
        result.queued_field7_0xc = 0;
    }

    result.counter = true;
    result.reason = CounterResultReason::Counter;
    result.updated_current_counter_chance = force_counter ? 100 : 0;
    return result;
}

} // namespace

CounterSimulation simulate_counter_check(std::uint32_t state, const CounterInputs& inputs) {
    std::optional<std::uint16_t> counter_rand;
    std::uint32_t next_state = state;

    const bool no_draw =
        (inputs.target_status_flags & 0x6D00) != 0
        || ((inputs.attacker_slot < 4) == (inputs.target_slot < 4))
        || inputs.attack_was_critical
        || ((inputs.target_status_flags & 0x2) != 0)
        || ((inputs.target_status_flags & 0x800000) != 0)
        || inputs.target_base_counter_chance == 0;
    if (!no_draw) {
        const auto draw = draw_rand15(state);
        counter_rand = draw.value;
        next_state = draw.next_state;
    }

    auto result = simulate_counter_check_after_draw(inputs, counter_rand);
    result.end_state = result.draws_consumed == 0 ? state : next_state;
    return result;
}

CounterSimulation simulate_counter_check_from_rand(
    const CounterInputs& inputs,
    std::uint16_t counter_rand) {
    return simulate_counter_check_after_draw(inputs, counter_rand);
}

const char* counter_result_reason_name(CounterResultReason reason) {
    switch (reason) {
    case CounterResultReason::Counter:
        return "Counter";
    case CounterResultReason::StatusSuppressed:
        return "StatusSuppressed";
    case CounterResultReason::SameSide:
        return "SameSide";
    case CounterResultReason::CriticalSuppressed:
        return "CriticalSuppressed";
    case CounterResultReason::ZeroBaseChance:
        return "ZeroBaseChance";
    case CounterResultReason::RandomFailed:
        return "RandomFailed";
    case CounterResultReason::MovementFlagSuppressed:
        return "MovementFlagSuppressed";
    }
    return "RandomFailed";
}

} // namespace savor::predict
