#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "Core/Memory/MemView.h"
#include "Runner/Breakpoints/BpRegistry.h"
#include "Runner/InputMacro/Providers/BattleCommandInputMacroProvider.h"

namespace {

namespace legacy = phase::battle::macroprobe;
namespace inputmacro = savor::inputmacro;

using Provider = inputmacro::BattleCommandInputMacroProvider;

void SetBattleSlot(
    soa::battle::ctx::BattleContext& context,
    std::uint32_t slot,
    bool is_player,
    bool present,
    bool is_alive) {
    ASSERT_LT(slot, static_cast<std::uint32_t>(soa::battle::ctx::SLOT_COUNT));
    context.slots_[slot].is_player = is_player ? 1 : 0;
    context.slots_[slot].present = present ? 1 : 0;
    context.slots_[slot].is_alive = is_alive ? 1 : 0;
}

soa::battle::ctx::BattleContext MakeBattleContext() {
    soa::battle::ctx::BattleContext context{};
    for (std::uint32_t slot = 0; slot < 4; ++slot) {
        SetBattleSlot(context, slot, true, true, true);
    }
    SetBattleSlot(context, 4, false, true, true);
    SetBattleSlot(context, 6, false, true, true);
    return context;
}

soa::battle::actions::BattleCommand MakeAction(
    soa::battle::actions::BattleAction action,
    std::uint8_t actor_slot,
    std::uint8_t target_slot = 0xFF) {
    soa::battle::actions::BattleCommand command{};
    command.actor_slot = actor_slot;
    command.macro = action;
    command.params.target_slot = target_slot;
    return command;
}

void ExpectActionEquivalent(
    const inputmacro::InputMacroAction& lhs,
    const inputmacro::InputMacroAction& rhs) {
    ASSERT_EQ(lhs.index(), rhs.index());
    std::visit(
        [&](const auto& left) {
            using T = std::decay_t<decltype(left)>;
            const auto& right = std::get<T>(rhs);
            if constexpr (std::is_same_v<T, inputmacro::BreakpointWaitAction>) {
                EXPECT_EQ(left.expected_keys, right.expected_keys);
                EXPECT_EQ(left.input, right.input);
                EXPECT_EQ(
                    left.hold_input_through_hit_opcode,
                    right.hold_input_through_hit_opcode);
            } else if constexpr (std::is_same_v<T, inputmacro::NeutralFramesAction>) {
                EXPECT_EQ(left.frame_count, right.frame_count);
            } else if constexpr (std::is_same_v<T, inputmacro::CaptureU32BaselineAction>) {
                EXPECT_EQ(left.baseline_id, right.baseline_id);
                EXPECT_EQ(left.address, right.address);
            } else {
                static_assert(std::is_same_v<T, inputmacro::WaitU32ChangeAction>);
                EXPECT_EQ(left.baseline_id, right.baseline_id);
                EXPECT_EQ(left.address, right.address);
                EXPECT_EQ(left.diagnostic_cycle_index, right.diagnostic_cycle_index);
            }
        },
        lhs);
}

void ExpectStepsEquivalent(
    const std::vector<inputmacro::InputMacroStep>& lhs,
    const std::vector<inputmacro::InputMacroStep>& rhs) {
    ASSERT_EQ(lhs.size(), rhs.size());
    for (size_t index = 0; index < lhs.size(); ++index) {
        EXPECT_EQ(lhs[index].label, rhs[index].label) << index;
        ExpectActionEquivalent(lhs[index].action, rhs[index].action);
    }
}

void ExpectAllPlanKeysDeclared(const inputmacro::InputMacroPlan& plan) {
    const auto declared = Provider::required_breakpoint_keys();
    for (const auto& step : plan.steps) {
        const auto* wait = std::get_if<inputmacro::BreakpointWaitAction>(&step.action);
        if (wait == nullptr) continue;
        ASSERT_FALSE(wait->expected_keys.empty()) << step.label;
        for (BPKey key : wait->expected_keys) {
            EXPECT_NE(std::find(declared.begin(), declared.end(), key), declared.end())
                << step.label << " emitted key " << key;
        }
    }
}

class FakeProviderHost final : public inputmacro::IBattleCommandInputMacroProviderHost {
public:
    BPKey current{bp::battle::TurnInputs};
    std::deque<inputmacro::BattleCommandProviderWaitResult> results;
    std::vector<std::vector<BPKey>> waits;
    bool capture_ok{true};
    std::string mem1 = std::string(static_cast<size_t>(savor::MemView::kMem1Size), '\0');

