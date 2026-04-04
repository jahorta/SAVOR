#include "Common/Migrations/MigrationRunner.h"

namespace simcore::db {

bool Stage1ScaffoldReady() {
    return !ListAllMigrationContexts().empty();
}

} // namespace simcore::db
