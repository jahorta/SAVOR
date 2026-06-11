#pragma once

#include <string>

struct sqlite3;

namespace savor::db {

bool Stage3cWorkflowSliceReady(sqlite3* db, std::string* reason_out);

} // namespace savor::db
