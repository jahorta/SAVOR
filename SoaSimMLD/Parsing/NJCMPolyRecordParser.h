#pragma once

#include "NJCMDecodeContext.h"

namespace soasim::mld::parsing {

void parsePolyRecords(const NjcmDecodeContext& ctx, std::size_t polyListOffset, model::NjAttachRecord& attach);

} // namespace soasim::mld::parsing
