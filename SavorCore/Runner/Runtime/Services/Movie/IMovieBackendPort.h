#pragma once

#include "Runner/Runtime/Services/Movie/MovieTypes.h"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace savor::runtime {

struct MovieBackendResult
{
    bool ok = false;
    StateIntegrity integrity = StateIntegrity::Preserved;
    std::string message;

    [[nodiscard]] static MovieBackendResult Success()
    {
        return {true, StateIntegrity::Preserved, {}};
    }

    [[nodiscard]] static MovieBackendResult Failure(
        std::string message,
        StateIntegrity integrity = StateIntegrity::Preserved)
    {
        return {false, integrity, std::move(message)};
    }
};

struct MoviePlaybackPrepareResult
{
    MovieBackendResult result;
    std::optional<std::filesystem::path> startup_savestate;
};

struct MovieRecordingFinalizeResult
{
    MovieBackendResult result;
    std::optional<std::filesystem::path> starting_savestate;
};

struct MovieCheckpointBackendResult
{
    MovieBackendResult result;
    MovieCheckpointMetadata checkpoint;
};

class IMovieBackendPort
{
public:
    virtual ~IMovieBackendPort() = default;

    // Stages the exact DTM so the shared concrete backend can call
    // MovieManager::PlayInput before its next boot.
    virtual MoviePlaybackPrepareResult PrepareReadOnlyPlaybackBeforeBoot(
        const std::filesystem::path& dtm_path) = 0;
    virtual MovieBackendResult StopMovie() noexcept = 0;

    virtual MovieBackendResult BeginRecording() = 0;
    virtual MovieRecordingFinalizeResult FinalizeRecording(
        const std::filesystem::path& dtm_path) = 0;
    virtual MovieBackendResult CancelRecording() noexcept = 0;

    [[nodiscard]] virtual MovieSnapshot Snapshot() const = 0;
    virtual MovieCheckpointBackendResult CaptureRecordingCheckpoint() = 0;

    // These operations stage and verify movie state around StateService's
    // backend replacement. They do not advance or load guest state.
    virtual MovieBackendResult PrepareStateReplacement(
        const StateReplacementContext& context) = 0;
    virtual MovieBackendResult CommitStateReplacement(
        const StateReplacementContext& context) = 0;
    virtual MovieBackendResult RollbackStateReplacement(
        const StateReplacementContext& context) noexcept = 0;

protected:
    IMovieBackendPort() = default;
};

struct MovieInputReservationReceipt
{
    MovieServiceResult result;
    MovieReservationId reservation;
};

class IMovieInputReservationPort
{
public:
    virtual ~IMovieInputReservationPort() = default;

    virtual MovieInputReservationReceipt
    AcquireUnsuspendableMovieReservation() = 0;
    virtual MovieServiceResult ReleaseMovieReservation(
        MovieReservationId reservation) noexcept = 0;

protected:
    IMovieInputReservationPort() = default;
};

} // namespace savor::runtime
