#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "Phases/Programs/BattleEndResults/BattleEndResultsPayload.h"
#include "Phases/Programs/BattleEndResults/BattleEndResultsReport.h"
#include "Phases/Programs/BattleEndResults/BattleEndResultsScript.h"
#include "Phases/Programs/BattleCompletion/BattleCompletionManifest.h"
#include "Runner/IPC/Wire.h"
#include "Runner/InputMacro/Providers/BattleCompletionInputMacroProvider.h"
#include "Runner/InputMacro/Providers/BattleEndResultsInputMacroProvider.h"
#include "Runner/InputMacro/Providers/BattleResultsScreenInputMacroProvider.h"
#include "Runner/Script/CtxRegistry.h"

namespace savor {
namespace bp = ::bp;
}

namespace {

namespace endresults = phase::battle::endresults;
using savor::GCInputFrame;
using savor::inputmacro::BattleCompletionInputMacroProvider;
using savor::inputmacro::BattleEndResultsInputMacroProvider;
using savor::inputmacro::BattleResultsScreenInputMacroProvider;
using savor::inputmacro::BreakpointWaitAction;
using savor::inputmacro::IInputMacroDriverHost;
using savor::inputmacro::InputMacroActionKind;
using savor::inputmacro::InputMacroDriverDecision;
using savor::inputmacro::InputMacroDriverStatus;
using savor::inputmacro::InputMacroFailure;
using savor::inputmacro::InputMacroRuntimeState;
using savor::inputmacro::InputMacroStepResult;
using savor::inputmacro::InputMacroStopInfo;
using savor::inputmacro::InputMacroTerminalStatus;

struct FakeDriverHost final : IInputMacroDriverHost {
    InputMacroStopInfo stop{};
    std::unordered_map<std::uint32_t, std::uint8_t> memory;
    std::uint32_t failed_read_address{0};

    InputMacroStopInfo current_stop() const override { return stop; }

    bool read_guest_memory(
        std::uint32_t address,
        std::span<std::byte> output) const override
    {
        if (address < 0x80000000u
            || output.size() > static_cast<std::size_t>(0x81800000u - address)
            || (failed_read_address != 0 && address == failed_read_address)) {
            return false;
        }
        for (std::size_t i = 0; i < output.size(); ++i) {
            const auto it = memory.find(address + static_cast<std::uint32_t>(i));
            output[i] = static_cast<std::byte>(it == memory.end() ? 0u : it->second);
        }
        return true;
    }

    void PutU8(std::uint32_t address, std::uint8_t value) { memory[address] = value; }

    void PutU16(std::uint32_t address, std::uint16_t value)
    {
        PutU8(address, static_cast<std::uint8_t>(value >> 8));
        PutU8(address + 1, static_cast<std::uint8_t>(value));
    }

