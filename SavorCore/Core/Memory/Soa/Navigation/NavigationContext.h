#pragma once

#include <cstddef>
#include <cstdint>

namespace soa::navigation::ctx {

inline constexpr std::uint32_t CapturePc = 0x80111770u;
inline constexpr std::uint32_t PlayerWorksheetPointerAddress = 0x80347450u;
inline constexpr std::uint32_t AreaAddress = 0x80311ac4u;
inline constexpr std::uint32_t SubareaAddress = 0x80311ac8u;
inline constexpr std::uint32_t PostInputMovementSuppressAddress = 0x80347578u;
inline constexpr std::uint32_t StepDistanceCarryInAddress = 0x80347410u;
inline constexpr std::uint32_t GroundSelectorPointerOffset = 0x1b8u;
inline constexpr std::uint32_t GroundTblIdOffset = 0x2au;
inline constexpr std::size_t GroundSelectorRecordSize = 0x34u;

struct NavigationContext {
    std::uint32_t capture_pc{0};
    std::uint32_t player_worksheet{0};
    std::uint32_t area{0};
    std::uint8_t subarea{0};
    std::uint16_t motion_state{0};
    std::uint16_t motion_substate{0};
    std::uint32_t post_input_movement_suppress{0};

    float position_x{0.0f};
    float position_y{0.0f};
    float position_z{0.0f};
    std::uint32_t rotation_x_raw{0};
    std::uint32_t rotation_y_raw{0};
    std::uint32_t rotation_z_raw{0};

    float previous_position_x{0.0f};
    float previous_position_y{0.0f};
    float previous_position_z{0.0f};
    std::uint32_t previous_rotation_x_raw{0};
    std::uint32_t previous_rotation_y_raw{0};
    std::uint32_t previous_rotation_z_raw{0};

    float step_distance_carry_in{0.0f};
    bool has_ground{false};
    std::uint16_t ground_tbl_id{0};
};

} // namespace soa::navigation::ctx
