#pragma once

#include <filesystem>
#include <string_view>

#include "../ProgramKindDescriptor.h"

namespace savor::db {
struct IExecutionDb;
struct IStateDb;
}

namespace savor::db::execution::programdb::navigationcontext {

inline constexpr std::string_view UnitKind = "navigation.context_probe";
inline constexpr std::string_view StepKind = "navigation.context_probe";
inline constexpr std::string_view ProgramRefKind = "state_savestate";
inline constexpr std::string_view InputKey = "entry_savestate";
inline constexpr std::string_view InputDataKind = "state.movie_inactive_savestate_id";
inline constexpr std::string_view OutputKey = "navigation_context";
inline constexpr std::string_view OutputDataKind =
    "state_artifact.navigation_context_id";
inline constexpr std::string_view OutputRefKind = "state_artifact";
inline constexpr std::string_view ResultKind = "state.navigation_context.artifact";
inline constexpr std::string_view DerivationMethod = "navigation_context_capture";
inline constexpr std::string_view DerivationSourceContextKind = "state_artifact";

struct NavigationContextPhaseRegistrationConfig {
    std::filesystem::path working_dir_root;
};

ProgramKindDescriptor BuildNavigationContextProbeDescriptor(
    savor::db::IExecutionDb* execution_db,
    savor::db::IStateDb* state_db,
    NavigationContextPhaseRegistrationConfig config = {});

} // namespace savor::db::execution::programdb::navigationcontext