    void PutU32(std::uint32_t address, std::uint32_t value)
    {
        PutU8(address, static_cast<std::uint8_t>(value >> 24));
        PutU8(address + 1, static_cast<std::uint8_t>(value >> 16));
        PutU8(address + 2, static_cast<std::uint8_t>(value >> 8));
        PutU8(address + 3, static_cast<std::uint8_t>(value));
    }
};

const BreakpointWaitAction& Wait(const InputMacroDriverDecision& decision)
{
    EXPECT_EQ(decision.status, InputMacroDriverStatus::PlanReady);
    EXPECT_EQ(decision.plan.steps.size(), 1u);
    const auto* wait = std::get_if<BreakpointWaitAction>(&decision.plan.steps.front().action);
    EXPECT_NE(wait, nullptr);
    static const BreakpointWaitAction empty{};
    return wait == nullptr ? empty : *wait;
}

InputMacroStepResult Complete(
    FakeDriverHost& host,
    BPKey key,
    std::uint32_t pc,
    std::uint64_t sequence,
    std::uint64_t epoch,
    GCInputFrame input = {},
    bool acknowledged = true)
{
    host.stop = InputMacroStopInfo{
        .key = key,
        .pc = pc,
        .stop_sequence = sequence,
        .input_epoch = epoch,
        .requested_input = input,
        .input_poll_count = acknowledged ? 1u : 0u,
        .input_acknowledged = acknowledged,
    };
    return InputMacroStepResult{
        .state = InputMacroRuntimeState::Completed,
        .terminal_status = InputMacroTerminalStatus::Completed,
        .failure = InputMacroFailure::None,
        .step_completed = true,
        .action_kind = InputMacroActionKind::BreakpointWait,
        .hit_key = key,
        .hit_pc = pc,
        .stop_sequence = sequence,
        .input_epoch = epoch,
        .requested_input = input,
        .input_poll_count = acknowledged ? 1u : 0u,
        .input_acknowledged = acknowledged,
    };
}

void SeedStartMemory(FakeDriverHost& host)
{
    constexpr std::uint32_t controller0 = 0x80410000u;
    host.PutU32(BattleEndResultsInputMacroProvider::GuestAddresses::Controller0Pointer, controller0);
    host.PutU32(controller0, 0);
    host.PutU32(controller0 + 8u, 0);
    host.PutU32(BattleEndResultsInputMacroProvider::GuestAddresses::ControllerInfo0, 0);
    host.PutU32(BattleEndResultsInputMacroProvider::GuestAddresses::ControllerInfo0 + 4u, 0);
    host.PutU32(BattleEndResultsInputMacroProvider::GuestAddresses::ControllerInfo0 + 8u, 0);
    for (std::uint32_t character = 0; character < endresults::CharacterCount; ++character) {
        const auto base = BattleEndResultsInputMacroProvider::GuestAddresses::PcDataBase
            + character * BattleEndResultsInputMacroProvider::GuestAddresses::PcDataStride;
        host.PutU8(base + 0x0B, static_cast<std::uint8_t>(10 + character));
        host.PutU32(base + 0x24, 1000 + character);
        for (std::uint32_t element = 0; element < endresults::ElementCount; ++element) {
            host.PutU8(base + 0x34 + element, static_cast<std::uint8_t>(element));
            host.PutU32(base + 0x44 + element * 4, 100 + element);
        }
    }
}

void SeedCompletionRewards(FakeDriverHost& host)
{
    host.PutU32(BattleCompletionInputMacroProvider::GuestAddresses::NormalExperienceReward, 250);
    host.PutU32(BattleCompletionInputMacroProvider::GuestAddresses::MagicExperienceReward, 30);
    host.PutU32(BattleCompletionInputMacroProvider::GuestAddresses::GoldReward, 800);
    host.PutU16(BattleCompletionInputMacroProvider::GuestAddresses::RewardItemsBase, 0x0123u);
    host.PutU8(BattleCompletionInputMacroProvider::GuestAddresses::RewardItemsBase + 2u, 2u);
    host.PutU8(BattleCompletionInputMacroProvider::GuestAddresses::RewardItemsBase + 3u, 0u);
    for (std::uint32_t item = 1; item < phase::battle::completion::RewardItemCount; ++item) {
        const auto base = BattleCompletionInputMacroProvider::GuestAddresses::RewardItemsBase
            + item * BattleCompletionInputMacroProvider::GuestAddresses::RewardItemStride;
        host.PutU16(base, 0xFFFFu);
        host.PutU8(base + 2u, 0u);
        host.PutU8(base + 3u, 0u);
    }
}

InputMacroDriverDecision AdvanceCompletionToRewardCommit(
    BattleCompletionInputMacroProvider& provider,
    FakeDriverHost& host)
{
    auto decision = provider.Start(host);
    const auto& victory_wait = Wait(decision);
    EXPECT_EQ(victory_wait.expected_keys.size(), 2u);
    EXPECT_TRUE(victory_wait.hold_input_through_hit_opcode);

    host.PutU32(BattleCompletionInputMacroProvider::GuestAddresses::BattleInputState, 2u);
    decision = provider.Advance(
        host,
        Complete(
            host,
            savor::bp::battle::BattleEndVictorySlotsComplete,
            0x8006F590u,
            1u,
            1u));
    EXPECT_EQ(Wait(decision).expected_keys, (std::vector<BPKey>{
        savor::bp::battle::BattleEndRewardEntry}));

    decision = provider.Advance(
        host,
        Complete(
            host,
            savor::bp::battle::BattleEndRewardEntry,
            0x8006F598u,
            2u,
            2u));
    EXPECT_EQ(Wait(decision).expected_keys, (std::vector<BPKey>{
        savor::bp::battle::BattleEndRewardCommitComplete}));
    return decision;
}

std::string MakeCompletionManifest()
{
    phase::battle::completion::Manifest manifest{};
    manifest.invariant_flags =
        phase::battle::completion::ManifestInvariantPreCaptured
        | phase::battle::completion::ManifestInvariantPostCaptured
        | phase::battle::completion::ManifestInvariantRewardItemsCaptured;
    std::string error;
    EXPECT_TRUE(phase::battle::completion::DeriveExpectedView(manifest, &error))
        << error;
    std::string bytes;
    EXPECT_TRUE(phase::battle::completion::EncodeManifest(manifest, bytes));
    return bytes;
}

InputMacroDriverDecision AdvanceToFirstDispatch(
    BattleEndResultsInputMacroProvider& provider,
    FakeDriverHost& host)
{
    auto decision = provider.Start(host);
    EXPECT_EQ(Wait(decision).expected_keys.size(), 2u);

    host.PutU32(BattleEndResultsInputMacroProvider::GuestAddresses::BattleInputState, 2);
    decision = provider.Advance(
        host,
        Complete(
            host,
            savor::bp::battle::BattleEndVictoryCountdownComplete,
            0x8006F554u,
            1,
            1));
    EXPECT_EQ(Wait(decision).expected_keys.front(), savor::bp::battle::BattleEndRewardEntry);

    decision = provider.Advance(
        host,
        Complete(
            host,
            savor::bp::battle::BattleEndRewardEntry,
            0x8006F598u,
            2,
            2));
    EXPECT_EQ(Wait(decision).expected_keys.front(), savor::bp::battle::BattleEndRewardCommitComplete);

    host.PutU32(BattleEndResultsInputMacroProvider::GuestAddresses::RewardPhase, 6);
    decision = provider.Advance(
        host,
        Complete(
            host,
            savor::bp::battle::BattleEndRewardCommitComplete,
            0x8006FD58u,
            3,
            3));
    EXPECT_EQ(Wait(decision).expected_keys.front(), savor::bp::battle::BattleEndResultDispatch);
    return decision;
}

void SeedSettledConfirmResult(FakeDriverHost& host, std::uint32_t result_object)
{
    host.PutU32(BattleEndResultsInputMacroProvider::GuestAddresses::ResultObjectPointer, result_object);
    host.PutU32(result_object + 0x04, 13);
    host.PutU32(result_object + 0x08, 0);
    host.PutU32(result_object + 0x0C, 0);
}

void AdvanceToConfirmReleaseWitness(
    BattleEndResultsInputMacroProvider& provider,
    FakeDriverHost& host,
    std::uint32_t result_object)
{
    AdvanceToFirstDispatch(provider, host);
    SeedSettledConfirmResult(host, result_object);
    auto decision = provider.Advance(
        host,
        Complete(host, savor::bp::battle::BattleEndResultDispatch, 0x800E4660u, 3, 3));
    ASSERT_EQ(Wait(decision).expected_keys.front(), savor::bp::battle::BattleEndResultConfirmReady);
    decision = provider.Advance(
        host,
        Complete(host, savor::bp::battle::BattleEndResultConfirmReady, 0x800E6128u, 4, 4));
    ASSERT_EQ(Wait(decision).expected_keys.front(), savor::bp::battle::BattleEndResultConfirmAccepted);
    GCInputFrame press_a{};
    press_a.A();
    decision = provider.Advance(
        host,
        Complete(host, savor::bp::battle::BattleEndResultConfirmAccepted, 0x800E6138u, 5, 5, press_a));
    ASSERT_EQ(Wait(decision).expected_keys.front(), savor::bp::battle::BattleEndController0NeutralCopied);
}

TEST(BattleEndResultsReport, VersionOneRoundTripsExpectedAndObservedData)
{
    endresults::Report report{};
    report.policy = endresults::AccelerationPolicy::FullAdaptive;
    report.outcome = endresults::Outcome::Completed;
    report.expected.characters[0].character_id = 0;
    report.expected.characters[0].level_before = 10;
    report.expected.characters[0].level_after = 11;
    report.expected.characters[0].elements[2] = {
        .xp_before = 120,
        .xp_after = 180,
        .rank_before = 2,
        .rank_after = 3,
    };
    report.expected.characters[0].learned_magic_ids = {4, 9};
    report.expected.expected_stat_waves = 3;
    report.expected.expected_learned_waves = 1;
    report.actions.push_back(endresults::ActionTrace{
        .kind = endresults::ActionKind::MandatoryConfirm,
        .state = 13,
        .occurrence_token = 42,
        .ready_key = savor::bp::battle::BattleEndResultConfirmReady,
        .accepted_key = savor::bp::battle::BattleEndResultConfirmAccepted,
        .ready_pc = 0x800E6128u,
        .accepted_pc = 0x800E6138u,
        .neutral_epoch = 7,
        .input_epoch = 8,
        .release_epoch = 9,
        .input_poll_count = 1,
        .input_acknowledged = true,
        .release_observed = true,
        .progress_observed = true,
    });

    std::string bytes;
    ASSERT_TRUE(endresults::EncodeReport(report, bytes));
    ASSERT_GE(bytes.size(), 8u);
    EXPECT_EQ(bytes.substr(0, 4), "BERB");
    endresults::Report decoded{};
    ASSERT_TRUE(endresults::DecodeReport(bytes, decoded));
    EXPECT_EQ(decoded.policy, report.policy);
    EXPECT_EQ(decoded.expected.characters[0].level_after, 11);
    EXPECT_EQ(decoded.expected.characters[0].elements[2].rank_after, 3);
    EXPECT_EQ(decoded.expected.characters[0].learned_magic_ids, (std::vector<std::uint8_t>{4, 9}));
    ASSERT_EQ(decoded.actions.size(), 1u);
    EXPECT_TRUE(decoded.actions[0].release_observed);
    EXPECT_TRUE(decoded.actions[0].progress_observed);
}

TEST(BattleCompletionManifest, VersionOneRoundTripsRawProgressAndDerivedMagic)
{
    phase::battle::completion::Manifest manifest{};
    manifest.invariant_flags =
        phase::battle::completion::ManifestInvariantPreCaptured
        | phase::battle::completion::ManifestInvariantPostCaptured
        | phase::battle::completion::ManifestInvariantRewardItemsCaptured;
    manifest.pre_character_records[0][0x0B] = 10;
    manifest.post_character_records[0][0x0B] = 11;
    manifest.post_character_records[0][0x34] = 1;
    manifest.normal_experience_reward = 10;
    manifest.reward_items[0] = {.item_id = 7, .quantity = 1, .opaque = 0x55};
    std::string error;
    ASSERT_TRUE(phase::battle::completion::DeriveExpectedView(manifest, &error))
        << error;
    EXPECT_EQ(manifest.presentation.level_panels, 1u);
    EXPECT_EQ(manifest.presentation.stat_waves, 3u);
    EXPECT_EQ(manifest.presentation.learned_magic_waves, 1u);
    ASSERT_EQ(manifest.expected.characters[0].learned_magic_ids,
        (std::vector<std::uint8_t>{0}));

    std::string bytes;
    ASSERT_TRUE(phase::battle::completion::EncodeManifest(manifest, bytes));
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes.substr(0, 4), "BCMB");
    phase::battle::completion::Manifest decoded{};
    ASSERT_TRUE(phase::battle::completion::DecodeManifest(bytes, decoded));
    EXPECT_EQ(decoded.post_character_records[0][0x0B], 11);
    EXPECT_EQ(decoded.reward_items[0].item_id, 7);
    EXPECT_EQ(decoded.expected.characters[0].learned_magic_ids,
        (std::vector<std::uint8_t>{0}));
}

