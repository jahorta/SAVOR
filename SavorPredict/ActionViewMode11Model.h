#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace savor::predict {

struct ActionViewMode11Vec3Bits {
    std::uint32_t x_bits = 0;
    std::uint32_t y_bits = 0;
    std::uint32_t z_bits = 0;
};

struct ActionViewMode11CameraOperands {
    std::optional<ActionViewMode11Vec3Bits> current_position;
    std::optional<ActionViewMode11Vec3Bits> desired_position;
    std::optional<ActionViewMode11Vec3Bits> current_center;
    std::optional<ActionViewMode11Vec3Bits> desired_center;
    std::string provenance;
};

enum class ActionViewMode11Branch {
    Equal,
    Interpolation,
    ProvisionalInterpolation,
};

enum class ActionViewMode11Status {
    Matched,
    Provisional,
};

struct ActionViewMode11SetupResult {
    ActionViewMode11Status status = ActionViewMode11Status::Provisional;
    ActionViewMode11Branch branch =
        ActionViewMode11Branch::ProvisionalInterpolation;
    int substate = 1;
    int counter = 15;
    bool advance_on_setup_visit = true;
    std::string provenance;
};

struct ActionViewMode11CounterStep {
    int counter_before = 0;
    int counter_after = 0;
    bool clear_gate = false;
};

bool action_view_mode11_ppc_float_equal(
    std::uint32_t lhs_bits,
    std::uint32_t rhs_bits);

ActionViewMode11SetupResult select_action_view_mode11_branch(
    const std::optional<ActionViewMode11CameraOperands>& operands);

ActionViewMode11CounterStep advance_action_view_mode11_counter(int counter);

const char* action_view_mode11_branch_name(ActionViewMode11Branch branch);

} // namespace savor::predict
