#include "BattleResultsHandler.h"

#include <utility>

namespace savor::runtime::program::composition {
namespace {

void SetDiagnostic(std::string* diagnostic, std::string message)
{
    if (diagnostic)
        *diagnostic = std::move(message);
}

} // namespace

bool BattleResultsHandlerV1::Begin(
    const BattleResultsStopProvenanceV1& entry,
    std::uint32_t entry_rng,
    std::string* diagnostic)
{
    if (state_ != State::Inactive)
        return Fail("Battle Results handler was started more than once", diagnostic);
    if (entry.pc != kBattleResultsDescriptorReadyPc ||
        entry.workset_epoch == 0)
    {
        return Fail(
            "Battle Results handler did not start at DescriptorReady with a valid workset epoch",
            diagnostic);
    }
    entry_ = entry;
    entry_rng_ = entry_rng;
    state_ = State::AwaitingReady;
    transition_sequence_ = 1;
    if (diagnostic)
        diagnostic->clear();
    return true;
}

BattleResultsHandlerStepV1 BattleResultsHandlerV1::NextStep() const
{
    BattleResultsHandlerStepV1 step;
    step.sequence = transition_sequence_;
    switch (state_)
    {
    case State::AwaitingReady:
        step.wake_pcs[0] = kBattleResultsConfirmReadyPc;
        step.wake_pcs[1] = kBattleResultsLifecycleExitPc;
        step.wake_pc_count = 2;
        step.diagnostic_selector = "battle-results/final-confirm-ready";
        return step;
    case State::AwaitingAccepted:
        step.action = BattleResultsHandlerAction::PressA;
        step.wake_pcs[0] = kBattleResultsConfirmAcceptedPc;
        step.wake_pc_count = 1;
        step.diagnostic_selector = "battle-results/confirm";
        return step;
    case State::AwaitingNeutralObserved:
        step.action = BattleResultsHandlerAction::ReleaseA;
        step.wake_pcs[0] = kBattleResultsGuestPadReadReturnedPc;
        step.wake_pc_count = 1;
        step.diagnostic_selector = "battle-results/neutral-observed";
        return step;
    case State::AwaitingLifecycleExit:
        step.wake_pcs[0] = kBattleResultsLifecycleExitPc;
        step.wake_pc_count = 1;
        step.diagnostic_selector = "battle-results/lifecycle-exit";
        return step;
    case State::AwaitingCleanup:
        step.wake_pcs[0] = kBattleResultsCleanupCompletePc;
        step.wake_pc_count = 1;
        step.diagnostic_selector = "battle-results/cleanup";
        return step;
    case State::Complete:
        step.action = BattleResultsHandlerAction::Complete;
        step.diagnostic_selector = "battle-results/complete";
        return step;
    case State::Inactive:
    case State::Failed:
        step.action = BattleResultsHandlerAction::Complete;
        step.diagnostic_selector = "battle-results/invalid";
        return step;
    }
    return step;
}

bool BattleResultsHandlerV1::Observe(
    const BattleResultsStopProvenanceV1& stop,
    std::string* diagnostic)
{
    if (stop.workset_epoch == 0 || stop.workset_epoch != entry_.workset_epoch)
        return Fail("Battle Results stop has a mismatched workset epoch", diagnostic);
    if (++transition_sequence_ > 16)
        return Fail("Battle Results handler exceeded its transition budget", diagnostic);

    switch (state_)
    {
    case State::AwaitingReady:
        if (stop.pc == kBattleResultsLifecycleExitPc)
            return Fail("Battle Results exited before final confirmation", diagnostic);
        if (stop.pc != kBattleResultsConfirmReadyPc)
            return Fail("Battle Results handler reached an unknown ready point", diagnostic);
        state_ = State::AwaitingAccepted;
        break;
    case State::AwaitingAccepted:
        if (stop.pc != kBattleResultsConfirmAcceptedPc)
            return Fail("Battle Results handler reached the wrong accepted point", diagnostic);
        state_ = State::AwaitingNeutralObserved;
        break;
    case State::AwaitingNeutralObserved:
        if (stop.pc != kBattleResultsGuestPadReadReturnedPc)
            return Fail("Battle Results handler did not observe guest neutral input", diagnostic);
        state_ = State::AwaitingLifecycleExit;
        break;
    case State::AwaitingLifecycleExit:
        if (stop.pc != kBattleResultsLifecycleExitPc)
            return Fail("Battle Results handler expected lifecycle exit", diagnostic);
        state_ = State::AwaitingCleanup;
        break;
    case State::AwaitingCleanup:
        if (stop.pc != kBattleResultsCleanupCompletePc)
            return Fail("Battle Results handler expected cleanup completion", diagnostic);
        terminal_ = stop;
        state_ = State::Complete;
        break;
    case State::Inactive:
    case State::Complete:
    case State::Failed:
        return Fail("Battle Results handler received a stop outside an active transition", diagnostic);
    }
    if (diagnostic)
        diagnostic->clear();
    return true;
}

bool BattleResultsHandlerV1::Finalize(
    std::uint32_t exit_rng,
    std::uint32_t completion_flag,
    std::uint32_t result_pointer,
    std::uint32_t game_mode,
    BattleResultsHandlerReceiptV1& receipt,
    std::string* diagnostic) const
{
    if (state_ != State::Complete ||
        terminal_.pc != kBattleResultsCleanupCompletePc)
    {
        SetDiagnostic(diagnostic,
            "Battle Results handler cannot finalize before cleanup completion");
        return false;
    }
    if (entry_rng_ != exit_rng || completion_flag != 1u ||
        result_pointer != 0u || game_mode != 6u)
    {
        SetDiagnostic(diagnostic,
            "Battle Results terminal invariants did not hold");
        return false;
    }
    receipt = {
        .entry = entry_,
        .terminal = terminal_,
        .entry_rng = entry_rng_,
        .exit_rng = exit_rng,
    };
    if (diagnostic)
        diagnostic->clear();
    return true;
}

bool BattleResultsHandlerV1::active() const noexcept
{
    return state_ != State::Inactive && state_ != State::Complete &&
        state_ != State::Failed;
}

bool BattleResultsHandlerV1::complete() const noexcept
{
    return state_ == State::Complete;
}

bool BattleResultsHandlerV1::Fail(
    std::string message,
    std::string* diagnostic)
{
    state_ = State::Failed;
    SetDiagnostic(diagnostic, std::move(message));
    return false;
}

} // namespace savor::runtime::program::composition
