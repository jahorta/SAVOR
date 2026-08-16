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
    WorksetEpoch epoch)
{
    if (!OnOwnerThread())
    {
        return {false, InputLeaseStatus::Rejected, {}, epoch,
            "InputArbiter mutation was attempted off its actor thread",
            InputArbiterErrorCode::WrongThread};
    }
    if (IsStopped())
    {
        return {false, InputLeaseStatus::Rejected, {}, epoch,
            "InputArbiter is shut down", InputArbiterErrorCode::Stopped};
    }
    if (!request.owner || !epoch)
        return {false, InputLeaseStatus::Rejected, {}, epoch,
            "owner and epoch are required"};
    if (!request.movie_exclusive && !backend_.IsAvailable(request.port))
        return {false, InputLeaseStatus::Rejected, {}, epoch,
            "input port is unavailable"};
    if (epoch_ && epoch_ != epoch)
        return {false, InputLeaseStatus::Rejected, {}, epoch,
            "stale input epoch"};
    if (!epoch_)
        epoch_ = epoch;

    bool inherits_backend = false;
    if (active_)
    {
        LeaseState* current = FindLease(*active_);
        if (!current)
            return {false, InputLeaseStatus::Rejected, {}, epoch,
                "active lease is missing"};
        if (current->request.movie_exclusive || !current->request.suspendable)
            return {false, InputLeaseStatus::Rejected, {}, epoch,
                "active lease is unsuspendable"};
        if (request.priority <= current->request.priority)
            return {false, InputLeaseStatus::Rejected, {}, epoch,
                "lease priority is insufficient"};
        current->status = InputLeaseStatus::Suspended;
        suspended_.push_back(current->id);
        inherits_backend = true;
    }

    const InputLeaseId id(next_lease_++);
    LeaseState state{
        .request = request,
        .id = id,
        .epoch = epoch,
        .status = InputLeaseStatus::Active,
        .input_state = InputState::Neutral,
        .state_generation = 1,
        .current_binding = std::nullopt,
        .backend_neutral = !inherits_backend,
        .mutated_backend = false};
    leases_.emplace(id.value(), std::move(state));
    active_ = id;
    return {true, InputLeaseStatus::Active, id, epoch, {}};
}

InputLeaseReceipt InputArbiter::Borrow(
    InputLeaseId parent,
    const InputLeaseRequest& request,
    WorksetEpoch epoch)
{
    if (!OnOwnerThread())
    {
        return {false, InputLeaseStatus::Rejected, {}, epoch,
            "InputArbiter mutation was attempted off its actor thread",
            InputArbiterErrorCode::WrongThread};
    }
    if (IsStopped())
    {
        return {false, InputLeaseStatus::Rejected, {}, epoch,
            "InputArbiter is shut down", InputArbiterErrorCode::Stopped};
    }
    LeaseState* parent_state = FindLease(parent);
    if (!parent_state || !active_ || *active_ != parent ||
        parent_state->epoch != epoch || !IsCurrent(epoch))
    {
        return {false, InputLeaseStatus::Rejected, {}, epoch,
            "parent lease is not active"};
    }
    if (!parent_state->request.interruption_borrowable)
        return {false, InputLeaseStatus::Rejected, {}, epoch,
            "parent lease forbids borrowing"};
    if (parent_state->request.borrow_policy ==
            InputBorrowPolicy::RequireStableNeutral &&
        (parent_state->input_state != InputState::Neutral ||
         !parent_state->backend_neutral))
    {
        return {false, InputLeaseStatus::Rejected, {}, epoch,
            "borrowing requires a stable neutral parent lease"};
    }

    InputLeaseRequest borrower = request;
    borrower.priority = std::max(
        borrower.priority,
        parent_state->request.priority + 1);
    return Acquire(borrower, epoch);
}

