#pragma once

#include "../ProgramKindDescriptor.h"
#include "../../../../SavorCore/Core/Input/SoaBattle/ActionTypes.h"
#include "../../../../SavorCore/Core/Memory/Soa/Battle/BattleContext.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace savor::db {
struct IAnalysisDb;
struct IAuthoringDb;
struct BattlePlanSnapshot;
struct IExecutionDb;
struct IStateDb;
}

namespace savor::db::execution::programdb::battle {

enum class BattleTargetAvailability {
    Available,
    Unavailable,
    Unknown,
};

[[nodiscard]] bool ValidateBattlePlanTurnSequence(
    const savor::db::BattlePlanSnapshot& plan,
    std::string* error_out = nullptr);

[[nodiscard]] BattleTargetAvailability ClassifyBattleTurnTargets(
    const soa::battle::ctx::BattleContext* context,
    const soa::battle::actions::BattleTurnCommandSet& commands) noexcept;

struct BattleSingleTurnPhaseRegistrationConfig {
    std::filesystem::path working_dir_root;
};

// ExecutionRuntime-backed production descriptor.
ProgramKindDescriptor BuildBattleSingleTurnProgramDescriptor(
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    savor::db::IAnalysisDb* analysis_db,
    savor::db::IAuthoringDb* authoring_db,
    BattleSingleTurnPhaseRegistrationConfig config = {});

// Backend command used by planning/UI callers when automatic wave triggering
// is disabled.  Only successful ReachedNextTurn jobs from the complete parent
// wave are eligible.  Repeating the exact request is idempotent.
struct RequestBattleWaveContinuationCommand {
    std::int64_t workflow_instance_id = 0;
    std::int64_t parent_workflow_step_id = 0;
    std::int64_t parent_wave_id = 0;
    std::vector<std::int64_t> selected_turn_job_ids;
    int priority = 0;
    std::string requested_by;
};

struct RequestBattleWaveContinuationReceipt {
    std::vector<std::int64_t> child_wave_ids;
    std::size_t newly_created_wave_count = 0;
};

struct BattleWaveCandidateRank {
    std::uint32_t cumulative_fake_attacks = 0;
    std::uint64_t delta_vi = 0;
    std::uint32_t pred_passed = 0;
    std::int64_t stable_job_id = 0;
};

[[nodiscard]] bool BattleWaveCandidateRanksBefore(
    const BattleWaveCandidateRank& lhs,
    const BattleWaveCandidateRank& rhs) noexcept;

bool RequestBattleWaveContinuation(
    savor::db::IExecutionDb* execution_db,
    savor::db::IAnalysisDb* analysis_db,
    savor::db::IAuthoringDb* authoring_db,
    const RequestBattleWaveContinuationCommand& command,
    RequestBattleWaveContinuationReceipt* receipt_out = nullptr,
    std::string* error_out = nullptr);

} // namespace savor::db::execution::programdb::battle
