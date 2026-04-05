#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Common/Migrations/MigrationRunner.h"

TEST(Stage1DbMigrate, HasMigrationPerContext) {
    namespace fs = std::filesystem;
    using namespace simcore::db::migrations;

    const auto root = fs::weakly_canonical(fs::path("../SimCoreDB/migration"));
    const auto contexts = ListAllMigrationContexts();

    ASSERT_EQ(contexts.size(), 8u);

    for (const auto context : contexts) {
        const auto entries = LoadContextMigrations(root, context);
        ASSERT_FALSE(entries.empty()) << "Expected at least one migration in context " << ToString(context);

        bool executed = RunContextMigrations(
            root,
            context,
            [](MigrationContext, const MigrationEntry& entry, std::string* error_out) {
                if (entry.sql.empty()) {
                    if (error_out) *error_out = "empty sql";
                    return false;
                }
                return true;
            });

        EXPECT_TRUE(executed) << "No-op migration failed in context " << ToString(context);
    }
}
