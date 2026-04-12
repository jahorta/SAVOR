#pragma once

#include "NJCMDecodeContext.h"

namespace soasim::mld::parsing {

[[nodiscard]] std::optional<model::NjAttachRecord> parseAttachRecord(const NjcmDecodeContext& ctx, std::size_t attachOffset);

} // namespace soasim::mld::parsing
