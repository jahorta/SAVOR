#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace savor::runner::parallel::savordb {

struct FleetStartupSlotSnapshot {
    std::size_t worker_id = 0;
    std::uint32_t attempt_count = 0;
    std::uint32_t maximum_attempts = 0;
    bool ready = false;
    bool starting = false;
    bool retry_pending = false;
    bool exhausted = false;
    std::string terminal_diagnostic;

    bool operator==(const FleetStartupSlotSnapshot&) const = default;
};

struct FleetStartupSnapshot {
    std::size_t desired = 0;
    std::size_t ready = 0;
    std::size_t starting = 0;
    std::size_t retry_pending = 0;
    std::size_t exhausted = 0;
    std::vector<FleetStartupSlotSnapshot> worker_slots;

    [[nodiscard]] bool full_pool_ready() const noexcept {
        return ready == desired;
    }

    [[nodiscard]] bool full_pool_impossible() const noexcept {
        return exhausted != 0;
    }

    bool operator==(const FleetStartupSnapshot&) const = default;
};

} // namespace savor::runner::parallel::savordb
