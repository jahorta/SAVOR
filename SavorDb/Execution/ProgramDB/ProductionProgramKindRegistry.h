#pragma once

#include <filesystem>
#include <string>

#include "BattleContext/BattleContextProgram.h"
#include "BattleCompletion/BattleCompletionProgram.h"
#include "BattleRecord/BattleRecordProgram.h"
#include "BattleRecord/BattleReplayProgram.h"
#include "BattleSingleTurn/BattleSingleTurnProgram.h"
#include "ProgramKindRegistry.h"
#include "SeedProbe/SeedProbeProgram.h"
#include "TasMovieValidation/TasMovieValidationProgram.h"
#include "TasMovieValidation/TasMovieCheckpointSterilizationProgram.h"
#include "TasMovieValidation/TasMovieInputEpochProgram.h"

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
    tasmovieinputepoch::TasMovieInputEpochProgramConfig
        tas_movie_input_epoch_annotation;
    tasmovieinputepoch::TasMovieInputEpochProgramConfig
        tas_movie_input_epoch_rewrite;
    seedprobe::SeedProbeProgramConfig seed_probe;
    battlecontext::BattleContextProgramConfig battle_context;
    battlecompletion::BattleCompletionProgramConfig battle_completion;
    battlerecord::BattleRecordProgramConfig battle_record;
    battlereplay::BattleReplayProgramConfig battle_replay;
    battle::BattleSingleTurnPhaseRegistrationConfig battle_single_turn;
};

ProductionProgramKindRegistryConfig MakeProductionProgramKindRegistryConfig(
    const std::filesystem::path& runtime_working_dir_root);

bool BuildProductionProgramKindRegistry(
    const ProductionProgramKindRegistryDependencies& dependencies,
    ProductionProgramKindRegistryConfig config,
    ProgramKindRegistry* registry_out,
    std::string* error_out = nullptr);

} // namespace savor::db::execution::programdb
