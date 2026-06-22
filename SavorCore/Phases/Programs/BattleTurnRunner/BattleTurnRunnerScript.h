#pragma once

#include "../../../Runner/Script/PhaseScriptVM.h"
#include "../../../Runner/Script/CtxRegistry.h"
#include "../../../Runner/Breakpoints/BpRegistry.h"
#include "../../../Core/Memory/Soa/SoaAddrRegistry.h"
#include "../BattleRunner/BattleOutcome.h"

namespace phase::battle::turnrunner {

    static constexpr BPKey BP_BattleLoadComplete = bp::battle::BattleLoadComplete;
    static constexpr BPKey BP_BattleAcceptInput = bp::battle::TurnInputs;
    static constexpr BPKey BP_BattleInputsDone = bp::battle::TurnIsReady;
    static constexpr BPKey BP_Victory = bp::battle::EndBattleVictory;
    static constexpr BPKey BP_Defeat = bp::battle::EndBattleDefeat;

    static constexpr savor::context::key::KeyId DW_Outcome = savor::context::key::core::DW_RUN_OUTCOME_CODE;
    static constexpr savor::context::key::KeyId Battle_Outcome = savor::context::key::battle::BATTLE_OUTCOME;

    static const std::string LabelStartAttempt = "START_ATTEMPT";
    static const std::string LabelAdvanceToTurnInput = "ADV_TO_TURN_INPUT";
    static const std::string LabelTurnInputs = "AFTER_PRELUDE";
    static const std::string LabelMaterializeTurnMacro = "MATERIALIZE_TURN_MACRO";
    static const std::string LabelRunTurnMacro = "RUN_TURN_MACRO";
    static const std::string LabelAfterTurnMacro = "AFTER_TURN_MACRO";
    static const std::string LabelConfirmTurnReady = "CONFIRM_TURN_READY";
    static const std::string LabelRunAppliedInputs = "RUN_APPLIED_INPUTS";
    static const std::string LabelRetryInput = "RETRY_INPUT";
    static const std::string LabelRetryExhausted = "RETRY_EXHAUSTED";

    static const std::string LabelRetReachedNext = "RET_NEXT_TURN";
    static const std::string LabelRetVictory = "RET_SUCCESS";
    static const std::string LabelRetDefeat = "RET_FAILURE";
    static const std::string LabelRetPredFail = "RET_PRED_FAILURE";
    static const std::string LabelRetMaterializeFail = "RET_PLAN_MAT_FAILURE";
    static const std::string LabelRetDWErr = "RET_DW_RUN_ERROR";
    static const std::string LabelRetOutOfTurns = "RET_OUT_OF_TURNS";

