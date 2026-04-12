#pragma once

#include "../Model/WorldModel.h"

#include <cstdint>
#include <span>
#include <vector>
#include <string_view>

namespace soasim::mld::parsing {

struct RawEntry {
    std::uint32_t sourceEntryId = 0;
    std::string_view fxnName{};
    std::uint32_t tblId = 0;
    model::Transform transform{};
    std::vector<std::uint32_t> objectAddresses{};
    std::span<const std::uint8_t> payload{};
};

class EntryHandler {
public:
    virtual ~EntryHandler() = default;

    [[nodiscard]] virtual bool canHandle(std::string_view fxnName) const = 0;
    virtual void parse(const RawEntry& entry, model::WorldModel& out) const = 0;
};

} // namespace soasim::mld::parsing
