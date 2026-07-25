#include "ActionViewMode11Model.h"

#include <bit>

namespace savor::predict {
namespace {

bool vec3_equal(
    const ActionViewMode11Vec3Bits& lhs,
    const ActionViewMode11Vec3Bits& rhs) {
    return action_view_mode11_ppc_float_equal(lhs.x_bits, rhs.x_bits)
        && action_view_mode11_ppc_float_equal(lhs.y_bits, rhs.y_bits)
        && action_view_mode11_ppc_float_equal(lhs.z_bits, rhs.z_bits);
}

} // namespace

bool action_view_mode11_ppc_float_equal(
    std::uint32_t lhs_bits,
    std::uint32_t rhs_bits) {
    return std::bit_cast<float>(lhs_bits) == std::bit_cast<float>(rhs_bits);
}

ActionViewMode11SetupResult select_action_view_mode11_branch(
    const std::optional<ActionViewMode11CameraOperands>& operands) {
    if (!operands.has_value()
        || !operands->current_position.has_value()
        || !operands->desired_position.has_value()
        || !operands->current_center.has_value()
        || !operands->desired_center.has_value()) {
        return {
            .status = ActionViewMode11Status::Provisional,
            .branch = ActionViewMode11Branch::ProvisionalInterpolation,
            .substate = 1,
            .counter = 15,
            .advance_on_setup_visit = true,
            .provenance =
                "mode-0x11 camera operands are incomplete; the unequal branch "
                "is selected provisionally so scheduling remains observable",
        };
    }

    const bool equal =
        vec3_equal(*operands->current_position, *operands->desired_position)
        && vec3_equal(*operands->current_center, *operands->desired_center);
    if (equal) {
        return {
            .status = ActionViewMode11Status::Matched,
            .branch = ActionViewMode11Branch::Equal,
            .substate = 2,
            .counter = 5,
            .advance_on_setup_visit = false,
            .provenance =
                "FUN_800521C4 decoded all current and desired camera components "
                "as equal PPC single-precision values",
        };
    }
    return {
        .status = ActionViewMode11Status::Matched,
        .branch = ActionViewMode11Branch::Interpolation,
        .substate = 1,
        .counter = 15,
        .advance_on_setup_visit = true,
        .provenance =
            "FUN_800521C4 observed at least one unequal decoded camera component",
    };
}

ActionViewMode11CounterStep advance_action_view_mode11_counter(int counter) {
    ActionViewMode11CounterStep result{
        .counter_before = counter,
        .counter_after = counter,
    };
    if (counter <= 0) {
        result.counter_after = 0;
        result.clear_gate = true;
        return result;
    }
    result.counter_after = counter - 1;
    return result;
}

const char* action_view_mode11_branch_name(ActionViewMode11Branch branch) {
    switch (branch) {
    case ActionViewMode11Branch::Equal:
        return "equal";
    case ActionViewMode11Branch::Interpolation:
        return "interpolation";
    case ActionViewMode11Branch::ProvisionalInterpolation:
        return "provisional_interpolation";
    }
    return "provisional_interpolation";
}

} // namespace savor::predict
