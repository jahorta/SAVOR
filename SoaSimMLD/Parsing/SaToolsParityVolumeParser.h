#pragma once

#include "NJCMDecodeContext.h"

namespace soasim::mld::parsing::satools_parity {

void parseVolumeChunk(const NjcmDecodeContext& ctx,
    std::size_t chunkStart,
    std::size_t chunkEnd,
    std::uint8_t type,
    model::NjPolyChunkRecord& polyChunk,
    model::NjSemanticPolygon& semanticPolygon,
    std::size_t& attachTriangleCount);

} // namespace soasim::mld::parsing::satools_parity
