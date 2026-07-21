#pragma once

#include <filesystem>

#include "../ProgramKindDescriptor.h"

namespace savor::db {
struct IAnalysisDb;
struct IAuthoringDb;
struct IExecutionDb;
struct IStateDb;
}

namespace savor::db::execution::programdb::battleend {

struct BattleEndWorkflowPhaseRegistrationConfig {
    savor::db::IAuthoringDb* authoring_db = nullptr;
    std::filesystem::path working_dir_root;
};

using BattleEndResultsPhaseRegistrationConfig = BattleEndWorkflowPhaseRegistrationConfig;

ProgramKindDescriptor BuildBattleCompletionDescriptor(
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    savor::db::IAnalysisDb* analysis_db,
    BattleEndWorkflowPhaseRegistrationConfig config = {});

ProgramKindDescriptor BuildFieldReturnSeedProbeDescriptor(
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    savor::db::IAnalysisDb* analysis_db,
    BattleEndWorkflowPhaseRegistrationConfig config = {});

ProgramKindDescriptor BuildFieldReturnSeedProbeGridDescriptor(
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    savor::db::IAnalysisDb* analysis_db,
    BattleEndWorkflowPhaseRegistrationConfig config = {});

ProgramKindDescriptor BuildFieldReturnSeedProbeUniqueDescriptor(
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    savor::db::IAnalysisDb* analysis_db,
    BattleEndWorkflowPhaseRegistrationConfig config = {});

ProgramKindDescriptor BuildFieldReturnSeedMaterializeDescriptor(
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    savor::db::IAnalysisDb* analysis_db,
    BattleEndWorkflowPhaseRegistrationConfig config = {});

ProgramKindDescriptor BuildBattleResultsScreenDescriptor(
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    savor::db::IAnalysisDb* analysis_db,
    BattleEndWorkflowPhaseRegistrationConfig config = {});

ProgramKindDescriptor BuildBattleEndResultsDescriptor(
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    savor::db::IAnalysisDb* analysis_db,
    BattleEndWorkflowPhaseRegistrationConfig config = {});

} // namespace savor::db::execution::programdb::battleend
