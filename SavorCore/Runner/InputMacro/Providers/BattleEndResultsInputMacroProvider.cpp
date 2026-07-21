#include "BattleEndResultsInputMacroProvider.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include "../../Breakpoints/BpRegistry.h"
#include "../../../Phases/Programs/BattleCompletion/BattleCompletionManifest.h"

namespace savor::inputmacro {
namespace {

namespace endresults = phase::battle::endresults;

constexpr std::uint32_t kMem1Begin = 0x80000000u;
constexpr std::uint32_t kMem1End = 0x81800000u;
constexpr std::uint32_t kResultObjectReadSize = 0x1600u;
constexpr std::uint32_t kEndBattleVictoryPc = 0x800706D8u;
constexpr std::uint32_t kControllerNewPressesOffset = 0x08u;
constexpr std::uint32_t kMaxRows = 4u;
constexpr std::uint32_t kMaxStatWaves = 3u;
constexpr std::uint32_t kMaxLearnedWaves = 18u;

constexpr std::array<BPKey, 29> kRequiredBreakpointKeys{
    bp::battle::BattleEndVictoryCountdownComplete,
    bp::battle::BattleEndVictorySlotsComplete,
    bp::battle::BattleEndResultDispatch,
    bp::battle::BattleEndResultIntroReady,
    bp::battle::BattleEndResultIntroAccepted,
    bp::battle::BattleEndResultGoldReady,
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
    bp::battle::BattleEndRewardEntry,
    bp::battle::BattleEndRewardCommitComplete,
    bp::battle::BattleEndController0NeutralCopied,
    bp::battle::BattleEndResultGoldArmed,
};

struct ActionDefinition {
    endresults::ActionKind kind;
    BPKey ready_key;
    std::uint32_t ready_pc;
    BPKey accepted_key;
    std::uint32_t accepted_pc;
    std::uint32_t max_occurrences;
};

constexpr std::array<ActionDefinition, 10> kActions{{
    {endresults::ActionKind::Intro, bp::battle::BattleEndResultIntroReady, 0x800E46BCu,
        bp::battle::BattleEndResultIntroAccepted, 0x800E46CCu, 1},
    {endresults::ActionKind::Gold, bp::battle::BattleEndResultGoldArmed, 0x800E488Cu,
        bp::battle::BattleEndResultGoldAccepted, 0x800E48A8u, 1},
    {endresults::ActionKind::NormalExp, bp::battle::BattleEndResultNormalExpReady, 0x800E4D40u,
        bp::battle::BattleEndResultNormalExpAccepted, 0x800E4D50u, 1},
    {endresults::ActionKind::StatWave, bp::battle::BattleEndResultStatWaveReady, 0x800E4F2Cu,
        bp::battle::BattleEndResultStatWaveAccepted, 0x800E4F3Cu, kMaxStatWaves},
    {endresults::ActionKind::MagicEntry, bp::battle::BattleEndResultMagicEntryReady, 0x800E52D8u,
        bp::battle::BattleEndResultMagicEntryAccepted, 0x800E52E8u, 1},
    {endresults::ActionKind::MagicExp, bp::battle::BattleEndResultMagicExpReady, 0x800E5460u,
        bp::battle::BattleEndResultMagicExpAccepted, 0x800E5470u, 1},
    {endresults::ActionKind::LearnedMagicWave, bp::battle::BattleEndResultLearnedWaveReady, 0x800E5C3Cu,
        bp::battle::BattleEndResultLearnedWaveAccepted, 0x800E5C4Cu, kMaxLearnedWaves},
    {endresults::ActionKind::ItemPopup, bp::battle::BattleEndResultItemPopupReady, 0x800E5F80u,
        bp::battle::BattleEndResultItemPopupAccepted, 0x800E5F90u, 1},
    {endresults::ActionKind::MandatoryConfirm, bp::battle::BattleEndResultConfirmReady, 0x800E6128u,
        bp::battle::BattleEndResultConfirmAccepted, 0x800E6138u, 1},
    {endresults::ActionKind::Fade, bp::battle::BattleEndResultFadeReady, 0x800E6470u,
        bp::battle::BattleEndResultFadeAccepted, 0x800E6480u, 1},
}};

const ActionDefinition& Definition(endresults::ActionKind kind)
{
    return kActions.at(static_cast<std::size_t>(kind));
}

bool IsNeutral(const GCInputFrame& frame) noexcept
{
    return frame.buttons == 0
        && frame.main_x == 128 && frame.main_y == 128
        && frame.c_x == 128 && frame.c_y == 128
        && frame.trig_l == 0 && frame.trig_r == 0;
}

bool HasAOnly(const GCInputFrame& frame) noexcept
{
    return frame.buttons == GC_A
        && frame.main_x == 128 && frame.main_y == 128
        && frame.c_x == 128 && frame.c_y == 128
        && frame.trig_l == 0 && frame.trig_r == 0;
}

GCInputFrame PressA()
{
    GCInputFrame frame{};
    frame.A();
    return frame;
}

bool IsMem1Range(std::uint32_t address, std::size_t size) noexcept
{
    if (address < kMem1Begin || address >= kMem1End) return false;
    return size <= static_cast<std::size_t>(kMem1End - address);
}

bool ReadBytes(
    IInputMacroDriverHost& host,
    std::uint32_t address,
    std::span<std::byte> output)
{
    return IsMem1Range(address, output.size()) && host.read_guest_memory(address, output);
}

bool ReadU32BE(IInputMacroDriverHost& host, std::uint32_t address, std::uint32_t& out)
{
    std::array<std::byte, 4> bytes{};
    if (!ReadBytes(host, address, bytes)) return false;
    out = (std::to_integer<std::uint32_t>(bytes[0]) << 24)
        | (std::to_integer<std::uint32_t>(bytes[1]) << 16)
        | (std::to_integer<std::uint32_t>(bytes[2]) << 8)
        | std::to_integer<std::uint32_t>(bytes[3]);
    return true;
}

bool ReadController0Rearmed(IInputMacroDriverHost& host, bool& rearmed_out)
{
    std::uint32_t controller = 0;
    std::uint32_t raw_current = 0;
    std::uint32_t raw_new = 0;
    std::uint32_t controller_current = 0;
    std::uint32_t controller_new = 0;
    if (!ReadU32BE(host, BattleEndResultsInputMacroProvider::GuestAddresses::Controller0Pointer, controller)
        || !IsMem1Range(controller, kControllerNewPressesOffset + sizeof(std::uint32_t))
        || !ReadU32BE(host, controller, raw_current)
        || !ReadU32BE(host, controller + kControllerNewPressesOffset, raw_new)
        || !ReadU32BE(host, BattleEndResultsInputMacroProvider::GuestAddresses::ControllerInfo0, controller_current)
        || !ReadU32BE(host, BattleEndResultsInputMacroProvider::GuestAddresses::ControllerInfo0 + 8u, controller_new)) {
        return false;
    }
    // lastPresses is intentionally historical and still contains the prior
    // A press on the first valid neutral poll. current/new are the causal
    // fields that prove a later A press will be a fresh edge.
    rearmed_out = raw_current == 0
        && raw_new == 0
        && controller_current == 0
        && controller_new == 0;
    return true;
}

std::uint16_t U16BE(std::span<const std::byte> bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(
        (std::to_integer<std::uint16_t>(bytes[offset]) << 8)
        | std::to_integer<std::uint16_t>(bytes[offset + 1]));
}

std::int16_t I16BE(std::span<const std::byte> bytes, std::size_t offset)
{
    return static_cast<std::int16_t>(U16BE(bytes, offset));
}

std::uint32_t U32BE(std::span<const std::byte> bytes, std::size_t offset)
{
    return (std::to_integer<std::uint32_t>(bytes[offset]) << 24)
        | (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 16)
        | (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 8)
        | std::to_integer<std::uint32_t>(bytes[offset + 3]);
}

std::uint8_t U8(std::span<const std::byte> bytes, std::size_t offset)
{
    return std::to_integer<std::uint8_t>(bytes[offset]);
}

class TokenBuilder {
public:
    void Add(std::uint64_t value) noexcept
    {
        for (unsigned i = 0; i < 8; ++i) {
            value_ ^= static_cast<std::uint8_t>(value >> (i * 8));
            value_ *= 1099511628211ull;
        }
    }

