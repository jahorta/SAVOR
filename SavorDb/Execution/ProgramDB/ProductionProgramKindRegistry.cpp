#include "ProductionProgramKindRegistry.h"

#include <exception>
#include <string>
#include <utility>

#include "../../../SavorCore/Runner/Runtime/ProgramKind.h"

namespace savor::db::execution::programdb {
namespace {

bool Fail(std::string message, std::string* error_out) {
    if (error_out != nullptr) {
        *error_out = std::move(message);
    }
    return false;
}

} // namespace

ProductionProgramKindRegistryConfig MakeProductionProgramKindRegistryConfig(
    const std::filesystem::path& runtime_working_dir_root) {
    ProductionProgramKindRegistryConfig config{};
    config.tas_movie_validation.working_dir_root =
        runtime_working_dir_root / "tasmovie-validation";
    config.tas_movie_checkpoint_sterilization.working_dir_root =
        runtime_working_dir_root / "tasmovie-checkpoint-sterilization";
    config.seed_probe.working_dir_root =
        runtime_working_dir_root / "seedprobe";
    config.battle_context.working_dir_root =
        runtime_working_dir_root / "battle-context";
    config.battle_completion.working_dir_root =
        runtime_working_dir_root / "battle-completion";
    config.battle_record.working_dir_root =
        runtime_working_dir_root / "battle-record";
    config.battle_replay.working_dir_root =
        runtime_working_dir_root / "battle-replay";
    config.battle_single_turn.working_dir_root =
        runtime_working_dir_root / "battle-single-turn";
    return config;
}

bool BuildProductionProgramKindRegistry(
    const ProductionProgramKindRegistryDependencies& dependencies,
    ProductionProgramKindRegistryConfig config,
    ProgramKindRegistry* registry_out,
    std::string* error_out) {
    if (registry_out == nullptr) {
        return Fail("Production program registry output is required", error_out);
    }
    if (dependencies.execution_db == nullptr) {
        return Fail(
            "Production program registry execution DB dependency is required",
            error_out);
    }
    if (dependencies.state_db == nullptr) {
        return Fail(
            "Production program registry state DB dependency is required",
            error_out);
    }
    if (dependencies.analysis_db == nullptr) {
        return Fail(
            "Production program registry analysis DB dependency is required",
            error_out);
    }
    if (dependencies.authoring_db == nullptr) {
        return Fail(
            "Production program registry authoring DB dependency is required",
            error_out);
    }

    try {
        ProgramKindRegistry registry;
        auto battle_context =
            battlecontext::BuildBattleContextProgramDescriptor(
                dependencies.execution_db,
                dependencies.state_db,
                dependencies.analysis_db,
                std::move(config.battle_context));
        if (battle_context.program_kind
                != static_cast<std::int32_t>(savor::PK_BattleContext)
            || battle_context.job_materializer == nullptr
            || battle_context.workset_reconstruction == nullptr
            || battle_context.result_handler == nullptr) {
            return Fail("Battle Context production descriptor is incomplete", error_out);
        }
        if (!registry.Register(battle_context)
            || !registry.RegisterForStepKind("battle.context", battle_context)) {
            return Fail("Battle Context production descriptor registration failed", error_out);
        }

        auto battle_completion =
            battlecompletion::BuildBattleCompletionProgramDescriptor(
                dependencies.execution_db,
                dependencies.state_db,
                dependencies.analysis_db,
                std::move(config.battle_completion));
        if (battle_completion.program_kind
                != static_cast<std::int32_t>(savor::PK_BattleCompletion)
            || !battle_completion.full_phase_identity
            || battle_completion.job_materializer == nullptr
            || battle_completion.workset_reconstruction == nullptr
            || battle_completion.result_handler == nullptr) {
            return Fail("Battle Completion production descriptor is incomplete", error_out);
        }
        if (!registry.Register(battle_completion)
            || !registry.RegisterForStepKind("battle.completion", battle_completion)) {
            return Fail("Battle Completion production descriptor registration failed", error_out);
        }

        auto battle_record =
            battlerecord::BuildBattleRecordProgramDescriptor(
                dependencies.execution_db,
                dependencies.state_db,
                dependencies.analysis_db,
                std::move(config.battle_record));
        if (battle_record.program_kind
                != static_cast<std::int32_t>(savor::PK_BattleRecord)
            || !battle_record.full_phase_identity
            || battle_record.job_materializer == nullptr
            || battle_record.workset_reconstruction == nullptr
            || battle_record.result_handler == nullptr
            || battle_record.workflow_transition == nullptr) {
            return Fail("Battle Recording production descriptor is incomplete", error_out);
        }
        if (!registry.Register(battle_record)
            || !registry.RegisterForStepKind("battle.record", battle_record)) {
            return Fail("Battle Recording production descriptor registration failed", error_out);
        }

        auto battle_replay =
            battlereplay::BuildBattleReplayProgramDescriptor(
                dependencies.execution_db,
                dependencies.state_db,
                dependencies.analysis_db,
                std::move(config.battle_replay));
        if (battle_replay.program_kind !=
                static_cast<std::int32_t>(savor::PK_BattleReplay) ||
            !battle_replay.full_phase_identity ||
            battle_replay.job_materializer == nullptr ||
            battle_replay.workset_reconstruction == nullptr ||
            battle_replay.result_handler == nullptr ||
            battle_replay.workflow_transition != nullptr) {
            return Fail("Battle Replay production descriptor is incomplete", error_out);
        }
        if (!registry.Register(battle_replay) ||
            !registry.RegisterForStepKind("battle.replay", battle_replay)) {
            return Fail("Battle Replay production descriptor registration failed", error_out);
        }

        auto battle_single_turn =
            battle::BuildBattleSingleTurnProgramDescriptor(
                dependencies.execution_db,
                dependencies.state_db,
                dependencies.analysis_db,
                dependencies.authoring_db,
                std::move(config.battle_single_turn));
        if (battle_single_turn.program_kind
                != static_cast<std::int32_t>(savor::PK_BattleSingleTurnRunner)
            || !battle_single_turn.full_phase_identity
            || battle_single_turn.job_materializer == nullptr
            || battle_single_turn.workset_reconstruction == nullptr
            || battle_single_turn.result_handler == nullptr) {
            return Fail("Battle Single Turn production descriptor is incomplete", error_out);
        }
        if (!registry.Register(battle_single_turn)
            || !registry.RegisterForStepKind("battle.start", battle_single_turn)
            || !registry.RegisterForStepKind("battle.single_turn", battle_single_turn)) {
            return Fail("Battle Single Turn production descriptor registration failed", error_out);
        }

        auto seed_probe =
            seedprobe::BuildSeedProbeProgramDescriptor(
                dependencies.execution_db,
                dependencies.state_db,
                dependencies.analysis_db,
                dependencies.authoring_db,
                std::move(config.seed_probe));
        if (seed_probe.program_kind
                != static_cast<std::int32_t>(
                    savor::PK_SeedProbe)
            || seed_probe.job_materializer == nullptr
            || seed_probe.workset_reconstruction == nullptr
            || seed_probe.result_handler == nullptr) {
            return Fail(
                "SeedProbe production descriptor is incomplete",
                error_out);
        }
        if (!registry.Register(seed_probe)
            || !registry.RegisterForStepKind(
                "seedprobe.run",
                seed_probe)) {
            return Fail(
                "SeedProbe production descriptor registration failed",
                error_out);
        }

        auto tas_movie_validation =
            tasmovievalidation::BuildTasMovieValidationProgramDescriptor(
                dependencies.execution_db,
                dependencies.state_db,
                dependencies.analysis_db,
                std::move(config.tas_movie_validation));
        if (tas_movie_validation.program_kind
                != static_cast<std::int32_t>(savor::PK_TasMovie)
            || tas_movie_validation.job_materializer == nullptr
            || tas_movie_validation.workset_reconstruction == nullptr
            || tas_movie_validation.result_handler == nullptr) {
            return Fail(
                "TAS Movie validation production descriptor is incomplete",
                error_out);
        }
        if (!registry.Register(tas_movie_validation)
            || !registry.RegisterForStepKind(
                "tasmovie.establish_root_cursor",
                tas_movie_validation)
            || !registry.RegisterForStepKind(
                "tasmovie.validate_root",
                tas_movie_validation)
            || !registry.RegisterForStepKind(
                "tasmovie.validate_tree",
                tas_movie_validation)) {
            return Fail(
                "TAS Movie validation production descriptor registration failed",
                error_out);
        }

        auto tas_movie_checkpoint_sterilization =
            tasmoviecheckpointsterilization::
                BuildTasMovieCheckpointSterilizationProgramDescriptor(
                    dependencies.execution_db,
                    dependencies.state_db,
                    dependencies.analysis_db,
                    std::move(config.tas_movie_checkpoint_sterilization));
        if (tas_movie_checkpoint_sterilization.program_kind
                != static_cast<std::int32_t>(
                    savor::PK_TasMovieCheckpointSterilize)
            || tas_movie_checkpoint_sterilization.job_materializer == nullptr
            || tas_movie_checkpoint_sterilization.workset_reconstruction == nullptr
            || tas_movie_checkpoint_sterilization.result_handler == nullptr) {
            return Fail(
                "TAS Movie checkpoint sterilization production descriptor is incomplete",
                error_out);
        }
        if (!registry.Register(tas_movie_checkpoint_sterilization)
            || !registry.RegisterForStepKind(
                "tasmovie.checkpoint_sterilize",
                tas_movie_checkpoint_sterilization)) {
            return Fail(
                "TAS Movie checkpoint sterilization production descriptor registration failed",
                error_out);
        }

        *registry_out = std::move(registry);
    } catch (const std::exception& exception) {
        return Fail(
            std::string(
                "Production descriptor construction failed: ")
                + exception.what(),
            error_out);
    } catch (...) {
        return Fail(
            "Production descriptor construction failed",
            error_out);
    }

    if (error_out != nullptr) {
        error_out->clear();
    }
    return true;
}

} // namespace savor::db::execution::programdb
