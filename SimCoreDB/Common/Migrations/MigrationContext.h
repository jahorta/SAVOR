#pragma once

#include <string>

namespace simcore::db::migrations {

enum class MigrationContext {
    Execution,
    State,
    // Logical schema groups inside a single Analysis DB file.
    AnalysisSpine,
    AnalysisSeedProbe,
    AnalysisBattle,
    Authoring,
    UIRead,
    Archive,
};

const char* ToString(MigrationContext context);

std::string FolderName(MigrationContext context);

} // namespace simcore::db::migrations