    std::uint64_t value() const noexcept { return value_; }

private:
    std::uint64_t value_{1469598103934665603ull};
};

InputMacroPlan WaitPlan(
    std::string label,
    std::vector<BPKey> expected_keys,
    GCInputFrame input = {},
    bool hold_input_through_hit_opcode = false)
{
    InputMacroPlan plan{};
    plan.steps.push_back(InputMacroStep{
        .label = std::move(label),
        .action = BreakpointWaitAction{
            .expected_keys = std::move(expected_keys),
            .input = input,
            .hold_input_through_hit_opcode = hold_input_through_hit_opcode,
        },
    });
    return plan;
}

InputMacroDriverDecision Ready(InputMacroPlan plan)
{
    return InputMacroDriverDecision{
        .status = InputMacroDriverStatus::PlanReady,
        .plan = std::move(plan),
        .failure = InputMacroFailure::None,
    };
}

InputMacroFailure MapRuntimeFailure(InputMacroFailure failure) noexcept
{
    return failure == InputMacroFailure::None ? InputMacroFailure::HostFailure : failure;
}

endresults::FailureCode MapDetailedFailure(InputMacroFailure failure) noexcept
{
    switch (failure) {
    case InputMacroFailure::BreakpointTimeout:
    case InputMacroFailure::MemoryTimeout:
        return endresults::FailureCode::Timeout;
    case InputMacroFailure::UnexpectedBreakpoint:
        return endresults::FailureCode::UnexpectedBreakpoint;
    case InputMacroFailure::MemoryReadFailed:
    case InputMacroFailure::MissingBaseline:
        return endresults::FailureCode::MemoryReadFailed;
    case InputMacroFailure::Cancelled:
        return endresults::FailureCode::Cancelled;
    default:
        return endresults::FailureCode::RuntimeFailure;
    }
}

} // namespace

class BattleEndResultsInputMacroProvider::Impl {
public:
    explicit Impl(Request request) : request_(request)
    {
        report_.policy = request.acceleration_policy;
    }

    enum class Stage : std::uint8_t {
        Idle,
        VictoryCompletion,
        RewardEntry,
        RewardCommit,
        DescriptorReady,
        FirstDispatch,
        Dispatch,
        ReadyGate,
        AcceptedGate,
        NeutralWitness,
        Progress,
        CleanupCompletion,
        Completed,
        Failed,
        Cancelled,
    };

    struct RowObservation {
        std::uint8_t character_id{0xFF};
        std::uint8_t participant_selector{0};
        std::uint8_t stat_mask{0};
        std::uint8_t level_pending{0};
        std::uint8_t rank_pending{0};
        std::uint8_t learned_remaining{0};
        std::uint8_t learned_cursor{0};
        std::uint8_t learned_toggle{0};
        std::array<std::uint8_t, 4> displayed_learned{};
        std::array<std::int8_t, 36> learned_queue{};
        std::int16_t primary_selector{-1};
        std::int16_t descriptor_kind{-1};
        std::int16_t control_selector{-1};
        std::uint16_t control_value{0};
        std::uint16_t level_control{0};
        std::uint16_t rank_control{0};
        bool selector_valid{false};
    };

    struct Observation {
        std::uint32_t pointer{0};
        std::uint32_t state{0};
        std::uint32_t substate{0};
        std::uint32_t row_count{0};
        std::uint32_t remaining_exp{0};
        std::uint32_t remaining_magic_exp{0};
        std::uint32_t target_gold{0};
        std::uint32_t displayed_gold{0};
        std::uint32_t timer{0};
        std::uint16_t popup_control{0};
        std::array<std::int16_t, 3> item_ids{{-1, -1, -1}};
        std::array<RowObservation, kMaxRows> rows{};
        bool all_settled{false};
        bool stat_wave_live{false};
        bool learned_wave_live{false};
    };

    InputMacroDriverDecision Start(IInputMacroDriverHost& host)
    {
        if (stage_ != Stage::Idle) {
            return Fail(
                endresults::FailureCode::InvalidRequest,
                InputMacroFailure::HostFailure,
                "battle-end-results driver was started more than once");
        }
        if (!endresults::IsValidPolicy(request_.acceleration_policy)) {
            return Fail(
                endresults::FailureCode::InvalidRequest,
                InputMacroFailure::HostFailure,
                "invalid battle-end-results acceleration policy");
        }

        const auto stop = host.current_stop();
        last_hit_key_ = stop.key;
        last_hit_pc_ = stop.pc;
        if (request_.start_at_results_screen) {
            phase::battle::completion::Manifest manifest{};
            if (!phase::battle::completion::DecodeManifest(
                    request_.completion_manifest_blob, manifest)) {
                return Fail(
                    endresults::FailureCode::InvalidRequest,
                    InputMacroFailure::HostFailure,
                    "missing or invalid BCMB v1 completion manifest");
            }
            if (stop.key != bp::battle::BattleEndFieldReturnReseedComplete
                || stop.pc != 0x801012B4u) {
                return Fail(
                    endresults::FailureCode::InvalidSource,
                    InputMacroFailure::HostFailure,
                    "source savestate is not paused at the field-return reseed completion");
            }
            report_.expected = manifest.expected;
            report_.start_vi = host.current_vi();
            if (!ReadU32BE(host, GuestAddresses::RngSeed, entry_rng_seed_)) {
                return Fail(
                    endresults::FailureCode::MemoryReadFailed,
                    InputMacroFailure::MemoryReadFailed,
                    "failed to read results-screen entry RNG seed");
            }
            completion_manifest_ = std::move(manifest);
            report_.invariant_flags |= endresults::InvariantSourcePc;
            stage_ = Stage::DescriptorReady;
            return Plan(
                "battle_results.descriptor_ready",
                {bp::battle::BattleEndResultDescriptorReady});
        }
        if (stop.key != bp::battle::EndBattleVictory || stop.pc != kEndBattleVictoryPc) {
            return Fail(
                endresults::FailureCode::InvalidSource,
                InputMacroFailure::HostFailure,
                "source savestate is not paused at EndBattleVictory (0x800706D8)");
        }
        report_.invariant_flags |= endresults::InvariantSourcePc;
        if (!CapturePcData(host, false)) {
            return Fail(
                endresults::FailureCode::MemoryReadFailed,
                InputMacroFailure::MemoryReadFailed,
                "failed to capture pre-reward PC_Data records");
        }

        stage_ = Stage::VictoryCompletion;
        return Plan(
            "battle_end.victory_wait",
            {bp::battle::BattleEndVictoryCountdownComplete,
             bp::battle::BattleEndVictorySlotsComplete},
            {},
            true);
    }

