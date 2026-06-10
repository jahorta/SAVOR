#pragma once

#include <cstdint>

namespace savor::db::types {

struct JobId {
    std::int64_t value = 0;
};

struct JobSetId {
    std::int64_t value = 0;
};

struct EventId {
    std::int64_t value = 0;
};

} // namespace savor::db::types