    BPKey current_breakpoint_key() const override { return current; }

    inputmacro::BattleCommandProviderWaitResult wait_for_breakpoints(
        std::span<const BPKey> expected_keys) override {
        waits.emplace_back(expected_keys.begin(), expected_keys.end());
        if (results.empty()) return {};
        auto result = results.front();
        results.pop_front();
        current = result.hit_key;
        return result;
    }

    bool capture_mem1(std::string& out_mem1) override {
        if (!capture_ok) return false;
        out_mem1 = mem1;
        return true;
    }
};

TEST(BattleCommandInputMacroProvider, DeclaresExactlyTheInternalCommandBreakpointSet) {
    const auto keys = Provider::required_breakpoint_keys();
    ASSERT_EQ(keys.size(), 18u);
    EXPECT_EQ(std::count(keys.begin(), keys.end(), bp::battle::BattleMacroInputReadyGate), 1);
    EXPECT_EQ(std::count(keys.begin(), keys.end(), bp::battle::TurnInputs), 0);

    for (BPKey key : keys) {
        const BPAddr* descriptor = bp::BpRegistry::FindRuntime(key);
        ASSERT_NE(descriptor, nullptr) << key;
        EXPECT_EQ(descriptor->visibility, BreakpointVisibility::Internal) << key;
        EXPECT_EQ(descriptor->owner, BreakpointOwner::InputMacro) << key;
        EXPECT_TRUE(bp::BpRegistry::IsAllowed(key, BreakpointConsumer::InputMacroControl));
        EXPECT_FALSE(bp::BpRegistry::IsAllowed(key, BreakpointConsumer::Predicate));
    }
}

TEST(BattleCommandInputMacroProvider, ProbeRequestMatchesCompatibilityCompilerAndDeclaresEveryKey) {
    const auto battle_context = MakeBattleContext();
    const auto planning_context = legacy::BuildPlanningContext(battle_context);
    Provider::ProbeRequest request{
        .commands = {
            {.mode = legacy::MacroMode::Block, .target_slot = 4},
            {.mode = legacy::MacroMode::Attack, .target_slot = 6},
        },
        .transition_neutral_frames = 3,
        .fake_attack_count = 2,
        .fake_attack_pattern = {
            .memory_gate_mode = legacy::FakeAttackMemoryGateMode::Both,
            .target_neutral_before_b_frames = 1,
            .input_neutral_after_b_frames = 2,
        },
    };

    legacy::FailureCode legacy_failure = legacy::FailureCode::Ok;
    const auto expected = legacy::BuildMacroProbePlanSteps(
        request.commands,
        request.transition_neutral_frames,
        request.fake_attack_count,
        request.fake_attack_pattern,
        &planning_context,
        &legacy_failure);
    const auto actual = Provider{}.compile(Provider::Request{request}, battle_context);

    ASSERT_EQ(legacy_failure, legacy::FailureCode::Ok);
    ASSERT_TRUE(actual.ok()) << actual.diagnostic;
    ExpectStepsEquivalent(actual.plan.steps, expected);
    ExpectAllPlanKeysDeclared(actual.plan);
}

TEST(BattleCommandInputMacroProvider, AuthoredTurnMatchesCompatibilityCompilerAndDeclaresEveryKey) {
    const auto battle_context = MakeBattleContext();
    const auto planning_context = legacy::BuildPlanningContext(battle_context);
    soa::battle::actions::TurnPlan turn{};
    turn.fake_attack_count = 1;
    turn.commands = {
        MakeAction(soa::battle::actions::BattleAction::Attack, 0, 6),
        MakeAction(soa::battle::actions::BattleAction::Defend, 1),
    };
    const Provider::AuthoredTurnRequest request{
        .turn_plan = turn,
        .transition_neutral_frames = 4,
    };

    soa::battle::actions::MaterializeErr legacy_error = soa::battle::actions::MaterializeErr::OK;
    const auto expected = legacy::BuildMacroPlanStepsFromTurnPlan(
        turn,
        request.transition_neutral_frames,
        &planning_context,
        &legacy_error);
    const auto actual = Provider{}.compile(Provider::Request{request}, battle_context);

    ASSERT_EQ(legacy_error, soa::battle::actions::MaterializeErr::OK);
    ASSERT_TRUE(actual.ok()) << actual.diagnostic;
    EXPECT_EQ(actual.materialize_error, soa::battle::actions::MaterializeErr::OK);
    ExpectStepsEquivalent(actual.plan.steps, expected);
    ExpectAllPlanKeysDeclared(actual.plan);
}

TEST(BattleCommandInputMacroProvider, SynchronizesThroughTurnInputsAndInputReadyBeforeCapture) {
    FakeProviderHost host;
    host.current = 0;
    host.results = {
        {.hit = true, .hit_key = bp::battle::TurnInputs, .hit_pc = 0x80071740u},
        {.hit = true, .hit_key = bp::battle::BattleMacroInputReadyGate, .hit_pc = 0x8007cec4u},
    };
    Provider::ProbeRequest request{
        .commands = {{.mode = legacy::MacroMode::Block, .target_slot = 4}},
    };

    const auto result = Provider{}.prepare(host, Provider::Request{request});

    ASSERT_TRUE(result.ok()) << result.diagnostic;
    ASSERT_EQ(host.waits.size(), 2u);
    EXPECT_EQ(host.waits[0], std::vector<BPKey>{bp::battle::TurnInputs});
    EXPECT_EQ(host.waits[1], std::vector<BPKey>{bp::battle::BattleMacroInputReadyGate});
    EXPECT_EQ(result.last_expected_key, bp::battle::BattleMacroInputReadyGate);
    EXPECT_EQ(result.last_hit_key, bp::battle::BattleMacroInputReadyGate);
}

TEST(BattleCommandInputMacroProvider, MapsSynchronizationAndCaptureFailuresCompatibly) {
    Provider::ProbeRequest request{
        .commands = {{.mode = legacy::MacroMode::Block, .target_slot = 4}},
    };

    FakeProviderHost failed_host;
    failed_host.results = {{.hit = false}};
    const auto failed = Provider{}.prepare(failed_host, Provider::Request{request});
    EXPECT_EQ(failed.failure, legacy::FailureCode::HostFailure);

    FakeProviderHost unexpected_host;
    unexpected_host.results = {{.hit = true, .hit_key = bp::battle::BattleMacroMagicReady}};
    const auto unexpected = Provider{}.prepare(unexpected_host, Provider::Request{request});
    EXPECT_EQ(unexpected.failure, legacy::FailureCode::UnexpectedBreakpoint);

    FakeProviderHost capture_host;
    capture_host.results = {{
        .hit = true,
        .hit_key = bp::battle::BattleMacroInputReadyGate,
        .hit_pc = 0x8007cec4u,
    }};
    capture_host.capture_ok = false;
    const auto capture = Provider{}.prepare(capture_host, Provider::Request{request});
    EXPECT_EQ(capture.failure, legacy::FailureCode::BattleContextUnavailable);

    FakeProviderHost decode_host;
    decode_host.results = {{
        .hit = true,
        .hit_key = bp::battle::BattleMacroInputReadyGate,
        .hit_pc = 0x8007cec4u,
    }};
    decode_host.mem1 = "too small";
    const auto decode = Provider{}.prepare(decode_host, Provider::Request{request});
    EXPECT_EQ(decode.failure, legacy::FailureCode::BattleContextUnavailable);
}

TEST(BattleCommandInputMacroProvider, MapsCompilerFailuresWithoutProducingRuntimeState) {
    const auto battle_context = MakeBattleContext();

    const auto empty = Provider{}.compile(
        Provider::Request{Provider::ProbeRequest{}},
        battle_context);
    EXPECT_EQ(empty.failure, legacy::FailureCode::NoSteps);
    EXPECT_TRUE(empty.plan.steps.empty());
    EXPECT_FALSE(empty.diagnostic.empty());

    soa::battle::actions::TurnPlan invalid_turn{};
    invalid_turn.commands = {
        MakeAction(soa::battle::actions::BattleAction::Attack, 0, 11),
    };
    const auto invalid = Provider{}.compile(
        Provider::Request{Provider::AuthoredTurnRequest{.turn_plan = invalid_turn}},
        battle_context);
    EXPECT_EQ(invalid.materialize_error, soa::battle::actions::MaterializeErr::NoValidTarget);
    EXPECT_EQ(invalid.failure, legacy::FailureCode::InvalidTarget);
    EXPECT_TRUE(invalid.plan.steps.empty());
    EXPECT_FALSE(invalid.diagnostic.empty());
}

} // namespace