    InputMacroDriverDecision Advance(
        IInputMacroDriverHost& host,
        const InputMacroStepResult& result)
    {
        if (stage_ == Stage::Completed) return CompletedDecision();
        if (stage_ == Stage::Failed || stage_ == Stage::Cancelled) {
            return FailedDecision(MapRuntimeFailure(result.failure));
        }
        if (stage_ == Stage::Idle) {
            return Fail(
                endresults::FailureCode::InvalidRequest,
                InputMacroFailure::NotRunning,
                "battle-end-results driver was advanced before Start");
        }
        if (result.failure != InputMacroFailure::None
            || result.terminal_status == InputMacroTerminalStatus::Failed
            || result.terminal_status == InputMacroTerminalStatus::Cancelled) {
            return Fail(
                MapDetailedFailure(result.failure),
                MapRuntimeFailure(result.failure),
                result.diagnostic.empty()
                    ? "input-macro segment failed"
                    : result.diagnostic);
        }
        if (!result.step_completed || result.action_kind != InputMacroActionKind::BreakpointWait) {
            return Fail(
                endresults::FailureCode::RuntimeFailure,
                InputMacroFailure::HostFailure,
                "adaptive provider received a non-breakpoint or incomplete segment result");
        }

        const auto stop = host.current_stop();
        last_hit_key_ = result.hit_key;
        last_hit_pc_ = result.hit_pc;
        if (stop.key != result.hit_key
            || stop.pc != result.hit_pc
            || stop.stop_sequence != result.stop_sequence) {
            return Fail(
                endresults::FailureCode::RuntimeFailure,
                InputMacroFailure::HostFailure,
                "driver stop snapshot does not match completed segment result");
        }

        switch (stage_) {
        case Stage::VictoryCompletion:
            return OnVictoryCompletion(host, result);
        case Stage::RewardEntry:
            return OnRewardEntry(host, result);
        case Stage::RewardCommit:
            return OnRewardCommit(host, result);
        case Stage::DescriptorReady:
            return OnDescriptorReady(host, result);
        case Stage::FirstDispatch:
        case Stage::Dispatch:
            return OnDispatchOrLifecycle(host, result);
        case Stage::ReadyGate:
            return OnReady(host, result);
        case Stage::AcceptedGate:
            return OnAccepted(result);
        case Stage::NeutralWitness:
            return OnNeutralWitness(host, result);
        case Stage::Progress:
            return OnProgress(host, result);
        case Stage::CleanupCompletion:
            return OnCleanupComplete(host, result);
        case Stage::Idle:
        case Stage::Completed:
        case Stage::Failed:
        case Stage::Cancelled:
            break;
        }
        return Fail(
            endresults::FailureCode::RuntimeFailure,
            InputMacroFailure::HostFailure,
            "invalid battle-end-results driver stage");
    }

    void Cancel() noexcept
    {
        if (stage_ == Stage::Completed || stage_ == Stage::Failed || stage_ == Stage::Cancelled) return;
        stage_ = Stage::Cancelled;
        failure_ = endresults::FailureCode::Cancelled;
        diagnostic_ = "battle-end-results driver cancelled";
        report_.outcome = endresults::Outcome::Failed;
        report_.failure = failure_;
        report_.diagnostic = diagnostic_;
        RefreshReportBlob();
    }

    endresults::FailureCode failure() const noexcept { return failure_; }
    const std::string& diagnostic() const noexcept { return diagnostic_; }
    const endresults::Report& report() const noexcept { return report_; }
    const std::string& report_blob() const noexcept { return report_blob_; }
    bool completed() const noexcept { return stage_ == Stage::Completed; }

    DiagnosticsSnapshot Diagnostics() const noexcept
    {
        return DiagnosticsSnapshot{
            .policy = request_.acceleration_policy,
            .outcome = completed() ? endresults::Outcome::Completed : endresults::Outcome::Failed,
            .failure = failure_,
            .completed = completed(),
            .action_count = static_cast<std::uint32_t>(report_.actions.size()),
            .input_request_count = input_request_count_,
            .input_observed_count = input_observed_count_,
            .release_request_count = release_request_count_,
            .release_observed_count = release_observed_count_,
            .last_expected_key = last_expected_key_,
            .last_hit_key = last_hit_key_,
            .last_hit_pc = last_hit_pc_,
            .last_state = last_state_,
            .last_substate = last_substate_,
            .last_token = last_token_,
            .expected_stat_waves = report_.expected.expected_stat_waves,
            .observed_stat_waves = report_.observed_stat_waves,
            .expected_learned_waves = report_.expected.expected_learned_waves,
            .observed_learned_waves = report_.observed_learned_waves,
            .expected_item_popup = report_.expected.expected_item_popup,
            .observed_item_popup = report_.observed_item_popup,
            .mismatch_flags = report_.mismatch_flags,
            .invariant_flags = report_.invariant_flags,
            .entry_rng_seed = entry_rng_seed_,
            .final_rng_seed = final_rng_seed_,
        };
    }

private:
    void RememberAcknowledgedNeutralEpoch(const InputMacroStepResult& result) noexcept
    {
        if (result.input_acknowledged
            && result.input_poll_count != 0
            && result.input_epoch > last_input_epoch_
            && IsNeutral(result.requested_input)) {
            last_input_epoch_ = result.input_epoch;
        }
    }

    InputMacroDriverDecision OnVictoryCompletion(
        IInputMacroDriverHost& host,
        const InputMacroStepResult& result)
    {
        if ((result.hit_key != bp::battle::BattleEndVictoryCountdownComplete
                || result.hit_pc != 0x8006F554u)
            && (result.hit_key != bp::battle::BattleEndVictorySlotsComplete
                || result.hit_pc != 0x8006F590u)) {
            return Unexpected("unexpected victory-completion breakpoint");
        }
        std::uint32_t battle_input_state = 0;
        if (!ReadU32BE(host, GuestAddresses::BattleInputState, battle_input_state)) {
            return Fail(
                endresults::FailureCode::MemoryReadFailed,
                InputMacroFailure::MemoryReadFailed,
                "failed to read victory-completion battle input state");
        }
        if (battle_input_state != 2u) {
            return Fail(
                endresults::FailureCode::VictoryInvariantMismatch,
                InputMacroFailure::HostFailure,
                "victory completion did not publish battleInputState == 2");
        }
        if (!result.input_acknowledged
            || result.input_poll_count == 0
            || !IsNeutral(result.requested_input)
            || result.input_epoch == 0) {
            return Fail(
                endresults::FailureCode::GuestNeutralTimeout,
                InputMacroFailure::HostFailure,
                "victory completion lacks a guest-observed full-neutral epoch");
        }
        last_input_epoch_ = result.input_epoch;
        report_.invariant_flags |= endresults::InvariantVictoryState;
        stage_ = Stage::RewardEntry;
        return Plan(
            "battle_end.reward_entry",
            {bp::battle::BattleEndRewardEntry});
    }

    InputMacroDriverDecision OnRewardEntry(
        IInputMacroDriverHost& host,
        const InputMacroStepResult& result)
    {
        if (result.hit_key != bp::battle::BattleEndRewardEntry
            || result.hit_pc != 0x8006F598u) {
            return Unexpected("unexpected reward-entry breakpoint");
        }
        std::uint32_t battle_input_state = 0;
        if (!ReadU32BE(host, GuestAddresses::BattleInputState, battle_input_state)) {
            return Fail(
                endresults::FailureCode::MemoryReadFailed,
                InputMacroFailure::MemoryReadFailed,
                "failed to read reward-entry battle input state");
        }
        if (battle_input_state != 2u) {
            return Fail(
                endresults::FailureCode::RewardInvariantMismatch,
                InputMacroFailure::HostFailure,
                "reward entry was not reached through battleInputState == 2");
        }
        stage_ = Stage::RewardCommit;
        return Plan(
            "battle_end.reward_commit",
            {bp::battle::BattleEndRewardCommitComplete});
    }

    InputMacroDriverDecision OnRewardCommit(
        IInputMacroDriverHost& host,
        const InputMacroStepResult& result)
    {
        if (result.hit_key != bp::battle::BattleEndRewardCommitComplete
            || result.hit_pc != 0x8006FD58u) {
            return Unexpected("unexpected reward-commit breakpoint");
        }
        std::uint32_t reward_phase = 0;
        if (!ReadU32BE(host, GuestAddresses::RewardPhase, reward_phase)) {
            return Fail(
                endresults::FailureCode::MemoryReadFailed,
                InputMacroFailure::MemoryReadFailed,
                "failed to read reward-phase invariant");
        }
        if (reward_phase != 6u) {
            return Fail(
                endresults::FailureCode::RewardInvariantMismatch,
                InputMacroFailure::HostFailure,
                "reward checkpoint was not reached through qualified phase 6");
        }
        report_.invariant_flags |= endresults::InvariantRewardPhase;
        if (!CapturePcData(host, true)) {
            return Fail(
                endresults::FailureCode::MemoryReadFailed,
                InputMacroFailure::MemoryReadFailed,
                "failed to capture post-reward PC_Data records");
        }
        FinalizePersistentExpectations();
        stage_ = Stage::FirstDispatch;
        return Plan(
            "battle_end.first_dispatch",
            {bp::battle::BattleEndResultDispatch,
             bp::battle::BattleEndResultLifecycleExit});
    }