TEST(BattleEndResultsPayload, VersionThreeRequiresManifestPolicyAndOutputPath)
{
    endresults::EncodeSpec spec{
        .acceleration_policy = endresults::AccelerationPolicy::FullAdaptive,
        .completion_manifest_blob = MakeCompletionManifest(),
        .output_savestate_path = "battle-end.sav",
    };
    std::vector<std::uint8_t> payload;
    ASSERT_TRUE(endresults::encode_payload(spec, payload));
    ASSERT_EQ(payload.front(), savor::PK_BattleEndResultsRunner);

    savor::PSContext context;
    ASSERT_TRUE(endresults::decode_payload(payload, context));
    std::uint32_t value = 0;
    EXPECT_TRUE(context.get(savor::context::key::battleend::ACCELERATION_POLICY, value));
    EXPECT_EQ(value, static_cast<std::uint32_t>(endresults::AccelerationPolicy::FullAdaptive));

    spec.output_savestate_path.clear();
    EXPECT_FALSE(endresults::encode_payload(spec, payload));

    auto legacy = payload;
    ASSERT_TRUE(endresults::encode_payload(
        endresults::EncodeSpec{
            .acceleration_policy =
                endresults::AccelerationPolicy::FullAdaptive,
            .completion_manifest_blob = MakeCompletionManifest(),
            .output_savestate_path = "battle-end.sav",
        },
        legacy));
    legacy[1] = 2;
    EXPECT_FALSE(endresults::decode_payload(legacy, context));
}

TEST(BattleCompletionProvider, CapturesPreAndPostStateAndStopsAtRewardCommit)
{
    FakeDriverHost host;
    SeedStartMemory(host);
    host.stop = {
        .key = savor::bp::battle::EndBattleVictory,
        .pc = 0x800706D8u,
    };
    BattleCompletionInputMacroProvider provider;
    AdvanceCompletionToRewardCommit(provider, host);

    const auto first_character =
        BattleCompletionInputMacroProvider::GuestAddresses::PcDataBase;
    host.PutU8(first_character + 0x0Bu, 11u);
    host.PutU32(first_character + 0x24u, 1250u);
    SeedCompletionRewards(host);
    host.PutU32(BattleCompletionInputMacroProvider::GuestAddresses::RewardPhase, 6u);

    const auto final_result = Complete(
        host,
        savor::bp::battle::BattleEndRewardCommitComplete,
        0x8006FD58u,
        3u,
        3u);
    const auto final_stop = host.stop;
    const auto decision = provider.Advance(host, final_result);

    EXPECT_EQ(decision.status, InputMacroDriverStatus::Completed);
    EXPECT_TRUE(provider.completed());
    EXPECT_EQ(provider.failure(), endresults::FailureCode::None);
    EXPECT_EQ(host.stop.key, savor::bp::battle::BattleEndRewardCommitComplete);
    EXPECT_EQ(host.stop.pc, 0x8006FD58u);
    EXPECT_EQ(host.stop.stop_sequence, final_stop.stop_sequence);

    phase::battle::completion::Manifest decoded{};
    ASSERT_FALSE(provider.manifest_blob().empty());
    ASSERT_TRUE(phase::battle::completion::DecodeManifest(
        provider.manifest_blob(), decoded));
    EXPECT_NE(decoded.pre_character_records[0], decoded.post_character_records[0]);
    EXPECT_EQ(decoded.normal_experience_reward, 250u);
    EXPECT_EQ(decoded.magic_experience_reward, 30u);
    EXPECT_EQ(decoded.gold_reward, 800u);
    EXPECT_EQ(decoded.reward_items[0].item_id, 0x0123);
    EXPECT_EQ(decoded.reward_items[0].quantity, 2u);
    EXPECT_EQ(
        decoded.invariant_flags & phase::battle::completion::RequiredManifestInvariants,
        phase::battle::completion::RequiredManifestInvariants);
}

