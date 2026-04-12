#pragma once

#include "NJCMDecodeContext.h"

namespace soasim::mld::parsing {

void parseVertexRecords(const NjcmDecodeContext& ctx, std::size_t vertexListOffset, model::NjAttachRecord& attach);

} // namespace soasim::mld::parsing
