#pragma once

#include <vector>

namespace savor::predict {

// Models the game/library qsort routine at 8025eb4c for callers that sort
// fixed-size records by a single signed integer comparator key.
std::vector<int> soa_qsort_indices_by_key_ascending(const std::vector<int>& sort_keys);

} // namespace savor::predict
