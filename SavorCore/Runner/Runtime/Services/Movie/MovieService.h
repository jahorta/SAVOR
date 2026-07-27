#pragma once

#include "Runner/Runtime/Services/Movie/IMovieBackendPort.h"
#include "Runner/Runtime/Services/State/StateService.h"

#include <optional>
#include <thread>

namespace savor::runtime {

class MovieService final : public IStateReplacementParticipant
{
public:
    MovieService(
        IMovieBackendPort& backend,
        IMovieInputReservationPort& reservations,
        StateService& state_service);

    MovieService(const MovieService&) = delete;
    MovieService& operator=(const MovieService&) = delete;

    [[nodiscard]] MovieServiceResult RegisterForStateReplacement();

    [[nodiscard]] MovieOperationReceipt StartReadOnlyPlayback(
        const MoviePlaybackRequest& request);
    [[nodiscard]] MovieOperationReceipt StopPlayback() noexcept;
    [[nodiscard]] MovieOperationReceipt StartRecording(
        const MovieRecordingRequest& request = {});
    [[nodiscard]] MovieOperationReceipt FinalizeRecording(
        const MovieFinalizeRequest& request);
    [[nodiscard]] MovieOperationReceipt CancelRecording() noexcept;
    [[nodiscard]] MovieCheckpointReceipt CaptureCheckpoint();

    [[nodiscard]] MovieSnapshot snapshot() const;
    [[nodiscard]] MovieActivity activity() const noexcept { return activity_; }
    [[nodiscard]] MovieReservationId reservation() const noexcept
    {
        return reservation_;
    }
    [[nodiscard]] bool is_tainted() const noexcept { return tainted_; }

    StateServiceResult PrepareStateReplacement(
        const StateReplacementContext& context) override;
    StateServiceResult CommitStateReplacement(
        const StateReplacementContext& context) override;
    StateServiceResult RollbackStateReplacement(
        const StateReplacementContext& context) noexcept override;

private:
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
    [[nodiscard]] static MovieServiceResult FromStateResult(
        const StateServiceResult& result);
    [[nodiscard]] static StateServiceResult FromBackendResult(
        const MovieBackendResult& result);
    [[nodiscard]] bool OnOwnerThread() const noexcept;
    [[nodiscard]] static MovieServiceResult WrongThread();
    [[nodiscard]] StateServiceResult WrongThreadParticipant() const;

    IMovieBackendPort& backend_;
    IMovieInputReservationPort& reservations_;
    StateService& state_service_;
    std::thread::id owner_thread_;
    MovieActivity activity_ = MovieActivity::Inactive;
    MovieReservationId reservation_;
    std::optional<MovieCheckpointMetadata> active_movie_;
    std::optional<MovieCheckpointMetadata> pending_start_movie_;
    std::optional<MovieCheckpointMetadata> original_movie_;
    MovieActivity original_activity_ = MovieActivity::Inactive;
    bool registered_ = false;
    bool starting_playback_ = false;
    bool replacement_prepared_ = false;
    bool acquired_for_replacement_ = false;
    bool tainted_ = false;
};

} // namespace savor::runtime
