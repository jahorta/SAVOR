#pragma once

#include "NJCMDecodeContext.h"

#include <optional>

namespace soasim::mld::parsing::satools_parity {

[[nodiscard]] std::optional<model::NjAttachRecord> parseAttach(const NjcmDecodeContext& ctx, std::size_t attachOffset);

} // namespace soasim::mld::parsing::satools_parity
