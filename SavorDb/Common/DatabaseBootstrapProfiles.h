#pragma once

#include "DatabaseBootstrap.h"

namespace savor::db::core {
class DBService;
}

namespace savor::db::bootstrap {

bool ApplyDatabaseBootstrapProfile(
    core::DBService& service,
    DatabaseBootstrapProfileId profile_id,
    DatabaseBootstrapRecordCounts* counts,
    std::string* error_out = nullptr);

} // namespace savor::db::bootstrap
