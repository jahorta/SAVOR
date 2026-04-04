#include "MigrationRunner.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace simcore::db::migrations {

std::vector<MigrationContext> ListAllMigrationContexts() {
    return {
        MigrationContext::Execution,
        MigrationContext::State,
        MigrationContext::AnalysisSpine,
        MigrationContext::AnalysisSeedProbe,
        MigrationContext::AnalysisBattle,
        MigrationContext::Authoring,
        MigrationContext::UIRead,
        MigrationContext::Archive,
    };
}

std::vector<MigrationEntry> LoadContextMigrations(const std::filesystem::path& migration_root, MigrationContext context) {
    std::vector<MigrationEntry> entries;
    const auto dir = migration_root / FolderName(context);
    if (!std::filesystem::exists(dir)) {
        return entries;
    }

    for (const auto& item : std::filesystem::directory_iterator(dir)) {
        if (!item.is_regular_file()) {
            continue;
        }
        if (item.path().extension() != ".sql") {
            continue;
        }

        std::ifstream ifs(item.path(), std::ios::binary);
        std::ostringstream os;
        os << ifs.rdbuf();

        entries.push_back(MigrationEntry{
            .name = item.path().filename().string(),
            .path = item.path(),
            .sql = os.str(),
        });
    }

    std::sort(entries.begin(), entries.end(), [](const MigrationEntry& a, const MigrationEntry& b) {
        return a.name < b.name;
    });

    return entries;
}

bool RunContextMigrations(
    const std::filesystem::path& migration_root,
    MigrationContext context,
    const MigrationExecutor& exec,
    std::string* error_out) {
    const auto entries = LoadContextMigrations(migration_root, context);
    for (const auto& entry : entries) {
        if (!exec(context, entry, error_out)) {
            return false;
        }
    }
    return true;
}

} // namespace simcore::db::migrations
