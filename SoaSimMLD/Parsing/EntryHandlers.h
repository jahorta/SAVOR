#pragma once

#include "../Model/WorldModel.h"

#include <cstdint>
#include <span>

namespace soasim::mld::parsing {

struct RawEntry {
    std::uint32_t sourceEntryId = 0;
    std::uint32_t fxn = 0;
    model::Transform transform{};
    std::span<const std::uint8_t> payload{};
};

class EntryHandler {
public:
    virtual ~EntryHandler() = default;

    [[nodiscard]] virtual bool canHandle(std::uint32_t fxn) const = 0;
    virtual void parse(const RawEntry& entry, model::WorldModel& out) const = 0;
};

} // namespace soasim::mld::parsing
