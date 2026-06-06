#pragma once

#include "../../../Runner/Script/PhaseScriptVM.h"
#include "../../../Runner/Script/KeyRegistry.h"
#include "../../../Runner/Breakpoints/BPRegistry.h"
#include "../../../Core/Memory/Soa/SoaAddrRegistry.h"
#include "../BattleRunner/BattleOutcome.h"

namespace phase::battle::turnrunner {

    static constexpr BPKey BP_BattleLoadComplete = bp::battle::BattleLoadComplete;
    static constexpr BPKey BP_BattleAcceptInput = bp::battle::TurnInputs;
    static constexpr BPKey BP_BattleInputsDone = bp::battle::TurnIsReady;
    static constexpr BPKey BP_Victory = bp::battle::EndBattleVictory;
    static constexpr BPKey BP_Defeat = bp::battle::EndBattleDefeat;

    static constexpr simcore::keys::KeyId DW_Outcome = simcore::keys::core::DW_RUN_OUTCOME_CODE;
    static constexpr simcore::keys::KeyId Battle_Outcome = simcore::keys::battle::BATTLE_OUTCOME;

    static const std::string LabelStartAttempt = "START_ATTEMPT";
    static const std::string LabelAdvanceToTurnInput = "ADV_TO_TURN_INPUT";
    static const std::string LabelTurnInputs = "AFTER_PRELUDE";
    static const std::string LabelApplyTurn = "APPLY_TURN";
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

