#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "../ProgramKindDescriptor.h"

namespace savor::db {
struct IAnalysisDb;
struct IExecutionDb;
struct IStateDb;
struct TasMovieInputEpochAnnotationAttemptRecord;
struct TasMovieInputEpochRewriteAttemptRecord;
struct TasMovieInputEpochRewriteRequestRecord;
struct TasMovieRootEstablishmentAttemptRecord;
}

namespace savor::db::execution::programdb::tasmovieinputepoch {

struct TasMovieInputEpochProgramConfig {
    std::filesystem::path working_dir_root;
    std::string capture_module_sha256;
    bool enable_seed_call_progress = false;
};

struct TasMovieDelayPlacementResolution {
    std::uint64_t insert_before_epoch = 0;
    std::string placement_profile;
    auto operator<=>(const TasMovieDelayPlacementResolution&) const = default;
};

struct TasMovieDelayPreparationIdentity {
    std::string source_dtm_sha256;
    std::string source_schedule_sha256;
    std::uint32_t root_pc = 0;
    std::uint64_t root_movie_input_cursor = 0;
    std::string root_itinerary_sha256;
    std::uint64_t insert_before_epoch = 0;
    std::uint64_t neutral_epoch_count = 0;
    std::string placement_profile;
    std::int64_t full_phase_program_kind = 0;
    std::int64_t full_phase_program_version = 0;
    std::string full_phase_canonical_id;
    std::int64_t full_phase_contract_revision = 0;
    std::string full_phase_sha256;
    std::string module_canonical_id;
    std::int64_t module_revision = 0;
    std::string module_sha256;
    std::int64_t revise_graph_revision_id = 0;
    auto operator<=>(const TasMovieDelayPreparationIdentity&) const = default;
};

bool ResolveTasMovieDelayPlacement(IStateDb* state_db,
    const TasMovieInputEpochAnnotationAttemptRecord& annotation,
    std::optional<std::int64_t> explicit_epoch,
    std::optional<std::string_view> placement_profile,
    TasMovieDelayPlacementResolution* resolution_out,
    std::string* error_out = nullptr);
TasMovieDelayPreparationIdentity BuildTasMovieDelayPreparationIdentity(
    const TasMovieInputEpochAnnotationAttemptRecord& annotation,
    const TasMovieRootEstablishmentAttemptRecord& root_establishment,
    const TasMovieDelayPlacementResolution& placement,
    std::uint64_t neutral_epoch_count,
    std::int64_t revise_graph_revision_id);
std::string CanonicalTasMovieDelayPreparationIdentity(
    const TasMovieDelayPreparationIdentity& identity);
std::string HashTasMovieDelayPreparationIdentity(
    const TasMovieDelayPreparationIdentity& identity);
bool TasMovieRewriteRequestMatchesPreparation(
    const TasMovieInputEpochRewriteRequestRecord& request,
    const TasMovieRootEstablishmentAttemptRecord& root_establishment,
    const TasMovieDelayPreparationIdentity& identity);
bool ValidateReusableTasMovieDelayPreparation(IStateDb* state_db,
    IAnalysisDb* analysis_db,
    const TasMovieInputEpochRewriteRequestRecord& request,
    const TasMovieInputEpochRewriteAttemptRecord& attempt,
    const TasMovieDelayPreparationIdentity& identity,
    std::string* error_out = nullptr);

ProgramKindDescriptor BuildAnnotationProgramDescriptor(
    IExecutionDb* execution_db,
    IStateDb* state_db,
    IAnalysisDb* analysis_db,
    TasMovieInputEpochProgramConfig config = {});

ProgramKindDescriptor BuildBreakpointDiagnosticProgramDescriptor(
    IExecutionDb* execution_db,
    IStateDb* state_db,
    IAnalysisDb* analysis_db,
    TasMovieInputEpochProgramConfig config = {});

ProgramKindDescriptor BuildRewriteProgramDescriptor(
    IExecutionDb* execution_db,
    IStateDb* state_db,
    IAnalysisDb* analysis_db,
    TasMovieInputEpochProgramConfig config = {});

ProgramKindDescriptor BuildCutsceneProgramDescriptor(
    IExecutionDb* execution_db,
    IStateDb* state_db,
    IAnalysisDb* analysis_db,
    TasMovieInputEpochProgramConfig config = {});

} // namespace savor::db::execution::programdb::tasmovieinputepoch
