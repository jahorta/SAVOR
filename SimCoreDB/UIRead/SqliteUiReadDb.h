#pragma once

#include <sqlite3.h>

#include "IUiReadDb.h"

namespace simcore::db {

class SqliteUiReadDb final : public IUiReadDb {
public:
    explicit SqliteUiReadDb(sqlite3* db);

    std::optional<UiProjectionCheckpoint> GetProjectionCheckpoint(
        const std::string& projector_name) const override;
    bool UpsertProjectionCheckpoint(const UiProjectionCheckpoint& checkpoint) override;

private:
    sqlite3* db_ = nullptr;
};

} // namespace simcore::db
