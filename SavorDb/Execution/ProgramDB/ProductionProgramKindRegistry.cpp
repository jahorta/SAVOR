#include "ProductionProgramKindRegistry.h"

#include <exception>
#include <string>
#include <utility>

#include "../../../SavorCore/Runner/IPC/Wire.h"

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
    // Keep the old per-kind configuration values constructible for developer
    // tools while their descriptors are rewritten. They are intentionally not
    // registered into the production coordinator in this infrastructure slice.
    ProductionProgramKindRegistryConfig config{};
    config.tas_movie.working_dir_root =
        runtime_working_dir_root / "tasmovie";
    config.seed_probe.working_dir_root =
        runtime_working_dir_root / "seedprobe";
    config.battle_context.working_dir_root =
        runtime_working_dir_root / "battle-context";
    config.battle_single_turn.working_dir_root =
        runtime_working_dir_root / "battle-single-turn";
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

        *registry_out = std::move(registry);
    } catch (const std::exception& exception) {
        return Fail(
            std::string(
                "SeedProbe production descriptor construction failed: ")
                + exception.what(),
            error_out);
    } catch (...) {
        return Fail(
            "SeedProbe production descriptor construction failed",
            error_out);
    }

    if (error_out != nullptr) {
        error_out->clear();
    }
    return true;
}

} // namespace savor::db::execution::programdb