    InputMacroDriverDecision OnDescriptorReady(
        IInputMacroDriverHost& host,
        const InputMacroStepResult& result)
    {
        if (result.hit_key != bp::battle::BattleEndResultDescriptorReady
            || result.hit_pc != 0x800E35F0u) {
            return Unexpected("unexpected result-descriptor breakpoint");
        }
        RememberAcknowledgedNeutralEpoch(result);

        std::uint32_t game_mode = 0;
        std::uint32_t field_state = 0;
        std::uint32_t pointer = 0;
        if (!ReadU32BE(host, GuestAddresses::GameMode, game_mode)
            || !ReadU32BE(host, GuestAddresses::FieldControllerState, field_state)
            || !ReadU32BE(host, GuestAddresses::ResultObjectPointer, pointer)) {
            return Fail(endresults::FailureCode::MemoryReadFailed,
                        InputMacroFailure::MemoryReadFailed,
                        "failed to qualify result descriptor");
        }
        if (game_mode != 6u || field_state != 4u
            || (pointer & 3u) != 0 || !IsMem1Range(pointer, kResultObjectReadSize)) {
            return Fail(endresults::FailureCode::QualificationMismatch,
                        InputMacroFailure::HostFailure,
                        "result descriptor failed mode, field-state, or pointer qualification");
        }

        std::array<std::byte, kResultObjectReadSize> bytes{};
        if (!ReadBytes(host, pointer, bytes)) {
            return Fail(endresults::FailureCode::MemoryReadFailed,
                        InputMacroFailure::MemoryReadFailed,
                        "failed to read result descriptor object");
        }
        const auto view = std::span<const std::byte>(bytes);
        const auto row_count = U32BE(view, 0x0Cu);
        if (row_count > kMaxRows) {
            return Fail(endresults::FailureCode::InvalidRowCount,
                        InputMacroFailure::HostFailure,
                        "result descriptor has too many rows");
        }

        for (std::size_t index = 0;
             index < completion_manifest_.reward_items.size(); ++index) {
            const auto offset = 0x28u + index * 4u;
            if (I16BE(view, offset)
                    != completion_manifest_.reward_items[index].item_id
                || U8(view, offset + 2u)
                    != completion_manifest_.reward_items[index].quantity) {
                report_.mismatch_flags |= endresults::MismatchItemPopup;
                return Fail(endresults::FailureCode::QualificationMismatch,
                            InputMacroFailure::HostFailure,
                            "result item descriptor does not match BCMB reward items");
            }
        }

        std::array<bool, endresults::CharacterCount> seen{};
        for (std::uint32_t row = 0; row < row_count; ++row) {
            const auto character_id = U8(view, 0x50u + row * 0x0Cu);
            if (character_id >= endresults::CharacterCount || seen[character_id]) {
                return Fail(endresults::FailureCode::QualificationMismatch,
                            InputMacroFailure::HostFailure,
                            "result descriptor has invalid or duplicate character rows");
            }
            seen[character_id] = true;
            std::vector<std::uint8_t> learned;
            const auto queue_base = 0x1DEu + row * 0x42u;
            for (std::uint32_t i = 0; i < 36; ++i) {
                const auto id = static_cast<std::int8_t>(U8(view, queue_base + i));
                if (id >= 0) learned.push_back(static_cast<std::uint8_t>(id));
            }
            auto expected = report_.expected.characters[character_id].learned_magic_ids;
            std::sort(learned.begin(), learned.end());
            std::sort(expected.begin(), expected.end());
            if (learned != expected) {
                report_.mismatch_flags |= endresults::MismatchLearnedQueue;
                return Fail(endresults::FailureCode::QualificationMismatch,
                            InputMacroFailure::HostFailure,
                            "result learned-magic queue does not match BCMB rank deltas");
            }
        }
        for (std::size_t character_id = 0;
             character_id < report_.expected.characters.size();
             ++character_id) {
            if (!report_.expected.characters[character_id].learned_magic_ids.empty()
                && !seen[character_id]) {
                report_.mismatch_flags |= endresults::MismatchLearnedQueue;
                return Fail(endresults::FailureCode::QualificationMismatch,
                            InputMacroFailure::HostFailure,
                            "BCMB expects learned magic for a character absent from the descriptor");
            }
        }
        presentation_expectation_captured_ = true;
        stage_ = Stage::FirstDispatch;
        return Plan("battle_results.first_dispatch",
                    {bp::battle::BattleEndResultDispatch,
                     bp::battle::BattleEndResultLifecycleExit});
    }

    InputMacroDriverDecision OnDispatchOrLifecycle(
        IInputMacroDriverHost& host,
        const InputMacroStepResult& result)
    {
        if (result.hit_key == bp::battle::BattleEndResultLifecycleExit) {
            if (result.hit_pc != 0x800E64A0u) {
                return Unexpected("lifecycle-exit key hit at an unexpected PC");
            }
            stage_ = Stage::CleanupCompletion;
            return Plan(
                "battle_end.cleanup_complete",
                {bp::battle::BattleEndResultCleanupComplete});
        }
        if (result.hit_key != bp::battle::BattleEndResultDispatch
            || result.hit_pc != 0x800E4660u) {
            return Unexpected("unexpected result-dispatch breakpoint");
        }
        RememberAcknowledgedNeutralEpoch(result);

        Observation observation{};
        const auto read_status = ReadObservation(host, observation);
        if (read_status != endresults::FailureCode::None) {
            return Fail(read_status, InputMacroFailure::MemoryReadFailed, "failed to qualify live result state");
        }
        last_state_ = observation.state;
        last_substate_ = observation.substate;
        if (!presentation_expectation_captured_) CapturePresentationExpectation(observation);

        const auto action = SelectAction(observation);
        if (!action.has_value()) {
            stage_ = Stage::Dispatch;
            return Plan(
                "battle_end.dispatch_neutral",
                {bp::battle::BattleEndResultDispatch,
                 bp::battle::BattleEndResultLifecycleExit});
        }

        pending_action_ = *action;
        pending_observation_ = observation;
        pending_token_ = TokenFor(observation, *action);
        last_token_ = pending_token_;
        const auto& definition = Definition(*action);
        stage_ = Stage::ReadyGate;
        return Plan(
            std::string("battle_end.ready.") + endresults::ActionKindName(*action),
            {definition.ready_key});
    }