    inline savor::PhaseScript MakeBattleTurnRunnerProgram(uint32_t long_timeout = 120000, uint32_t gate_timeout = 20000)
    {
        using savor::battle::Outcome;

        savor::PhaseScript ps{};
        ps.canonical_bp_keys = { BP_BattleAcceptInput, BP_BattleInputsDone, BP_Victory, BP_Defeat, BP_BattleLoadComplete };
        ps.reserved_bp_keys = {
            bp::battle::BattleMacroInputReadyGate,
            bp::battle::BattleMacroMainMenuMoveHigher,
            bp::battle::BattleMacroMainMenuMoveLower,
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
        ps.ops.push_back(savor::OpArmBpsFromPredTable());
        ps.ops.push_back(savor::OpLoadSnapshot());
        ps.ops.push_back(savor::OpClearMemoryWatchpoints());
        ps.ops.push_back(savor::OpSetTimeoutToMS(long_timeout));
        ps.ops.push_back(savor::OpSetU32(savor::context::key::core::RUN_POLL_MS, 0u));
        ps.ops.push_back(savor::OpSetU32(savor::context::key::battle::INPUT_RETRY_COUNT, 0u));

        // Infer prelude path by current turn. current_turn > 1 starts near TurnInputs and should not apply initial input.
        ps.ops.push_back(savor::OpLabel(LabelStartAttempt));
        ps.ops.push_back(savor::OpGotoIf(savor::context::key::battle::TURN_OUTPUT_INDEX, savor::PSCmp::GT, 1u, LabelTurnInputs));

        // Turn 1 path: apply initial input only if caller supplied it.
        ps.ops.push_back(savor::OpGotoIf(savor::context::key::battle::HAS_INITIAL_INPUT, savor::PSCmp::EQ, 0u, LabelAdvanceToTurnInput));
        ps.ops.push_back(savor::OpApplyInputFrom(savor::context::key::battle::INITIAL_INPUT));

        // ============  Label Advance To Turn Input  ===================
        // Tolerate battle-load prelude on fresh runs until we hit TurnInputs.
        ps.ops.push_back(savor::OpLabel(LabelAdvanceToTurnInput));
        ps.ops.push_back(savor::OpRunUntilBp());
        ps.ops.push_back(savor::OpGotoIf(DW_Outcome, savor::PSCmp::NE, 0, LabelRetDWErr));
        ps.ops.push_back(savor::OpCapturePredBaselines());
        ps.ops.push_back(savor::OpEvalPredicatesAtHitBP());

        ps.ops.push_back(savor::OpGotoIf(savor::context::key::core::PRED_ABORT_RUN, savor::PSCmp::EQ, 1u, LabelRetPredFail));
        ps.ops.push_back(savor::OpGotoIf(savor::context::key::core::RUN_HIT_BP_KEY, savor::PSCmp::EQ, (uint32_t)BP_Victory, LabelRetVictory));
        ps.ops.push_back(savor::OpGotoIf(savor::context::key::core::RUN_HIT_BP_KEY, savor::PSCmp::EQ, (uint32_t)BP_Defeat, LabelRetDefeat));
        ps.ops.push_back(savor::OpGotoIf(savor::context::key::core::RUN_HIT_BP_KEY, savor::PSCmp::EQ, (uint32_t)BP_BattleLoadComplete, LabelTurnInputs));
        ps.ops.push_back(savor::OpGotoIf(savor::context::key::core::RUN_HIT_BP_KEY, savor::PSCmp::NE, (uint32_t)BP_BattleAcceptInput, LabelAdvanceToTurnInput));

        // ============  Label Turn Inputs  ===================
        ps.ops.push_back(savor::OpLabel(LabelTurnInputs));
        ps.ops.push_back(savor::OpSetU32(savor::context::key::core::RUN_POLL_MS, 10u));
        ps.ops.push_back(savor::OpLabel(LabelMaterializeTurnMacro));
        ps.ops.push_back(savor::OpMaterializeBattleTurnMacroSteps());
        ps.ops.push_back(savor::OpGotoIf(savor::context::key::battle::PLAN_MATERIALIZE_ERR, savor::PSCmp::NE, 0u, LabelRetMaterializeFail));
        ps.ops.push_back(savor::OpGotoIf(savor::context::key::battle::MACRO_FAILURE_CODE, savor::PSCmp::NE, 0u, LabelRetryInput));

        ps.ops.push_back(savor::OpLabel(LabelRunTurnMacro));
        ps.ops.push_back(savor::OpExecuteBattleMacroStep());
        ps.ops.push_back(savor::OpGotoIf(savor::context::key::battle::MACRO_FAILURE_CODE, savor::PSCmp::NE, 0u, LabelRetryInput));
        ps.ops.push_back(savor::OpGotoIf(savor::context::key::battle::MACRO_RESULT, savor::PSCmp::EQ, 0u, LabelAfterTurnMacro));
        ps.ops.push_back(savor::OpGoto(LabelRunTurnMacro));

        ps.ops.push_back(savor::OpLabel(LabelAfterTurnMacro));
        ps.ops.push_back(savor::OpSetU32(savor::context::key::core::RUN_POLL_MS, 0u));
        ps.ops.push_back(savor::OpGoto(LabelConfirmTurnReady));

        // ============  Label Confirm Turn Ready  ===================
        // Require the game to accept the applied turn and finish instruction generation before normal post-input wait.
        ps.ops.push_back(savor::OpLabel(LabelConfirmTurnReady));
        ps.ops.push_back(savor::OpSetU32(savor::context::key::core::RUN_POLL_MS, 0u));
        ps.ops.push_back(savor::OpSetTimeoutToMS(gate_timeout));
        ps.ops.push_back(savor::OpRunUntilBp());
        ps.ops.push_back(savor::OpGotoIf(DW_Outcome, savor::PSCmp::NE, 0u, LabelRetryInput));
        ps.ops.push_back(savor::OpGotoIf(savor::context::key::core::RUN_HIT_BP_KEY, savor::PSCmp::NE, (uint32_t)BP_BattleInputsDone, LabelRetryInput));
        ps.ops.push_back(savor::OpSetTimeoutToMS(long_timeout));
        ps.ops.push_back(savor::OpArmCaptureMemoryWatchpoints());
        ps.ops.push_back(savor::OpGoto(LabelRunAppliedInputs));

        // ============  Label Run Applied Inputs  ===================
        // Run one segment after the turn-ready gate.
        ps.ops.push_back(savor::OpLabel(LabelRunAppliedInputs));
        ps.ops.push_back(savor::OpRunUntilBp());
        ps.ops.push_back(savor::OpGotoIf(DW_Outcome, savor::PSCmp::NE, 0u, LabelRetDWErr));
        ps.ops.push_back(savor::OpCapturePredBaselines());
        ps.ops.push_back(savor::OpEvalPredicatesAtHitBP());

        ps.ops.push_back(savor::OpGotoIf(savor::context::key::core::PRED_ABORT_RUN, savor::PSCmp::EQ, 1u, LabelRetPredFail));
        ps.ops.push_back(savor::OpGotoIf(savor::context::key::core::RUN_HIT_BP_KEY, savor::PSCmp::EQ, (uint32_t)BP_Victory, LabelRetVictory));
        ps.ops.push_back(savor::OpGotoIf(savor::context::key::core::RUN_HIT_BP_KEY, savor::PSCmp::EQ, (uint32_t)BP_Defeat, LabelRetDefeat));
        ps.ops.push_back(savor::OpGotoIf(savor::context::key::core::RUN_HIT_BP_KEY, savor::PSCmp::EQ, (uint32_t)BP_BattleAcceptInput, LabelRetReachedNext));

        // Keep running until one of the terminals above.
        ps.ops.push_back(savor::OpStepOpcode(true));
        ps.ops.push_back(savor::OpGoto(LabelRunAppliedInputs));

        // ============  Label Retry Input  ===================
        // Retry once from the per-job baseline, using the same materialized plan in safe playback mode.
        ps.ops.push_back(savor::OpLabel(LabelRetryInput));
        ps.ops.push_back(savor::OpGotoIf(savor::context::key::battle::INPUT_RETRY_COUNT, savor::PSCmp::GE, 1u, LabelRetryExhausted));
        ps.ops.push_back(savor::OpAddU32(savor::context::key::battle::INPUT_RETRY_COUNT, 1u));
        ps.ops.push_back(savor::OpLoadSnapshot());
        ps.ops.push_back(savor::OpClearMemoryWatchpoints());
        ps.ops.push_back(savor::OpSetTimeoutToMS(long_timeout));
        ps.ops.push_back(savor::OpSetU32(savor::context::key::core::RUN_POLL_MS, 0u));
        ps.ops.push_back(savor::OpGoto(LabelStartAttempt));

        ps.ops.push_back(savor::OpLabel(LabelRetryExhausted));
        ps.ops.push_back(savor::OpGotoIf(DW_Outcome, savor::PSCmp::NE, 0u, LabelRetDWErr));
        ps.ops.push_back(savor::OpSetU32(DW_Outcome, static_cast<uint32_t>(savor::RunToBpOutcome::InputPlaybackFailed)));
        ps.ops.push_back(savor::OpGoto(LabelRetDWErr));

        // ============  Label Return Reached Next  ===================
        // return labels read ending RNG before returning if there are more turns
        ps.ops.push_back(savor::OpLabel(LabelRetReachedNext));
        ps.ops.push_back(savor::OpAddU32(savor::context::key::battle::TURN_OUTPUT_INDEX, 1u));
        ps.ops.push_back(savor::OpGotoIfKeys(savor::context::key::battle::TURN_OUTPUT_INDEX, savor::PSCmp::GT, savor::context::key::battle::LAST_TURN, LabelRetOutOfTurns));
        ps.ops.push_back(savor::OpReadU32(addr::AddrRegistry::base(addr::core::RNG_SEED), savor::context::key::seed::RNG_SEED));
        ps.ops.push_back(savor::OpSaveSavestateFrom(savor::context::key::battle::OUTPUT_SAVESTATE_PATH));
		ps.ops.push_back(savor::OpGetBattleContext());
        ps.ops.push_back(savor::OpReturnResult(Battle_Outcome, (uint32_t)Outcome::ReachedNextTurn));

        // ============  Label Return Out of Turns  ===================
        // return out of turns of there are no more turns
        ps.ops.push_back(savor::OpLabel(LabelRetOutOfTurns));
        ps.ops.push_back(savor::OpReadU32(addr::AddrRegistry::base(addr::core::RNG_SEED), savor::context::key::seed::RNG_SEED));
        ps.ops.push_back(savor::OpReturnResult(Battle_Outcome, (uint32_t)Outcome::HitTurnLimit));

        // ============  Label Return Victory  ===================
        ps.ops.push_back(savor::OpLabel(LabelRetVictory));
        ps.ops.push_back(savor::OpReadU32(addr::AddrRegistry::base(addr::core::RNG_SEED), savor::context::key::seed::RNG_SEED));
        ps.ops.push_back(savor::OpSaveSavestateFrom(savor::context::key::battle::OUTPUT_SAVESTATE_PATH));
        ps.ops.push_back(savor::OpReturnResult(Battle_Outcome, (uint32_t)Outcome::Victory));

        // ============  Label Return Defeat  ===================
        ps.ops.push_back(savor::OpLabel(LabelRetDefeat));
        ps.ops.push_back(savor::OpReadU32(addr::AddrRegistry::base(addr::core::RNG_SEED), savor::context::key::seed::RNG_SEED));
        ps.ops.push_back(savor::OpReturnResult(Battle_Outcome, (uint32_t)Outcome::Defeat));

        // ============  Label Return Predicate Failure  ===================
        ps.ops.push_back(savor::OpLabel(LabelRetPredFail));
        ps.ops.push_back(savor::OpReadU32(addr::AddrRegistry::base(addr::core::RNG_SEED), savor::context::key::seed::RNG_SEED));
        ps.ops.push_back(savor::OpReturnResult(Battle_Outcome, (uint32_t)Outcome::PredFailure));

        // ============  Label Return Materialize Failure  ===================
        ps.ops.push_back(savor::OpLabel(LabelRetMaterializeFail));
        ps.ops.push_back(savor::OpReadU32(addr::AddrRegistry::base(addr::core::RNG_SEED), savor::context::key::seed::RNG_SEED));
        ps.ops.push_back(savor::OpReturnResult(Battle_Outcome, (uint32_t)Outcome::PlanMaterializeFailure));

        // ============  Label Return Dolphin Worker Error  ===================
        ps.ops.push_back(savor::OpLabel(LabelRetDWErr));
        ps.ops.push_back(savor::OpReadU32(addr::AddrRegistry::base(addr::core::RNG_SEED), savor::context::key::seed::RNG_SEED));
        ps.ops.push_back(savor::OpReturnResult(Battle_Outcome, (uint32_t)Outcome::DWRunErr));

        return ps;
    }

} // namespace phase::battle::turnrunner
