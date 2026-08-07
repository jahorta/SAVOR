#pragma once

#include <filesystem>
#include <string>

#include "BattleContext/BattleContextProbePhaseRegistration.h"
#include "BattleSingleTurn/BattleSingleTurnPhaseRegistration.h"
#include "NavigationContext/NavigationContextPhaseRegistration.h"
#include "ProgramKindRegistry.h"
#include "SeedProbe/SeedProbeProgram.h"
#include "TasMovieValidation/TasMovieValidationProgram.h"
#include "TasMovieValidation/TasMovieCheckpointSterilizationProgram.h"

namespace savor::db::execution::programdb {

struct ProductionProgramKindRegistryDependencies {
    savor::db::IExecutionDb* execution_db = nullptr;
    savor::db::IStateDb* state_db = nullptr;
    savor::db::IAnalysisDb* analysis_db = nullptr;
    savor::db::IAuthoringDb* authoring_db = nullptr;
};

struct ProductionProgramKindRegistryConfig {
    tasmovievalidation::TasMovieValidationProgramConfig tas_movie_validation;
    tasmoviecheckpointsterilization::TasMovieCheckpointSterilizationProgramConfig
        tas_movie_checkpoint_sterilization;
    seedprobe::SeedProbeProgramConfig seed_probe;
    battlecontext::BattleContextProbePhaseRegistrationConfig battle_context;
    battle::BattleSingleTurnPhaseRegistrationConfig battle_single_turn;
    navigationcontext::NavigationContextPhaseRegistrationConfig navigation_context;
};

ProductionProgramKindRegistryConfig MakeProductionProgramKindRegistryConfig(
    const std::filesystem::path& runtime_working_dir_root);

bool BuildProductionProgramKindRegistry(
    const ProductionProgramKindRegistryDependencies& dependencies,
    ProductionProgramKindRegistryConfig config,
    ProgramKindRegistry* registry_out,
    std::string* error_out = nullptr);

} // namespace savor::db::execution::programdb
