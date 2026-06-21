#include "RngCore.h"

namespace savor::predict {

namespace {
constexpr std::uint32_t kMultiplier = 0x41C64E6D;
constexpr std::uint32_t kIncrement = 0x00003039;
}

std::uint32_t advance_once(std::uint32_t state) {
    return state * kMultiplier + kIncrement;
}

RngDraw draw_rand15(std::uint32_t state) {
    const auto next = advance_once(state);
    return {next, static_cast<std::uint16_t>((next >> 16) & 0x7FFF)};
}

std::optional<int> bounded_distance(std::uint32_t start, std::uint32_t target, int max_draws) {
    if (start == target) {
        return 0;
    }

    auto state = start;
    for (int draws = 1; draws <= max_draws; ++draws) {
        state = advance_once(state);
        if (state == target) {
            return draws;
        }
    }

    return std::nullopt;
}

} // namespace savor::predict
