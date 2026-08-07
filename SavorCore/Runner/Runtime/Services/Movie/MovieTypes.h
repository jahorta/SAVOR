#pragma once

#include "Runner/Runtime/Services/Savestate/SavestateTypes.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace savor::runtime {

struct MovieReservationIdTag;
using MovieReservationId = StrongId<MovieReservationIdTag>;
struct MoviePreparationIdTag;
using MoviePreparationId = StrongId<MoviePreparationIdTag>;

enum class MovieActivity : std::uint8_t
{
    Inactive,
    PreparedReadOnlyPlayback,
    ReadOnlyPlayback,
    Recording,
};

enum class MovieOperation : std::uint8_t
{
    PreparePlayback,
    StartPlayback,
    StopPlayback,
    StartRecording,
    FinalizeRecording,
    CancelRecording,
    CaptureCheckpoint,
};

enum class MovieServiceErrorCode : std::uint16_t
{
    None,
    InvalidArgument,
    InvalidState,
    Unsupported,
    ReservationFailure,
    SavestateFailure,
    BackendFailure,
    ArtifactFailure,
    IntegrityFailure,
};

struct MovieServiceResult
{
    bool ok = false;
    MovieServiceErrorCode code = MovieServiceErrorCode::BackendFailure;
    GuestIntegrity integrity = GuestIntegrity::Preserved;
    std::string message;

    [[nodiscard]] static MovieServiceResult Success()
    {
        return {true, MovieServiceErrorCode::None, GuestIntegrity::Preserved, {}};
    }

    [[nodiscard]] static MovieServiceResult Failure(
        MovieServiceErrorCode code,
        std::string message,
        GuestIntegrity integrity = GuestIntegrity::Preserved)
    {
        return {false, code, integrity, std::move(message)};
    }
};

struct MovieSnapshot
{
    MovieActivity activity = MovieActivity::Inactive;
    bool read_only = true;
    bool ended = false;
    std::uint64_t current_frame = 0;
    std::uint64_t current_input_count = 0;
};

struct MovieOperationReceipt
{
    MovieServiceResult result;
    MovieOperation operation = MovieOperation::StartPlayback;
    MovieActivity activity = MovieActivity::Inactive;
    WorksetEpoch workset_epoch;
    MovieReservationId reservation;
    MoviePreparationId preparation;
    std::string dtm_sha256;
    std::filesystem::path artifact_path;
    std::optional<std::filesystem::path> starting_savestate;
};

struct MovieCheckpointReceipt
{
    MovieServiceResult result;
    WorksetEpoch workset_epoch;
    std::optional<MovieCheckpointMetadata> checkpoint;
};

struct MoviePlaybackRequest
{
    std::filesystem::path dtm_path;
};

struct SavestateMovieRestoreContext
{
    WorksetEpoch workset_epoch;
    std::optional<MovieCheckpointMetadata> movie;
    bool external_artifact = false;
};

struct MovieRecordingRequest
{
    std::string diagnostic_label;
};

struct MovieFinalizeRequest
{
    std::filesystem::path dtm_path;
};

} // namespace savor::runtime