    InputMacroDriverDecision OnReady(
        IInputMacroDriverHost& host,
        const InputMacroStepResult& result)
    {
        if (!pending_action_.has_value()) {
            return Fail(
                endresults::FailureCode::RuntimeFailure,
                InputMacroFailure::HostFailure,
                "ready gate has no pending semantic action");
        }
        const auto& definition = Definition(*pending_action_);
        if (result.hit_key != definition.ready_key || result.hit_pc != definition.ready_pc) {
            return Unexpected("semantic ready gate hit at an unexpected key or PC");
        }
        if (!IsNeutral(result.requested_input)) {
            return Fail(
                endresults::FailureCode::GuestNeutralTimeout,
                InputMacroFailure::HostFailure,
                "ready gate was reached without a full-neutral request");
        }
        bool controller_neutral = false;
        if (!ReadController0Rearmed(host, controller_neutral)) {
            return Fail(
                endresults::FailureCode::MemoryReadFailed,
                InputMacroFailure::MemoryReadFailed,
                "failed to inspect controller-0 fields at ready gate");
        }
        if (!controller_neutral || last_input_epoch_ == 0) {
            return Fail(
                endresults::FailureCode::GuestNeutralTimeout,
                InputMacroFailure::HostFailure,
                "ready gate lacks a prior guest-observed full-neutral rearm");
        }
        if (result.input_acknowledged && result.input_poll_count != 0) {
            if (result.input_epoch <= last_input_epoch_) {
                return Fail(
                    endresults::FailureCode::GuestNeutralTimeout,
                    InputMacroFailure::HostFailure,
                    "ready gate reported a stale neutral input epoch");
            }
            last_input_epoch_ = result.input_epoch;
        }

        Observation observation{};
        const auto read_status = ReadObservation(host, observation);
        if (read_status != endresults::FailureCode::None) {
            return Fail(read_status, InputMacroFailure::MemoryReadFailed, "failed to requalify semantic ready gate");
        }
        auto token = TokenFor(observation, *pending_action_);
        const bool gold_pre_input_latch = *pending_action_ == endresults::ActionKind::Gold
            && pending_observation_.state == 1u;
        if (gold_pre_input_latch) {
            if (!IsEligible(observation, *pending_action_)) {
                pending_action_.reset();
                pending_token_ = 0;
                stage_ = Stage::Dispatch;
                return ContinueFromObservation(observation);
            }
            // 0x800E488C is after state 2 is published but before its first
            // controller decision. Bind the occurrence here, then publish A
            // early enough for the next controller copy.
            pending_observation_ = observation;
            pending_token_ = token;
            last_token_ = token;
        } else if (!IsEligible(observation, *pending_action_) || token != pending_token_) {
            return Fail(
                endresults::FailureCode::QualificationMismatch,
                InputMacroFailure::HostFailure,
                "result state changed between dispatcher and ready gate");
        }
        const auto index = static_cast<std::size_t>(*pending_action_);
        if (occurrence_counts_[index] >= definition.max_occurrences) {
            return Fail(
                endresults::FailureCode::OccurrenceLimit,
                InputMacroFailure::HostFailure,
                "semantic action exceeded its statically bounded occurrence count");
        }
        if (consumed_tokens_[index] == token) {
            return Fail(
                endresults::FailureCode::StaleOccurrence,
                InputMacroFailure::HostFailure,
                "semantic action attempted to reuse a consumed occurrence token");
        }

        pending_observation_ = observation;
        pending_neutral_epoch_ = last_input_epoch_;
        ++input_request_count_;
        stage_ = Stage::AcceptedGate;
        return Plan(
            std::string("battle_end.accept.") + endresults::ActionKindName(*pending_action_),
            {definition.accepted_key},
            PressA());
    }

    InputMacroDriverDecision OnAccepted(const InputMacroStepResult& result)
    {
        if (!pending_action_.has_value()) {
            return Fail(
                endresults::FailureCode::RuntimeFailure,
                InputMacroFailure::HostFailure,
                "accepted gate has no pending semantic action");
        }
        const auto& definition = Definition(*pending_action_);
        if (result.hit_key != definition.accepted_key || result.hit_pc != definition.accepted_pc) {
            return Unexpected("semantic accepted gate hit at an unexpected key or PC");
        }
        if (!result.input_acknowledged
            || result.input_poll_count == 0
            || !HasAOnly(result.requested_input)
            || result.input_epoch <= pending_neutral_epoch_) {
            return Fail(
                endresults::FailureCode::QualificationMismatch,
                InputMacroFailure::HostFailure,
                "accepted gate lacks an acknowledged fresh A-only input epoch");
        }

        ++input_observed_count_;
        ++release_request_count_;
        last_input_epoch_ = result.input_epoch;
        const auto index = static_cast<std::size_t>(*pending_action_);
        ++occurrence_counts_[index];
        consumed_tokens_[index] = pending_token_;

        report_.actions.push_back(endresults::ActionTrace{
            .kind = *pending_action_,
            .state = static_cast<std::uint8_t>(pending_observation_.state),
            .substate = static_cast<std::uint8_t>(pending_observation_.substate),
            .occurrence_token = pending_token_,
            .ready_key = definition.ready_key,
            .accepted_key = definition.accepted_key,
            .ready_pc = definition.ready_pc,
            .accepted_pc = definition.accepted_pc,
            .neutral_epoch = pending_neutral_epoch_,
            .input_epoch = result.input_epoch,
            .input_poll_count = result.input_poll_count,
            .input_acknowledged = true,
        });

        switch (*pending_action_) {
        case endresults::ActionKind::StatWave:
            ++report_.observed_stat_waves;
            break;
        case endresults::ActionKind::LearnedMagicWave:
            ++report_.observed_learned_waves;
            break;
        case endresults::ActionKind::ItemPopup:
            report_.observed_item_popup = true;
            break;
        default:
            break;
        }

        stage_ = Stage::NeutralWitness;
        return Plan(
            "battle_end.release_witness",
            {bp::battle::BattleEndController0NeutralCopied});
    }

    InputMacroDriverDecision OnNeutralWitness(
        IInputMacroDriverHost& host,
        const InputMacroStepResult& result)
    {
        if (result.hit_key != bp::battle::BattleEndController0NeutralCopied
            || result.hit_pc != 0x801C7948u) {
            return Unexpected("unexpected controller-neutral witness breakpoint");
        }
        if (!result.input_acknowledged
            || result.input_poll_count == 0
            || !IsNeutral(result.requested_input)
            || result.input_epoch <= last_input_epoch_) {
            return Fail(
                endresults::FailureCode::GuestNeutralTimeout,
                InputMacroFailure::HostFailure,
                "release was not observed as a fresh full-neutral guest input epoch");
        }
        last_input_epoch_ = result.input_epoch;

        bool controller_neutral = false;
        if (!ReadController0Rearmed(host, controller_neutral)) {
            return Fail(
                endresults::FailureCode::MemoryReadFailed,
                InputMacroFailure::MemoryReadFailed,
                "failed to inspect controller-0 release fields");
        }
        if (!controller_neutral) {
            return Fail(
                endresults::FailureCode::GuestNeutralTimeout,
                InputMacroFailure::HostFailure,
                "controller-0 raw/current/new fields are not neutral at release witness");
        }

        ++release_observed_count_;
        if (!report_.actions.empty()) {
            auto& action = report_.actions.back();
            action.release_epoch = result.input_epoch;
            action.release_observed = true;
        }
        stage_ = Stage::Progress;
        return Plan(
            "battle_end.progress",
            {bp::battle::BattleEndResultDispatch,
             bp::battle::BattleEndResultLifecycleExit});
    }

    InputMacroDriverDecision OnProgress(
        IInputMacroDriverHost& host,
        const InputMacroStepResult& result)
    {
        if (!pending_action_.has_value()) {
            return Fail(
                endresults::FailureCode::RuntimeFailure,
                InputMacroFailure::HostFailure,
                "progress gate has no pending semantic action");
        }

        bool progressed = false;
        Observation observation{};
        if (result.hit_key == bp::battle::BattleEndResultLifecycleExit) {
            if (result.hit_pc != 0x800E64A0u) {
                return Unexpected("lifecycle-exit key hit at an unexpected PC");
            }
            progressed = *pending_action_ == endresults::ActionKind::Fade;
        } else if (result.hit_key == bp::battle::BattleEndResultDispatch
            && result.hit_pc == 0x800E4660u) {
            const auto read_status = ReadObservation(host, observation);
            if (read_status != endresults::FailureCode::None) {
                return Fail(read_status, InputMacroFailure::MemoryReadFailed, "failed to inspect semantic progress");
            }
            progressed = MadeProgress(pending_observation_, observation, *pending_action_);
        } else {
            return Unexpected("unexpected semantic progress breakpoint");
        }

        if (!progressed) {
            return Fail(
                endresults::FailureCode::NoProgress,
                InputMacroFailure::HostFailure,
                "accepted result input produced no declared semantic progress");
        }
        if (!report_.actions.empty()) report_.actions.back().progress_observed = true;
        pending_action_.reset();

        if (result.hit_key == bp::battle::BattleEndResultLifecycleExit) {
            stage_ = Stage::CleanupCompletion;
            return Plan(
                "battle_end.cleanup_complete",
                {bp::battle::BattleEndResultCleanupComplete});
        }
        stage_ = Stage::Dispatch;
        return ContinueFromObservation(observation);
    }

