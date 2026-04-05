#pragma once

#include <string>

struct sqlite3;

namespace simcore::db {

bool Stage1ScaffoldReady();
bool Stage3cWorkflowSliceReady(sqlite3* db, std::string* reason_out);

} // namespace simcore::db
