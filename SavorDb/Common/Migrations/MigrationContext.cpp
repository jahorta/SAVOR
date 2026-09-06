#include "MigrationContext.h"

namespace savor::db::migrations {

const char* ToString(MigrationContext context) {
    switch (context) {
    case MigrationContext::Execution: return "Execution";
    case MigrationContext::State: return "State";
    case MigrationContext::AnalysisSeedProbe: return "AnalysisSeedProbe";
    case MigrationContext::AnalysisTasMovie: return "AnalysisTasMovie";
    case MigrationContext::AnalysisBattle: return "AnalysisBattle";
    case MigrationContext::Authoring: return "Authoring";
    case MigrationContext::UIRead: return "UIRead";
    case MigrationContext::Archive: return "Archive";
    }
    return "Unknown";
}

std::string FolderName(MigrationContext context) {
    return std::string(ToString(context));
}

} // namespace savor::db::migrations