TEST(BattleCompletionProvider, RejectsWrongSource)
{
    FakeDriverHost host;
    SeedStartMemory(host);
    host.stop = {
        .key = savor::bp::battle::EndBattleVictory,
        .pc = 0x800706DCu,
    };
    BattleCompletionInputMacroProvider provider;
    const auto decision = provider.Start(host);
    EXPECT_EQ(decision.status, InputMacroDriverStatus::Failed);
    EXPECT_EQ(provider.failure(), endresults::FailureCode::InvalidSource);
}

TEST(BattleCompletionProvider, RejectsBadRewardPhase)
{
    FakeDriverHost host;
    SeedStartMemory(host);
    host.stop = {
        .key = savor::bp::battle::EndBattleVictory,
        .pc = 0x800706D8u,
    };
    BattleCompletionInputMacroProvider provider;
    AdvanceCompletionToRewardCommit(provider, host);
    host.PutU32(BattleCompletionInputMacroProvider::GuestAddresses::RewardPhase, 5u);
    const auto decision = provider.Advance(
        host,
        Complete(
            host,
            savor::bp::battle::BattleEndRewardCommitComplete,
            0x8006FD58u,
            3u,
            3u));
    EXPECT_EQ(decision.status, InputMacroDriverStatus::Failed);
    EXPECT_EQ(provider.failure(), endresults::FailureCode::RewardInvariantMismatch);
}

TEST(BattleCompletionProvider, RejectsRewardPhaseReadFailure)
{
    FakeDriverHost host;
    SeedStartMemory(host);
    host.stop = {
        .key = savor::bp::battle::EndBattleVictory,
        .pc = 0x800706D8u,
    };
    BattleCompletionInputMacroProvider provider;
    AdvanceCompletionToRewardCommit(provider, host);
    host.failed_read_address =
        BattleCompletionInputMacroProvider::GuestAddresses::RewardPhase;
    const auto decision = provider.Advance(
        host,
        Complete(
            host,
            savor::bp::battle::BattleEndRewardCommitComplete,
            0x8006FD58u,
            3u,
            3u));
    EXPECT_EQ(decision.status, InputMacroDriverStatus::Failed);
    EXPECT_EQ(provider.failure(), endresults::FailureCode::MemoryReadFailed);
}

TEST(BattleEndResultsProvider, RejectsSourceThatIsNotExactVictoryBreakpoint)
{
    FakeDriverHost host;
    SeedStartMemory(host);
    host.stop = {
        .key = savor::bp::battle::EndBattleVictory,
        .pc = 0x800706DCu,
    };
    BattleEndResultsInputMacroProvider provider;
    const auto decision = provider.Start(host);
    EXPECT_EQ(decision.status, InputMacroDriverStatus::Failed);
    EXPECT_EQ(provider.failure(), endresults::FailureCode::InvalidSource);
    EXPECT_FALSE(provider.report_blob().empty());
}

TEST(BattleResultsScreenProvider, StartsAtFieldReseedAndValidatesDescriptorManifest)
{
    FakeDriverHost host;
    SeedStartMemory(host);
    host.stop = {
        .key = savor::bp::battle::BattleEndFieldReturnReseedComplete,
        .pc = 0x801012B4u,
    };
    host.PutU32(BattleResultsScreenInputMacroProvider::GuestAddresses::RngSeed,
                0x12345678u);
    BattleResultsScreenInputMacroProvider provider({
        .acceleration_policy = endresults::AccelerationPolicy::FullAdaptive,
        .completion_manifest_blob = MakeCompletionManifest(),
    });
    auto decision = provider.Start(host);
    ASSERT_EQ(decision.status, InputMacroDriverStatus::PlanReady);
    ASSERT_EQ(Wait(decision).expected_keys,
        (std::vector<BPKey>{savor::bp::battle::BattleEndResultDescriptorReady}));

    constexpr std::uint32_t result_object = 0x80400000u;
    host.PutU32(BattleResultsScreenInputMacroProvider::GuestAddresses::GameMode, 6);
    host.PutU32(BattleResultsScreenInputMacroProvider::GuestAddresses::FieldControllerState, 4);
    host.PutU32(BattleResultsScreenInputMacroProvider::GuestAddresses::ResultObjectPointer,
                result_object);
    host.PutU32(result_object + 0x0C, 0);
    host.PutU16(result_object + 0x28, 0xFFFFu);
    host.PutU16(result_object + 0x2C, 0xFFFFu);
    host.PutU16(result_object + 0x30, 0xFFFFu);
    decision = provider.Advance(
        host,
        Complete(host, savor::bp::battle::BattleEndResultDescriptorReady,
                 0x800E35F0u, 1, 1, {}, false));
    ASSERT_EQ(decision.status, InputMacroDriverStatus::PlanReady);
    EXPECT_EQ(Wait(decision).expected_keys.front(),
              savor::bp::battle::BattleEndResultDispatch);

    host.PutU32(result_object + 0x04, 0);
    host.PutU32(result_object + 0x08, 0);
    decision = provider.Advance(
        host,
        Complete(host, savor::bp::battle::BattleEndResultDispatch,
                 0x800E4660u, 2, 2));
    ASSERT_EQ(decision.status, InputMacroDriverStatus::PlanReady);
    EXPECT_EQ(Wait(decision).expected_keys.front(),
              savor::bp::battle::BattleEndResultIntroReady);

    // The ready breakpoint can immediately follow the acknowledged neutral
    // dispatch without another guest controller poll. The prior dispatch
    // epoch is the causal neutral rearm for the following A edge.
    decision = provider.Advance(
        host,
        Complete(host, savor::bp::battle::BattleEndResultIntroReady,
                 0x800E46BCu, 3, 3, {}, false));
    ASSERT_EQ(decision.status, InputMacroDriverStatus::PlanReady);
    EXPECT_EQ(Wait(decision).expected_keys.front(),
              savor::bp::battle::BattleEndResultIntroAccepted);
    EXPECT_EQ(Wait(decision).input.buttons, savor::GC_A);
}

TEST(BattleEndResultsProvider, RejectsVictoryCompletionWithoutPublishedBattleInputState)
{
    FakeDriverHost host;
    SeedStartMemory(host);
    host.stop = {
        .key = savor::bp::battle::EndBattleVictory,
        .pc = 0x800706D8u,
    };
    BattleEndResultsInputMacroProvider provider;
    const auto start = provider.Start(host);
    ASSERT_EQ(start.status, InputMacroDriverStatus::PlanReady);
    ASSERT_EQ(start.plan.steps.size(), 1u);
    const auto* wait = std::get_if<BreakpointWaitAction>(&start.plan.steps.front().action);
    ASSERT_NE(wait, nullptr);
    EXPECT_TRUE(wait->hold_input_through_hit_opcode);
    const auto decision = provider.Advance(
        host,
        Complete(
            host,
            savor::bp::battle::BattleEndVictorySlotsComplete,
            0x8006F590u,
            1,
            1));
    EXPECT_EQ(decision.status, InputMacroDriverStatus::Failed);
    EXPECT_EQ(provider.failure(), endresults::FailureCode::VictoryInvariantMismatch);
}