    inline simcore::PhaseScript MakeBattleTurnRunnerProgram(uint32_t long_timeout = 120000, uint32_t gate_timeout = 20000)
    {
        using simcore::battle::Outcome;

        simcore::PhaseScript ps{};
        ps.canonical_bp_keys = { BP_BattleAcceptInput, BP_BattleInputsDone, BP_Victory, BP_Defeat, BP_BattleLoadComplete };

        ps.ops.push_back(simcore::OpArmPhaseBps());
        ps.ops.push_back(simcore::OpArmBpsFromPredTable());
        ps.ops.push_back(simcore::OpLoadSnapshot());
        ps.ops.push_back(simcore::OpSetTimeoutToMS(long_timeout));
        ps.ops.push_back(simcore::OpSetU32(simcore::keys::battle::INPUT_RETRY_COUNT, 0u));

        // Infer prelude path by current turn. current_turn > 1 starts near TurnInputs and should not apply initial input.
        ps.ops.push_back(simcore::OpLabel(LabelStartAttempt));
        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::battle::TURN_OUTPUT_INDEX, simcore::PSCmp::GT, 1u, LabelTurnInputs));

        // Turn 1 path: apply initial input only if caller supplied it.
        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::battle::HAS_INITIAL_INPUT, simcore::PSCmp::EQ, 0u, LabelAdvanceToTurnInput));
        ps.ops.push_back(simcore::OpApplyInputFrom(simcore::keys::battle::INITIAL_INPUT));
        
        // ============  Label Advance To Turn Input  ===================
        // Tolerate battle-load prelude on fresh runs until we hit TurnInputs.
        ps.ops.push_back(simcore::OpLabel(LabelAdvanceToTurnInput));
        ps.ops.push_back(simcore::OpRunUntilBp());
        ps.ops.push_back(simcore::OpGotoIf(DW_Outcome, simcore::PSCmp::NE, 0, LabelRetDWErr));
        ps.ops.push_back(simcore::OpEvalPredicatesAtHitBP());

        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::core::PRED_ABORT_RUN, simcore::PSCmp::EQ, 1u, LabelRetPredFail));
        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::core::RUN_HIT_BP_KEY, simcore::PSCmp::EQ, (uint32_t)BP_Victory, LabelRetVictory));
        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::core::RUN_HIT_BP_KEY, simcore::PSCmp::EQ, (uint32_t)BP_Defeat, LabelRetDefeat));
        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::core::RUN_HIT_BP_KEY, simcore::PSCmp::EQ, (uint32_t)BP_BattleLoadComplete, LabelTurnInputs));
        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::core::RUN_HIT_BP_KEY, simcore::PSCmp::NE, (uint32_t)BP_BattleAcceptInput, LabelAdvanceToTurnInput));

        // ============  Label Turn Inputs  ===================
        // current_turn > 1 path: run a single frame to avoid desync before materialization
        ps.ops.push_back(simcore::OpLabel(LabelTurnInputs));
        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::battle::TURN_OUTPUT_INDEX, simcore::PSCmp::LE, 1u, LabelApplyTurn));

        // Build and apply exactly one turn
        ps.ops.push_back(simcore::OpGetBattleContext());
        ps.ops.push_back(simcore::OpBuildTurnInputFromActions());
        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::battle::PLAN_MATERIALIZE_ERR, simcore::PSCmp::NE, 0u, LabelRetMaterializeFail));

        ps.ops.push_back(simcore::OpApplyPlanFrameFrom(simcore::keys::battle::ACTIVE_TURN));
        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::battle::INPUT_PLAYBACK_ERR, simcore::PSCmp::NE, 0u, LabelRetryInput));
        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::core::PLAN_DONE, simcore::PSCmp::EQ, 1u, LabelConfirmTurnReady));
        ps.ops.push_back(simcore::OpGoto(LabelApplyTurn));

        // ============  Label Confirm Turn Ready  ===================
        // Require the game to accept the applied turn and finish instruction generation before normal post-input wait.
        ps.ops.push_back(simcore::OpLabel(LabelConfirmTurnReady));
        ps.ops.push_back(simcore::OpSetTimeoutToMS(gate_timeout));
        ps.ops.push_back(simcore::OpRunUntilBp());
        ps.ops.push_back(simcore::OpGotoIf(DW_Outcome, simcore::PSCmp::NE, 0u, LabelRetryInput));
        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::core::RUN_HIT_BP_KEY, simcore::PSCmp::NE, (uint32_t)BP_BattleInputsDone, LabelRetryInput));
        ps.ops.push_back(simcore::OpSetTimeoutToMS(long_timeout));
        ps.ops.push_back(simcore::OpGoto(LabelRunAppliedInputs));

        // ============  Label Run Applied Inputs  ===================
        // Run one segment after the turn-ready gate.
        ps.ops.push_back(simcore::OpLabel(LabelRunAppliedInputs));
        ps.ops.push_back(simcore::OpRunUntilBp());
        ps.ops.push_back(simcore::OpGotoIf(DW_Outcome, simcore::PSCmp::NE, 0u, LabelRetDWErr));
        ps.ops.push_back(simcore::OpEvalPredicatesAtHitBP());

        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::core::PRED_ABORT_RUN, simcore::PSCmp::EQ, 1u, LabelRetPredFail));
        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::core::RUN_HIT_BP_KEY, simcore::PSCmp::EQ, (uint32_t)BP_Victory, LabelRetVictory));
        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::core::RUN_HIT_BP_KEY, simcore::PSCmp::EQ, (uint32_t)BP_Defeat, LabelRetDefeat));
        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::core::RUN_HIT_BP_KEY, simcore::PSCmp::EQ, (uint32_t)BP_BattleAcceptInput, LabelRetReachedNext));
        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::core::RUN_HIT_BP_KEY, simcore::PSCmp::NE, (uint32_t)BP_BattleInputsDone, LabelRunAppliedInputs));
        ps.ops.push_back(simcore::OpStepOpcode(true));

        // Keep running until one of the terminals above.
        ps.ops.push_back(simcore::OpGoto(LabelRunAppliedInputs));

        // ============  Label Retry Input  ===================
        // Retry once from the per-job baseline, using the same materialized plan in safe playback mode.
        ps.ops.push_back(simcore::OpLabel(LabelRetryInput));
        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::battle::INPUT_RETRY_COUNT, simcore::PSCmp::GE, 1u, LabelRetryExhausted));
        ps.ops.push_back(simcore::OpAddU32(simcore::keys::battle::INPUT_RETRY_COUNT, 1u));
        ps.ops.push_back(simcore::OpLoadSnapshot());
        ps.ops.push_back(simcore::OpSetTimeoutToMS(long_timeout));
        ps.ops.push_back(simcore::OpGoto(LabelStartAttempt));

        ps.ops.push_back(simcore::OpLabel(LabelRetryExhausted));
        ps.ops.push_back(simcore::OpGotoIf(DW_Outcome, simcore::PSCmp::NE, 0u, LabelRetDWErr));
        ps.ops.push_back(simcore::OpSetU32(DW_Outcome, static_cast<uint32_t>(simcore::RunToBpOutcome::Aborted)));
        ps.ops.push_back(simcore::OpGoto(LabelRetDWErr));

        // ============  Label Return Reached Next  ===================
        // return labels read ending RNG before returning if there are more turns
        ps.ops.push_back(simcore::OpLabel(LabelRetReachedNext));
        ps.ops.push_back(simcore::OpAddU32(simcore::keys::battle::TURN_OUTPUT_INDEX, 1u));
        ps.ops.push_back(simcore::OpGotoIfKeys(simcore::keys::battle::TURN_OUTPUT_INDEX, simcore::PSCmp::GT, simcore::keys::battle::LAST_TURN, LabelRetOutOfTurns));
        ps.ops.push_back(simcore::OpReadU32(addr::Registry::base(addr::core::RNG_SEED), simcore::keys::seed::RNG_SEED));
        ps.ops.push_back(simcore::OpSaveSavestateFrom(simcore::keys::battle::OUTPUT_SAVESTATE_PATH));
        ps.ops.push_back(simcore::OpReturnResult(Battle_Outcome, (uint32_t)Outcome::ReachedNextTurn));

        // ============  Label Return Out of Turns  ===================
        // return out of turns of there are no more turns
        ps.ops.push_back(simcore::OpLabel(LabelRetOutOfTurns));
        ps.ops.push_back(simcore::OpReadU32(addr::Registry::base(addr::core::RNG_SEED), simcore::keys::seed::RNG_SEED));
        ps.ops.push_back(simcore::OpReturnResult(Battle_Outcome, (uint32_t)Outcome::HitTurnLimit));

        // ============  Label Return Victory  ===================
        ps.ops.push_back(simcore::OpLabel(LabelRetVictory));
        ps.ops.push_back(simcore::OpReadU32(addr::Registry::base(addr::core::RNG_SEED), simcore::keys::seed::RNG_SEED));
        ps.ops.push_back(simcore::OpSaveSavestateFrom(simcore::keys::battle::OUTPUT_SAVESTATE_PATH));
        ps.ops.push_back(simcore::OpReturnResult(Battle_Outcome, (uint32_t)Outcome::Victory));

        // ============  Label Return Defeat  ===================
        ps.ops.push_back(simcore::OpLabel(LabelRetDefeat));
        ps.ops.push_back(simcore::OpReadU32(addr::Registry::base(addr::core::RNG_SEED), simcore::keys::seed::RNG_SEED));
        ps.ops.push_back(simcore::OpReturnResult(Battle_Outcome, (uint32_t)Outcome::Defeat));

        // ============  Label Return Predicate Failure  ===================
        ps.ops.push_back(simcore::OpLabel(LabelRetPredFail));
        ps.ops.push_back(simcore::OpReadU32(addr::Registry::base(addr::core::RNG_SEED), simcore::keys::seed::RNG_SEED));
        ps.ops.push_back(simcore::OpReturnResult(Battle_Outcome, (uint32_t)Outcome::PredFailure));

        // ============  Label Return Materialize Failure  ===================
        ps.ops.push_back(simcore::OpLabel(LabelRetMaterializeFail));
        ps.ops.push_back(simcore::OpReadU32(addr::Registry::base(addr::core::RNG_SEED), simcore::keys::seed::RNG_SEED));
        ps.ops.push_back(simcore::OpReturnResult(Battle_Outcome, (uint32_t)Outcome::PlanMaterializeFailure));

        // ============  Label Return Dolphin Worker Error  ===================
        ps.ops.push_back(simcore::OpLabel(LabelRetDWErr));
        ps.ops.push_back(simcore::OpReadU32(addr::Registry::base(addr::core::RNG_SEED), simcore::keys::seed::RNG_SEED));
        ps.ops.push_back(simcore::OpReturnResult(Battle_Outcome, (uint32_t)Outcome::DWRunErr));

        return ps;
    }

} // namespace phase::battle::turnrunner
