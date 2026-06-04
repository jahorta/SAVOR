#pragma once

#include "../Model/GvmTextureModel.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace soasim::gvm::parsing {

struct ParseOptions {
    bool decodeBaseLevel = true;
    bool keepRawEncodedPayload = false;
};

[[nodiscard]] model::GvmArchive parseGvmArchive(std::span<const std::uint8_t> bytes,
    std::size_t startOffset,
    const ParseOptions& options = {});

[[nodiscard]] model::GvrTexture parseGvrTexture(std::span<const std::uint8_t> bytes,
    std::size_t sourceOffset,
    const ParseOptions& options = {});

} // namespace soasim::gvm::parsing