TEST(BattleEndResultsProvider, RejectsRewardCommitOutsideQualifiedRewardPhase)
{
    FakeDriverHost host;
    SeedStartMemory(host);
    host.stop = {
        .key = savor::bp::battle::EndBattleVictory,
        .pc = 0x800706D8u,
    };
    BattleEndResultsInputMacroProvider provider;
    ASSERT_EQ(provider.Start(host).status, InputMacroDriverStatus::PlanReady);
    host.PutU32(BattleEndResultsInputMacroProvider::GuestAddresses::BattleInputState, 2);
    ASSERT_EQ(
        provider.Advance(
            host,
            Complete(
                host,
                savor::bp::battle::BattleEndVictoryCountdownComplete,
                0x8006F554u,
                1,
                1)).status,
        InputMacroDriverStatus::PlanReady);
    ASSERT_EQ(
        provider.Advance(
            host,
            Complete(
                host,
                savor::bp::battle::BattleEndRewardEntry,
                0x8006F598u,
                2,
                2)).status,
        InputMacroDriverStatus::PlanReady);
    const auto decision = provider.Advance(
        host,
        Complete(
            host,
            savor::bp::battle::BattleEndRewardCommitComplete,
            0x8006FD58u,
            3,
            3));
    EXPECT_EQ(decision.status, InputMacroDriverStatus::Failed);
    EXPECT_EQ(provider.failure(), endresults::FailureCode::RewardInvariantMismatch);
}

TEST(BattleEndResultsProvider, QualifiesIndependentLiveStatDescriptorPairs)
{
    struct Case {
        std::uint32_t state;
        std::uint16_t kind;
        std::uint8_t mask;
    };
    constexpr std::array cases{
        Case{6, 2, 6},
        Case{6, 3, 4},
        Case{6, 4, 0},
        Case{7, 2, 6},
        Case{8, 3, 4},
    };

    for (const auto& value : cases) {
        SCOPED_TRACE(value.state);
        FakeDriverHost host;
        SeedStartMemory(host);
        host.stop = {
            .key = savor::bp::battle::EndBattleVictory,
            .pc = 0x800706D8u,
        };
        BattleEndResultsInputMacroProvider provider;
        AdvanceToFirstDispatch(provider, host);

        constexpr std::uint32_t result_object = 0x80400000u;
        host.PutU32(BattleEndResultsInputMacroProvider::GuestAddresses::ResultObjectPointer, result_object);
        host.PutU32(result_object + 0x04, value.state);
        host.PutU32(result_object + 0x0C, 1);
        host.PutU8(result_object + 0x1C2, value.mask);
        // D=P+2C8: sel=0, kind=D+4, ctrl=D+8. Row 0 requires ctrl 4.
        host.PutU16(result_object + 0x2C8, 0);
        host.PutU16(result_object + 0x2CC, value.kind);
        host.PutU16(result_object + 0x2D0, 4);
        host.PutU16(result_object + 0x580 + 4 * 0x6C + 0x46, 1);
        for (std::uint32_t i = 0; i < 36; ++i) host.PutU8(result_object + 0x1DE + i, 0xFF);

        const auto decision = provider.Advance(
            host,
            Complete(host, savor::bp::battle::BattleEndResultDispatch, 0x800E4660u, 3, 3));
        ASSERT_EQ(decision.status, InputMacroDriverStatus::PlanReady);
        EXPECT_EQ(Wait(decision).expected_keys.front(), savor::bp::battle::BattleEndResultStatWaveReady);
    }
}

TEST(BattleEndResultsProvider, QualifiesFinalLearnedWaveAfterQueueCountReachesZero)
{
    FakeDriverHost host;
    SeedStartMemory(host);
    host.stop = {
        .key = savor::bp::battle::EndBattleVictory,
        .pc = 0x800706D8u,
    };
    BattleEndResultsInputMacroProvider provider;
    AdvanceToFirstDispatch(provider, host);

    constexpr std::uint32_t result_object = 0x80400000u;
    host.PutU32(BattleEndResultsInputMacroProvider::GuestAddresses::ResultObjectPointer, result_object);
    host.PutU32(result_object + 0x04, 11);
    host.PutU32(result_object + 0x0C, 1);
    host.PutU8(result_object + 0x1D7, 0); // The final wave has already been dequeued.
    host.PutU16(result_object + 0x2C8, 0);
    host.PutU16(result_object + 0x2CC, 6);
    host.PutU16(result_object + 0x2D0, 4);
    host.PutU16(result_object + 0x580 + 4 * 0x6C + 0x46, 1);

    const auto decision = provider.Advance(
        host,
        Complete(host, savor::bp::battle::BattleEndResultDispatch, 0x800E4660u, 3, 3));
    ASSERT_EQ(decision.status, InputMacroDriverStatus::PlanReady);
    EXPECT_EQ(
        Wait(decision).expected_keys.front(),
        savor::bp::battle::BattleEndResultLearnedWaveReady);
}

TEST(BattleEndResultsProvider, FullAdaptiveLeavesFinalFadeNeutral)
{
    FakeDriverHost host;
    SeedStartMemory(host);
    host.stop = {
        .key = savor::bp::battle::EndBattleVictory,
        .pc = 0x800706D8u,
    };
    BattleEndResultsInputMacroProvider provider;
    AdvanceToFirstDispatch(provider, host);

    constexpr std::uint32_t result_object = 0x80400000u;
    host.PutU32(BattleEndResultsInputMacroProvider::GuestAddresses::ResultObjectPointer, result_object);
    host.PutU32(result_object + 0x04, 14);
    host.PutU32(result_object + 0x0C, 0);

    const auto decision = provider.Advance(
        host,
        Complete(host, savor::bp::battle::BattleEndResultDispatch, 0x800E4660u, 3, 3));
    ASSERT_EQ(decision.status, InputMacroDriverStatus::PlanReady);
    EXPECT_EQ(
        Wait(decision).expected_keys,
        (std::vector<BPKey>{
            savor::bp::battle::BattleEndResultDispatch,
            savor::bp::battle::BattleEndResultLifecycleExit}));
}

