#include "BattleCompletionInputMacroProvider.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "../../Breakpoints/BpRegistry.h"

namespace savor::inputmacro {
namespace {

namespace completion = phase::battle::completion;
namespace endresults = phase::battle::endresults;

constexpr std::uint32_t kEndBattleVictoryPc = 0x800706D8u;
constexpr std::array<BPKey, 4> kRequiredBreakpointKeys{
    bp::battle::BattleEndVictoryCountdownComplete,
    bp::battle::BattleEndVictorySlotsComplete,
    bp::battle::BattleEndRewardEntry,
    bp::battle::BattleEndRewardCommitComplete,
};

bool ReadBytes(IInputMacroDriverHost& host, std::uint32_t address,
               std::span<std::byte> output)
{
    constexpr std::uint32_t Mem1Begin = 0x80000000u;
    constexpr std::uint32_t Mem1End = 0x81800000u;
    return address >= Mem1Begin && address < Mem1End
        && output.size() <= static_cast<std::size_t>(Mem1End - address)
        && host.read_guest_memory(address, output);
}

bool ReadU32BE(IInputMacroDriverHost& host, std::uint32_t address,
               std::uint32_t& value)
{
    std::array<std::byte, 4> bytes{};
    if (!ReadBytes(host, address, bytes)) return false;
    value = (std::to_integer<std::uint32_t>(bytes[0]) << 24)
        | (std::to_integer<std::uint32_t>(bytes[1]) << 16)
        | (std::to_integer<std::uint32_t>(bytes[2]) << 8)
        | std::to_integer<std::uint32_t>(bytes[3]);
    return true;
}

InputMacroPlan WaitPlan(std::string label, std::vector<BPKey> expected,
                        bool hold = false)
{
    InputMacroPlan plan{};
    plan.steps.push_back(InputMacroStep{
        .label = std::move(label),
        .action = BreakpointWaitAction{
            .expected_keys = std::move(expected),
            .input = {},
            .hold_input_through_hit_opcode = hold,
        },
    });
    return plan;
}

InputMacroFailure RuntimeFailureFor(InputMacroFailure failure)
{
    return failure == InputMacroFailure::None
        ? InputMacroFailure::HostFailure : failure;
}

endresults::FailureCode DetailedFailureFor(InputMacroFailure failure)
{
    switch (failure) {
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

class BattleCompletionInputMacroProvider::Impl {
public:
    enum class Stage : std::uint8_t {
        Idle, VictoryCompletion, RewardEntry, RewardCommit,
        Completed, Failed, Cancelled,
    };

    InputMacroDriverDecision Start(IInputMacroDriverHost& host)
    {
        if (stage_ != Stage::Idle)
            return Fail(endresults::FailureCode::InvalidRequest,
                        InputMacroFailure::HostFailure,
                        "battle-completion driver was started more than once");
        const auto stop = host.current_stop();
        last_hit_key_ = stop.key;
        last_hit_pc_ = stop.pc;
        if (stop.key != bp::battle::EndBattleVictory
            || stop.pc != kEndBattleVictoryPc) {
            return Fail(endresults::FailureCode::InvalidSource,
                        InputMacroFailure::HostFailure,
                        "source savestate is not paused at EndBattleVictory (0x800706D8)");
        }
        manifest_.start_vi = host.current_vi();
        if (!CaptureCharacterRecords(host, false)) {
            return Fail(endresults::FailureCode::MemoryReadFailed,
                        InputMacroFailure::MemoryReadFailed,
                        "failed to capture pre-reward PC_Data records");
        }
        manifest_.invariant_flags |= completion::ManifestInvariantPreCaptured;
        stage_ = Stage::VictoryCompletion;
        return Plan("battle_completion.victory_wait",
                    {bp::battle::BattleEndVictoryCountdownComplete,
                     bp::battle::BattleEndVictorySlotsComplete}, true);
    }

    InputMacroDriverDecision Advance(IInputMacroDriverHost& host,
                                     const InputMacroStepResult& result)
    {
        if (stage_ == Stage::Completed) return CompleteDecision();
        if (stage_ == Stage::Failed || stage_ == Stage::Cancelled)
            return FailedDecision(RuntimeFailureFor(result.failure));
        if (stage_ == Stage::Idle)
            return Fail(endresults::FailureCode::InvalidRequest,
                        InputMacroFailure::NotRunning,
                        "battle-completion driver advanced before Start");
        if (result.failure != InputMacroFailure::None
            || result.terminal_status != InputMacroTerminalStatus::Completed
            || !result.step_completed
            || result.action_kind != InputMacroActionKind::BreakpointWait) {
            return Fail(DetailedFailureFor(result.failure),
                        RuntimeFailureFor(result.failure),
                        result.diagnostic.empty()
                            ? "battle-completion input-macro segment failed"
                            : result.diagnostic);
        }
        const auto stop = host.current_stop();
        if (stop.key != result.hit_key || stop.pc != result.hit_pc
            || stop.stop_sequence != result.stop_sequence) {
            return Fail(endresults::FailureCode::RuntimeFailure,
                        InputMacroFailure::HostFailure,
                        "driver stop snapshot does not match segment result");
        }
        last_hit_key_ = result.hit_key;
        last_hit_pc_ = result.hit_pc;
        switch (stage_) {
        case Stage::VictoryCompletion: return OnVictory(host, result);
        case Stage::RewardEntry: return OnRewardEntry(host, result);
        case Stage::RewardCommit: return OnRewardCommit(host, result);
        default: break;
        }
        return Fail(endresults::FailureCode::RuntimeFailure,
                    InputMacroFailure::HostFailure,
                    "invalid battle-completion stage");
    }

    void Cancel() noexcept
    {
        if (stage_ == Stage::Completed || stage_ == Stage::Failed
            || stage_ == Stage::Cancelled) return;
        stage_ = Stage::Cancelled;
        failure_ = endresults::FailureCode::Cancelled;
        diagnostic_ = "battle-completion driver cancelled";
    }

    endresults::FailureCode failure() const noexcept { return failure_; }
    const std::string& diagnostic() const noexcept { return diagnostic_; }
    const completion::Manifest& manifest() const noexcept { return manifest_; }
    const std::string& manifest_blob() const noexcept { return manifest_blob_; }
    bool completed() const noexcept { return stage_ == Stage::Completed; }

    DiagnosticsSnapshot Diagnostics() const noexcept
    {
        return DiagnosticsSnapshot{
            .outcome = completed() ? endresults::Outcome::Completed
                                   : endresults::Outcome::Failed,
            .failure = failure_,
            .completed = completed(),
            .last_expected_key = last_expected_key_,
            .last_hit_key = last_hit_key_,
            .last_hit_pc = last_hit_pc_,
            .invariant_flags = manifest_.invariant_flags,
            .expected_level_panels = manifest_.presentation.level_panels,
            .expected_stat_waves = manifest_.presentation.stat_waves,
            .expected_magic_rank_events = manifest_.presentation.magic_rank_events,
            .expected_learned_waves = manifest_.presentation.learned_magic_waves,
            .expected_item_popups = manifest_.presentation.item_popups,
            .start_vi = manifest_.start_vi,
            .end_vi = manifest_.end_vi,
        };
    }

private:
    InputMacroDriverDecision OnVictory(IInputMacroDriverHost& host,
                                       const InputMacroStepResult& result)
    {
        if ((result.hit_key != bp::battle::BattleEndVictoryCountdownComplete
                || result.hit_pc != 0x8006F554u)
            && (result.hit_key != bp::battle::BattleEndVictorySlotsComplete
                || result.hit_pc != 0x8006F590u)) {
            return Unexpected("unexpected victory-completion breakpoint");
        }
        std::uint32_t state = 0;
        if (!ReadU32BE(host, GuestAddresses::BattleInputState, state))
            return Fail(endresults::FailureCode::MemoryReadFailed,
                        InputMacroFailure::MemoryReadFailed,
                        "failed to read victory battle input state");
        if (state != 2u)
            return Fail(endresults::FailureCode::VictoryInvariantMismatch,
                        InputMacroFailure::HostFailure,
                        "victory completion did not publish battleInputState == 2");
        if (!result.input_acknowledged || result.input_poll_count == 0
            || result.input_epoch == 0 || result.requested_input.buttons != 0) {
            return Fail(endresults::FailureCode::GuestNeutralUnacknowledged,
                        InputMacroFailure::HostFailure,
                        "victory completion lacks a guest-observed neutral epoch");
        }
        stage_ = Stage::RewardEntry;
        return Plan("battle_completion.reward_entry",
                    {bp::battle::BattleEndRewardEntry});
    }

    InputMacroDriverDecision OnRewardEntry(IInputMacroDriverHost& host,
                                           const InputMacroStepResult& result)
    {
        if (result.hit_key != bp::battle::BattleEndRewardEntry
            || result.hit_pc != 0x8006F598u)
            return Unexpected("unexpected reward-entry breakpoint");
        std::uint32_t state = 0;
        if (!ReadU32BE(host, GuestAddresses::BattleInputState, state))
            return Fail(endresults::FailureCode::MemoryReadFailed,
                        InputMacroFailure::MemoryReadFailed,
                        "failed to read reward-entry battle input state");
        if (state != 2u)
            return Fail(endresults::FailureCode::RewardInvariantMismatch,
                        InputMacroFailure::HostFailure,
                        "reward entry was not reached through battleInputState == 2");
        stage_ = Stage::RewardCommit;
        return Plan("battle_completion.reward_commit",
                    {bp::battle::BattleEndRewardCommitComplete});
    }

    InputMacroDriverDecision OnRewardCommit(IInputMacroDriverHost& host,
                                            const InputMacroStepResult& result)
    {
        if (result.hit_key != bp::battle::BattleEndRewardCommitComplete
            || result.hit_pc != 0x8006FD58u)
            return Unexpected("unexpected reward-commit breakpoint");
        std::uint32_t reward_phase = 0;
        if (!ReadU32BE(host, GuestAddresses::RewardPhase, reward_phase))
            return Fail(endresults::FailureCode::MemoryReadFailed,
                        InputMacroFailure::MemoryReadFailed,
                        "failed to read reward phase");
        if (reward_phase != 6u)
            return Fail(endresults::FailureCode::RewardInvariantMismatch,
                        InputMacroFailure::HostFailure,
                        "reward checkpoint was not reached through phase 6");
        if (!CaptureCharacterRecords(host, true) || !CaptureRewardItems(host))
            return Fail(endresults::FailureCode::MemoryReadFailed,
                        InputMacroFailure::MemoryReadFailed,
                        "failed to capture post-reward persistent state");
        manifest_.invariant_flags |= completion::ManifestInvariantPostCaptured
            | completion::ManifestInvariantRewardItemsCaptured;
        manifest_.end_vi = host.current_vi();
        std::string error;
        if (!completion::DeriveExpectedView(manifest_, &error)
            || !completion::EncodeManifest(manifest_, manifest_blob_)) {
            return Fail(endresults::FailureCode::RewardInvariantMismatch,
                        InputMacroFailure::HostFailure,
                        error.empty() ? "failed to encode BCMB manifest" : error);
        }
        stage_ = Stage::Completed;
        failure_ = endresults::FailureCode::None;
        diagnostic_.clear();
        return CompleteDecision();
    }

    bool CaptureCharacterRecords(IInputMacroDriverHost& host, bool after)
    {
        auto& records = after ? manifest_.post_character_records
                              : manifest_.pre_character_records;
        std::array<std::byte,
            completion::CharacterCount * completion::CharacterRecordSize> bytes{};
        if (!ReadBytes(host, GuestAddresses::PcDataBase, bytes)) return false;
        for (std::size_t i = 0; i < records.size(); ++i) {
            std::memcpy(records[i].data(),
                        bytes.data() + i * completion::CharacterRecordSize,
                        completion::CharacterRecordSize);
        }
        return true;
    }

    bool CaptureRewardItems(IInputMacroDriverHost& host)
    {
        if (!ReadU32BE(host, GuestAddresses::NormalExperienceReward,
                       manifest_.normal_experience_reward)
            || !ReadU32BE(host, GuestAddresses::MagicExperienceReward,
                          manifest_.magic_experience_reward)
            || !ReadU32BE(host, GuestAddresses::GoldReward,
                          manifest_.gold_reward)) return false;
        std::array<std::byte,
            completion::RewardItemCount * GuestAddresses::RewardItemStride> bytes{};
        if (!ReadBytes(host, GuestAddresses::RewardItemsBase, bytes)) return false;
        for (std::size_t i = 0; i < manifest_.reward_items.size(); ++i) {
            const auto base = i * GuestAddresses::RewardItemStride;
            const auto raw_id = static_cast<std::uint16_t>(
                (std::to_integer<std::uint16_t>(bytes[base]) << 8)
                | std::to_integer<std::uint16_t>(bytes[base + 1]));
            manifest_.reward_items[i] = completion::RewardItem{
                .item_id = static_cast<std::int16_t>(raw_id),
                .quantity = std::to_integer<std::uint8_t>(bytes[base + 2]),
                .opaque = std::to_integer<std::uint8_t>(bytes[base + 3]),
            };
        }
        return true;
    }

    InputMacroDriverDecision Plan(std::string label,
                                  std::vector<BPKey> expected, bool hold = false)
    {
        last_expected_key_ = expected.empty() ? BPKey{0} : expected.front();
        return InputMacroDriverDecision{
            .status = InputMacroDriverStatus::PlanReady,
            .plan = WaitPlan(std::move(label), std::move(expected), hold),
        };
    }
    InputMacroDriverDecision Unexpected(std::string diagnostic)
    {
        return Fail(endresults::FailureCode::UnexpectedBreakpoint,
                    InputMacroFailure::UnexpectedBreakpoint,
                    std::move(diagnostic));
    }
    InputMacroDriverDecision Fail(endresults::FailureCode detailed,
                                  InputMacroFailure runtime,
                                  std::string diagnostic)
    {
        stage_ = Stage::Failed;
        failure_ = detailed;
        diagnostic_ = std::move(diagnostic);
        return FailedDecision(runtime);
    }
    InputMacroDriverDecision FailedDecision(InputMacroFailure failure) const
    {
        return {.status = InputMacroDriverStatus::Failed,
                .failure = failure,
                .diagnostic = diagnostic_};
    }
    InputMacroDriverDecision CompleteDecision() const
    {
        return {.status = InputMacroDriverStatus::Completed,
                .failure = InputMacroFailure::None};
    }

    Stage stage_{Stage::Idle};
    endresults::FailureCode failure_{endresults::FailureCode::None};
    std::string diagnostic_;
    completion::Manifest manifest_{};
    std::string manifest_blob_;
    BPKey last_expected_key_{0};
    BPKey last_hit_key_{0};
    std::uint32_t last_hit_pc_{0};
};

BattleCompletionInputMacroProvider::BattleCompletionInputMacroProvider()
    : impl_(std::make_unique<Impl>()) {}
BattleCompletionInputMacroProvider::~BattleCompletionInputMacroProvider() = default;
std::span<const BPKey> BattleCompletionInputMacroProvider::required_breakpoint_keys() noexcept
{ return kRequiredBreakpointKeys; }
std::span<const BPKey> BattleCompletionInputMacroProvider::declared_breakpoint_keys() const
{ return required_breakpoint_keys(); }
InputMacroDriverDecision BattleCompletionInputMacroProvider::Start(IInputMacroDriverHost& host)
{ return impl_->Start(host); }
InputMacroDriverDecision BattleCompletionInputMacroProvider::Advance(
    IInputMacroDriverHost& host, const InputMacroStepResult& result)
{ return impl_->Advance(host, result); }
void BattleCompletionInputMacroProvider::Cancel() noexcept { impl_->Cancel(); }
endresults::FailureCode BattleCompletionInputMacroProvider::failure() const noexcept
{ return impl_->failure(); }
const std::string& BattleCompletionInputMacroProvider::diagnostic() const noexcept
{ return impl_->diagnostic(); }
const completion::Manifest& BattleCompletionInputMacroProvider::manifest() const noexcept
{ return impl_->manifest(); }
const std::string& BattleCompletionInputMacroProvider::manifest_blob() const noexcept
{ return impl_->manifest_blob(); }
bool BattleCompletionInputMacroProvider::completed() const noexcept
{ return impl_->completed(); }
BattleCompletionInputMacroProvider::DiagnosticsSnapshot
BattleCompletionInputMacroProvider::diagnostics() const noexcept
{ return impl_->Diagnostics(); }

} // namespace savor::inputmacro
