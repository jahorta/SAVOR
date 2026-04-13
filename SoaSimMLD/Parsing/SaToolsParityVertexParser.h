#pragma once

#include "NJCMDecodeContext.h"

namespace soasim::mld::parsing::satools_parity {

void parseVertexChunks(const NjcmDecodeContext& ctx, std::size_t vertexListOffset, model::NjAttachRecord& attach);

} // namespace soasim::mld::parsing::satools_parity
