#pragma once

#include "NJCMParser.h"
#include "../Model/NjcmModel.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace soasim::mld::parsing::satools_parity {

[[nodiscard]] model::NjcmDecodedChunk decodeWithObjectModel(std::span<const std::uint8_t> njcmData,
    std::size_t chunkOffset,
    std::size_t chunkDataSize,
    bool chunkSizeLittleEndian,
    bool sawPof0Chunk,
    const NjcmParsePolicy& policy,
    std::span<const std::uint8_t> pof0Data);

} // namespace soasim::mld::parsing::satools_parity