TEST(BattleEndResultsProvider, GoldAccelerationArmsBeforeFirstStateTwoControllerRead)
{
    for (const auto remaining : std::array<std::uint32_t, 2>{{1u, 14u}}) {
        SCOPED_TRACE(remaining);
        FakeDriverHost host;
        SeedStartMemory(host);
        host.stop = {
            .key = savor::bp::battle::EndBattleVictory,
            .pc = 0x800706D8u,
        };
        BattleEndResultsInputMacroProvider provider;
        AdvanceToFirstDispatch(provider, host);

        constexpr std::uint32_t result_object = 0x80400000u;
        host.PutU32(BattleEndResultsInputMacroProvider::GuestAddresses::ResultObjectPointer, result_object);
        host.PutU32(result_object + 0x04, 1);
        host.PutU32(result_object + 0x0C, 0);

        auto decision = provider.Advance(
            host,
            Complete(host, savor::bp::battle::BattleEndResultDispatch, 0x800E4660u, 3, 3));
        ASSERT_EQ(decision.status, InputMacroDriverStatus::PlanReady);
        ASSERT_EQ(Wait(decision).expected_keys.size(), 1u);
        EXPECT_EQ(Wait(decision).expected_keys.front(), savor::bp::battle::BattleEndResultGoldArmed);
        EXPECT_EQ(Wait(decision).input.buttons, 0);

        host.PutU32(result_object + 0x04, 2);
        host.PutU32(result_object + 0x18, 100);
        host.PutU32(result_object + 0x24, 100 - remaining);
        decision = provider.Advance(
            host,
            Complete(host, savor::bp::battle::BattleEndResultGoldArmed, 0x800E488Cu, 4, 4));
        ASSERT_EQ(decision.status, InputMacroDriverStatus::PlanReady);
        ASSERT_EQ(Wait(decision).expected_keys.size(), 1u);
        EXPECT_EQ(Wait(decision).expected_keys.front(), savor::bp::battle::BattleEndResultGoldAccepted);
        EXPECT_EQ(Wait(decision).input.buttons, savor::GC_A);
    }
}

TEST(BattleEndResultsProvider, InactiveDescriptorSelectorRemainsNeutral)
{
    FakeDriverHost host;
    SeedStartMemory(host);
    host.stop = {
        .key = savor::bp::battle::EndBattleVictory,
        .pc = 0x800706D8u,
    };
    BattleEndResultsInputMacroProvider provider;
    AdvanceToFirstDispatch(provider, host);

    constexpr std::uint32_t result_object = 0x80400000u;
    host.PutU32(BattleEndResultsInputMacroProvider::GuestAddresses::ResultObjectPointer, result_object);
    host.PutU32(result_object + 0x04, 11);
    host.PutU32(result_object + 0x0C, 1);
    host.PutU8(result_object + 0x1D7, 1);
    host.PutU16(result_object + 0x2C8, 3); // No active selector at this instant.

    const auto decision = provider.Advance(
        host,
        Complete(host, savor::bp::battle::BattleEndResultDispatch, 0x800E4660u, 3, 3));
    ASSERT_EQ(decision.status, InputMacroDriverStatus::PlanReady);
    EXPECT_EQ(
        Wait(decision).expected_keys,
        (std::vector<BPKey>{
            savor::bp::battle::BattleEndResultDispatch,
            savor::bp::battle::BattleEndResultLifecycleExit}));
}

TEST(BattleEndResultsProvider, TerminalOptionalTicksRemainNeutral)
{
    enum class Case {
        Intro,
        NormalExp,
        MagicEntry,
        MagicExp,
        ItemPopup,
    };
    constexpr std::array cases{
        Case::Intro,
        Case::NormalExp,
        Case::MagicEntry,
        Case::MagicExp,
        Case::ItemPopup,
    };

    for (const auto test_case : cases) {
        SCOPED_TRACE(static_cast<int>(test_case));
        FakeDriverHost host;
        SeedStartMemory(host);
        host.stop = {
            .key = savor::bp::battle::EndBattleVictory,
            .pc = 0x800706D8u,
        };
        BattleEndResultsInputMacroProvider provider;
        AdvanceToFirstDispatch(provider, host);

        constexpr std::uint32_t result_object = 0x80400000u;
        host.PutU32(BattleEndResultsInputMacroProvider::GuestAddresses::ResultObjectPointer, result_object);
        host.PutU32(result_object + 0x0C, 0);
        switch (test_case) {
        case Case::Intro:
            host.PutU32(result_object + 0x04, 0);
            host.PutU32(result_object + 0x40, 0x55);
            break;
        case Case::NormalExp:
            host.PutU32(result_object + 0x04, 4);
            host.PutU32(result_object + 0x40, 0x1E);
            break;
        case Case::MagicEntry:
            host.PutU32(result_object + 0x04, 9);
            host.PutU32(result_object + 0x08, 0);
            break;
        case Case::MagicExp:
            host.PutU32(result_object + 0x04, 9);
            host.PutU32(result_object + 0x08, 1);
            host.PutU32(result_object + 0x40, 0x1E);
            break;
        case Case::ItemPopup:
            host.PutU32(result_object + 0x04, 12);
            host.PutU16(result_object + 0x28, 1);
            host.PutU16(result_object + 0x0EA2, 0);
            break;
        }

        const auto decision = provider.Advance(
            host,
            Complete(host, savor::bp::battle::BattleEndResultDispatch, 0x800E4660u, 3, 3));
        ASSERT_EQ(decision.status, InputMacroDriverStatus::PlanReady);
        EXPECT_EQ(
            Wait(decision).expected_keys,
            (std::vector<BPKey>{
                savor::bp::battle::BattleEndResultDispatch,
                savor::bp::battle::BattleEndResultLifecycleExit}));
    }
}

TEST(BattleEndResultsProvider, RejectsMismatchedStatDescriptorControlMapping)
{
    FakeDriverHost host;
    SeedStartMemory(host);
    host.stop = {
        .key = savor::bp::battle::EndBattleVictory,
        .pc = 0x800706D8u,
    };
    BattleEndResultsInputMacroProvider provider;
    AdvanceToFirstDispatch(provider, host);

    constexpr std::uint32_t result_object = 0x80400000u;
    host.PutU32(BattleEndResultsInputMacroProvider::GuestAddresses::ResultObjectPointer, result_object);
    host.PutU32(result_object + 0x04, 6);
    host.PutU32(result_object + 0x0C, 1);
    host.PutU8(result_object + 0x1C2, 6);
    host.PutU16(result_object + 0x2C8, 0);
    host.PutU16(result_object + 0x2CC, 3); // State 6 requires kind 2.
    host.PutU16(result_object + 0x2D0, 4);
    host.PutU16(result_object + 0x580 + 4 * 0x6C + 0x46, 1);
    const auto decision = provider.Advance(
        host,
        Complete(host, savor::bp::battle::BattleEndResultDispatch, 0x800E4660u, 3, 3));
    EXPECT_EQ(decision.status, InputMacroDriverStatus::Failed);
    EXPECT_EQ(provider.failure(), endresults::FailureCode::QualificationMismatch);
}

