#pragma once

#include <cstdint>

namespace simcore::db::types {

struct JobId {
    std::int64_t value = 0;
};

struct JobSetId {
    std::int64_t value = 0;
};

struct EventId {
    std::int64_t value = 0;
};

} // namespace simcore::db::types
