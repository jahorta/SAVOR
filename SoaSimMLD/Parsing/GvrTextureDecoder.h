#pragma once

#include "../Model/MldTextureArchiveModel.h"

#include <cstdint>
#include <string>
#include <vector>

namespace soasim::mld::parsing {

struct DecodedRgbaTexture {
    bool decoded = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba8{};
    std::vector<std::string> diagnostics{};
};

[[nodiscard]] DecodedRgbaTexture decodeGvrToRgba8(const model::MldTextureEntry& entry);

} // namespace soasim::mld::parsing