    InputMacroDriverDecision OnCleanupComplete(
        IInputMacroDriverHost& host,
        const InputMacroStepResult& result)
    {
        if (result.hit_key != bp::battle::BattleEndResultCleanupComplete
            || result.hit_pc != 0x800E3694u) {
            return Unexpected("unexpected result cleanup-completion breakpoint");
        }

        std::uint32_t lifecycle = 0;
        std::uint32_t done = 0;
        std::uint32_t result_pointer = 0;
        std::uint32_t game_mode = 0;
        if (!ReadU32BE(host, GuestAddresses::ResultLifecycleState, lifecycle)
            || !ReadU32BE(host, GuestAddresses::ResultDone, done)
            || !ReadU32BE(host, GuestAddresses::ResultObjectPointer, result_pointer)
            || !ReadU32BE(host, GuestAddresses::GameMode, game_mode)) {
            return Fail(
                endresults::FailureCode::MemoryReadFailed,
                InputMacroFailure::MemoryReadFailed,
                "failed to read battle-end completion invariants");
        }
        if (lifecycle != 0xFFu || done != 1u || result_pointer != 0u || game_mode != 6u) {
            std::ostringstream message;
            message << "completion invariant mismatch: lifecycle=" << lifecycle
                    << " done=" << done
                    << " result_pointer=0x" << std::hex << result_pointer
                    << " game_mode=" << std::dec << game_mode;
            return Fail(
                endresults::FailureCode::CompletionInvariantMismatch,
                InputMacroFailure::HostFailure,
                message.str());
        }
        report_.invariant_flags |= endresults::InvariantLifecycleState
            | endresults::InvariantCompletionState
            | endresults::InvariantCompletionPublished
            | endresults::InvariantResultPointerCleared
            | endresults::InvariantFieldMode;

        report_.end_vi = host.current_vi();
        if (request_.start_at_results_screen) {
            if (!ReadU32BE(host, GuestAddresses::RngSeed, final_rng_seed_)) {
                return Fail(
                    endresults::FailureCode::MemoryReadFailed,
                    InputMacroFailure::MemoryReadFailed,
                    "failed to read results-screen cleanup RNG seed");
            }
            if (final_rng_seed_ != entry_rng_seed_) {
                report_.mismatch_flags |= endresults::MismatchRngChanged;
                return Fail(
                    endresults::FailureCode::CompletionInvariantMismatch,
                    InputMacroFailure::HostFailure,
                    "results-screen phase violated Preserve(0) RNG effect");
            }
            report_.invariant_flags |= endresults::InvariantRngPreserved;
        }

        FinalizeMismatchFlags();
        stage_ = Stage::Completed;
        failure_ = endresults::FailureCode::None;
        diagnostic_.clear();
        report_.outcome = endresults::Outcome::Completed;
        report_.failure = failure_;
        report_.diagnostic.clear();
        RefreshReportBlob();
        return CompletedDecision();
    }

    InputMacroDriverDecision ContinueFromObservation(const Observation& observation)
    {
        last_state_ = observation.state;
        last_substate_ = observation.substate;
        const auto action = SelectAction(observation);
        if (!action.has_value()) {
            stage_ = Stage::Dispatch;
            return Plan(
                "battle_end.dispatch_neutral",
                {bp::battle::BattleEndResultDispatch,
                 bp::battle::BattleEndResultLifecycleExit});
        }
        pending_action_ = *action;
        pending_observation_ = observation;
        pending_token_ = TokenFor(observation, *action);
        last_token_ = pending_token_;
        stage_ = Stage::ReadyGate;
        const auto& definition = Definition(*action);
        return Plan(
            std::string("battle_end.ready.") + endresults::ActionKindName(*action),
            {definition.ready_key});
    }

    endresults::FailureCode ReadObservation(
        IInputMacroDriverHost& host,
        Observation& out) const
    {
        std::uint32_t pointer = 0;
        if (!ReadU32BE(host, GuestAddresses::ResultObjectPointer, pointer)) {
            return endresults::FailureCode::MemoryReadFailed;
        }
        if ((pointer & 3u) != 0 || !IsMem1Range(pointer, kResultObjectReadSize)) {
            return endresults::FailureCode::InvalidResultPointer;
        }

        std::vector<std::byte> bytes(kResultObjectReadSize);
        if (!ReadBytes(host, pointer, bytes)) return endresults::FailureCode::MemoryReadFailed;
        const auto view = std::span<const std::byte>(bytes);
        out.pointer = pointer;
        out.state = U32BE(view, 0x04);
        out.substate = U32BE(view, 0x08);
        out.row_count = U32BE(view, 0x0C);
        out.remaining_exp = U32BE(view, 0x10);
        out.remaining_magic_exp = U32BE(view, 0x14);
        out.target_gold = U32BE(view, 0x18);
        out.displayed_gold = U32BE(view, 0x24);
        out.timer = U32BE(view, 0x40);
        out.popup_control = U16BE(view, 0x0EA2);
        out.item_ids = {I16BE(view, 0x28), I16BE(view, 0x2C), I16BE(view, 0x30)};

        if (out.state > 14u) return endresults::FailureCode::InvalidResultState;
        if (out.row_count > kMaxRows) return endresults::FailureCode::InvalidRowCount;

        out.all_settled = true;
        for (std::uint32_t row = 0; row < out.row_count; ++row) {
            auto& observed = out.rows[row];
            observed.character_id = U8(view, 0x50 + row * 0x0C);
            observed.level_pending = U8(view, 0x34 + row);
            observed.rank_pending = U8(view, 0x38 + row);
            observed.level_control = U16BE(view, 0x0EC8 + row * 0x6E);
            observed.rank_control = U16BE(view, 0x0ECA + row * 0x6E);

            const auto row_base = 0x1C0u + row * 0x42u;
            observed.participant_selector = U8(view, row_base);
            observed.stat_mask = U8(view, row_base + 2);
            observed.learned_remaining = U8(view, row_base + 0x17);
            observed.learned_cursor = U8(view, row_base + 0x18);
            observed.learned_toggle = U8(view, row_base + 0x19);
            for (std::size_t i = 0; i < observed.displayed_learned.size(); ++i) {
                observed.displayed_learned[i] = U8(view, row_base + 0x1A + i);
            }
            for (std::size_t i = 0; i < observed.learned_queue.size(); ++i) {
                observed.learned_queue[i] = static_cast<std::int8_t>(
                    U8(view, row_base + 0x1E + i));
            }

            const auto descriptor_offset = 0x2C8u + row * 0x14u;
            observed.primary_selector = I16BE(view, descriptor_offset);
            if (observed.primary_selector == 0 || observed.primary_selector == 1) {
                const auto selected_offset = static_cast<std::uint32_t>(
                    observed.primary_selector) * 2u;
                observed.descriptor_kind = I16BE(
                    view,
                    descriptor_offset + 4u + selected_offset);
                observed.control_selector = I16BE(
                    view,
                    descriptor_offset + 8u + selected_offset);
                const auto expected_control = static_cast<std::int16_t>(
                    row + (observed.primary_selector != 0 ? 8u : 4u));
                if (observed.control_selector == expected_control) {
                    const auto control_offset = 0x580u
                        + static_cast<std::uint32_t>(observed.control_selector) * 0x6Cu
                        + 0x46u;
                    observed.control_value = U16BE(view, control_offset);
                    observed.selector_valid = true;
                }
            }

            if (out.state >= 6u && out.state <= 8u
                && observed.selector_valid
                && observed.control_value != 0) {
                const bool valid_pair =
                    (observed.descriptor_kind == 2 && observed.stat_mask == 6u)
                    || (observed.descriptor_kind == 3 && observed.stat_mask == 4u)
                    || (observed.descriptor_kind == 4 && observed.stat_mask == 0u);
                if (observed.descriptor_kind >= 2
                    && observed.descriptor_kind <= 4
                    && !valid_pair) {
                    return endresults::FailureCode::QualificationMismatch;
                }
                out.stat_wave_live = out.stat_wave_live || valid_pair;
            }
            out.learned_wave_live = out.learned_wave_live
                || (out.state == 11u
                    && observed.selector_valid
                    && (observed.descriptor_kind == 6 || observed.descriptor_kind == 7)
                    && observed.control_value != 0);
            out.all_settled = out.all_settled
                && observed.level_pending == 0
                && observed.rank_pending == 0
                && observed.level_control == 0
                && observed.rank_control == 0
                && observed.learned_remaining == 0;
        }
        return endresults::FailureCode::None;
    }

