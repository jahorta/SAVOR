#pragma once

#include <cstdint>
#include <optional>

namespace savor::predict {

struct RngDraw {
    std::uint32_t next_state = 0;
    std::uint16_t value = 0;
};

std::uint32_t advance_once(std::uint32_t state);
RngDraw draw_rand15(std::uint32_t state);
std::optional<int> bounded_distance(std::uint32_t start, std::uint32_t target, int max_draws);

} // namespace savor::predict
