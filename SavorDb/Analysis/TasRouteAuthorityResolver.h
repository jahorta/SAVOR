#pragma once

#include <cstdint>
#include <optional>
#include <string>

struct sqlite3;

namespace savor::db::analysis {

struct TasRouteRootAuthorityResolution {
    std::optional<std::int64_t> root_establishment_attempt_id;
    std::string diagnostic;
};

TasRouteRootAuthorityResolution ResolveTasRouteRootAuthority(
    sqlite3* execution,
    sqlite3* analysis,
    std::int64_t workflow_instance_id,
    std::int64_t entry_savestate_id);

} // namespace savor::db::analysis