InputExecutionBindingReceipt InputArbiter::ApplyState(
    InputLeaseId lease,
    const savor::GCInputFrame& frame,
    WorksetEpoch epoch)
{
    if (!OnOwnerThread())
    {
        return {.ok = false, .lease = lease, .epoch = epoch, .frame = frame,
            .message = "InputArbiter mutation was attempted off its actor thread",
            .error = InputArbiterErrorCode::WrongThread};
    }
    if (IsStopped())
    {
        return {.ok = false, .lease = lease, .epoch = epoch, .frame = frame,
            .message = "InputArbiter is shut down",
            .error = InputArbiterErrorCode::Stopped};
    }
    LeaseState* state = FindLease(lease);
    if (!state || !active_ || *active_ != lease ||
        state->status != InputLeaseStatus::Active)
    {
        return {.ok = false, .lease = lease, .epoch = epoch, .frame = frame,
            .message = "lease is not active"};
    }
    if (!IsCurrent(epoch) || state->epoch != epoch)
    {
        return {.ok = false, .lease = lease, .epoch = epoch, .frame = frame,
            .message = "stale input epoch",
            .error = InputArbiterErrorCode::WorksetEpochMismatch};
    }
    if (state->input_state == InputState::DeliveryPending)
    {
        return {.ok = false, .lease = lease, .epoch = epoch, .frame = frame,
            .message = "one-shot input delivery must complete before applying another state"};
    }

    const bool neutral = IsNeutral(frame);
    if (state->input_state == InputState::NeutralTransitionPending)
    {
        if (!neutral || !state->current_binding)
        {
            return {.ok = false, .lease = lease, .epoch = epoch, .frame = frame,
                .message = "neutral transition must be observed before another held state"};
        }
        const BindingState* current = FindBinding(*state->current_binding);
        if (!current)
        {
            return {.ok = false, .lease = lease, .epoch = epoch, .frame = frame,
                .message = "pending neutral transition binding is unavailable"};
        }
        return {
            true,
            current->id,
            current->lease,
            current->epoch,
            current->state_generation,
            current->frame,
            current->kind,
            current->requires_observation,
            {}};
    }

    if (neutral && state->input_state == InputState::Neutral &&
        state->backend_neutral)
    {
        return BindCurrentState(
            *state,
            frame,
            InputBindingKind::StableNeutral,
            std::nullopt);
    }

    const PublishedState published = PublishBackend(*state, frame);
    if (!published.ok)
    {
        return {.ok = false, .lease = lease, .epoch = epoch, .frame = frame,
            .message = published.message, .error = published.error};
    }
    ++state->state_generation;
    state->input_state = neutral
        ? InputState::NeutralTransitionPending
        : InputState::Held;
    state->backend_neutral = neutral;
    state->mutated_backend = true;
    return BindCurrentState(
        *state,
        frame,
        neutral ? InputBindingKind::NeutralTransition
                : InputBindingKind::Held,
        published);
}

InputExecutionBindingReceipt InputArbiter::BeginDelivery(
    InputLeaseId lease,
    const savor::GCInputFrame& frame,
    WorksetEpoch epoch)
{
    if (!OnOwnerThread())
    {
        return {.ok = false, .lease = lease, .epoch = epoch, .frame = frame,
            .message = "InputArbiter mutation was attempted off its actor thread",
            .error = InputArbiterErrorCode::WrongThread};
    }
    if (IsStopped())
    {
        return {.ok = false, .lease = lease, .epoch = epoch, .frame = frame,
            .message = "InputArbiter is shut down",
            .error = InputArbiterErrorCode::Stopped};
    }
    LeaseState* state = FindLease(lease);
    if (!state || !active_ || *active_ != lease ||
        state->status != InputLeaseStatus::Active)
    {
        return {.ok = false, .lease = lease, .epoch = epoch, .frame = frame,
            .message = "lease is not active"};
    }
    if (!IsCurrent(epoch) || state->epoch != epoch)
    {
        return {.ok = false, .lease = lease, .epoch = epoch, .frame = frame,
            .message = "stale input epoch",
            .error = InputArbiterErrorCode::WorksetEpochMismatch};
    }
    if (state->input_state != InputState::Neutral ||
        !state->backend_neutral)
    {
        return {.ok = false, .lease = lease, .epoch = epoch, .frame = frame,
            .message = "one-shot delivery requires a stable neutral lease"};
    }

    const PublishedState published = PublishBackend(*state, frame);
    if (!published.ok)
    {
        return {.ok = false, .lease = lease, .epoch = epoch, .frame = frame,
            .message = published.message, .error = published.error};
    }
    ++state->state_generation;
    state->input_state = InputState::DeliveryPending;
    state->backend_neutral = IsNeutral(frame);
    state->mutated_backend = true;
    return BindCurrentState(
        *state,
        frame,
        InputBindingKind::Delivery,
        published,
        InputDeliveryId(next_delivery_++));
}

