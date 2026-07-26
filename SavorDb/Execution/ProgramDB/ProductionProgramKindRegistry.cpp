#include "ProductionProgramKindRegistry.h"

#include <array>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

#include "../../../SavorCore/Runner/IPC/Wire.h"

namespace savor::db::execution::programdb {

namespace {

struct ExpectedDescriptor {
    std::int32_t program_kind;
    std::string_view program_name;
};

struct ExpectedStepDescriptor {
    std::string_view step_kind;
    ExpectedDescriptor descriptor;
};

bool Fail(std::string message, std::string* error_out) {
    if (error_out != nullptr) {
        *error_out = std::move(message);
    }
    return false;
}

bool ValidateWorkflowDescriptor(
    const ProgramKindDescriptor* descriptor,
    const ExpectedDescriptor& expected,
    std::string_view lookup_name,
    std::string* error_out) {
    const auto Prefix = [lookup_name]() {
        std::string prefix("Production program registry descriptor '");
        prefix.append(lookup_name);
        prefix.append("'");
        return prefix;
    };

    if (descriptor == nullptr) {
        return Fail(Prefix() + " is missing", error_out);
    }
    if (descriptor->program_kind != expected.program_kind) {
        return Fail(
            Prefix() + " has program kind "
                + std::to_string(descriptor->program_kind)
                + ", expected " + std::to_string(expected.program_kind),
            error_out);
    }
    if (descriptor->program_name != expected.program_name) {
        return Fail(
            Prefix() + " resolves to program name '"
                + descriptor->program_name + "', expected '"
                + std::string(expected.program_name) + "'",
            error_out);
    }
    if (descriptor->job_persistence == nullptr
        && descriptor->graph_job_persistence == nullptr) {
        return Fail(Prefix() + " has no persistence adapter", error_out);
    }
    if (descriptor->runtime_init == nullptr) {
        return Fail(Prefix() + " has no runtime-init adapter", error_out);
    }
    if (descriptor->result_mapper == nullptr) {
        return Fail(Prefix() + " has no result mapper", error_out);
    }
    if (descriptor->workflow_transition == nullptr) {
        return Fail(Prefix() + " has no workflow transition handler", error_out);
    }
    if (!descriptor->supports_workflow_orchestration) {
        return Fail(Prefix() + " does not support workflow orchestration", error_out);
    }
    return true;
}

bool ValidateProductionRegistry(
    const ProgramKindRegistry& registry,
    std::string* error_out) {
    constexpr std::array<ExpectedDescriptor, 7> canonical_descriptors{{
        {static_cast<std::int32_t>(savor::PK_TasMovie), "TasMovie"},
        {static_cast<std::int32_t>(savor::PK_SeedProbe), "SeedProbe"},
        {static_cast<std::int32_t>(savor::PK_BattleContextProbe), "BattleContextProbe"},
        {static_cast<std::int32_t>(savor::PK_BattleSingleTurnRunner), "BattleSingleTurnRunner"},
        {static_cast<std::int32_t>(savor::PK_BattleCompletionRunner), "BattleCompletionRunner"},
        {static_cast<std::int32_t>(savor::PK_BattleResultsScreenRunner), "BattleResultsScreenRunner"},
        {static_cast<std::int32_t>(savor::PK_NavigationContextRunner), "NavigationContextRunner"},
    }};

    for (const auto& expected : canonical_descriptors) {
        const auto lookup_name = std::string("program-kind:")
            + std::to_string(expected.program_kind);
        if (!ValidateWorkflowDescriptor(
                registry.Find(expected.program_kind),
                expected,
                lookup_name,
                error_out)) {
            return false;
        }
    }

    constexpr std::array<ExpectedStepDescriptor, 16> step_descriptors{{
        {"tas_movie", {static_cast<std::int32_t>(savor::PK_TasMovie), "TasMovie"}},
        {"tasmovie.play", {static_cast<std::int32_t>(savor::PK_TasMovie), "TasMovie"}},
        {"seed_probe_chain", {static_cast<std::int32_t>(savor::PK_SeedProbe), "SeedProbeChain"}},
        {"seedprobe.neutral", {static_cast<std::int32_t>(savor::PK_SeedProbe), "SeedProbe"}},
        {"seedprobe.grid", {static_cast<std::int32_t>(savor::PK_SeedProbe), "SeedProbe"}},
        {"seedprobe.unique", {static_cast<std::int32_t>(savor::PK_SeedProbe), "SeedProbe"}},
        {"battle_chain", {static_cast<std::int32_t>(savor::PK_BattleContextProbe), "BattleContextProbe"}},
        {"battle.context_probe", {static_cast<std::int32_t>(savor::PK_BattleContextProbe), "BattleContextProbe"}},
        {"battle.single_turn", {static_cast<std::int32_t>(savor::PK_BattleSingleTurnRunner), "BattleSingleTurnRunner"}},
        {"battle.completion", {static_cast<std::int32_t>(savor::PK_BattleCompletionRunner), "BattleCompletionRunner"}},
        {"battle.field_return_seed_probe", {static_cast<std::int32_t>(savor::PK_SeedProbe), "FieldReturnSeedProbe"}},
        {"battle.field_return_seed_probe.grid", {static_cast<std::int32_t>(savor::PK_SeedProbe), "SeedProbe"}},
        {"battle.field_return_seed_probe.unique", {static_cast<std::int32_t>(savor::PK_SeedProbe), "SeedProbe"}},
        {"battle.field_return_seed_probe.materialize", {static_cast<std::int32_t>(savor::PK_SeedProbe), "FieldReturnSeedMaterialize"}},
        {"battle.results_screen", {static_cast<std::int32_t>(savor::PK_BattleResultsScreenRunner), "BattleResultsScreenRunner"}},
        {"navigation.context_probe", {static_cast<std::int32_t>(savor::PK_NavigationContextRunner), "NavigationContextRunner"}},
    }};

    for (const auto& expected : step_descriptors) {
        const auto lookup_name = std::string("step-kind:") + std::string(expected.step_kind);
        if (!ValidateWorkflowDescriptor(
                registry.FindForStepKind(expected.step_kind),
                expected.descriptor,
                lookup_name,
                error_out)) {
            return false;
        }
    }

    return true;
}

} // namespace

ProductionProgramKindRegistryConfig MakeProductionProgramKindRegistryConfig(
    const std::filesystem::path& runtime_working_dir_root) {
    ProductionProgramKindRegistryConfig config{};
    config.tas_movie.working_dir_root =
        runtime_working_dir_root / "tasmovie";
    config.battle_context.working_dir_root =
        runtime_working_dir_root / "battle-context";
    config.battle_single_turn.working_dir_root =
        runtime_working_dir_root / "battle-single-turn";
    config.battle_end.working_dir_root =
        runtime_working_dir_root / "battle-end-workflow";
    config.navigation_context.working_dir_root =
        runtime_working_dir_root / "navigation-context";
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
        return Fail("Production program registry execution DB dependency is required", error_out);
    }
    if (dependencies.state_db == nullptr) {
        return Fail("Production program registry state DB dependency is required", error_out);
    }
    if (dependencies.analysis_db == nullptr) {
        return Fail("Production program registry analysis DB dependency is required", error_out);
    }
    if (dependencies.authoring_db == nullptr) {
        return Fail("Production program registry authoring DB dependency is required", error_out);
    }

