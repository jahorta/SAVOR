#include "WorkerCompletionLedger.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace savor::runtime {
namespace {

[[nodiscard]] bool SameItem(
    const WorkerItemTerminalCorrelation& lhs,
    const WorkerItemTerminalCorrelation& rhs) noexcept
{
    return lhs.workset_id == rhs.workset_id &&
        lhs.item_id == rhs.item_id &&
        lhs.item_ordinal == rhs.item_ordinal &&
        lhs.invocation_id == rhs.invocation_id &&
        lhs.attempt_id == rhs.attempt_id;
}

} // namespace

WorkerCompletionLedger::WorkerCompletionLedger(
    const WorkerWorksetLimits& limits)
    : maximum_terminals_(limits.maximum_retained_terminals),
      maximum_bytes_(limits.maximum_retained_terminal_bytes)
{
}

CompletionLedgerResult WorkerCompletionLedger::BindActorThread()
{
    const std::thread::id current = std::this_thread::get_id();
    if (actor_thread_ == std::thread::id{})
    {
        actor_thread_ = current;
        return CompletionLedgerResult::Success();
    }
    if (actor_thread_ == current)
        return CompletionLedgerResult::Success();
    return CompletionLedgerResult::Failure(
        CompletionLedgerErrorCode::WrongThread,
        "Worker completion ledger is already bound to another actor thread");
}

WorkerTerminalReservation WorkerCompletionLedger::ReserveTerminal(
    WorkerItemTerminalCorrelation correlation,
    std::size_t maximum_encoded_bytes)
{
    if (const CompletionLedgerResult actor = RequireActorThread(); !actor.ok)
        return {actor, {}};
    if (!correlation.workset_id || !correlation.item_id ||
        !correlation.invocation_id || !correlation.attempt_id ||
        correlation.terminal_id || correlation.terminal_order ||
        maximum_encoded_bytes == 0)
    {
        return {
            CompletionLedgerResult::Failure(
                CompletionLedgerErrorCode::InvalidArgument,
                "Terminal reservation requires complete item identity, "
                "unassigned terminal identity, and a nonzero byte bound"),
            {}};
    }
    if (HasDuplicateItem(correlation))
    {
        return {
            CompletionLedgerResult::Failure(
                CompletionLedgerErrorCode::DuplicateTerminal,
                "The workset item already has a retained terminal"),
            {}};
    }
    if (maximum_terminals_ == 0 ||
        entries_.size() >= maximum_terminals_ ||
        maximum_encoded_bytes > maximum_bytes_ ||
        retained_bytes_ > maximum_bytes_ - maximum_encoded_bytes)
    {
        return {
            CompletionLedgerResult::Failure(
                CompletionLedgerErrorCode::CapacityExceeded,
                "Terminal retention exceeds the configured completion-ledger "
                "count or byte bound"),
            {}};
    }
    if (next_terminal_id_ == 0 || next_terminal_order_ == 0)
    {
        return {
            CompletionLedgerResult::Failure(
                CompletionLedgerErrorCode::SequenceExhausted,
                "Terminal identity sequence is exhausted"),
            {}};
    }

    correlation.terminal_id = WorkerTerminalId(next_terminal_id_++);
    correlation.terminal_order =
        WorkerTerminalOrder(next_terminal_order_++);

    Entry entry;
    entry.correlation = correlation;
    entry.retained_bytes = maximum_encoded_bytes;
    entries_.push_back(std::move(entry));
    retained_bytes_ += maximum_encoded_bytes;
    return {
        CompletionLedgerResult::Success(),
        std::move(correlation)};
}