InputDeliveryReceipt InputArbiter::CompleteDelivery(
    InputLeaseId lease,
    InputExecutionBindingId binding,
    WorksetEpoch epoch)
{
    if (!OnOwnerThread())
    {
        return {.ok = false, .binding = binding, .lease = lease, .epoch = epoch,
            .message = "InputArbiter mutation was attempted off its actor thread",
            .error = InputArbiterErrorCode::WrongThread};
    }
    if (IsStopped())
    {
        return {.ok = false, .binding = binding, .lease = lease, .epoch = epoch,
            .message = "InputArbiter is shut down",
            .error = InputArbiterErrorCode::Stopped};
    }
    LeaseState* state = FindLease(lease);
    BindingState* delivery = FindBinding(binding);
    if (!state || !delivery || !active_ || *active_ != lease ||
        state->status != InputLeaseStatus::Active ||
        state->input_state != InputState::DeliveryPending ||
        state->current_binding != binding ||
        delivery->lease != lease || delivery->epoch != epoch ||
        delivery->kind != InputBindingKind::Delivery ||
        !delivery->delivery || !delivery->publication)
    {
        return {.ok = false, .binding = binding, .lease = lease, .epoch = epoch,
            .message = "delivery binding is not the exact active delivery"};
    }
    if (!delivery->observed || !delivery->poll)
    {
        return {.ok = false, .delivery = *delivery->delivery,
            .binding = binding, .lease = lease,
            .publication = *delivery->publication, .epoch = epoch,
            .state_generation = delivery->state_generation,
            .frame = delivery->frame,
            .message = "delivery has not been observed by the guest"};
    }

    InputDeliveryReceipt receipt{
        true,
        *delivery->delivery,
        binding,
        lease,
        *delivery->publication,
        delivery->poll,
        epoch,
        delivery->state_generation,
        delivery->callback_count,
        delivery->frame,
        {}};

    if (!IsNeutral(delivery->frame))
    {
        const PublishedState neutral = PublishBackend(*state, NeutralFrame());
        if (!neutral.ok)
        {
            receipt.ok = false;
            receipt.message = neutral.message;
            receipt.error = neutral.error;
            return receipt;
        }
        state->mutated_backend = true;
    }
    ++state->state_generation;
    state->input_state = InputState::Neutral;
    state->backend_neutral = true;
    EraseBindingsForLease(lease);
    state->current_binding.reset();
    return receipt;
}

InputArbiterOperationReceipt InputArbiter::ValidateBinding(
    const InputExecutionBindingEvidence& evidence) const noexcept
{
    if (!OnOwnerThread())
    {
        return {false, InputArbiterErrorCode::WrongThread,
            "InputArbiter access was attempted off its actor thread"};
    }
    if (IsStopped())
    {
        return {false, InputArbiterErrorCode::Stopped,
            "InputArbiter is shut down"};
    }
    const LeaseState* lease = FindLease(evidence.lease);
    const BindingState* binding = FindBinding(evidence.binding);
    if (!lease || !binding || !active_ || *active_ != evidence.lease ||
        lease->status != InputLeaseStatus::Active ||
        lease->epoch != evidence.epoch || !IsCurrent(evidence.epoch) ||
        lease->current_binding != evidence.binding ||
        lease->state_generation != evidence.state_generation ||
        binding->lease != evidence.lease ||
        binding->epoch != evidence.epoch ||
        binding->state_generation != evidence.state_generation ||
        binding->frame != evidence.frame ||
        binding->publication.value_or(InputPublicationToken{}) !=
            evidence.publication)
    {
        return {false, InputArbiterErrorCode::None,
            "input execution binding does not name the exact current lease state"};
    }
    return {true, InputArbiterErrorCode::None, {}};
}