    config.tas_movie.authoring_db = dependencies.authoring_db;
    config.seed_probe.authoring_db = dependencies.authoring_db;
    config.battle_context.authoring_db = dependencies.authoring_db;
    config.battle_single_turn.authoring_db = dependencies.authoring_db;
    config.battle_end.authoring_db = dependencies.authoring_db;

    try {
        ProgramKindRegistry candidate;

        tasmovie::RegisterTasMoviePhaseDescriptor(
            &candidate,
            dependencies.execution_db,
            dependencies.state_db,
            dependencies.analysis_db,
            std::move(config.tas_movie));
        seedprobe::RegisterSeedProbePhaseDescriptors(
            &candidate,
            dependencies.execution_db,
            dependencies.analysis_db,
            std::move(config.seed_probe));
        battlecontext::RegisterBattleContextProbePhaseDescriptor(
            &candidate,
            dependencies.execution_db,
            dependencies.analysis_db,
            std::move(config.battle_context));
        battle::RegisterBattleSingleTurnPhaseDescriptor(
            &candidate,
            dependencies.execution_db,
            dependencies.state_db,
            dependencies.analysis_db,
            std::move(config.battle_single_turn));
        battleend::RegisterBattleEndWorkflowPhaseDescriptors(
            &candidate,
            dependencies.execution_db,
            dependencies.state_db,
            dependencies.analysis_db,
            std::move(config.battle_end));
        navigationcontext::RegisterNavigationContextProbePhaseDescriptor(
            &candidate,
            dependencies.execution_db,
            dependencies.state_db,
            std::move(config.navigation_context));

        if (!ValidateProductionRegistry(candidate, error_out)) {
            return false;
        }

        *registry_out = std::move(candidate);
        if (error_out != nullptr) {
            error_out->clear();
        }
        return true;
    } catch (const std::exception& exception) {
        return Fail(
            std::string("Production program registry construction failed: ")
                + exception.what(),
            error_out);
    }
}

} // namespace savor::db::execution::programdb
