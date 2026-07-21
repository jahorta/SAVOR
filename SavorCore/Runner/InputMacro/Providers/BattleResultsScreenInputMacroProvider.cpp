#include "BattleResultsScreenInputMacroProvider.h"

#include <array>
#include <utility>

#include "../../Breakpoints/BpRegistry.h"

namespace savor::inputmacro {
namespace {

constexpr std::array<BPKey, 26> kRequiredBreakpointKeys{
    bp::battle::BattleEndResultDescriptorReady,
    bp::battle::BattleEndResultDispatch,
    bp::battle::BattleEndResultIntroReady,
    bp::battle::BattleEndResultIntroAccepted,
    bp::battle::BattleEndResultGoldReady,
    bp::battle::BattleEndResultGoldArmed,
    bp::battle::BattleEndResultGoldAccepted,
    bp::battle::BattleEndResultNormalExpReady,
    bp::battle::BattleEndResultNormalExpAccepted,
    bp::battle::BattleEndResultStatWaveReady,
    bp::battle::BattleEndResultStatWaveAccepted,
    bp::battle::BattleEndResultMagicEntryReady,
    bp::battle::BattleEndResultMagicEntryAccepted,
    bp::battle::BattleEndResultMagicExpReady,
    bp::battle::BattleEndResultMagicExpAccepted,
    bp::battle::BattleEndResultLearnedWaveReady,
    bp::battle::BattleEndResultLearnedWaveAccepted,
    bp::battle::BattleEndResultItemPopupReady,
    bp::battle::BattleEndResultItemPopupAccepted,
    bp::battle::BattleEndResultConfirmReady,
    bp::battle::BattleEndResultConfirmAccepted,
    bp::battle::BattleEndResultFadeReady,
    bp::battle::BattleEndResultFadeAccepted,
    bp::battle::BattleEndResultLifecycleExit,
    bp::battle::BattleEndResultCleanupComplete,
    bp::battle::BattleEndController0NeutralCopied,
};

} // namespace

BattleResultsScreenInputMacroProvider::BattleResultsScreenInputMacroProvider(Request request)
    : implementation_(BattleEndResultsInputMacroProvider::Request{
        .acceleration_policy = request.acceleration_policy,
        .start_at_results_screen = true,
        .completion_manifest_blob = std::move(request.completion_manifest_blob),
    }) {}
BattleResultsScreenInputMacroProvider::~BattleResultsScreenInputMacroProvider() = default;
std::span<const BPKey> BattleResultsScreenInputMacroProvider::required_breakpoint_keys() noexcept
{ return kRequiredBreakpointKeys; }
std::span<const BPKey> BattleResultsScreenInputMacroProvider::declared_breakpoint_keys() const
{ return required_breakpoint_keys(); }
InputMacroDriverDecision BattleResultsScreenInputMacroProvider::Start(IInputMacroDriverHost& host)
{ return implementation_.Start(host); }
InputMacroDriverDecision BattleResultsScreenInputMacroProvider::Advance(
    IInputMacroDriverHost& host, const InputMacroStepResult& result)
{ return implementation_.Advance(host, result); }
void BattleResultsScreenInputMacroProvider::Cancel() noexcept { implementation_.Cancel(); }
phase::battle::endresults::FailureCode
BattleResultsScreenInputMacroProvider::failure() const noexcept
{ return implementation_.failure(); }
const std::string& BattleResultsScreenInputMacroProvider::diagnostic() const noexcept
{ return implementation_.diagnostic(); }
const phase::battle::endresults::Report&
BattleResultsScreenInputMacroProvider::report() const noexcept
{ return implementation_.report(); }
const std::string& BattleResultsScreenInputMacroProvider::report_blob() const noexcept
{ return implementation_.report_blob(); }
bool BattleResultsScreenInputMacroProvider::completed() const noexcept
{ return implementation_.completed(); }
BattleResultsScreenInputMacroProvider::DiagnosticsSnapshot
BattleResultsScreenInputMacroProvider::diagnostics() const noexcept
{ return implementation_.diagnostics(); }

} // namespace savor::inputmacro
