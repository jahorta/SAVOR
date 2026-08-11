#pragma once

#include "Runner/Runtime/Services/Movie/IMovieBackendPort.h"

#include <functional>
#include <optional>
#include <thread>

namespace savor::runtime {

// Workset-local movie authority.  The service never owns or advances an
// epoch: it observes the epoch allocated by the workset initialization
// transaction.
class MovieService final
{
public:
    MovieService(
        IMovieBackendPort& backend,
        IMovieInputReservationPort& reservations,
        std::function<WorksetEpoch()> active_workset_epoch,
        std::function<MovieServiceResult()> validate_before_core_stop,
        std::function<MovieServiceResult()> settle_after_core_stop,
        std::function<MovieServiceResult()> validate_after_core_start);

    MovieService(const MovieService&) = delete;
    MovieService& operator=(const MovieService&) = delete;

    [[nodiscard]] MovieOperationReceipt PrepareReadOnlyPlayback(
        const MoviePlaybackRequest& request);
    // Returns the exact paused workset-owned preparation created during
    // atomic workset initialization. It does not mutate the guest or acquire
    // another reservation.
    [[nodiscard]] std::optional<MovieOperationReceipt>
    PreparedReadOnlyPlaybackReceipt() const;
    [[nodiscard]] MovieOperationReceipt StartPreparedReadOnlyPlayback(
        MoviePreparationId preparation);
    [[nodiscard]] MovieOperationReceipt AbandonPreparedReadOnlyPlayback(
        MoviePreparationId preparation) noexcept;
    [[nodiscard]] MovieOperationReceipt StopPlayback() noexcept;
    [[nodiscard]] MovieOperationReceipt StartRecording(
        const MovieRecordingRequest& request = {});
    [[nodiscard]] MovieOperationReceipt FinalizeRecording(
        const MovieFinalizeRequest& request);
    [[nodiscard]] MovieOperationReceipt CancelRecording() noexcept;
    [[nodiscard]] MovieCheckpointReceipt CaptureCheckpoint();

    // WorksetStateCoordinator uses this handshake only around a savestate
    // load.  It stages the exact DTM history before Dolphin reads the state,
    // then proves the restored cursor before the transaction is committed.
    [[nodiscard]] MovieServiceResult PrepareSavestateRestore(
        const SavestateMovieRestoreContext& context);
    [[nodiscard]] MovieServiceResult CommitSavestateRestore(
        const SavestateMovieRestoreContext& context);
    [[nodiscard]] MovieServiceResult RollbackSavestateRestore(
        const SavestateMovieRestoreContext& context) noexcept;

    [[nodiscard]] MovieSnapshot snapshot() const;
    [[nodiscard]] MovieActivity activity() const noexcept { return activity_; }
    [[nodiscard]] MovieReservationId reservation() const noexcept
    {
        return reservation_;
    }
    [[nodiscard]] MoviePreparationId preparation() const noexcept
    {
        return preparation_;
    }
    [[nodiscard]] bool is_tainted() const noexcept { return tainted_; }

private:
    [[nodiscard]] WorksetEpoch ActiveEpoch() const noexcept;
    [[nodiscard]] MovieOperationReceipt BaseReceipt(
        MovieOperation operation) const;
    [[nodiscard]] MovieServiceResult ValidateIdle() const;
    [[nodiscard]] MovieServiceResult AcquireReservation();
    [[nodiscard]] MovieServiceResult ReleaseReservation() noexcept;
    [[nodiscard]] MovieServiceResult ValidateSnapshotFor(
        const std::optional<MovieCheckpointMetadata>& movie) const;
    [[nodiscard]] MovieServiceResult ValidateDtm(
        const std::filesystem::path& path,
        MovieCheckpointMetadata& metadata) const;
    [[nodiscard]] MovieServiceResult ValidateCheckpoint(
        MovieCheckpointMetadata& metadata) const;
    [[nodiscard]] static MovieServiceResult FromBackendResult(
        const MovieBackendResult& result,
        std::string fallback);
    [[nodiscard]] bool OnOwnerThread() const noexcept;
    [[nodiscard]] static MovieServiceResult WrongThread();

    IMovieBackendPort& backend_;
    IMovieInputReservationPort& reservations_;
    std::function<WorksetEpoch()> active_workset_epoch_;
    std::function<MovieServiceResult()> validate_before_core_stop_;
    std::function<MovieServiceResult()> settle_after_core_stop_;
    std::function<MovieServiceResult()> validate_after_core_start_;
    std::thread::id owner_thread_;
    MovieActivity activity_ = MovieActivity::Inactive;
    MovieReservationId reservation_;
    MoviePreparationId preparation_;
    std::uint64_t next_preparation_ = 1;
    std::optional<std::filesystem::path> prepared_starting_savestate_;
    bool prepared_core_started_ = false;
    std::optional<MovieCheckpointMetadata> active_movie_;
    std::optional<MovieCheckpointMetadata> original_movie_;
    MovieActivity original_activity_ = MovieActivity::Inactive;
    bool restore_prepared_ = false;
    bool acquired_for_restore_ = false;
    bool tainted_ = false;
};

} // namespace savor::runtime
