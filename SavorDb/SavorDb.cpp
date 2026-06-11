#include "Common/Migrations/MigrationRunner.h"

namespace savor::db {

bool Stage1ScaffoldReady() {
    return !migrations::ListAllMigrationContexts().empty();
}

bool Stage3cWorkflowSliceReady(sqlite3* db, std::string* reason_out) {
    constexpr auto kRequiredStage3bVersion = 202604051200LL;

    if (db == nullptr) {
        if (reason_out) {
            *reason_out = "Stage3c workflow slice readiness failed: null sqlite3 handle.";
        }
        return false;
    }

    std::string err;
    if (!migrations::EnsureMigrationTrackingTables(db, &err)) {
        if (reason_out) {
            *reason_out = "Stage3c workflow slice readiness failed: unable to read migration tracking tables: " + err;
        }
        return false;
    }

    const auto execution_version =
        migrations::GetCurrentContextSchemaVersion(db, migrations::MigrationContext::Execution, &err);
    if (!execution_version.has_value()) {
        if (reason_out) {
            *reason_out = "Stage3c workflow slice readiness failed: unable to read Execution schema version: " + err;
        }
        return false;
    }
    if (*execution_version < kRequiredStage3bVersion) {
        if (reason_out) {
            *reason_out = "Stage3c workflow slice readiness failed: Execution schema version "
                + std::to_string(*execution_version)
                + " is older than required Stage 3b baseline "
                + std::to_string(kRequiredStage3bVersion) + ".";
        }
        return false;
    }

    const auto ui_read_version =
        migrations::GetCurrentContextSchemaVersion(db, migrations::MigrationContext::UIRead, &err);
    if (!ui_read_version.has_value()) {
        if (reason_out) {
            *reason_out = "Stage3c workflow slice readiness failed: unable to read UIRead schema version: " + err;
        }
        return false;
    }
    if (*ui_read_version < kRequiredStage3bVersion) {
        if (reason_out) {
            *reason_out = "Stage3c workflow slice readiness failed: UIRead schema version "
                + std::to_string(*ui_read_version)
                + " is older than required Stage 3b baseline "
                + std::to_string(kRequiredStage3bVersion) + ".";
        }
        return false;
    }

    if (reason_out) {
        *reason_out = "OK";
    }
    return true;
}

} // namespace savor::db
