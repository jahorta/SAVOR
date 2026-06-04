#pragma once

#include "../Model/GvmTextureModel.h"

namespace soasim::gvm::decoding {

[[nodiscard]] model::RgbaImage decodeBaseLevel(const model::GvrTexture& texture,
    std::vector<std::string>& diagnostics);

[[nodiscard]] model::RgbaImage makeErrorTexture(std::uint32_t width = 64, std::uint32_t height = 64);

} // namespace soasim::gvm::decoding
