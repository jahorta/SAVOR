#include "Runner/Runtime/Services/Movie/InputMovieReservationAdapter.h"

#include <limits>
#include <utility>

namespace savor::runtime {
namespace {

MovieServiceResult Failure(std::string message)
{
    return MovieServiceResult::Failure(
        MovieServiceErrorCode::ReservationFailure,
        std::move(message));
}

} // namespace

InputMovieReservationAdapter::InputMovieReservationAdapter(
    InputArbiter& input,
    std::function<StateEpoch()> current_epoch)
    : input_(input),
      current_epoch_(std::move(current_epoch))
{
}

MovieInputReservationReceipt
InputMovieReservationAdapter::AcquireUnsuspendableMovieReservation()
{
    if (reservation_)
    {
        return {
            Failure("A movie input reservation is already active"),
            {}};
    }
    StateEpoch epoch = current_epoch_ ? current_epoch_() : StateEpoch{};
    if (!epoch)
        epoch = StateEpoch(1);
    InputLeaseReceipt acquired = AcquireLease(epoch);
    if (!acquired.ok)
    {
        return {
            Failure(
                acquired.message.empty()
                    ? "InputArbiter rejected the movie reservation"
                    : std::move(acquired.message)),
            {}};
    }
    if (next_reservation_ == std::numeric_limits<std::uint64_t>::max())
    {
        (void)input_.BeginRelease(acquired.lease, epoch);
        return {
            Failure("Movie reservation identity space is exhausted"),
            {}};
    }
    reservation_ = MovieReservationId(next_reservation_++);
    lease_ = acquired.lease;
    return {
        MovieServiceResult::Success(),
        reservation_};
}

MovieServiceResult
InputMovieReservationAdapter::ReleaseMovieReservation(
    MovieReservationId reservation) noexcept
{
    if (!reservation_ && !lease_)
        return MovieServiceResult::Success();
    if (!reservation || reservation != reservation_)
        return Failure("Movie input reservation identity does not match");

    StateEpoch epoch = current_epoch_ ? current_epoch_() : StateEpoch{};
    if (!epoch)
        epoch = input_.snapshot().epoch;
    InputReleaseReceipt released = input_.BeginRelease(lease_, epoch);
    if (!released.ok ||
        released.status != InputLeaseStatus::Released)
    {
        return Failure(
            released.message.empty()
                ? "Movie input lease could not be neutralized and released"
                : std::move(released.message));
    }
    reservation_ = {};
    lease_ = {};
    return MovieServiceResult::Success();
}

MovieServiceResult InputMovieReservationAdapter::CommitStateEpoch(
    StateEpoch epoch) noexcept
{
    const bool rebind = reservation_ && lease_;
    if (input_.snapshot().epoch == epoch)
    {
        const InputArbiterOperationReceipt committed =
            input_.CommitStateEpoch(epoch);
        if (!committed.ok)
        {
            return MovieServiceResult::Failure(
                MovieServiceErrorCode::IntegrityFailure,
                committed.message.empty()
                    ? "InputArbiter rejected the movie state epoch"
                    : std::string(committed.message),
                StateIntegrity::Unknown);
        }
        return MovieServiceResult::Success();
    }
    const InputArbiterOperationReceipt invalidated =
        input_.InvalidateForStateReplacement(epoch);
    if (!invalidated.ok)
    {
        return MovieServiceResult::Failure(
            MovieServiceErrorCode::IntegrityFailure,
            invalidated.message.empty()
                ? "Movie input state could not be invalidated"
                : std::string(invalidated.message),
            StateIntegrity::Unknown);
    }
    if (!rebind)
        return MovieServiceResult::Success();

    InputLeaseReceipt acquired = AcquireLease(epoch);
    if (!acquired.ok)
    {
        lease_ = {};
        return MovieServiceResult::Failure(
            MovieServiceErrorCode::IntegrityFailure,
            acquired.message.empty()
                ? "Movie input reservation could not be rebound"
                : std::move(acquired.message),
            StateIntegrity::Unknown);
    }
    lease_ = acquired.lease;
    return MovieServiceResult::Success();
}

MovieServiceResult InputMovieReservationAdapter::Shutdown() noexcept
{
    if (!reservation_)
        return MovieServiceResult::Success();
    return ReleaseMovieReservation(reservation_);
}

InputLeaseReceipt InputMovieReservationAdapter::AcquireLease(
    StateEpoch epoch)
{
    InputLeaseRequest request;
    request.owner = InputOwnerId(1);
    request.port = 0;
    request.priority = std::numeric_limits<std::int32_t>::max();
    request.suspendable = false;
    request.interruption_borrowable = false;
    request.require_neutral_acknowledgement = false;
    request.movie_exclusive = true;
    return input_.Acquire(request, epoch);
}

} // namespace savor::runtime
