#pragma once

#include <cstdint>
#include <string>

struct sqlite3;

namespace simcore::db::uiread::projectors {

class ArtifactProjector {
public:
    explicit ArtifactProjector(sqlite3* db);

    bool ProjectFromOutbox(const std::string& projector_name, int max_batch_size, std::string* error_out, int max_attempts = 5);

    bool ProjectAll(std::string* error_out);

private:

    sqlite3* db_ = nullptr;
};

} // namespace simcore::db::uiread::projectors