TEST(BattleEndResultsProvider, RecognizesClearedPresentationControlsAsProgress)
{
    enum class Case { MagicEntry, ItemPopup };
    for (const auto test_case : std::array{Case::MagicEntry, Case::ItemPopup}) {
        SCOPED_TRACE(static_cast<int>(test_case));
        FakeDriverHost host;
        SeedStartMemory(host);
        host.stop = {
            .key = savor::bp::battle::EndBattleVictory,
            .pc = 0x800706D8u,
        };
        BattleEndResultsInputMacroProvider provider;
        AdvanceToFirstDispatch(provider, host);

        constexpr std::uint32_t result_object = 0x80400000u;
        host.PutU32(BattleEndResultsInputMacroProvider::GuestAddresses::ResultObjectPointer, result_object);
        host.PutU32(result_object + 0x0C, test_case == Case::MagicEntry ? 1u : 0u);
        BPKey ready_key = 0;
        std::uint32_t ready_pc = 0;
        BPKey accepted_key = 0;
        std::uint32_t accepted_pc = 0;
        std::uint32_t live_control_address = 0;
        if (test_case == Case::MagicEntry) {
            host.PutU32(result_object + 0x04, 9);
            host.PutU32(result_object + 0x08, 0);
            host.PutU16(result_object + 0x2C8, 0);
            host.PutU16(result_object + 0x2CC, 5);
            host.PutU16(result_object + 0x2D0, 4);
            live_control_address = result_object + 0x580 + 4 * 0x6C + 0x46;
            ready_key = savor::bp::battle::BattleEndResultMagicEntryReady;
            ready_pc = 0x800E52D8u;
            accepted_key = savor::bp::battle::BattleEndResultMagicEntryAccepted;
            accepted_pc = 0x800E52E8u;
        } else {
            host.PutU32(result_object + 0x04, 12);
            host.PutU16(result_object + 0x28, 1);
            live_control_address = result_object + 0x0EA2;
            ready_key = savor::bp::battle::BattleEndResultItemPopupReady;
            ready_pc = 0x800E5F80u;
            accepted_key = savor::bp::battle::BattleEndResultItemPopupAccepted;
            accepted_pc = 0x800E5F90u;
        }
        host.PutU16(live_control_address, 1);

        auto decision = provider.Advance(
            host,
            Complete(host, savor::bp::battle::BattleEndResultDispatch, 0x800E4660u, 3, 3));
        ASSERT_EQ(Wait(decision).expected_keys.front(), ready_key);
        decision = provider.Advance(host, Complete(host, ready_key, ready_pc, 4, 4));
        ASSERT_EQ(Wait(decision).expected_keys.front(), accepted_key);

        GCInputFrame press_a{};
        press_a.A();
        decision = provider.Advance(
            host,
            Complete(host, accepted_key, accepted_pc, 5, 5, press_a));
        ASSERT_EQ(
            Wait(decision).expected_keys.front(),
            savor::bp::battle::BattleEndController0NeutralCopied);
        decision = provider.Advance(
            host,
            Complete(
                host,
                savor::bp::battle::BattleEndController0NeutralCopied,
                0x801C7948u,
                6,
                6));
        ASSERT_EQ(Wait(decision).expected_keys.front(), savor::bp::battle::BattleEndResultDispatch);

        host.PutU16(live_control_address, 0);
        decision = provider.Advance(
            host,
            Complete(host, savor::bp::battle::BattleEndResultDispatch, 0x800E4660u, 7, 7));
        EXPECT_EQ(decision.status, InputMacroDriverStatus::PlanReady);
        ASSERT_EQ(provider.report().actions.size(), 1u);
        EXPECT_TRUE(provider.report().actions.front().progress_observed);
    }
}

TEST(BattleEndResultsProvider, RequiredOnlyConfirmsAndCompletesAtExactCleanupBoundary)
{
    FakeDriverHost host;
    SeedStartMemory(host);
    host.stop = {
        .key = savor::bp::battle::EndBattleVictory,
        .pc = 0x800706D8u,
    };
    BattleEndResultsInputMacroProvider provider({
        .acceleration_policy = endresults::AccelerationPolicy::RequiredOnly,
    });
    AdvanceToFirstDispatch(provider, host);

    constexpr std::uint32_t result_object = 0x80400000u;
    host.PutU32(BattleEndResultsInputMacroProvider::GuestAddresses::ResultObjectPointer, result_object);
    host.PutU32(result_object + 0x04, 13);
    host.PutU32(result_object + 0x08, 0);
    host.PutU32(result_object + 0x0C, 0);

    auto decision = provider.Advance(
        host,
        Complete(host, savor::bp::battle::BattleEndResultDispatch, 0x800E4660u, 3, 3));
    EXPECT_EQ(Wait(decision).expected_keys.front(), savor::bp::battle::BattleEndResultConfirmReady);

    decision = provider.Advance(
        host,
        Complete(host, savor::bp::battle::BattleEndResultConfirmReady, 0x800E6128u, 4, 4));
    EXPECT_EQ(Wait(decision).expected_keys.front(), savor::bp::battle::BattleEndResultConfirmAccepted);
    EXPECT_EQ(Wait(decision).input.buttons, savor::GC_A);

    GCInputFrame press_a{};
    press_a.A();
    decision = provider.Advance(
        host,
        Complete(host, savor::bp::battle::BattleEndResultConfirmAccepted, 0x800E6138u, 5, 5, press_a));
    EXPECT_EQ(Wait(decision).expected_keys.front(), savor::bp::battle::BattleEndController0NeutralCopied);

    constexpr std::uint32_t controller = 0x80500000u;
    host.PutU32(BattleEndResultsInputMacroProvider::GuestAddresses::Controller0Pointer, controller);
    host.PutU32(controller + 0x08, 0);
    host.PutU32(
        BattleEndResultsInputMacroProvider::GuestAddresses::ControllerInfo0 + 4u,
        savor::GC_A);
    decision = provider.Advance(
        host,
        Complete(host, savor::bp::battle::BattleEndController0NeutralCopied, 0x801C7948u, 6, 6));
    EXPECT_EQ(Wait(decision).expected_keys.front(), savor::bp::battle::BattleEndResultDispatch);

    host.PutU32(result_object + 0x04, 14);
    decision = provider.Advance(
        host,
        Complete(host, savor::bp::battle::BattleEndResultDispatch, 0x800E4660u, 7, 7));
    EXPECT_EQ(Wait(decision).expected_keys.size(), 2u);

    decision = provider.Advance(
        host,
        Complete(host, savor::bp::battle::BattleEndResultLifecycleExit, 0x800E64A0u, 8, 8));
    EXPECT_EQ(Wait(decision).expected_keys.front(), savor::bp::battle::BattleEndResultCleanupComplete);

    host.PutU32(BattleEndResultsInputMacroProvider::GuestAddresses::ResultLifecycleState, 0xFF);
    host.PutU32(BattleEndResultsInputMacroProvider::GuestAddresses::ResultDone, 1);
    host.PutU32(BattleEndResultsInputMacroProvider::GuestAddresses::ResultObjectPointer, 0);
    host.PutU32(BattleEndResultsInputMacroProvider::GuestAddresses::GameMode, 6);
    decision = provider.Advance(
        host,
        Complete(host, savor::bp::battle::BattleEndResultCleanupComplete, 0x800E3694u, 9, 9));
    EXPECT_EQ(decision.status, InputMacroDriverStatus::Completed);
    EXPECT_TRUE(provider.completed());
    ASSERT_EQ(provider.report().actions.size(), 1u);
    EXPECT_EQ(provider.report().actions.front().kind, endresults::ActionKind::MandatoryConfirm);
    EXPECT_TRUE(provider.report().actions.front().input_acknowledged);
    EXPECT_TRUE(provider.report().actions.front().release_observed);
    EXPECT_TRUE(provider.report().actions.front().progress_observed);
    EXPECT_EQ(
        provider.report().invariant_flags
            & (endresults::InvariantCompletionState
                | endresults::InvariantCompletionPublished
                | endresults::InvariantResultPointerCleared
                | endresults::InvariantFieldMode),
        endresults::InvariantCompletionState
            | endresults::InvariantCompletionPublished
            | endresults::InvariantResultPointerCleared
            | endresults::InvariantFieldMode);
}

