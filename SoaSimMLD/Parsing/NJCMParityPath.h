#pragma once

#include "NJCMParser.h"

namespace soasim::mld::parsing {

[[nodiscard]] model::NjcmDecodedChunk decodeNjcmChunkSaToolsParity(std::span<const std::uint8_t> njcmData,
    std::size_t chunkOffset,
    std::size_t chunkDataSize,
    bool chunkSizeLittleEndian,
    bool sawPof0Chunk,
    const NjcmParsePolicy& policy,
    std::span<const std::uint8_t> pof0Data);

} // namespace soasim::mld::parsing
