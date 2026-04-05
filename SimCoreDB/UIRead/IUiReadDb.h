#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "../Common/Types/UtcTimestamp.h"

namespace simcore::db {

struct UiProjectionCheckpoint {
    std::string projector_name;
    std::string last_event_id;
    std::int64_t last_outbox_id = 0;
    types::UtcTimePoint updated_at_utc{};
};

struct IUiReadDb {
    virtual ~IUiReadDb() = default;

    // Gets a projector checkpoint row by projector name.
    virtual std::optional<UiProjectionCheckpoint> GetProjectionCheckpoint(
        const std::string& projector_name) const = 0;

    // Creates or updates a projector checkpoint.
    virtual bool UpsertProjectionCheckpoint(
        const UiProjectionCheckpoint& checkpoint) = 0;
};

} // namespace simcore::db