InputExecutionRelationshipReceipt InputArbiter::CreateExecutionRelationship(
    const InputExecutionBindingEvidence& evidence)
{
    const InputArbiterOperationReceipt validated = ValidateBinding(evidence);
    if (!validated.ok)
    {
        return {false, {}, evidence.epoch, std::string(validated.message),
            validated.error};
    }
    const InputExecutionRelationshipId id(next_relationship_++);
    relationships_.emplace(
        id.value(),
        RelationshipState{
            id,
            evidence.binding,
            evidence.lease,
            evidence.epoch,
            evidence.state_generation});
    return {true, id, evidence.epoch, {}};
}

InputArbiterOperationReceipt InputArbiter::RemoveExecutionRelationship(
    InputExecutionRelationshipId relationship) noexcept
{
    if (!OnOwnerThread())
    {
        return {false, InputArbiterErrorCode::WrongThread,
            "InputArbiter mutation was attempted off its actor thread"};
    }
    if (IsStopped())
    {
        return {false, InputArbiterErrorCode::Stopped,
            "InputArbiter is shut down"};
    }
    relationships_.erase(relationship.value());
    return {true, InputArbiterErrorCode::None, {}};
}

InputLeaseCloseReceipt InputArbiter::CloseLease(
    InputLeaseId lease,
    WorksetEpoch epoch)
{
    if (!OnOwnerThread())
    {
        return {false, InputLeaseStatus::Rejected, lease, {}, {}, epoch,
            "InputArbiter mutation was attempted off its actor thread",
            InputArbiterErrorCode::WrongThread};
    }
    if (IsStopped())
    {
        return {false, InputLeaseStatus::Rejected, lease, {}, {}, epoch,
            "InputArbiter is shut down", InputArbiterErrorCode::Stopped};
    }
    LeaseState* state = FindLease(lease);
    if (!state)
        return {true, InputLeaseStatus::Released, lease, {}, {}, epoch, {}};
    if (!IsCurrent(epoch) || state->epoch != epoch)
        return {false, InputLeaseStatus::Rejected, lease, {}, {}, epoch,
            "stale input lease", InputArbiterErrorCode::WorksetEpochMismatch};
    if (!active_ || *active_ != lease)
        return {false, state->status, lease, {}, {}, epoch,
            "only the active lease can close"};

    InputPublicationToken neutral_publication;
    const bool parent_can_resume_without_mutation =
        !suspended_.empty() && !state->mutated_backend;
    if (!parent_can_resume_without_mutation &&
        !(state->request.movie_exclusive &&
          !backend_.IsAvailable(state->request.port)) &&
        !state->backend_neutral)
    {
        const PublishedState neutral = PublishBackend(*state, NeutralFrame());
        if (!neutral.ok)
        {
            return {false, state->status, lease, {}, {}, epoch,
                neutral.message, neutral.error};
        }
        neutral_publication = neutral.publication;
        state->backend_neutral = true;
        state->mutated_backend = true;
    }
    InputLeaseCloseReceipt result = FinishClose(*state);
    result.neutral_publication = neutral_publication;
    return result;
}

InputArbiterOperationReceipt InputArbiter::InitializeWorksetEpoch(
    WorksetEpoch epoch) noexcept
{
    if (!OnOwnerThread())
    {
        return {false, InputArbiterErrorCode::WrongThread,
            "InputArbiter mutation was attempted off its actor thread"};
    }
    if (IsStopped())
    {
        return {false, InputArbiterErrorCode::Stopped,
            "InputArbiter is shut down"};
    }
    if (!epoch)
    {
        return {false, InputArbiterErrorCode::InvalidArgument,
            "InputArbiter requires a nonzero WorksetEpoch"};
    }
    if (epoch_ && epoch_ != epoch)
    {
        return {false, InputArbiterErrorCode::WorksetEpochMismatch,
            "InputArbiter cannot change its owning WorksetEpoch"};
    }
    epoch_ = epoch;
    return {true, InputArbiterErrorCode::None, {}};
}

