#pragma once

#include <cstdint>
#include <vector>

namespace soasim::mld::model {

struct U32List {
    std::uint32_t pointer = 0;
    bool valid = false;
    std::vector<std::uint32_t> values{};
};

} // namespace soasim::mld::model
