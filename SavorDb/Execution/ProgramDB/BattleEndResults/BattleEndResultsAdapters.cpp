#include "BattleEndResultsAdapters.h"

#include <utility>

namespace savor::db::execution::programdb::battleend {

ProgramKindDescriptor BuildBattleEndResultsDescriptor(
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    savor::db::IAnalysisDb* analysis_db,
    BattleEndWorkflowPhaseRegistrationConfig config) {
    // Source-compatible API only. Program kind 8 now denotes the split
    // results-screen phase; the legacy victory-to-BATTLE_END v1 adapter is
    // intentionally unavailable.
    return BuildBattleResultsScreenDescriptor(
        execution_db,
        state_db,
        analysis_db,
        std::move(config));
}

} // namespace savor::db::execution::programdb::battleend