CompletionLedgerResult WorkerCompletionLedger::CompleteTerminal(
    const WorkerItemTerminalCorrelation& correlation,
    std::vector<std::uint8_t> payload)
{
    if (const CompletionLedgerResult actor = RequireActorThread(); !actor.ok)
        return actor;
    if (!CompleteCorrelation(correlation))
    {
        return CompletionLedgerResult::Failure(
            CompletionLedgerErrorCode::InvalidArgument,
            "Terminal completion requires exact correlation");
    }
    Entry* const entry = Find(correlation.terminal_id);
    if (!entry)
    {
        return CompletionLedgerResult::Failure(
            CompletionLedgerErrorCode::TerminalNotFound,
            "Terminal reservation was not found");
    }
    if (entry->correlation != correlation)
    {
        return CompletionLedgerResult::Failure(
            CompletionLedgerErrorCode::TerminalMismatch,
            "Terminal completion does not match the exact reservation");
    }
    if (entry->state != RetainedTerminalState::PendingFinalization)
    {
        return CompletionLedgerResult::Failure(
            CompletionLedgerErrorCode::InvalidState,
            "Terminal is no longer pending finalization");
    }

    const std::size_t actual_bytes = payload.size();
    if (actual_bytes > entry->retained_bytes)
    {
        const std::size_t growth = actual_bytes - entry->retained_bytes;
        if (growth > maximum_bytes_ ||
            retained_bytes_ > maximum_bytes_ - growth)
        {
            return CompletionLedgerResult::Failure(
                CompletionLedgerErrorCode::CapacityExceeded,
                "Final terminal encoding exceeds its reservation and the "
                "completion-ledger byte bound");
        }
        retained_bytes_ += growth;
    }
    else
    {
        retained_bytes_ -= entry->retained_bytes - actual_bytes;
    }
    entry->retained_bytes = actual_bytes;
    entry->payload =
        std::make_shared<const std::vector<std::uint8_t>>(
            std::move(payload));
    entry->state = RetainedTerminalState::Ready;
    return CompletionLedgerResult::Success();
}

WorkerOutboundSequenceReceipt
WorkerCompletionLedger::ReserveOutboundSequence()
{
    if (const CompletionLedgerResult actor = RequireActorThread(); !actor.ok)
        return {actor, {}};
    return ReserveOutboundSequenceUnchecked();
}

CompletionLedgerResult
WorkerCompletionLedger::ConfirmOutboundSequence(
    WorkerOutboundSequence sequence)
{
    if (const CompletionLedgerResult actor = RequireActorThread(); !actor.ok)
        return actor;
    if (!sequence || sequence != open_outbound_sequence_)
    {
        return CompletionLedgerResult::Failure(
            CompletionLedgerErrorCode::TerminalMismatch,
            "Outbound confirmation does not match the active publication");
    }
    open_outbound_sequence_ = {};
    return CompletionLedgerResult::Success();
}

CompletionLedgerResult
WorkerCompletionLedger::AbandonOutboundSequence(
    WorkerOutboundSequence sequence)
{
    if (const CompletionLedgerResult actor = RequireActorThread(); !actor.ok)
        return actor;
    if (!sequence || sequence != open_outbound_sequence_)
    {
        return CompletionLedgerResult::Failure(
            CompletionLedgerErrorCode::TerminalMismatch,
            "Outbound abandonment does not match the active publication");
    }
    open_outbound_sequence_ = {};
    return CompletionLedgerResult::Success();
}

WorkerTerminalPublicationReceipt
WorkerCompletionLedger::BeginNextPublication()
{
    if (const CompletionLedgerResult actor = RequireActorThread(); !actor.ok)
        return {actor, std::nullopt};

    for (Entry& entry : entries_)
    {
        if (entry.state ==
            RetainedTerminalState::AwaitingAcknowledgement)
        {
            continue;
        }
        if (entry.state == RetainedTerminalState::PendingFinalization)
            return {CompletionLedgerResult::Success(), std::nullopt};
        if (entry.state == RetainedTerminalState::Publishing)
        {
            return {
                CompletionLedgerResult::Failure(
                    CompletionLedgerErrorCode::InvalidState,
                    "A terminal publication is already in progress"),
                std::nullopt};
        }
        if (entry.state != RetainedTerminalState::Ready || !entry.payload)
        {
            return {
                CompletionLedgerResult::Failure(
                    CompletionLedgerErrorCode::InvalidState,
                    "Ready terminal has no immutable payload"),
                std::nullopt};
        }

        if (!entry.outbound_sequence)
        {
            const WorkerOutboundSequenceReceipt sequence =
                ReserveOutboundSequenceUnchecked();
            if (!sequence.result.ok)
                return {
                    sequence.result,
                    std::nullopt};
            entry.outbound_sequence = sequence.sequence;
        }
        else if (open_outbound_sequence_ != entry.outbound_sequence)
        {
            return {
                CompletionLedgerResult::Failure(
                    CompletionLedgerErrorCode::InvalidState,
                    "Terminal retry lost its exclusive outbound reservation"),
                std::nullopt};
        }
        entry.state = RetainedTerminalState::Publishing;
        return {
            CompletionLedgerResult::Success(),
            WorkerTerminalPublication{
                entry.correlation,
                entry.outbound_sequence,
                entry.payload}};
    }
    return {CompletionLedgerResult::Success(), std::nullopt};
}

