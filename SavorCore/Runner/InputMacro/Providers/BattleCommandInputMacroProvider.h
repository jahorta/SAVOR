#pragma once

#include "../InputMacroPlan.h"
#include "../../../Core/Input/SoaBattle/ActionTypes.h"
#include "../../../Core/Input/SoaBattle/PlanWriter.h"
#include "../../../Core/Memory/Soa/Battle/BattleContext.h"
#include "../../../Phases/Programs/BattleMacroProbe/BattleMacroProbePayload.h"
#include "../../Breakpoints/BpRegistry.h"

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace savor::inputmacro {

struct BattleCommandProviderWaitResult {
    bool hit{false};
    BPKey hit_key{0};
    std::uint32_t hit_pc{0};
};

// Narrow host seam for the provider-owned preflight. The generic macro runtime
// owns execution after preparation; this interface only synchronizes command
// authoring with the battle input state and captures the planning snapshot.
class IBattleCommandInputMacroProviderHost {
public:
    virtual ~IBattleCommandInputMacroProviderHost() = default;

    virtual BPKey current_breakpoint_key() const = 0;
    virtual BattleCommandProviderWaitResult wait_for_breakpoints(
        std::span<const BPKey> expected_keys) = 0;
    virtual bool capture_mem1(std::string& out_mem1) = 0;
};

class BattleCommandInputMacroProvider {
public:
    struct ProbeRequest {
        std::vector<phase::battle::macroprobe::MacroCommand> commands;
        std::uint32_t transition_neutral_frames{3};
        std::uint32_t fake_attack_count{0};
        phase::battle::macroprobe::FakeAttackPattern fake_attack_pattern{};
        bool use_mixed_fake_attack_patterns{false};
        phase::battle::macroprobe::FakeAttackPattern first_fake_attack_pattern{};
        bool use_final_fake_attack_pattern{false};
        phase::battle::macroprobe::FakeAttackPattern final_fake_attack_pattern{};
    };

    struct AuthoredTurnRequest {
        soa::battle::actions::TurnPlan turn_plan;
        std::uint32_t transition_neutral_frames{3};
    };

    using Request = std::variant<ProbeRequest, AuthoredTurnRequest>;

    struct PrepareResult {
        InputMacroPlan plan;
        phase::battle::macroprobe::FailureCode failure{
            phase::battle::macroprobe::FailureCode::Ok};
        soa::battle::actions::MaterializeErr materialize_error{
            soa::battle::actions::MaterializeErr::OK};
        phase::battle::macroprobe::BattleMacroPlanningContext planning_context;
        BPKey last_expected_key{0};
        BPKey last_hit_key{0};
        std::uint32_t last_hit_pc{0};
        std::string diagnostic;

        bool ok() const noexcept {
            return failure == phase::battle::macroprobe::FailureCode::Ok && !plan.steps.empty();
        }
    };

    // The exact internal keys this provider is allowed to emit. TurnInputs is
    // a public/shared synchronization point and is deliberately not included.
    static std::span<const BPKey> required_breakpoint_keys() noexcept;

    PrepareResult prepare(
        IBattleCommandInputMacroProviderHost& host,
        const Request& request) const;

    PrepareResult compile(
        const Request& request,
        const soa::battle::ctx::BattleContext& battle_context) const;

private:
    static bool plan_uses_only_declared_breakpoints(const InputMacroPlan& plan) noexcept;
};

} // namespace savor::inputmacro