TEST(BattleEndResultsProvider, RefusesAPressWithoutFreshAcknowledgedInputEpoch)
{
    FakeDriverHost host;
    SeedStartMemory(host);
    host.stop = {
        .key = savor::bp::battle::EndBattleVictory,
        .pc = 0x800706D8u,
    };
    BattleEndResultsInputMacroProvider provider({
        .acceleration_policy = endresults::AccelerationPolicy::RequiredOnly,
    });
    AdvanceToFirstDispatch(provider, host);

    constexpr std::uint32_t result_object = 0x80400000u;
    host.PutU32(BattleEndResultsInputMacroProvider::GuestAddresses::ResultObjectPointer, result_object);
    host.PutU32(result_object + 0x04, 13);
    host.PutU32(result_object + 0x0C, 0);
    auto decision = provider.Advance(
        host,
        Complete(host, savor::bp::battle::BattleEndResultDispatch, 0x800E4660u, 3, 3));
    ASSERT_EQ(decision.status, InputMacroDriverStatus::PlanReady);

    decision = provider.Advance(
        host,
        Complete(
            host,
            savor::bp::battle::BattleEndResultConfirmReady,
            0x800E6128u,
            4,
            4,
            {},
            false));
    ASSERT_EQ(decision.status, InputMacroDriverStatus::PlanReady);
    ASSERT_EQ(
        Wait(decision).expected_keys.front(),
        savor::bp::battle::BattleEndResultConfirmAccepted);

    GCInputFrame press_a{};
    press_a.A();
    decision = provider.Advance(
        host,
        Complete(
            host,
            savor::bp::battle::BattleEndResultConfirmAccepted,
            0x800E6138u,
            5,
            5,
            press_a,
            false));
    EXPECT_EQ(decision.status, InputMacroDriverStatus::Failed);
    EXPECT_EQ(provider.failure(), endresults::FailureCode::QualificationMismatch);
    EXPECT_TRUE(provider.report().actions.empty());
}

TEST(BattleEndResultsProvider, RequiresCausalRawAndControllerInfoReleaseFieldsToBeZero)
{
    enum class Field {
        RawCurrent,
        RawNew,
        ControllerCurrent,
        ControllerNew,
    };
    constexpr std::array fields{
        Field::RawCurrent,
        Field::RawNew,
        Field::ControllerCurrent,
        Field::ControllerNew,
    };

    for (const auto field : fields) {
        SCOPED_TRACE(static_cast<int>(field));
        FakeDriverHost host;
        SeedStartMemory(host);
        host.stop = {
            .key = savor::bp::battle::EndBattleVictory,
            .pc = 0x800706D8u,
        };
        BattleEndResultsInputMacroProvider provider({
            .acceleration_policy = endresults::AccelerationPolicy::RequiredOnly,
        });
        constexpr std::uint32_t result_object = 0x80400000u;
        AdvanceToConfirmReleaseWitness(provider, host, result_object);

        constexpr std::uint32_t controller = 0x80500000u;
        host.PutU32(BattleEndResultsInputMacroProvider::GuestAddresses::Controller0Pointer, controller);
        switch (field) {
        case Field::RawCurrent: host.PutU32(controller, 1); break;
        case Field::RawNew: host.PutU32(controller + 8, 1); break;
        case Field::ControllerCurrent:
            host.PutU32(BattleEndResultsInputMacroProvider::GuestAddresses::ControllerInfo0, 1);
            break;
        case Field::ControllerNew:
            host.PutU32(BattleEndResultsInputMacroProvider::GuestAddresses::ControllerInfo0 + 8, 1);
            break;
        }
        const auto decision = provider.Advance(
            host,
            Complete(
                host,
                savor::bp::battle::BattleEndController0NeutralCopied,
                0x801C7948u,
                6,
                6));
        EXPECT_EQ(decision.status, InputMacroDriverStatus::Failed);
        EXPECT_EQ(
            provider.failure(),
            endresults::FailureCode::GuestNeutralUnacknowledged);
    }
}

TEST(BattleEndResultsScript, UsesOnlyCoordinatorMaterializedContextAndStopsAfterSave)
{
    const auto script = endresults::MakeBattleResultsScreenProgram();
    ASSERT_EQ(script.canonical_bp_keys, (std::vector<BPKey>{
        savor::bp::battle::BattleEndFieldReturnReseedComplete}));
    EXPECT_EQ(
        script.gated_bp_keys.size(),
        BattleResultsScreenInputMacroProvider::required_breakpoint_keys().size());
    ASSERT_FALSE(script.ops.empty());
    EXPECT_EQ(script.ops.front().code, savor::PSOpCode::ARM_PHASE_BPS_ONCE);
    EXPECT_NE(
        std::find_if(
            script.ops.begin(),
            script.ops.end(),
            [](const savor::PSOp& op) {
                return op.code == savor::PSOpCode::MATERIALIZE_BATTLE_RESULTS_SCREEN_MACRO_STEPS;
            }),
        script.ops.end());
}

} // namespace