CompletionLedgerResult WorkerCompletionLedger::ConfirmPublished(
    const WorkerItemTerminalCorrelation& correlation)
{
    if (const CompletionLedgerResult actor = RequireActorThread(); !actor.ok)
        return actor;
    Entry* const entry = Find(correlation.terminal_id);
    if (!entry)
    {
        return CompletionLedgerResult::Failure(
            CompletionLedgerErrorCode::TerminalNotFound,
            "Published terminal was not found");
    }
    if (entry->correlation != correlation)
    {
        return CompletionLedgerResult::Failure(
            CompletionLedgerErrorCode::TerminalMismatch,
            "Publication confirmation does not match the exact terminal");
    }
    if (entry->state != RetainedTerminalState::Publishing)
    {
        return CompletionLedgerResult::Failure(
            CompletionLedgerErrorCode::InvalidState,
            "Terminal is not being published");
    }
    const CompletionLedgerResult outbound =
        ConfirmOutboundSequence(entry->outbound_sequence);
    if (!outbound.ok)
        return outbound;
    entry->state = RetainedTerminalState::AwaitingAcknowledgement;
    return CompletionLedgerResult::Success();
}

CompletionLedgerResult WorkerCompletionLedger::AbortPublication(
    const WorkerItemTerminalCorrelation& correlation)
{
    if (const CompletionLedgerResult actor = RequireActorThread(); !actor.ok)
        return actor;
    Entry* const entry = Find(correlation.terminal_id);
    if (!entry)
    {
        return CompletionLedgerResult::Failure(
            CompletionLedgerErrorCode::TerminalNotFound,
            "Terminal publication was not found");
    }
    if (entry->correlation != correlation)
    {
        return CompletionLedgerResult::Failure(
            CompletionLedgerErrorCode::TerminalMismatch,
            "Publication abort does not match the exact terminal");
    }
    if (entry->state != RetainedTerminalState::Publishing)
    {
        return CompletionLedgerResult::Failure(
            CompletionLedgerErrorCode::InvalidState,
            "Terminal is not being published");
    }
    entry->state = RetainedTerminalState::Ready;
    return CompletionLedgerResult::Success();
}

CompletionLedgerResult WorkerCompletionLedger::AcknowledgeTerminal(
    const WorkerItemTerminalCorrelation& correlation)
{
    if (const CompletionLedgerResult actor = RequireActorThread(); !actor.ok)
        return actor;
    if (!CompleteCorrelation(correlation))
    {
        return CompletionLedgerResult::Failure(
            CompletionLedgerErrorCode::InvalidArgument,
            "Terminal acknowledgement requires complete exact correlation");
    }

    const auto found = std::find_if(
        entries_.begin(),
        entries_.end(),
        [&](const Entry& entry) {
            return entry.correlation.terminal_id ==
                correlation.terminal_id;
        });
    if (found == entries_.end())
    {
        const auto tombstone = std::find_if(
            acknowledged_tombstones_.begin(),
            acknowledged_tombstones_.end(),
            [&](const WorkerItemTerminalCorrelation& prior)
            {
                return prior.terminal_id ==
                    correlation.terminal_id;
            });
        if (tombstone != acknowledged_tombstones_.end())
        {
            return *tombstone == correlation
                ? CompletionLedgerResult::Success()
                : CompletionLedgerResult::Failure(
                      CompletionLedgerErrorCode::TerminalMismatch,
                      "Acknowledgement reuses a terminal identity with different correlation");
        }
        return CompletionLedgerResult::Failure(
            CompletionLedgerErrorCode::TerminalNotFound,
            "Acknowledged terminal was not retained");
    }
    if (found->correlation != correlation)
    {
        return CompletionLedgerResult::Failure(
            CompletionLedgerErrorCode::TerminalMismatch,
            "Acknowledgement does not match the exact retained terminal");
    }
    if (found->state !=
        RetainedTerminalState::AwaitingAcknowledgement)
    {
        return CompletionLedgerResult::Failure(
            CompletionLedgerErrorCode::InvalidState,
            "Terminal cannot be acknowledged before publication");
    }

    retained_bytes_ -= found->retained_bytes;
    acknowledged_tombstones_.push_back(found->correlation);
    const std::size_t maximum_tombstones =
        std::max<std::size_t>(1, maximum_terminals_);
    while (acknowledged_tombstones_.size() >
           maximum_tombstones)
    {
        acknowledged_tombstones_.pop_front();
    }
    entries_.erase(found);
    return CompletionLedgerResult::Success();
}

