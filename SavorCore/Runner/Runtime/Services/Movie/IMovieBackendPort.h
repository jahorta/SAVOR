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
    GuestIntegrity integrity = GuestIntegrity::Preserved;
    std::string message;

    [[nodiscard]] static MovieBackendResult Success()
    {
        return {true, GuestIntegrity::Preserved, {}};
    }

    [[nodiscard]] static MovieBackendResult Failure(
        std::string message,
        GuestIntegrity integrity = GuestIntegrity::Preserved)
    {
        return {false, integrity, std::move(message)};
    }
};

// One coherent raw physical observation from the emulator. Implementations
// may collect it only while the guest core is already authoritatively paused;
// the observation operation must never pause or otherwise control execution.
// This deliberately carries no semantic "ended" state: only MovieService has
// the lifecycle context needed to distinguish natural playback exhaustion from
// ordinary inactivity.
struct MovieBackendObservation
{
    MovieBackendResult result;
    bool playing = false;
    bool recording = false;
    bool read_only = true;
    std::uint64_t current_frame = 0;
    std::uint64_t current_input_count = 0;
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

    // Stages the exact DTM/startup-state pair for the next same-wrapper guest
    // core restart. Core startup and playback activation are deliberately
    // separate: startup must establish an authoritative paused baseline during
    // workset initialization, while activation occurs only after the job has
    // installed its subscriptions.
    virtual MoviePlaybackPrepareResult PrepareReadOnlyPlaybackForRestart(
        const std::filesystem::path& dtm_path) = 0;
    virtual MovieBackendResult StopCoreForPreparedReadOnlyMovie() = 0;
    virtual MovieBackendResult StartPreparedReadOnlyMovieCorePaused() = 0;
    virtual MovieBackendResult ActivatePreparedReadOnlyMoviePlayback() = 0;
    virtual MovieBackendResult DiscardPreparedReadOnlyMovie() noexcept = 0;
    virtual MovieBackendResult StopMovie() noexcept = 0;

    virtual MovieBackendResult BeginRecording() = 0;
    // Converts the exact current read-only playback cursor into a writable
    // rerecording session without advancing the guest or discarding the
    // already-played input prefix.
    virtual MovieBackendResult BranchReadOnlyPlaybackToRecording() = 0;
    virtual MovieRecordingFinalizeResult FinalizeRecording(
        const std::filesystem::path& dtm_path) = 0;
    virtual MovieBackendResult CancelRecording() noexcept = 0;

    [[nodiscard]] virtual MovieBackendObservation
    ObserveMovieWhilePaused() const = 0;
    // Installs a session-local override for Dolphin's existing pause-at-movie-
    // end behavior and restores the exact prior CurrentRun value on release.
    virtual MovieBackendResult AcquirePauseAtPlaybackEnd() = 0;
    virtual MovieBackendResult ReleasePauseAtPlaybackEnd() noexcept = 0;
    virtual MovieCheckpointBackendResult CaptureRecordingCheckpoint() = 0;

    // These operations stage and verify movie history around one workset-owned
    // savestate load. They never own the load or advance the workset epoch.
    virtual MovieBackendResult PrepareSavestateRestore(
        const SavestateMovieRestoreContext& context) = 0;
    virtual MovieBackendResult CommitSavestateRestore(
        const SavestateMovieRestoreContext& context) = 0;
    virtual MovieBackendResult RollbackSavestateRestore(
        const SavestateMovieRestoreContext& context) noexcept = 0;

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
