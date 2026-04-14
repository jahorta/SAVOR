#pragma once

#include "../Model/NjtlModel.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace soasim::mld::parsing {

[[nodiscard]] model::NjtlBlock parseNjtlBlock(std::span<const std::uint8_t> data,
    std::size_t chunkOffset,
    std::size_t chunkDataSize,
    bool chunkSizeLittleEndian);

} // namespace soasim::mld::parsing
