#include "PreparedSterilizedCheckpointEvidence.h"

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <vector>
#include <utility>

#include "../../../../SavorCore/Phases/Programs/TasMovieValidation/TasMovieValidationModule.h"
#include "../../../../SavorCore/Utils/Hash.h"

namespace savor::db::execution::programdb::tasmovieevidence {
namespace {

constexpr std::string_view kSterilizedType =
    "TAS_MOVIE_STERILIZED_CHECKPOINT";
constexpr std::string_view kSterilizationContext =
    "tmv_checkpoint_sterilization_request";
constexpr std::string_view kValidationContext = "tmv_validation_request";

bool Fail(std::string message, std::string* error_out) {
    if (error_out != nullptr) *error_out = std::move(message);
    return false;
}

std::optional<std::string> HashFile(const std::filesystem::path& path) {
    try {
        return hash::sha256_of_file(path.string());
    } catch (...) {
        return std::nullopt;
    }
}

bool VerifyArtifactFile(
    const ArtifactRecord& artifact,
    const std::string_view expected_kind,
    const std::optional<std::string_view> expected_extension,
    std::string* error_out) {
    const std::filesystem::path path(artifact.object_path);
    std::error_code error;
    if (artifact.artifact_kind != expected_kind
        || (expected_extension && artifact.file_ext != *expected_extension)
        || artifact.size_bytes < 0
        || !std::filesystem::is_regular_file(path, error) || error) {
        return Fail("artifact metadata or file is unavailable: " + path.string(),
                    error_out);
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error || size != static_cast<std::uintmax_t>(artifact.size_bytes)) {
        return Fail("artifact size drifted: " + path.string(), error_out);
    }
    const auto actual_sha = HashFile(path);
    if (!actual_sha || *actual_sha != artifact.sha256) {
        return Fail("artifact SHA-256 drifted: " + path.string(), error_out);
    }
    return true;
}

bool VerifySavestateArtifact(
    IStateDb* state_db,
    const SavestateRecord& state,
    std::string* error_out) {
    const auto artifact = state_db != nullptr
        ? state_db->GetArtifact(state.artifact_id)
        : std::nullopt;
    if (!artifact || artifact->sha256 != state.artifact_sha256
        || artifact->size_bytes != state.artifact_size_bytes
        || artifact->object_path != state.artifact_filename
        || artifact->file_ext != state.artifact_file_ext
        || artifact->artifact_kind != state.artifact_kind) {
        return Fail("savestate artifact record drifted", error_out);
    }
    return VerifyArtifactFile(*artifact, "SAV", ".sav", error_out);
}

bool HasAdjacentDtmSidecar(const std::filesystem::path& savestate_path) {
    std::error_code error;
    if (std::filesystem::is_regular_file(savestate_path.string() + ".dtm", error)) {
        return true;
    }
    error.clear();
    auto replaced = savestate_path;
    replaced.replace_extension(".dtm");
    return replaced != savestate_path
        && std::filesystem::is_regular_file(replaced, error);
}

} // namespace

bool VerifyMoviePairedCheckpointSnapshot(
    IStateDb* state_db,
    const TasMovieCheckpointSterilizationRequestRecord& request,
    SavestateRecord* source_out,
    std::string* error_out) {
    const auto source = state_db != nullptr
        ? state_db->GetSavestate(request.source_savestate_id)
        : std::nullopt;
    if (!source || !source->is_complete
        || source->playback_state != SavestatePlaybackState::MoviePaired
        || source->artifact_id != request.source_savestate_artifact_id
        || source->artifact_sha256 != request.source_savestate_sha256
        || source->dtm_artifact_id != request.source_dtm_artifact_id
        || source->dtm_sha256 != request.source_dtm_sha256
        || !source->dtm_filename) {
        return Fail("movie-paired checkpoint snapshot drifted", error_out);
    }
    if (!VerifySavestateArtifact(state_db, *source, error_out)) return false;
    const auto dtm = state_db->GetArtifact(request.source_dtm_artifact_id);
    if (!dtm || dtm->sha256 != request.source_dtm_sha256
        || dtm->object_path != *source->dtm_filename
        || !VerifyArtifactFile(*dtm, "DTM", ".dtm", error_out)) {
        return Fail(error_out != nullptr && !error_out->empty()
                ? *error_out
                : "movie-paired DTM artifact drifted",
            error_out);
    }
    if (source_out != nullptr) *source_out = *source;
    return true;
}

bool VerifyCanonicalSterilizedCheckpoint(
    IStateDb* state_db,
    const TasMovieCheckpointSterilizationRequestRecord& request,
    const std::int64_t result_savestate_id,
    const std::optional<std::string_view> expected_sha,
    std::string* error_out) {
    const auto result = state_db != nullptr
        ? state_db->GetSavestate(result_savestate_id)
        : std::nullopt;
    if (!result || !result->is_complete
        || result->savestate_type != kSterilizedType
        || result->artifact_kind != "SAV"
        || result->artifact_file_ext != ".sav"
        || result->playback_state != SavestatePlaybackState::MovieInactive
        || result->dtm_artifact_id || result->dtm_sha256
        || result->dtm_filename) {
        return Fail("canonical sterilized checkpoint evidence drifted", error_out);
    }
    if (expected_sha && result->artifact_sha256 != *expected_sha) {
        return Fail("canonical sterilized checkpoint conflicts with worker bytes",
                    error_out);
    }
    if (!VerifySavestateArtifact(state_db, *result, error_out)) return false;
    if (HasAdjacentDtmSidecar(result->artifact_filename)) {
        return Fail("movie-inactive checkpoint unexpectedly has a DTM sidecar",
                    error_out);
    }
    const auto derivations = state_db->ListIncomingSavestateDerivations(
        result_savestate_id);
    const auto derivation = std::ranges::find_if(
        derivations, [&](const SavestateDerivationRecord& value) {
            return value.from_savestate_id == request.source_savestate_id
                && value.method_kind
                    == savor::runtime::tasmovie::SterilizationDerivationMethod;
        });
    if (derivation == derivations.end()) {
        return Fail("canonical sterilization derivation is unavailable", error_out);
    }
    return true;
}

bool ResolvePreparedSterilizedCheckpointEvidence(
    IStateDb* state_db,
    IAnalysisDb* analysis_db,
    const std::int64_t selected_savestate_id,
    PreparedSterilizedCheckpointEvidence* evidence_out,
    std::string* error_out) {
    if (state_db == nullptr || analysis_db == nullptr
        || selected_savestate_id <= 0 || evidence_out == nullptr) {
        return Fail("prepared checkpoint evidence dependencies are unavailable",
                    error_out);
    }
    const auto selected = state_db->GetSavestate(selected_savestate_id);
    if (!selected) return Fail("selected savestate does not exist", error_out);

    const auto incoming = state_db->ListIncomingSavestateDerivations(
        selected_savestate_id);
    std::vector<SavestateDerivationRecord> canonical_derivations;
    std::ranges::copy_if(
        incoming, std::back_inserter(canonical_derivations),
        [](const SavestateDerivationRecord& value) {
            return value.method_kind
                == savor::runtime::tasmovie::SterilizationDerivationMethod;
        });
    if (canonical_derivations.size() != 1) {
        return Fail("selected checkpoint must have exactly one canonical sterilization derivation",
                    error_out);
    }
    const auto& derivation = canonical_derivations.front();
    if (derivation.source_context_kind != kSterilizationContext
        || derivation.source_context_id <= 0) {
        return Fail("sterilization derivation has invalid request provenance",
                    error_out);
    }
    const auto request = analysis_db->GetTasMovieCheckpointSterilizationRequest(
        derivation.source_context_id);
    if (!request || request->sterilization_request_id
            != derivation.source_context_id
        || request->source_savestate_id != derivation.from_savestate_id) {
        return Fail("sterilization request does not match the derivation", error_out);
    }
    if (!VerifyCanonicalSterilizedCheckpoint(
            state_db, *request, selected_savestate_id,
            selected->artifact_sha256, error_out)) {
        return false;
    }

    const auto attempts =
        analysis_db->ListTasMovieCheckpointSterilizationAttemptsForRequest(
            request->sterilization_request_id);
    const auto matching_attempt = std::ranges::find_if(
        attempts.rbegin(), attempts.rend(), [&](const auto& attempt) {
            return attempt.sterilization_request_id
                    == request->sterilization_request_id
                && attempt.produced_savestate_id == selected_savestate_id
                && attempt.candidate_savestate_sha256
                    == selected->artifact_sha256;
        });
    if (matching_attempt == attempts.rend()) {
        return Fail("no durable sterilization attempt produced the selected checkpoint",
                    error_out);
    }

    SavestateRecord paired;
    if (!VerifyMoviePairedCheckpointSnapshot(
            state_db, *request, &paired, error_out)) {
        return false;
    }
    const auto root = state_db->FindTasMovieRootByDtmArtifactId(
        request->source_dtm_artifact_id);
    if (!root || root->checkpoint_savestate_id != paired.savestate_id
        || root->dtm_artifact_id != request->source_dtm_artifact_id
        || root->source_context_kind != kValidationContext
        || root->source_context_id <= 0) {
        return Fail("movie-paired source is not linked to a canonical TAS Movie root",
                    error_out);
    }
    const auto validation_request = analysis_db->GetTasMovieValidationRequest(
        root->source_context_id);
    if (!validation_request
        || validation_request->operation != TasMovieValidationOperation::Validate
        || validation_request->source_kind
            != TasMovieValidationSourceKind::RootEstablishment
        || validation_request->effective_dtm_sha256
            != request->source_dtm_sha256
        || validation_request->itinerary_artifact_id
            != root->itinerary_artifact_id) {
        return Fail("TAS Movie validation request does not match the paired root",
                    error_out);
    }
    const auto status = analysis_db->GetTasMovieValidationStatus(
        validation_request->effective_dtm_sha256);
    const auto validation_attempt = status
        ? analysis_db->GetTasMovieValidationAttempt(
              status->validation_attempt_id)
        : std::nullopt;
    if (!status || status->status != TasMovieValidationStatus::Valid
        || !validation_attempt
        || validation_attempt->validation_request_id
            != validation_request->validation_request_id
        || validation_attempt->outcome != TasMovieValidationOutcome::Valid
        || validation_attempt->produced_tas_movie_root_id
            != root->tas_movie_root_id) {
        return Fail("TAS Movie root lacks matching durable Valid evidence", error_out);
    }
    const auto dtm = state_db->GetArtifact(root->dtm_artifact_id);
    const auto itinerary = state_db->GetArtifact(root->itinerary_artifact_id);
    if (!dtm || dtm->sha256 != validation_request->effective_dtm_sha256
        || !VerifyArtifactFile(*dtm, "DTM", ".dtm", error_out)
        || !itinerary
        || (validation_request->itinerary_sha256
            && itinerary->sha256 != *validation_request->itinerary_sha256)
        || !VerifyArtifactFile(
            *itinerary, "TAS_MOVIE_ITINERARY", std::nullopt, error_out)) {
        return Fail(error_out != nullptr && !error_out->empty()
                ? *error_out
                : "TAS Movie root artifacts are unavailable",
            error_out);
    }

    *evidence_out = {
        .selected_checkpoint = *selected,
        .sterilization_derivation = derivation,
        .sterilization_request = *request,
        .sterilization_attempt = *matching_attempt,
        .paired_source_checkpoint = paired,
        .tas_movie_root = *root,
        .validation_request = *validation_request,
        .validation_attempt = *validation_attempt,
        .effective_dtm_artifact = *dtm,
        .itinerary_artifact = *itinerary,
    };
    return true;
}

} // namespace savor::db::execution::programdb::tasmovieevidence
