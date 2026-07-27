#pragma once

#include "Runner/Runtime/Services/State/StateTypes.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace savor::runtime {

struct MovieReservationIdTag;
using MovieReservationId = StrongId<MovieReservationIdTag>;

enum class MovieActivity : std::uint8_t
{
    Inactive,
    ReadOnlyPlayback,
    Recording,
};

enum class MovieOperation : std::uint8_t
{
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
    StateFailure,
    BackendFailure,
    ArtifactFailure,
    IntegrityFailure,
};

struct MovieServiceResult
{
    bool ok = false;
    MovieServiceErrorCode code = MovieServiceErrorCode::BackendFailure;
    StateIntegrity integrity = StateIntegrity::Preserved;
    std::string message;

    [[nodiscard]] static MovieServiceResult Success()
    {
        return {true, MovieServiceErrorCode::None, StateIntegrity::Preserved, {}};
    }

    [[nodiscard]] static MovieServiceResult Failure(
        MovieServiceErrorCode code,
        std::string message,
        StateIntegrity integrity = StateIntegrity::Preserved)
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
    StateEpoch state_epoch;
    MovieReservationId reservation;
    std::string dtm_sha256;
    std::filesystem::path artifact_path;
    std::optional<std::filesystem::path> starting_savestate;
};

struct MovieCheckpointReceipt
{
    MovieServiceResult result;
    StateEpoch state_epoch;
    std::optional<MovieCheckpointMetadata> checkpoint;
};

struct MoviePlaybackRequest
{
    std::filesystem::path dtm_path;
    StateBootRequest boot;
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
