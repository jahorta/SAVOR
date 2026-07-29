#pragma once

#include "WorksetTypes.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace savor::runtime {

enum class CompletionLedgerErrorCode : std::uint16_t
{
    None,
    WrongThread,
    InvalidArgument,
    CapacityExceeded,
    DuplicateTerminal,
    TerminalNotFound,
    TerminalMismatch,
    InvalidState,
    SequenceExhausted,
};

struct CompletionLedgerResult
{
    bool ok = false;
    CompletionLedgerErrorCode code =
        CompletionLedgerErrorCode::InvalidState;
    std::string message;

    [[nodiscard]] static CompletionLedgerResult Success()
    {
        return {true, CompletionLedgerErrorCode::None, {}};
    }

    [[nodiscard]] static CompletionLedgerResult Failure(
        CompletionLedgerErrorCode code,
        std::string message)
    {
        return {false, code, std::move(message)};
    }
};

enum class RetainedTerminalState : std::uint8_t
{
    PendingFinalization,
    Ready,
    Publishing,
    AwaitingAcknowledgement,
};

struct WorkerTerminalReservation
{
    CompletionLedgerResult result;
    WorkerItemTerminalCorrelation correlation;
};

struct WorkerTerminalPublication
{
    WorkerItemTerminalCorrelation correlation;
    WorkerOutboundSequence outbound_sequence;
    std::shared_ptr<const std::vector<std::uint8_t>> payload;
};

struct WorkerTerminalPublicationReceipt
{
    CompletionLedgerResult result;
    std::optional<WorkerTerminalPublication> publication;
};

struct WorkerOutboundSequenceReceipt
{
    CompletionLedgerResult result;
    WorkerOutboundSequence sequence;
};

struct WorkerCompletionLedgerSnapshot
{
    bool on_actor_thread = false;
    std::size_t retained_terminals = 0;
    std::size_t retained_bytes = 0;
    std::size_t pending_finalization = 0;
    std::size_t ready = 0;
    std::size_t publishing = 0;
    std::size_t awaiting_acknowledgement = 0;
    WorkerTerminalOrder next_terminal_order;
    WorkerOutboundSequence next_outbound_sequence;
    WorkerOutboundSequence open_outbound_sequence;
};

// Actor-owned retention for authoritative item terminals. Terminal order is
// assigned before host-only finalization begins. A later ready terminal cannot
// pass an earlier pending terminal, while already-published terminals do not
// prevent the next ordered terminal from being published.
class WorkerCompletionLedger final
{
public:
    explicit WorkerCompletionLedger(
        const WorkerWorksetLimits& limits = {});

    WorkerCompletionLedger(const WorkerCompletionLedger&) = delete;
    WorkerCompletionLedger& operator=(const WorkerCompletionLedger&) = delete;

    // WorkerRuntime constructs most members before ActorMain starts. Affinity
    // is therefore adopted explicitly once, on the actor, rather than guessed
    // from the construction thread.
    [[nodiscard]] CompletionLedgerResult BindActorThread();

    [[nodiscard]] WorkerTerminalReservation ReserveTerminal(
        WorkerItemTerminalCorrelation correlation,
        std::size_t maximum_encoded_bytes);

    [[nodiscard]] CompletionLedgerResult CompleteTerminal(
        const WorkerItemTerminalCorrelation& correlation,
        std::vector<std::uint8_t> payload);

    // WorkerRuntime uses the same actor-owned source for item-start, progress,
    // terminal, and workset events, preserving one outbound sequence across
    // worksets. Terminal publication reserves through this source internally.
    [[nodiscard]] WorkerOutboundSequenceReceipt ReserveOutboundSequence();
    [[nodiscard]] CompletionLedgerResult ConfirmOutboundSequence(
        WorkerOutboundSequence sequence);
    [[nodiscard]] CompletionLedgerResult AbandonOutboundSequence(
        WorkerOutboundSequence sequence);

    // Returns success with no publication when the ledger is empty or the
    // earliest unpublished terminal is still finalizing.
    [[nodiscard]] WorkerTerminalPublicationReceipt BeginNextPublication();

    [[nodiscard]] CompletionLedgerResult ConfirmPublished(
        const WorkerItemTerminalCorrelation& correlation);

    // A transport failure may return the exact terminal to Ready. Its assigned
    // outbound sequence is retained so a retry cannot create a second logical
    // event.
    [[nodiscard]] CompletionLedgerResult AbortPublication(
        const WorkerItemTerminalCorrelation& correlation);

    // Acknowledgement is accepted only for the complete exact correlation of a
    // terminal which has already been published.
    [[nodiscard]] CompletionLedgerResult AcknowledgeTerminal(
        const WorkerItemTerminalCorrelation& correlation);

    [[nodiscard]] WorkerCompletionLedgerSnapshot snapshot() const noexcept;

private:
    struct Entry
    {
        WorkerItemTerminalCorrelation correlation;
        RetainedTerminalState state =
            RetainedTerminalState::PendingFinalization;
        std::size_t retained_bytes = 0;
        WorkerOutboundSequence outbound_sequence;
        std::shared_ptr<const std::vector<std::uint8_t>> payload;
    };

    [[nodiscard]] bool OnActorThread() const noexcept;
    [[nodiscard]] CompletionLedgerResult RequireActorThread() const;
    [[nodiscard]] Entry* Find(WorkerTerminalId terminal_id) noexcept;
    [[nodiscard]] const Entry* Find(
        WorkerTerminalId terminal_id) const noexcept;
    [[nodiscard]] bool HasDuplicateItem(
        const WorkerItemTerminalCorrelation& correlation) const noexcept;
    [[nodiscard]] static bool CompleteCorrelation(
        const WorkerItemTerminalCorrelation& correlation) noexcept;
    [[nodiscard]] WorkerOutboundSequenceReceipt
        ReserveOutboundSequenceUnchecked();

    std::thread::id actor_thread_;
    std::size_t maximum_terminals_ = 0;
    std::size_t maximum_bytes_ = 0;
    std::size_t retained_bytes_ = 0;
    std::uint64_t next_terminal_id_ = 1;
    std::uint64_t next_terminal_order_ = 1;
    std::uint64_t next_outbound_sequence_ = 1;
    WorkerOutboundSequence open_outbound_sequence_;
    std::deque<Entry> entries_;
    std::deque<WorkerItemTerminalCorrelation>
        acknowledged_tombstones_;
};

} // namespace savor::runtime