WorkerCompletionLedgerSnapshot
WorkerCompletionLedger::snapshot() const noexcept
{
    WorkerCompletionLedgerSnapshot result;
    result.on_actor_thread = OnActorThread();
    if (!result.on_actor_thread)
        return result;
    result.retained_terminals = entries_.size();
    result.retained_bytes = retained_bytes_;
    result.next_terminal_order =
        WorkerTerminalOrder(next_terminal_order_);
    result.next_outbound_sequence =
        WorkerOutboundSequence(next_outbound_sequence_);
    result.open_outbound_sequence =
        open_outbound_sequence_;
    for (const Entry& entry : entries_)
    {
        switch (entry.state)
        {
        case RetainedTerminalState::PendingFinalization:
            ++result.pending_finalization;
            break;
        case RetainedTerminalState::Ready:
            ++result.ready;
            break;
        case RetainedTerminalState::Publishing:
            ++result.publishing;
            break;
        case RetainedTerminalState::AwaitingAcknowledgement:
            ++result.awaiting_acknowledgement;
            break;
        }
    }
    return result;
}

bool WorkerCompletionLedger::OnActorThread() const noexcept
{
    return actor_thread_ != std::thread::id{} &&
        std::this_thread::get_id() == actor_thread_;
}

CompletionLedgerResult
WorkerCompletionLedger::RequireActorThread() const
{
    if (OnActorThread())
        return CompletionLedgerResult::Success();
    return CompletionLedgerResult::Failure(
        CompletionLedgerErrorCode::WrongThread,
        "Worker completion ledger is actor-thread-owned");
}

WorkerCompletionLedger::Entry* WorkerCompletionLedger::Find(
    WorkerTerminalId terminal_id) noexcept
{
    const auto found = std::find_if(
        entries_.begin(),
        entries_.end(),
        [&](const Entry& entry) {
            return entry.correlation.terminal_id == terminal_id;
        });
    return found == entries_.end() ? nullptr : &*found;
}

const WorkerCompletionLedger::Entry* WorkerCompletionLedger::Find(
    WorkerTerminalId terminal_id) const noexcept
{
    const auto found = std::find_if(
        entries_.begin(),
        entries_.end(),
        [&](const Entry& entry) {
            return entry.correlation.terminal_id == terminal_id;
        });
    return found == entries_.end() ? nullptr : &*found;
}

bool WorkerCompletionLedger::HasDuplicateItem(
    const WorkerItemTerminalCorrelation& correlation) const noexcept
{
    return std::any_of(
        entries_.begin(),
        entries_.end(),
        [&](const Entry& entry) {
            return SameItem(entry.correlation, correlation);
        });
}

bool WorkerCompletionLedger::CompleteCorrelation(
    const WorkerItemTerminalCorrelation& correlation) noexcept
{
    return correlation.workset_id && correlation.item_id &&
        correlation.invocation_id && correlation.attempt_id &&
        correlation.terminal_id && correlation.terminal_order;
}

WorkerOutboundSequenceReceipt
WorkerCompletionLedger::ReserveOutboundSequenceUnchecked()
{
    if (open_outbound_sequence_)
    {
        return {
            CompletionLedgerResult::Failure(
                CompletionLedgerErrorCode::InvalidState,
                "A prior outbound publication remains unresolved"),
            {}};
    }
    if (next_outbound_sequence_ == 0)
    {
        return {
            CompletionLedgerResult::Failure(
                CompletionLedgerErrorCode::SequenceExhausted,
                "Outbound event sequence is exhausted"),
            {}};
    }
    open_outbound_sequence_ =
        WorkerOutboundSequence(next_outbound_sequence_++);
    return {
        CompletionLedgerResult::Success(),
        open_outbound_sequence_};
}

} // namespace savor::runtime
