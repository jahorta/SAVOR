#pragma once

#include "../Model/MldTextureArchiveModel.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace soasim::mld::parsing {

[[nodiscard]] model::MldTextureArchive parseMldTextureArchive(std::span<const std::uint8_t> bytes,
    std::size_t textureTableOffset);

} // namespace soasim::mld::parsing
