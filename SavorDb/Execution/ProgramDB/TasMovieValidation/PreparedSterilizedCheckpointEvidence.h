#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../../../Analysis/IAnalysisDb.h"
#include "../../../State/IStateDb.h"

namespace savor::db::execution::programdb::tasmovieevidence {

struct PreparedSterilizedCheckpointEvidence {
    SavestateRecord selected_checkpoint;
    SavestateDerivationRecord sterilization_derivation;
    TasMovieCheckpointSterilizationRequestRecord sterilization_request;
    TasMovieCheckpointSterilizationAttemptRecord sterilization_attempt;
    SavestateRecord paired_source_checkpoint;
    TasMovieRootRecord tas_movie_root;
    TasMovieValidationRequestRecord validation_request;
    TasMovieValidationAttemptRecord validation_attempt;
    ArtifactRecord effective_dtm_artifact;
    ArtifactRecord itinerary_artifact;
};

bool VerifyMoviePairedCheckpointSnapshot(
    IStateDb* state_db,
    const TasMovieCheckpointSterilizationRequestRecord& request,
    SavestateRecord* source_out = nullptr,
    std::string* error_out = nullptr);

bool VerifyCanonicalSterilizedCheckpoint(
    IStateDb* state_db,
    const TasMovieCheckpointSterilizationRequestRecord& request,
    std::int64_t result_savestate_id,
    std::optional<std::string_view> expected_sha = std::nullopt,
    std::string* error_out = nullptr);

bool ResolvePreparedSterilizedCheckpointEvidence(
    IStateDb* state_db,
    IAnalysisDb* analysis_db,
    std::int64_t selected_savestate_id,
    PreparedSterilizedCheckpointEvidence* evidence_out,
    std::string* error_out = nullptr);

} // namespace savor::db::execution::programdb::tasmovieevidence
