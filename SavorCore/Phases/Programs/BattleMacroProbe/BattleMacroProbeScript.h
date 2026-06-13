#pragma once

#include "../../../Runner/Breakpoints/BpRegistry.h"
#include "../../../Runner/Script/CtxRegistry.h"
#include "../../../Runner/Script/PhaseScriptVM.h"

namespace phase::battle::macroprobe {

inline savor::PhaseScript MakeBattleMacroProbeProgram()
{
    static const std::string LabelReturn = "RETURN";
    static const std::string LabelPostTurnReadyTail = "POST_TURN_READY_TAIL";
    static const std::string LabelClearTailResult = "CLEAR_TAIL_RESULT";

    savor::PhaseScript ps{};
    ps.canonical_bp_keys = {
        bp::battle::TurnInputs,
        bp::battle::TurnIsReady,
    };
    ps.reserved_bp_keys = {
        bp::battle::BattleMacroInputReadyGate,
        bp::battle::BattleMacroMainMenuMoveHigher,
        bp::battle::BattleMacroMainMenuMoveLower,
        bp::battle::BattleMacroMainMenuMoveHigherAlt,
        bp::battle::BattleMacroMainMenuMoveLowerAlt,
        bp::battle::BattleMacroCommandTransitionDone,
        bp::battle::BattleMacroMainMenuAcceptDispatch,
        bp::battle::BattleMacroDirectCommandQueued,
        bp::battle::BattleMacroAttackTargetSelectorCreated,
        bp::battle::BattleMacroEnemyTargetMoveDownAccepted,
        bp::battle::BattleMacroEnemyTargetMoveUpAccepted,
        bp::battle::BattleMacroEnemyTargetFinalized,
        bp::battle::BattleMacroMagicReady,
        bp::battle::BattleMacroSMoveReady,
        bp::battle::BattleMacroConditionalRunReady,
        bp::battle::BattleMacroItemCategoryReady,
        bp::battle::BattleMacroItemRowListReady,
        bp::battle::BattleMacroItemDetailReady,
        bp::battle::BattleMacroEnemyTargetReady,
        bp::battle::BattleMacroAllyTargetReady,
    };
    ps.ops.push_back(savor::OpArmPhaseBps());
    ps.ops.push_back(savor::OpLoadSnapshot());
    ps.ops.push_back(savor::OpExecuteBattleMacroProbe());
    ps.ops.push_back(savor::OpGotoIf(
        savor::context::key::battle::MACRO_RESULT,
        savor::PSCmp::NE,
        0u,
        LabelReturn));
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
    ps.ops.push_back(savor::OpSetTimeoutToMS(10000));
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