    bool CapturePcData(IInputMacroDriverHost& host, bool after)
    {
        constexpr std::size_t bytes_size = endresults::CharacterCount
            * GuestAddresses::PcDataStride;
        std::array<std::byte, bytes_size> bytes{};
        if (!ReadBytes(host, GuestAddresses::PcDataBase, bytes)) return false;
        const auto view = std::span<const std::byte>(bytes);
        for (std::size_t character_index = 0;
             character_index < report_.expected.characters.size();
             ++character_index) {
            auto& character = report_.expected.characters[character_index];
            const auto base = character_index * GuestAddresses::PcDataStride;
            character.character_id = static_cast<std::uint8_t>(character_index);
            if (after) {
                character.level_after = U8(view, base + 0x0B);
                character.experience_after = U32BE(view, base + 0x24);
            } else {
                character.level_before = U8(view, base + 0x0B);
                character.experience_before = U32BE(view, base + 0x24);
            }
            for (std::size_t element_index = 0;
                 element_index < character.elements.size();
                 ++element_index) {
                auto& element = character.elements[element_index];
                if (after) {
                    element.rank_after = U8(view, base + 0x34 + element_index);
                    element.xp_after = U32BE(view, base + 0x44 + element_index * 4);
                } else {
                    element.rank_before = U8(view, base + 0x34 + element_index);
                    element.xp_before = U32BE(view, base + 0x44 + element_index * 4);
                }
            }
        }
        return true;
    }

    void FinalizePersistentExpectations()
    {
        const bool any_level_gain = std::any_of(
            report_.expected.characters.begin(),
            report_.expected.characters.end(),
            [](const endresults::CharacterProgress& character) {
                return character.level_after > character.level_before;
            });
        report_.expected.expected_stat_waves = any_level_gain ? kMaxStatWaves : 0u;
    }

    void CapturePresentationExpectation(const Observation& observation)
    {
        presentation_expectation_captured_ = true;
        report_.expected.expected_item_popup = std::any_of(
            observation.item_ids.begin(),
            observation.item_ids.end(),
            [](std::int16_t id) { return id >= 0; });

        std::uint32_t max_pages = 0;
        for (std::uint32_t row = 0; row < observation.row_count; ++row) {
            const auto& observed = observation.rows[row];
            std::uint32_t queue_count = 0;
            for (const auto id : observed.learned_queue) {
                if (id >= 0 && id <= 35) ++queue_count;
            }
            max_pages = std::max(max_pages, (queue_count + 1u) / 2u);
            if (observed.character_id >= report_.expected.characters.size()) continue;
            auto& learned = report_.expected.characters[observed.character_id].learned_magic_ids;
            learned.clear();
            for (const auto id : observed.learned_queue) {
                if (id >= 0 && id <= 35) learned.push_back(static_cast<std::uint8_t>(id));
            }
            if (queue_count != observed.learned_remaining) {
                report_.mismatch_flags |= endresults::MismatchLearnedQueue;
            }
        }
        report_.expected.expected_learned_waves = max_pages;
    }

    std::optional<endresults::ActionKind> SelectAction(const Observation& observation) const
    {
        const bool full = request_.acceleration_policy == endresults::AccelerationPolicy::FullAdaptive;
        if (full) {
            switch (observation.state) {
            case 0: return EligibleOrNone(observation, endresults::ActionKind::Intro);
            // State 1 reaches 0x800E488C after publishing state 2 and before
            // its first controller read. Arm there so the A edge is sampled
            // before the transient gold tally can finish naturally.
            case 1:
                if (occurrence_counts_[static_cast<std::size_t>(endresults::ActionKind::Gold)]
                    < Definition(endresults::ActionKind::Gold).max_occurrences) {
                    return endresults::ActionKind::Gold;
                }
                break;
            case 4: return EligibleOrNone(observation, endresults::ActionKind::NormalExp);
            case 6:
            case 7:
            case 8: return EligibleOrNone(observation, endresults::ActionKind::StatWave);
            case 9:
                return EligibleOrNone(
                    observation,
                    observation.substate == 0
                        ? endresults::ActionKind::MagicEntry
                        : endresults::ActionKind::MagicExp);
            case 11: return EligibleOrNone(observation, endresults::ActionKind::LearnedMagicWave);
            case 12: return EligibleOrNone(observation, endresults::ActionKind::ItemPopup);
            // The fade completes automatically and its accepted path reaches
            // the lifecycle store in the same invocation. Keep production
            // neutral here; the dispatcher/lifecycle pair remains the exact
            // completion path.
            case 14: break;
            default: break;
            }
        }
        if (observation.state == 13) {
            return EligibleOrNone(observation, endresults::ActionKind::MandatoryConfirm);
        }
        return std::nullopt;
    }

    std::optional<endresults::ActionKind> EligibleOrNone(
        const Observation& observation,
        endresults::ActionKind kind) const
    {
        if (!IsEligible(observation, kind)) return std::nullopt;
        const auto index = static_cast<std::size_t>(kind);
        const auto token = TokenFor(observation, kind);
        if (occurrence_counts_[index] >= Definition(kind).max_occurrences
            || consumed_tokens_[index] == token) {
            return std::nullopt;
        }
        return kind;
    }

    bool IsEligible(const Observation& observation, endresults::ActionKind kind) const
    {
        switch (kind) {
        case endresults::ActionKind::Intro:
            return observation.state == 0 && observation.timer < 0x55u;
        case endresults::ActionKind::Gold:
            return observation.state == 2
                && observation.target_gold > observation.displayed_gold;
        case endresults::ActionKind::NormalExp:
            return observation.state == 4 && observation.timer < 0x1Eu;
        case endresults::ActionKind::StatWave:
            return observation.state >= 6 && observation.state <= 8 && observation.stat_wave_live;
        case endresults::ActionKind::MagicEntry:
            return observation.state == 9
                && observation.substate == 0
                && std::any_of(
                    observation.rows.begin(),
                    observation.rows.begin() + observation.row_count,
                    [](const RowObservation& row) {
                        return row.selector_valid && row.control_value != 0;
                    });
        case endresults::ActionKind::MagicExp:
            return observation.state == 9
                && observation.substate == 1
                && observation.timer < 0x1Eu;
        case endresults::ActionKind::LearnedMagicWave:
            return observation.state == 11 && observation.learned_wave_live;
        case endresults::ActionKind::ItemPopup:
            return observation.state == 12
                && observation.popup_control != 0
                && std::any_of(
                    observation.item_ids.begin(),
                    observation.item_ids.end(),
                    [](std::int16_t id) { return id >= 0; });
        case endresults::ActionKind::MandatoryConfirm:
            return observation.state == 13 && observation.all_settled;
        case endresults::ActionKind::Fade:
            return observation.state == 14;
        }
        return false;
    }

    std::uint64_t TokenFor(
        const Observation& observation,
        endresults::ActionKind kind) const
    {
        TokenBuilder token;
        token.Add(observation.pointer);
        token.Add(static_cast<std::uint8_t>(kind));
        token.Add(observation.state);
        token.Add(observation.substate);
        switch (kind) {
        case endresults::ActionKind::StatWave:
            for (std::uint32_t row = 0; row < observation.row_count; ++row) {
                const auto& value = observation.rows[row];
                token.Add(value.participant_selector);
                token.Add(value.stat_mask);
                token.Add(static_cast<std::uint16_t>(value.primary_selector));
                token.Add(static_cast<std::uint16_t>(value.descriptor_kind));
                token.Add(static_cast<std::uint16_t>(value.control_selector));
                token.Add(value.control_value);
            }
            break;
        case endresults::ActionKind::LearnedMagicWave:
            for (std::uint32_t row = 0; row < observation.row_count; ++row) {
                const auto& value = observation.rows[row];
                token.Add(value.learned_remaining);
                token.Add(value.learned_cursor);
                token.Add(value.learned_toggle);
                for (const auto id : value.displayed_learned) token.Add(id);
                token.Add(value.control_value);
            }
            break;
        case endresults::ActionKind::Gold:
            token.Add(observation.target_gold);
            token.Add(observation.displayed_gold);
            break;
        case endresults::ActionKind::NormalExp:
            token.Add(observation.remaining_exp);
            break;
        case endresults::ActionKind::MagicExp:
            token.Add(observation.remaining_magic_exp);
            break;
        case endresults::ActionKind::MagicEntry:
            for (std::uint32_t row = 0; row < observation.row_count; ++row) {
                const auto& value = observation.rows[row];
                token.Add(static_cast<std::uint16_t>(value.primary_selector));
                token.Add(static_cast<std::uint16_t>(value.descriptor_kind));
                token.Add(static_cast<std::uint16_t>(value.control_selector));
                token.Add(value.control_value);
            }
            break;
        case endresults::ActionKind::MandatoryConfirm:
            token.Add(observation.all_settled ? 1u : 0u);
            break;
        case endresults::ActionKind::ItemPopup:
            for (const auto id : observation.item_ids) token.Add(static_cast<std::uint16_t>(id));
            token.Add(observation.popup_control);
            break;
        case endresults::ActionKind::Intro:
        case endresults::ActionKind::Fade:
            break;
        }
        return token.value();
    }

