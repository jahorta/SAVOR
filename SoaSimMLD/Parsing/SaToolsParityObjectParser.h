#pragma once

#include "NJCMDecodeContext.h"

#include <optional>
#include <vector>

namespace soasim::mld::parsing::satools_parity {

[[nodiscard]] std::optional<model::NjObjectRecord> parseObject(const NjcmDecodeContext& ctx,
    std::size_t objOff,
    std::vector<std::size_t>& traversalStack);

} // namespace soasim::mld::parsing::satools_parity
