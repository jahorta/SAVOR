#pragma once

#include "NJCMDecodeContext.h"

namespace soasim::mld::parsing {

[[nodiscard]] std::optional<model::NjObjectRecord> parseObjectRecord(const NjcmDecodeContext& ctx,
    std::size_t objOff,
    std::vector<std::size_t>& traversalStack);

} // namespace soasim::mld::parsing
