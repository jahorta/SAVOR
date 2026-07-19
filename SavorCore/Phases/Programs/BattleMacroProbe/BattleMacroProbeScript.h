#pragma once

#include "../../../Runner/Breakpoints/BpRegistry.h"
#include "../../../Runner/InputMacro/Providers/BattleCommandInputMacroProvider.h"
#include "../../../Runner/Script/CtxRegistry.h"
#include "../../../Runner/Script/PhaseScriptProgram.h"

namespace phase::battle::macroprobe {

inline savor::PhaseScript MakeBattleMacroProbeProgram()
{
    static const std::string LabelReturn = "RETURN";
    static const std::string LabelRunMacroLoop = "RUN_MACRO_LOOP";
    static const std::string LabelAfterMacro = "AFTER_MACRO";
    static const std::string LabelPostTurnReadyTail = "POST_TURN_READY_TAIL";
    static const std::string LabelClearTailResult = "CLEAR_TAIL_RESULT";

    savor::PhaseScript ps{};
    ps.canonical_bp_keys = {
        bp::battle::TurnInputs,
        bp::battle::TurnIsReady,
    };
    const auto macro_breakpoints =
        savor::inputmacro::BattleCommandInputMacroProvider::required_breakpoint_keys();
    ps.gated_bp_keys.assign(macro_breakpoints.begin(), macro_breakpoints.end());
    ps.ops.push_back(savor::OpArmPhaseBps());
    ps.ops.push_back(savor::OpLoadSnapshot());
    ps.ops.push_back(savor::OpMaterializeBattleMacroSteps());
    ps.ops.push_back(savor::OpGotoIf(
        savor::context::key::battle::MACRO_FAILURE_CODE,
        savor::PSCmp::NE,
        0u,
        LabelReturn));
    ps.ops.push_back(savor::OpLabel(LabelRunMacroLoop));
    ps.ops.push_back(savor::OpExecuteBattleMacroStep());
    ps.ops.push_back(savor::OpGotoIf(
        savor::context::key::battle::MACRO_FAILURE_CODE,
        savor::PSCmp::NE,
        0u,
        LabelReturn));
    ps.ops.push_back(savor::OpGotoIf(
        savor::context::key::battle::MACRO_RESULT,
        savor::PSCmp::EQ,
        0u,
        LabelAfterMacro));
    ps.ops.push_back(savor::OpGoto(LabelRunMacroLoop));
    ps.ops.push_back(savor::OpLabel(LabelAfterMacro));
    ps.ops.push_back(savor::OpStepOpcode(true));
    ps.ops.push_back(savor::OpSetTimeoutToMS(1000));
    ps.ops.push_back(savor::OpRunUntilBp());
    ps.ops.push_back(savor::OpGotoIf(
        savor::context::key::core::RUN_HIT_BP_KEY,
        savor::PSCmp::EQ,
        static_cast<std::uint32_t>(bp::battle::TurnIsReady),
        LabelPostTurnReadyTail));
    ps.ops.push_back(savor::OpGoto(LabelClearTailResult));
    ps.ops.push_back(savor::OpLabel(LabelPostTurnReadyTail));
    ps.ops.push_back(savor::OpStepOpcode(true));
    ps.ops.push_back(savor::OpSetTimeoutFromKey(savor::context::key::battle::MACRO_OBSERVATION_TAIL_MS));
    ps.ops.push_back(savor::OpRunUntilBp());
    ps.ops.push_back(savor::OpLabel(LabelClearTailResult));
    ps.ops.push_back(savor::OpSetU32(savor::context::key::core::DW_RUN_OUTCOME_CODE, 0u));
    ps.ops.push_back(savor::OpLabel(LabelReturn));
    ps.ops.push_back(savor::OpReturnResult(
        savor::context::key::battle::BATTLE_OUTCOME,
        static_cast<std::uint32_t>(0)));
    return ps;
}

} // namespace phase::battle::macroprobe