    bool MadeProgress(
        const Observation& before,
        const Observation& after,
        endresults::ActionKind kind) const
    {
        switch (kind) {
        case endresults::ActionKind::Intro:
            return after.state != 0 || (before.timer != 0x55u && after.timer == 0x55u);
        case endresults::ActionKind::Gold:
            return after.state != 2 || after.displayed_gold != before.displayed_gold;
        case endresults::ActionKind::NormalExp:
            return after.state != 4 || after.remaining_exp == 0;
        case endresults::ActionKind::StatWave:
            return after.state < 6 || after.state > 8
                || TokenFor(before, kind) != TokenFor(after, kind);
        case endresults::ActionKind::MagicEntry:
            return after.state != 9 || after.substate != 0
                || TokenFor(before, kind) != TokenFor(after, kind);
        case endresults::ActionKind::MagicExp:
            return after.state != 9 || after.substate != 1 || after.remaining_magic_exp == 0;
        case endresults::ActionKind::LearnedMagicWave:
            return after.state != 11 || TokenFor(before, kind) != TokenFor(after, kind);
        case endresults::ActionKind::ItemPopup:
            return after.state != 12 || TokenFor(before, kind) != TokenFor(after, kind);
        case endresults::ActionKind::MandatoryConfirm:
            return after.state == 14;
        case endresults::ActionKind::Fade:
            return after.state != 14 || (before.timer != 0x28u && after.timer == 0x28u);
        }
        return false;
    }

    void FinalizeMismatchFlags()
    {
        if (request_.acceleration_policy == endresults::AccelerationPolicy::FullAdaptive) {
            if (report_.observed_stat_waves != report_.expected.expected_stat_waves) {
                report_.mismatch_flags |= endresults::MismatchStatWaveCount;
            }
            if (report_.observed_learned_waves != report_.expected.expected_learned_waves) {
                report_.mismatch_flags |= endresults::MismatchLearnedWaveCount;
            }
            if (report_.observed_item_popup != report_.expected.expected_item_popup) {
                report_.mismatch_flags |= endresults::MismatchItemPopup;
            }
        }
    }

    InputMacroDriverDecision Plan(
        std::string label,
        std::vector<BPKey> expected,
        GCInputFrame input = {},
        bool hold_input_through_hit_opcode = false)
    {
        last_expected_key_ = expected.empty() ? 0 : expected.front();
        RefreshReportBlob();
        return Ready(WaitPlan(
            std::move(label),
            std::move(expected),
            input,
            hold_input_through_hit_opcode));
    }

    InputMacroDriverDecision Unexpected(std::string diagnostic)
    {
        return Fail(
            endresults::FailureCode::UnexpectedBreakpoint,
            InputMacroFailure::UnexpectedBreakpoint,
            std::move(diagnostic));
    }

    InputMacroDriverDecision Fail(
        endresults::FailureCode detailed_failure,
        InputMacroFailure runtime_failure,
        std::string diagnostic)
    {
        if (stage_ != Stage::Failed && stage_ != Stage::Cancelled) {
            stage_ = Stage::Failed;
            failure_ = detailed_failure;
            diagnostic_ = std::move(diagnostic);
            report_.outcome = endresults::Outcome::Failed;
            report_.failure = failure_;
            report_.diagnostic = diagnostic_;
            RefreshReportBlob();
        }
        return FailedDecision(runtime_failure);
    }

    InputMacroDriverDecision FailedDecision(InputMacroFailure failure) const
    {
        return InputMacroDriverDecision{
            .status = InputMacroDriverStatus::Failed,
            .failure = failure,
            .diagnostic = diagnostic_,
        };
    }

    InputMacroDriverDecision CompletedDecision() const
    {
        return InputMacroDriverDecision{
            .status = InputMacroDriverStatus::Completed,
            .failure = InputMacroFailure::None,
        };
    }

    void RefreshReportBlob() noexcept
    {
        std::string encoded;
        if (endresults::EncodeReport(report_, encoded)) report_blob_ = std::move(encoded);
    }

    Request request_;
    Stage stage_{Stage::Idle};
    endresults::FailureCode failure_{endresults::FailureCode::None};
    std::string diagnostic_;
    endresults::Report report_{};
    phase::battle::completion::Manifest completion_manifest_{};
    std::string report_blob_;
    bool presentation_expectation_captured_{false};
    std::array<std::uint32_t, kActions.size()> occurrence_counts_{};
    std::array<std::uint64_t, kActions.size()> consumed_tokens_{};
    std::optional<endresults::ActionKind> pending_action_;
    Observation pending_observation_{};
    std::uint64_t pending_token_{0};
    std::uint64_t pending_neutral_epoch_{0};
    std::uint64_t last_input_epoch_{0};
    std::uint32_t input_request_count_{0};
    std::uint32_t input_observed_count_{0};
    std::uint32_t release_request_count_{0};
    std::uint32_t release_observed_count_{0};
    BPKey last_expected_key_{0};
    BPKey last_hit_key_{0};
    std::uint32_t last_hit_pc_{0};
    std::uint32_t last_state_{0};
    std::uint32_t last_substate_{0};
    std::uint64_t last_token_{0};
    std::uint32_t entry_rng_seed_{0};
    std::uint32_t final_rng_seed_{0};
};

BattleEndResultsInputMacroProvider::BattleEndResultsInputMacroProvider(Request request)
    : impl_(std::make_unique<Impl>(request))
{
}

BattleEndResultsInputMacroProvider::~BattleEndResultsInputMacroProvider() = default;

std::span<const BPKey> BattleEndResultsInputMacroProvider::required_breakpoint_keys() noexcept
{
    return kRequiredBreakpointKeys;
}

std::span<const BPKey> BattleEndResultsInputMacroProvider::declared_breakpoint_keys() const
{
    return required_breakpoint_keys();
}

InputMacroDriverDecision BattleEndResultsInputMacroProvider::Start(IInputMacroDriverHost& host)
{
    return impl_->Start(host);
}

InputMacroDriverDecision BattleEndResultsInputMacroProvider::Advance(
    IInputMacroDriverHost& host,
    const InputMacroStepResult& completed_segment_result)
{
    return impl_->Advance(host, completed_segment_result);
}

void BattleEndResultsInputMacroProvider::Cancel() noexcept
{
    impl_->Cancel();
}

endresults::FailureCode BattleEndResultsInputMacroProvider::failure() const noexcept
{
    return impl_->failure();
}

const std::string& BattleEndResultsInputMacroProvider::diagnostic() const noexcept
{
    return impl_->diagnostic();
}

const endresults::Report& BattleEndResultsInputMacroProvider::report() const noexcept
{
    return impl_->report();
}

const std::string& BattleEndResultsInputMacroProvider::report_blob() const noexcept
{
    return impl_->report_blob();
}

bool BattleEndResultsInputMacroProvider::completed() const noexcept
{
    return impl_->completed();
}

BattleEndResultsInputMacroProvider::DiagnosticsSnapshot
BattleEndResultsInputMacroProvider::diagnostics() const noexcept
{
    return impl_->Diagnostics();
}

} // namespace savor::inputmacro
