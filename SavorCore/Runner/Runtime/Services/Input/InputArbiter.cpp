#include "InputArbiter.h"

#include <algorithm>
#include <utility>

namespace savor::runtime {
namespace {

[[nodiscard]] savor::GCInputFrame NeutralFrame() noexcept
{
    return {};
}

} // namespace

InputArbiter::InputArbiter(IInputBackendPort& backend)
    : backend_(backend),
      owner_thread_(std::this_thread::get_id())
{
}

InputLeaseReceipt InputArbiter::Acquire(
    const InputLeaseRequest& request,
    StateEpoch epoch)
{
    if (!OnOwnerThread())
    {
        return {
            false,
            InputLeaseStatus::Rejected,
            {},
            epoch,
            "InputArbiter mutation was attempted off its actor thread",
            InputArbiterErrorCode::WrongThread};
    }
    if (IsStopped())
    {
        return {
            false,
            InputLeaseStatus::Rejected,
            {},
            epoch,
            "InputArbiter is shut down",
            InputArbiterErrorCode::Stopped};
    }
    if (!request.owner || !epoch)
        return {false, InputLeaseStatus::Rejected, {}, epoch, "owner and epoch are required"};
    if (!request.movie_exclusive && !backend_.IsAvailable(request.port))
        return {false, InputLeaseStatus::Rejected, {}, epoch, "input port is unavailable"};
    if (epoch_ && epoch_ != epoch)
        return {false, InputLeaseStatus::Rejected, {}, epoch, "stale input epoch"};
    if (!epoch_)
        epoch_ = epoch;

    if (active_)
    {
        LeaseState* current = FindLease(*active_);
        if (!current)
            return {false, InputLeaseStatus::Rejected, {}, epoch, "active lease is missing"};
        if (current->request.movie_exclusive || !current->request.suspendable)
            return {false, InputLeaseStatus::Rejected, {}, epoch, "active lease is unsuspendable"};
        if (request.priority <= current->request.priority)
            return {false, InputLeaseStatus::Rejected, {}, epoch, "lease priority is insufficient"};
        current->status = InputLeaseStatus::Suspended;
        suspended_.push_back(current->id);
    }

    const InputLeaseId id(next_lease_++);
    LeaseState state{
        .request = request,
        .id = id,
        .epoch = epoch,
        .status = InputLeaseStatus::Active};
    leases_.emplace(id.value(), std::move(state));
    active_ = id;
    return {true, InputLeaseStatus::Active, id, epoch, {}};
}

InputLeaseReceipt InputArbiter::Borrow(
    InputLeaseId parent,
    const InputLeaseRequest& request,
    StateEpoch epoch,
    std::optional<InputNeutralWitnessId> neutral_witness)
{
    if (!OnOwnerThread())
    {
        return {
            false,
            InputLeaseStatus::Rejected,
            {},
            epoch,
            "InputArbiter mutation was attempted off its actor thread",
            InputArbiterErrorCode::WrongThread};
    }
    if (IsStopped())
    {
        return {
            false,
            InputLeaseStatus::Rejected,
            {},
            epoch,
            "InputArbiter is shut down",
            InputArbiterErrorCode::Stopped};
    }
    LeaseState* parent_state = FindLease(parent);
    if (!parent_state || !active_ || *active_ != parent ||
        parent_state->epoch != epoch || !IsCurrent(epoch))
    {
        return {false, InputLeaseStatus::Rejected, {}, epoch, "parent lease is not active"};
    }
    if (!parent_state->request.interruption_borrowable)
        return {false, InputLeaseStatus::Rejected, {}, epoch, "parent lease forbids borrowing"};
    if (parent_state->request.borrow_policy ==
        InputBorrowPolicy::RequireNeutralWitness)
    {
        const auto found = neutral_witness.has_value()
            ? neutral_witnesses_.find(neutral_witness->value())
            : neutral_witnesses_.end();
        if (found == neutral_witnesses_.end() ||
            found->second.lease != parent ||
            found->second.epoch != epoch)
        {
            return {
                false,
                InputLeaseStatus::AwaitingNeutralAcknowledgement,
                {},
                epoch,
                "a fresh arbiter-issued neutral witness is required"};
        }
        const auto publication =
            publications_.find(found->second.publication.value());
        if (publication == publications_.end() ||
            publication->second.lease != parent ||
            publication->second.epoch != epoch ||
            !(publication->second.frame == NeutralFrame()))
        {
            neutral_witnesses_.erase(found);
            return {
                false,
                InputLeaseStatus::AwaitingNeutralAcknowledgement,
                {},
                epoch,
                "neutral witness no longer names the exact current publication"};
        }
        neutral_witnesses_.erase(found);
    }

    InputLeaseRequest borrower = request;
    borrower.priority = std::max(
        borrower.priority,
        parent_state->request.priority + 1);
    return Acquire(borrower, epoch);
}

InputPublicationReceipt InputArbiter::Publish(
    InputLeaseId lease,
    const savor::GCInputFrame& frame,
    StateEpoch epoch)
{
    if (!OnOwnerThread())
    {
        return {
            false,
            lease,
            {},
            epoch,
            frame,
            "InputArbiter mutation was attempted off its actor thread",
            InputArbiterErrorCode::WrongThread};
    }
    if (IsStopped())
    {
        return {
            false,
            lease,
            {},
            epoch,
            frame,
            "InputArbiter is shut down",
            InputArbiterErrorCode::Stopped};
    }
    LeaseState* state = FindLease(lease);
    if (!state || !active_ || *active_ != lease ||
        state->status != InputLeaseStatus::Active)
    {
        return {false, lease, {}, epoch, frame, "lease is not active"};
    }
    if (!IsCurrent(epoch) || state->epoch != epoch)
        return {false, lease, {}, epoch, frame, "stale input epoch"};
    return PublishInternal(*state, frame);
}

InputAcknowledgementReceipt InputArbiter::Observe(
    InputLeaseId lease,
    InputPublicationToken publication,
    StateEpoch epoch)
{
    if (!OnOwnerThread())
    {
        return {
            false,
            false,
            {},
            publication,
            epoch,
            0,
            "InputArbiter backend observation was attempted off its actor thread",
            InputArbiterErrorCode::WrongThread};
    }
    if (IsStopped())
    {
        return {
            false,
            false,
            {},
            publication,
            epoch,
            0,
            "InputArbiter is shut down",
            InputArbiterErrorCode::Stopped};
    }
    const auto found = publications_.find(publication.value());
    if (found == publications_.end() || found->second.lease != lease)
    {
        return {false, false, {}, publication, epoch, 0, "unknown input publication"};
    }
    if (!IsCurrent(epoch) || found->second.epoch != epoch)
    {
        return {false, false, {}, publication, epoch, 0, "stale input publication"};
    }
    const LeaseState* state = FindLease(lease);
    if (!state)
        return {false, false, {}, publication, epoch, 0, "input lease is unavailable"};

    BackendInputPoll poll = backend_.QueryPoll(state->request.port);
    if (!poll.result.ok)
    {
        return {
            false,
            false,
            {},
            publication,
            epoch,
            0,
            std::move(poll.result.message)};
    }
    const bool acknowledged =
        poll.sequence == found->second.backend_sequence &&
        poll.callback_count > 0;
    return {
        true,
        acknowledged,
        acknowledged ? InputPollReceiptId(next_poll_receipt_++) : InputPollReceiptId{},
        publication,
        epoch,
        poll.callback_count,
        acknowledged ? std::string{} : "guest has not observed the publication"};
}

InputNeutralWitnessReceipt InputArbiter::ProveNeutralWitness(
    InputLeaseId lease,
    InputPublicationToken publication,
    StateEpoch epoch)
{
    if (!OnOwnerThread())
    {
        return {
            false,
            {},
            lease,
            publication,
            {},
            epoch,
            "InputArbiter backend observation was attempted off its actor thread",
            InputArbiterErrorCode::WrongThread};
    }
    if (IsStopped())
    {
        return {
            false,
            {},
            lease,
            publication,
            {},
            epoch,
            "InputArbiter is shut down",
            InputArbiterErrorCode::Stopped};
    }
    const auto found = publications_.find(publication.value());
    if (found == publications_.end() ||
        found->second.lease != lease ||
        found->second.epoch != epoch ||
        !(found->second.frame == NeutralFrame()))
    {
        return {
            false,
            {},
            lease,
            publication,
            {},
            epoch,
            "neutral witness requires the exact current neutral publication"};
    }
    const InputAcknowledgementReceipt observed =
        Observe(lease, publication, epoch);
    if (!observed.ok || !observed.acknowledged ||
        !observed.receipt)
    {
        return {
            false,
            {},
            lease,
            publication,
            observed.receipt,
            epoch,
            observed.message.empty()
                ? "guest has not acknowledged the neutral publication"
                : observed.message,
            observed.error};
    }
    const InputNeutralWitnessId witness(
        next_neutral_witness_++);
    neutral_witnesses_.emplace(
        witness.value(),
        NeutralWitnessState{
            lease,
            publication,
            observed.receipt,
            epoch});
    return {
        true,
        witness,
        lease,
        publication,
        observed.receipt,
        epoch,
        {},
        InputArbiterErrorCode::None};
}

InputReleaseReceipt InputArbiter::BeginRelease(
    InputLeaseId lease,
    StateEpoch epoch)
{
    if (!OnOwnerThread())
    {
        return {
            false,
            InputLeaseStatus::Rejected,
            lease,
            {},
            {},
            epoch,
            "InputArbiter mutation was attempted off its actor thread",
            InputArbiterErrorCode::WrongThread};
    }
    if (IsStopped())
    {
        return {
            false,
            InputLeaseStatus::Rejected,
            lease,
            {},
            {},
            epoch,
            "InputArbiter is shut down",
            InputArbiterErrorCode::Stopped};
    }
    LeaseState* state = FindLease(lease);
    if (!state)
        return {true, InputLeaseStatus::Released, lease, {}, {}, epoch, {}};
    if (!IsCurrent(epoch) || state->epoch != epoch)
    {
        return {
            false,
            InputLeaseStatus::Rejected,
            lease,
            {},
            {},
            epoch,
            "stale input lease"};
    }
    if (!active_ || *active_ != lease)
        return {false, state->status, lease, {}, {}, epoch, "only the active lease can release"};

    if (state->request.movie_exclusive &&
        !backend_.IsAvailable(state->request.port))
    {
        return FinishRelease(*state);
    }

    const InputPublicationReceipt neutral = PublishInternal(*state, NeutralFrame());
    if (!neutral.ok)
        return {false, state->status, lease, {}, {}, epoch, neutral.message};
    if (!state->request.require_neutral_acknowledgement)
        return FinishRelease(*state);

    state->pending_neutral = neutral.publication;
    state->status = InputLeaseStatus::AwaitingNeutralAcknowledgement;
    return {
        true,
        state->status,
        lease,
        neutral.publication,
        {},
        epoch,
        "neutral publication requires guest acknowledgement"};
}

InputReleaseReceipt InputArbiter::CompleteRelease(
    InputLeaseId lease,
    InputPublicationToken neutral_publication,
    StateEpoch epoch)
{
    if (!OnOwnerThread())
    {
        return {
            false,
            InputLeaseStatus::Rejected,
            lease,
            neutral_publication,
            {},
            epoch,
            "InputArbiter mutation was attempted off its actor thread",
            InputArbiterErrorCode::WrongThread};
    }
    if (IsStopped())
    {
        return {
            false,
            InputLeaseStatus::Rejected,
            lease,
            neutral_publication,
            {},
            epoch,
            "InputArbiter is shut down",
            InputArbiterErrorCode::Stopped};
    }
    LeaseState* state = FindLease(lease);
    if (!state || state->pending_neutral != neutral_publication)
    {
        return {
            false,
            InputLeaseStatus::Rejected,
            lease,
            neutral_publication,
            {},
            epoch,
            "neutral release publication does not match"};
    }
    const InputAcknowledgementReceipt observed =
        Observe(lease, neutral_publication, epoch);
    if (!observed.ok || !observed.acknowledged)
    {
        return {
            observed.ok,
            InputLeaseStatus::AwaitingNeutralAcknowledgement,
            lease,
            neutral_publication,
            {},
            epoch,
            observed.message};
    }
    return FinishRelease(*state);
}

InputAdvanceBindingReceipt InputArbiter::CreateAdvanceBinding(
    InputLeaseId lease,
    std::vector<savor::GCInputFrame> frames,
    StateEpoch epoch,
    std::uint32_t retry_limit)
{
    if (!OnOwnerThread())
    {
        return {
            false,
            {},
            epoch,
            "InputArbiter mutation was attempted off its actor thread",
            InputArbiterErrorCode::WrongThread};
    }
    if (IsStopped())
    {
        return {
            false,
            {},
            epoch,
            "InputArbiter is shut down",
            InputArbiterErrorCode::Stopped};
    }
    const LeaseState* state = FindLease(lease);
    if (!state || state->epoch != epoch || frames.empty() || !IsCurrent(epoch))
    {
        return {
            false,
            {},
            epoch,
            "input advance binding requires a current lease and at least one frame"};
    }
    const InputAdvanceBindingId id(next_binding_++);
    bindings_.emplace(
        id.value(),
        BindingState{
            .lease = lease,
            .epoch = epoch,
            .frames = std::move(frames),
            .retry_limit = retry_limit});
    return {true, id, epoch, {}};
}

InputArbiterOperationReceipt InputArbiter::ValidatePublication(
    const InputPublicationEvidence& publication) const noexcept
{
    if (!OnOwnerThread())
    {
        return {
            false,
            InputArbiterErrorCode::WrongThread,
            "InputArbiter access was attempted off its actor thread"};
    }
    if (IsStopped())
    {
        return {
            false,
            InputArbiterErrorCode::Stopped,
            "InputArbiter is shut down"};
    }
    if (!MatchesPublication(publication))
    {
        return {
            false,
            InputArbiterErrorCode::None,
            "input publication does not name the exact current lease publication"};
    }
    return {true, InputArbiterErrorCode::None, {}};
}

InputAdvanceBindingReceipt
InputArbiter::CreatePublicationRelationship(
    const InputPublicationEvidence& publication)
{
    const InputArbiterOperationReceipt validated =
        ValidatePublication(publication);
    if (!validated.ok)
    {
        return {
            false,
            {},
            publication.epoch,
            std::string(validated.message),
            validated.error};
    }
    const InputAdvanceBindingId id(next_binding_++);
    bindings_.emplace(
        id.value(),
        BindingState{
            .lease = publication.lease,
            .epoch = publication.epoch,
            .publication_relationship = publication});
    return {true, id, publication.epoch, {}};
}

InputArbiterOperationReceipt InputArbiter::RemoveAdvanceBinding(
    InputAdvanceBindingId binding) noexcept
{
    if (!OnOwnerThread())
    {
        return {
            false,
            InputArbiterErrorCode::WrongThread,
            "InputArbiter mutation was attempted off its actor thread"};
    }
    if (IsStopped())
    {
        return {
            false,
            InputArbiterErrorCode::Stopped,
            "InputArbiter is shut down"};
    }
    bindings_.erase(binding.value());
    return {true, InputArbiterErrorCode::None, {}};
}

InputArbiterOperationReceipt InputArbiter::CommitStateEpoch(
    StateEpoch epoch) noexcept
{
    if (!OnOwnerThread())
    {
        return {
            false,
            InputArbiterErrorCode::WrongThread,
            "InputArbiter mutation was attempted off its actor thread"};
    }
    if (IsStopped())
    {
        return {
            false,
            InputArbiterErrorCode::Stopped,
            "InputArbiter is shut down"};
    }
    epoch_ = epoch;
    return {true, InputArbiterErrorCode::None, {}};
}

InputArbiterOperationReceipt InputArbiter::InvalidateForStateReplacement(
    StateEpoch next_epoch) noexcept
{
    if (!OnOwnerThread())
    {
        return {
            false,
            InputArbiterErrorCode::WrongThread,
            "InputArbiter mutation was attempted off its actor thread"};
    }
    if (IsStopped())
    {
        return {
            false,
            InputArbiterErrorCode::Stopped,
            "InputArbiter is shut down"};
    }
    bool neutralized = true;
    if (active_)
    {
        if (LeaseState* active = FindLease(*active_))
        {
            if (!(active->request.movie_exclusive &&
                  !backend_.IsAvailable(active->request.port)))
            {
                const BackendInputPublication publication =
                    backend_.Publish(active->request.port, NeutralFrame());
                neutralized = publication.result.ok &&
                    publication.sequence != 0;
            }
        }
    }
    ClearRetainedState();
    epoch_ = next_epoch;
    if (!neutralized)
    {
        return {
            false,
            InputArbiterErrorCode::BackendFailure,
            "input neutralization failed during state replacement"};
    }
    return {true, InputArbiterErrorCode::None, {}};
}

InputArbiterShutdownReceipt InputArbiter::Shutdown() noexcept
{
    if (!OnOwnerThread())
    {
        return {
            false,
            false,
            false,
            {},
            epoch_,
            "InputArbiter shutdown was attempted off its actor thread",
            InputArbiterErrorCode::WrongThread};
    }
    if (shutdown_receipt_.has_value())
        return *shutdown_receipt_;

    InputArbiterShutdownReceipt receipt{
        true,
        true,
        false,
        {},
        epoch_,
        {},
        InputArbiterErrorCode::None};

    if (active_)
    {
        LeaseState* state = FindLease(*active_);
        if (!state)
        {
            receipt.ok = false;
            receipt.cleanup_complete = false;
            receipt.taint_required = true;
            receipt.message = "active input lease was missing during shutdown";
        }
        else if (state->request.movie_exclusive &&
                 !backend_.IsAvailable(state->request.port))
        {
            // A pre-boot movie reservation has not yet installed a backend
            // input source, so there is no guest-observed state to neutralize.
        }
        else if (state->pending_neutral.has_value())
        {
            receipt.neutral_publication = *state->pending_neutral;
            const InputAcknowledgementReceipt observed =
                Observe(state->id, *state->pending_neutral, state->epoch);
            if (!observed.ok || !observed.acknowledged)
            {
                receipt.ok = false;
                receipt.cleanup_complete = false;
                receipt.taint_required = true;
                receipt.message = observed.message.empty()
                    ? "guest-observed neutral input could not be proven during shutdown"
                    : observed.message;
            }
        }
        else
        {
            const InputPublicationReceipt neutral =
                PublishInternal(*state, NeutralFrame());
            receipt.neutral_publication = neutral.publication;
            if (!neutral.ok)
            {
                receipt.ok = false;
                receipt.cleanup_complete = false;
                receipt.taint_required = true;
                receipt.message = neutral.message.empty()
                    ? "input neutralization failed during shutdown"
                    : neutral.message;
            }
            else if (state->request.require_neutral_acknowledgement)
            {
                const InputAcknowledgementReceipt observed =
                    Observe(state->id, neutral.publication, state->epoch);
                if (!observed.ok || !observed.acknowledged)
                {
                    receipt.ok = false;
                    receipt.cleanup_complete = false;
                    receipt.taint_required = true;
                    receipt.message = observed.message.empty()
                        ? "guest-observed neutral input could not be proven during shutdown"
                        : observed.message;
                }
            }
        }
    }
    else if (!leases_.empty())
    {
        receipt.ok = false;
        receipt.cleanup_complete = false;
        receipt.taint_required = true;
        receipt.message =
            "input leases remained without an active owner during shutdown";
    }

    ClearRetainedState();
    stopped_ = true;
    shutdown_receipt_ = receipt;
    return receipt;
}

InputArbiterSnapshot InputArbiter::snapshot() const noexcept
{
    bool movie = false;
    if (active_)
    {
        if (const LeaseState* state = FindLease(*active_))
            movie = state->request.movie_exclusive;
    }
    return {
        epoch_,
        active_,
        suspended_.size(),
        leases_.size(),
        publications_.size(),
        bindings_.size(),
        movie,
        stopped_};
}

InputAdvanceReceipt InputArbiter::Validate(
    InputAdvanceBindingId binding,
    StateEpoch epoch)
{
    return ValidateBinding(binding, epoch);
}

InputAdvanceReceipt InputArbiter::ValidateBinding(
    InputAdvanceBindingId binding,
    StateEpoch epoch) const
{
    if (!OnOwnerThread())
    {
        return BindingFailure(
            "InputArbiter access was attempted off its actor thread");
    }
    if (IsStopped())
        return BindingFailure("InputArbiter is shut down");
    const auto found = bindings_.find(binding.value());
    if (found == bindings_.end())
        return BindingFailure("input advance binding is unknown");
    if (!IsCurrent(epoch) || found->second.epoch != epoch)
        return BindingFailure("input advance binding has a stale epoch");
    const LeaseState* lease = FindLease(found->second.lease);
    if (!lease || !active_ || *active_ != lease->id)
        return BindingFailure("input advance lease is not active");
    if (found->second.publication_relationship &&
        !MatchesPublication(
            *found->second.publication_relationship))
    {
        return BindingFailure(
            "input publication relationship is no longer current");
    }
    return {true, InputAdvanceDecision::Continue, {}, {}};
}

InputAdvanceReceipt InputArbiter::PrepareNext(
    InputAdvanceBindingId binding,
    StateEpoch epoch,
    std::uint32_t advance_ordinal)
{
    InputAdvanceReceipt valid = ValidateBinding(binding, epoch);
    if (!valid.ok)
        return valid;

    BindingState& state = bindings_.at(binding.value());
    if (state.publication_relationship || state.frames.empty())
    {
        return BindingFailure(
            "input publication relationships cannot prepare an advance");
    }
    const std::size_t index = std::min<std::size_t>(
        advance_ordinal,
        state.frames.size() - 1);
    InputPublicationReceipt publication =
        Publish(state.lease, state.frames[index], epoch);
    if (!publication.ok)
        return BindingFailure(std::move(publication.message));
    state.prepared_ordinal =
        static_cast<std::uint32_t>(index);
    state.prepared_publication =
        publication.publication;
    return {
        .ok = true,
        .decision = InputAdvanceDecision::Continue,
        .publication = publication.publication,
        .publication_evidence = InputPublicationEvidence{
            publication.lease,
            publication.publication,
            publication.epoch,
            publication.frame}};
}

InputAdvanceReceipt InputArbiter::ObserveAcknowledgement(
    InputAdvanceBindingId binding,
    InputPublicationToken publication,
    StateEpoch epoch)
{
    InputAdvanceReceipt valid = ValidateBinding(binding, epoch);
    if (!valid.ok)
        return valid;
    BindingState& state = bindings_.at(binding.value());
    if (state.publication_relationship ||
        state.prepared_publication != publication)
    {
        return BindingFailure(
            "input acknowledgement does not match the prepared publication");
    }
    InputAcknowledgementReceipt observed =
        Observe(state.lease, publication, epoch);
    if (!observed.ok)
        return BindingFailure(std::move(observed.message));
    if (observed.acknowledged)
    {
        state.retry_count = 0;
        const bool sequence_complete =
            state.prepared_ordinal &&
            static_cast<std::size_t>(
                *state.prepared_ordinal) + 1 >=
                state.frames.size();
        return {
            true,
            sequence_complete
                ? InputAdvanceDecision::Complete
                : InputAdvanceDecision::Continue,
            publication,
            {}};
    }
    if (state.retry_count++ < state.retry_limit)
        return {true, InputAdvanceDecision::Retry, publication, observed.message};
    return BindingFailure("input acknowledgement retry budget exhausted");
}

InputAdvanceReceipt InputArbiter::Complete(
    InputAdvanceBindingId binding,
    StateEpoch epoch) noexcept
{
    if (!OnOwnerThread())
    {
        return {
            false,
            InputAdvanceDecision::Failed,
            {},
            "InputArbiter mutation was attempted off its actor thread"};
    }
    if (IsStopped())
    {
        return {
            false,
            InputAdvanceDecision::Failed,
            {},
            "InputArbiter is shut down"};
    }
    const auto found = bindings_.find(binding.value());
    if (found == bindings_.end())
        return {true, InputAdvanceDecision::Complete, {}, {}};
    if (found->second.epoch != epoch || !IsCurrent(epoch))
    {
        return {
            false,
            InputAdvanceDecision::Failed,
            {},
            "stale input advance binding"};
    }
    bindings_.erase(found);
    return {true, InputAdvanceDecision::Complete, {}, {}};
}

InputAdvanceReceipt InputArbiter::Cancel(
    InputAdvanceBindingId binding,
    StateEpoch epoch) noexcept
{
    if (!OnOwnerThread())
    {
        return {
            false,
            InputAdvanceDecision::Failed,
            {},
            "InputArbiter mutation was attempted off its actor thread"};
    }
    if (IsStopped())
    {
        return {
            false,
            InputAdvanceDecision::Failed,
            {},
            "InputArbiter is shut down"};
    }
    const auto found = bindings_.find(binding.value());
    if (found == bindings_.end())
        return {true, InputAdvanceDecision::Cancelled, {}, {}};
    if (found->second.epoch != epoch || !IsCurrent(epoch))
        return {false, InputAdvanceDecision::Failed, {}, "stale input advance binding"};
    if (!found->second.publication_relationship)
    {
        if (LeaseState* lease = FindLease(found->second.lease))
        {
            (void)backend_.Publish(
                lease->request.port,
                NeutralFrame());
        }
        ErasePublicationsForLease(found->second.lease);
    }
    bindings_.erase(found);
    return {true, InputAdvanceDecision::Cancelled, {}, {}};
}

InputArbiter::LeaseState* InputArbiter::FindLease(InputLeaseId lease) noexcept
{
    const auto found = leases_.find(lease.value());
    return found == leases_.end() ? nullptr : &found->second;
}

const InputArbiter::LeaseState* InputArbiter::FindLease(
    InputLeaseId lease) const noexcept
{
    const auto found = leases_.find(lease.value());
    return found == leases_.end() ? nullptr : &found->second;
}

bool InputArbiter::IsCurrent(StateEpoch epoch) const noexcept
{
    return epoch_ && epoch_ == epoch;
}

InputPublicationReceipt InputArbiter::PublishInternal(
    LeaseState& lease,
    const savor::GCInputFrame& frame)
{
    BackendInputPublication backend =
        backend_.Publish(lease.request.port, frame);
    if (!backend.result.ok || backend.sequence == 0)
    {
        return {
            false,
            lease.id,
            {},
            lease.epoch,
            frame,
            backend.result.message.empty()
                ? "input backend did not publish a sequence"
                : std::move(backend.result.message)};
    }
    const InputPublicationToken token(next_publication_++);
    ErasePublicationsForLease(lease.id);
    publications_.emplace(
        token.value(),
        PublicationState{
            lease.id,
            lease.epoch,
            backend.sequence,
            frame});
    lease.latest_publication = token;
    return {true, lease.id, token, lease.epoch, frame, {}};
}

InputReleaseReceipt InputArbiter::FinishRelease(LeaseState& lease)
{
    const InputLeaseId id = lease.id;
    const StateEpoch epoch = lease.epoch;
    active_.reset();
    suspended_.erase(
        std::remove(suspended_.begin(), suspended_.end(), id),
        suspended_.end());
    EraseLeaseState(id);

    std::optional<InputLeaseId> resumed;
    while (!suspended_.empty() && !resumed.has_value())
    {
        const InputLeaseId candidate = suspended_.back();
        suspended_.pop_back();
        if (LeaseState* parent = FindLease(candidate);
            parent &&
            parent->status == InputLeaseStatus::Suspended &&
            parent->epoch == epoch_)
        {
            parent->status = InputLeaseStatus::Active;
            active_ = parent->id;
            resumed = parent->id;
        }
    }
    return {
        true,
        InputLeaseStatus::Released,
        id,
        {},
        resumed,
        epoch,
        resumed ? "suspended lease requires fresh publication" : std::string{}};
}

InputAdvanceReceipt InputArbiter::BindingFailure(std::string message) const
{
    return {false, InputAdvanceDecision::Failed, {}, std::move(message)};
}

bool InputArbiter::MatchesPublication(
    const InputPublicationEvidence& publication) const noexcept
{
    if (!publication.lease || !publication.publication ||
        !publication.epoch || !IsCurrent(publication.epoch) ||
        !active_ || *active_ != publication.lease)
    {
        return false;
    }
    const LeaseState* lease = FindLease(publication.lease);
    if (!lease || lease->status != InputLeaseStatus::Active ||
        lease->epoch != publication.epoch ||
        lease->latest_publication != publication.publication)
    {
        return false;
    }
    const auto found =
        publications_.find(publication.publication.value());
    return found != publications_.end() &&
        found->second.lease == publication.lease &&
        found->second.epoch == publication.epoch &&
        found->second.frame == publication.frame;
}

bool InputArbiter::OnOwnerThread() const noexcept
{
    return std::this_thread::get_id() == owner_thread_;
}

bool InputArbiter::IsStopped() const noexcept
{
    return stopped_;
}

void InputArbiter::ErasePublicationsForLease(InputLeaseId lease) noexcept
{
    EraseWitnessesForLease(lease);
    for (auto item = publications_.begin(); item != publications_.end();)
    {
        if (item->second.lease == lease)
            item = publications_.erase(item);
        else
            ++item;
    }
}

void InputArbiter::EraseWitnessesForPublication(
    InputPublicationToken publication) noexcept
{
    for (auto item = neutral_witnesses_.begin();
         item != neutral_witnesses_.end();)
    {
        if (item->second.publication == publication)
            item = neutral_witnesses_.erase(item);
        else
            ++item;
    }
}

void InputArbiter::EraseWitnessesForLease(
    InputLeaseId lease) noexcept
{
    for (auto item = neutral_witnesses_.begin();
         item != neutral_witnesses_.end();)
    {
        if (item->second.lease == lease)
            item = neutral_witnesses_.erase(item);
        else
            ++item;
    }
}

void InputArbiter::EraseBindingsForLease(InputLeaseId lease) noexcept
{
    for (auto item = bindings_.begin(); item != bindings_.end();)
    {
        if (item->second.lease == lease)
            item = bindings_.erase(item);
        else
            ++item;
    }
}

void InputArbiter::EraseLeaseState(InputLeaseId lease) noexcept
{
    ErasePublicationsForLease(lease);
    EraseBindingsForLease(lease);
    leases_.erase(lease.value());
}

void InputArbiter::ClearRetainedState() noexcept
{
    active_.reset();
    suspended_.clear();
    bindings_.clear();
    publications_.clear();
    neutral_witnesses_.clear();
    leases_.clear();
}

} // namespace savor::runtime
