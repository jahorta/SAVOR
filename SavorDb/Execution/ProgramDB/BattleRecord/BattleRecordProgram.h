#pragma once

#include "../ProgramKindDescriptor.h"

#include <filesystem>
#include <optional>

#include "../../../../SavorCore/Phases/Programs/BattleRecord/BattleRecordModule.h"

namespace savor::db {
struct IAnalysisDb;
struct IExecutionDb;
struct IStateDb;
struct BattleCompletionRecord;
}

namespace savor::db::execution::programdb::battlerecord {

struct BattleRecordProgramConfig {
    std::filesystem::path working_dir_root;
};

ProgramKindDescriptor BuildBattleRecordProgramDescriptor(
    IExecutionDb* execution_db,
    IStateDb* state_db,
    IAnalysisDb* analysis_db,
    BattleRecordProgramConfig config = {});

// Canonical coordination-owned replay-plan compiler shared by battle.record
// and battle.replay. Workers receive only its concrete, immutable result.
std::optional<savor::runtime::battlerecord::BattleReplayPlanV1>
BuildCanonicalBattleReplayPlan(
    IStateDb* state_db,
    IAnalysisDb* analysis_db,
    const BattleCompletionRecord& completion,
    std::string* error_out = nullptr);

} // namespace savor::db::execution::programdb::battlerecord
