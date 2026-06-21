#include "CounterModel.h"

namespace savor::predict {

CounterSimulation simulate_counter_check(std::uint32_t state, const CounterInputs& inputs) {
    CounterSimulation result;
    result.end_state = state;
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

        const auto draw = draw_rand15(state);
        state = draw.next_state;
        result.end_state = state;
        result.counter_rand = draw.value;
        result.draws_consumed = 1;
        if ((draw.value % 100) >= inputs.target_current_counter_chance) {
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