InputArbiterShutdownReceipt InputArbiter::Shutdown() noexcept
{
    if (!OnOwnerThread())
    {
        return {false, false, false, {}, epoch_,
            "InputArbiter shutdown was attempted off its actor thread",
            InputArbiterErrorCode::WrongThread};
    }
    if (shutdown_receipt_)
        return *shutdown_receipt_;

    InputArbiterShutdownReceipt receipt{
        true, true, false, {}, epoch_, {}, InputArbiterErrorCode::None};
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
        else if (!(state->request.movie_exclusive &&
                   !backend_.IsAvailable(state->request.port)) &&
                 !state->backend_neutral)
        {
            const PublishedState neutral = PublishBackend(*state, NeutralFrame());
            if (!neutral.ok)
            {
                receipt.ok = false;
                receipt.cleanup_complete = false;
                receipt.taint_required = true;
                receipt.message = neutral.message.empty()
                    ? "input neutralization failed during shutdown"
                    : neutral.message;
                receipt.error = neutral.error;
            }
            else
            {
                receipt.neutral_publication = neutral.publication;
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
    InputState state = InputState::Neutral;
    if (active_)
    {
        if (const LeaseState* active = FindLease(*active_))
        {
            movie = active->request.movie_exclusive;
            state = active->input_state;
        }
    }
    return {
        epoch_, active_, suspended_.size(), leases_.size(), bindings_.size(),
        relationships_.size(), state, movie, stopped_};
}

InputExecutionRelationshipOperationReceipt InputArbiter::Validate(
    InputExecutionRelationshipId relationship,
    WorksetEpoch epoch)
{
    return ValidateRelationship(relationship, epoch);
}

InputExecutionRelationshipOperationReceipt InputArbiter::ValidateRelationship(
    InputExecutionRelationshipId relationship,
    WorksetEpoch epoch) const
{
    if (!OnOwnerThread())
        return RelationshipFailure(
            "InputArbiter access was attempted off its actor thread");
    if (IsStopped())
        return RelationshipFailure("InputArbiter is shut down");
    const auto found = relationships_.find(relationship.value());
    if (found == relationships_.end())
        return RelationshipFailure("input execution relationship is unknown");
    if (!IsCurrent(epoch) || found->second.epoch != epoch)
        return RelationshipFailure("input execution relationship has a stale epoch");
    const LeaseState* lease = FindLease(found->second.lease);
    const BindingState* binding = FindBinding(found->second.binding);
    if (!lease || !binding || !active_ || *active_ != lease->id ||
        lease->status != InputLeaseStatus::Active ||
        lease->current_binding != binding->id ||
        lease->state_generation != found->second.state_generation ||
        binding->state_generation != found->second.state_generation)
    {
        return RelationshipFailure(
            "input execution relationship no longer names the current lease state");
    }
    return {true, {}};
}

InputExecutionRelationshipOperationReceipt InputArbiter::Complete(
    InputExecutionRelationshipId relationship,
    WorksetEpoch epoch) noexcept
{
    const auto validated = ValidateRelationship(relationship, epoch);
    if (!validated.ok)
        return validated;
    const auto found = relationships_.find(relationship.value());
    BindingState* binding = found == relationships_.end()
        ? nullptr
        : FindBinding(found->second.binding);
    LeaseState* lease = found == relationships_.end()
        ? nullptr
        : FindLease(found->second.lease);
    if (!binding || !lease)
        return RelationshipFailure("input execution binding is unavailable");

    if (binding->requires_observation && !binding->observed)
    {
        BackendInputPoll poll = backend_.QueryPoll(lease->request.port);
        if (!poll.result.ok)
        {
            relationships_.erase(relationship.value());
            return RelationshipFailure(
                poll.result.message.empty()
                    ? "input backend poll query failed"
                    : std::move(poll.result.message));
        }
        if (poll.publication_epoch != binding->backend_publication_epoch ||
            poll.callback_count == 0)
        {
            relationships_.erase(relationship.value());
            return RelationshipFailure(
                "guest has not observed the input execution binding");
        }
        binding->observed = true;
        binding->poll = InputPollReceiptId(next_poll_receipt_++);
        binding->callback_count = poll.callback_count;
    }
    if (binding->kind == InputBindingKind::NeutralTransition)
        lease->input_state = InputState::Neutral;
    relationships_.erase(relationship.value());
    return {true, {}};
}

InputExecutionRelationshipOperationReceipt InputArbiter::Cancel(
    InputExecutionRelationshipId relationship,
    WorksetEpoch epoch) noexcept
{
    if (!OnOwnerThread())
        return RelationshipFailure(
            "InputArbiter mutation was attempted off its actor thread");
    if (IsStopped())
        return RelationshipFailure("InputArbiter is shut down");
    const auto found = relationships_.find(relationship.value());
    if (found == relationships_.end())
        return {true, {}};
    if (!IsCurrent(epoch) || found->second.epoch != epoch)
        return RelationshipFailure("stale input execution relationship");
    relationships_.erase(found);
    return {true, {}};
}

InputExecutionRelationshipInspection InputArbiter::Inspect(
    InputExecutionRelationshipId relationship,
    WorksetEpoch epoch) const noexcept
{
    try
    {
        const auto validated = ValidateRelationship(relationship, epoch);
        if (!validated.ok)
            return {.message = validated.message};

        const auto found = relationships_.find(relationship.value());
        const BindingState* binding = found == relationships_.end()
            ? nullptr
            : FindBinding(found->second.binding);
        const LeaseState* lease = found == relationships_.end()
            ? nullptr
            : FindLease(found->second.lease);
        if (!binding || !lease)
            return {.message = "input execution binding is unavailable"};

        const BackendInputPoll poll = backend_.QueryPoll(lease->request.port);
        if (!poll.result.ok)
        {
            return {
                .message = poll.result.message.empty()
                    ? "input backend poll inspection failed"
                    : poll.result.message};
        }
        return {
            .ok = true,
            .requires_observation = binding->requires_observation,
            .exact_publication_observed =
                !binding->requires_observation ||
                (poll.publication_epoch ==
                     binding->backend_publication_epoch &&
                 poll.a_control_callback_count != 0),
            .publication_epoch = poll.publication_epoch,
            .callback_count = poll.callback_count,
            .a_control_callback_count = poll.a_control_callback_count,
            .frame = poll.frame};
    }
    catch (const std::exception& ex)
    {
        return {.message = ex.what()};
    }
    catch (...)
    {
        return {.message = "input backend poll inspection threw"};
    }
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

InputArbiter::BindingState* InputArbiter::FindBinding(
    InputExecutionBindingId binding) noexcept
{
    const auto found = bindings_.find(binding.value());
    return found == bindings_.end() ? nullptr : &found->second;
}

const InputArbiter::BindingState* InputArbiter::FindBinding(
    InputExecutionBindingId binding) const noexcept
{
    const auto found = bindings_.find(binding.value());
    return found == bindings_.end() ? nullptr : &found->second;
}

bool InputArbiter::IsCurrent(WorksetEpoch epoch) const noexcept
{
    return epoch_ && epoch_ == epoch;
}

bool InputArbiter::IsNeutral(const savor::GCInputFrame& frame) const noexcept
{
    return frame == NeutralFrame();
}

InputArbiter::PublishedState InputArbiter::PublishBackend(
    LeaseState& lease,
    const savor::GCInputFrame& frame)
{
    BackendInputPublication backend =
        backend_.Publish(lease.request.port, frame);
    if (!backend.result.ok || backend.publication_epoch == 0)
    {
        return {
            false,
            {},
            0,
            backend.result.message.empty()
                ? "input backend did not publish an epoch"
                : std::move(backend.result.message),
            InputArbiterErrorCode::BackendFailure};
    }
    return {
        true,
        InputPublicationToken(next_publication_++),
        backend.publication_epoch,
        {},
        InputArbiterErrorCode::None};
}

InputExecutionBindingReceipt InputArbiter::BindCurrentState(
    LeaseState& lease,
    const savor::GCInputFrame& frame,
    InputBindingKind kind,
    std::optional<PublishedState> publication,
    std::optional<InputDeliveryId> delivery)
{
    EraseBindingsForLease(lease.id);
    const InputExecutionBindingId id(next_binding_++);
    BindingState state{
        .id = id,
        .lease = lease.id,
        .epoch = lease.epoch,
        .state_generation = lease.state_generation,
        .frame = frame,
        .kind = kind,
        .publication = publication
            ? std::optional(publication->publication)
            : std::nullopt,
        .backend_publication_epoch = publication
            ? publication->backend_publication_epoch
            : 0,
        .requires_observation = publication.has_value(),
        .observed = !publication.has_value(),
        .delivery = delivery};
    bindings_.emplace(id.value(), std::move(state));
    lease.current_binding = id;
    return {
        .ok = true,
        .binding = id,
        .lease = lease.id,
        .epoch = lease.epoch,
        .state_generation = lease.state_generation,
        .frame = frame,
        .kind = kind,
        .requires_observation = publication.has_value(),
        .publication = publication
            ? publication->publication
            : InputPublicationToken{}};
}

InputLeaseCloseReceipt InputArbiter::FinishClose(LeaseState& lease)
{
    const InputLeaseId id = lease.id;
    const WorksetEpoch epoch = lease.epoch;
    const bool mutated_backend = lease.mutated_backend;
    active_.reset();
    suspended_.erase(
        std::remove(suspended_.begin(), suspended_.end(), id),
        suspended_.end());
    EraseLeaseState(id);

    std::optional<InputLeaseId> resumed;
    while (!suspended_.empty() && !resumed)
    {
        const InputLeaseId candidate = suspended_.back();
        suspended_.pop_back();
        if (LeaseState* parent = FindLease(candidate);
            parent && parent->status == InputLeaseStatus::Suspended &&
            parent->epoch == epoch_)
        {
            parent->status = InputLeaseStatus::Active;
            active_ = parent->id;
            resumed = parent->id;
            if (mutated_backend)
            {
                ++parent->state_generation;
                parent->input_state = InputState::Neutral;
                parent->backend_neutral = true;
                parent->mutated_backend = true;
                EraseBindingsForLease(parent->id);
                parent->current_binding.reset();
            }
        }
    }
    return {
        true,
        InputLeaseStatus::Released,
        id,
        {},
        resumed,
        epoch,
        resumed && mutated_backend
            ? "suspended lease resumed in neutral state"
            : std::string{}};
}

InputExecutionRelationshipOperationReceipt InputArbiter::RelationshipFailure(
    std::string message) const
{
    return {false, std::move(message)};
}

bool InputArbiter::OnOwnerThread() const noexcept
{
    return std::this_thread::get_id() == owner_thread_;
}

bool InputArbiter::IsStopped() const noexcept
{
    return stopped_;
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
    EraseRelationshipsForLease(lease);
}

void InputArbiter::EraseRelationshipsForLease(InputLeaseId lease) noexcept
{
    for (auto item = relationships_.begin(); item != relationships_.end();)
    {
        if (item->second.lease == lease)
            item = relationships_.erase(item);
        else
            ++item;
    }
}

void InputArbiter::EraseLeaseState(InputLeaseId lease) noexcept
{
    EraseBindingsForLease(lease);
    leases_.erase(lease.value());
}

void InputArbiter::ClearRetainedState() noexcept
{
    active_.reset();
    suspended_.clear();
    relationships_.clear();
    bindings_.clear();
    leases_.clear();
}

} // namespace savor::runtime
