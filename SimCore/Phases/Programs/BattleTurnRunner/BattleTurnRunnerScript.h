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

    static const std::string LabelAdvanceToTurnInput = "ADV_TO_TURN_INPUT";
    static const std::string LabelAfterPrelude = "AFTER_PRELUDE";
    static const std::string LabelApplyTurn = "APPLY_TURN";
    static const std::string LabelRunAppliedInputs = "RUN_APPLIED_INPUTS";

    static const std::string LabelRetReachedNext = "RET_NEXT_TURN";
    static const std::string LabelRetVictory = "RET_SUCCESS";
    static const std::string LabelRetDefeat = "RET_FAILURE";
    static const std::string LabelRetPredFail = "RET_PRED_FAILURE";
    static const std::string LabelRetMaterializeFail = "RET_PLAN_MAT_FAILURE";
    static const std::string LabelRetDWErr = "RET_DW_RUN_ERROR";
    static const std::string LabelRetOutOfTurns = "RET_OUT_OF_TURNS";

    inline simcore::PhaseScript MakeBattleTurnRunnerProgram(uint32_t long_timeout = 120000)
    {
        using simcore::battle::Outcome;

        simcore::PhaseScript ps{};
        ps.canonical_bp_keys = { BP_BattleAcceptInput, BP_BattleInputsDone, BP_Victory, BP_Defeat, BP_BattleLoadComplete };

        ps.ops.push_back(simcore::OpArmPhaseBps());
        ps.ops.push_back(simcore::OpArmBpsFromPredTable());
        ps.ops.push_back(simcore::OpLoadSnapshot());
        ps.ops.push_back(simcore::OpSetTimeoutToMS(long_timeout));

        // Infer prelude path by current turn. current_turn > 1 starts near TurnInputs and should not apply initial input.
        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::battle::ACTIVE_TURN, simcore::PSCmp::GT, 1u, LabelAfterPrelude));

        // Turn 1 path: apply initial input only if caller supplied it.
        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::battle::HAS_INITIAL_INPUT, simcore::PSCmp::EQ, 0u, LabelAdvanceToTurnInput));
        ps.ops.push_back(simcore::OpApplyInputFrom(simcore::keys::battle::INITIAL_INPUT));

        // Tolerate battle-load prelude on fresh runs until we hit TurnInputs.
        ps.ops.push_back(simcore::OpLabel(LabelAdvanceToTurnInput));
        ps.ops.push_back(simcore::OpRunUntilBp());
        ps.ops.push_back(simcore::OpGotoIf(DW_Outcome, simcore::PSCmp::NE, 0, LabelRetDWErr));
        ps.ops.push_back(simcore::OpEvalPredicatesAtHitBP());

        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::core::PRED_ABORT_RUN, simcore::PSCmp::EQ, 1u, LabelRetPredFail));
        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::core::RUN_HIT_BP_KEY, simcore::PSCmp::EQ, (uint32_t)BP_Victory, LabelRetVictory));
        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::core::RUN_HIT_BP_KEY, simcore::PSCmp::EQ, (uint32_t)BP_Defeat, LabelRetDefeat));
        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::core::RUN_HIT_BP_KEY, simcore::PSCmp::EQ, (uint32_t)BP_BattleLoadComplete, LabelAdvanceToTurnInput));
        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::core::RUN_HIT_BP_KEY, simcore::PSCmp::NE, (uint32_t)BP_BattleAcceptInput, LabelAdvanceToTurnInput));

        // current_turn > 1 path: run a single frame to avoid desync before materialization
        ps.ops.push_back(simcore::OpLabel(LabelAfterPrelude));
        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::battle::TURN_INPUT_INDEX, simcore::PSCmp::LE, 1u, LabelApplyTurn));
        ps.ops.push_back(simcore::OpStepFrames(1, true));

        // Build and apply exactly one turn
        ps.ops.push_back(simcore::OpGetBattleContext());
        ps.ops.push_back(simcore::OpBuildTurnInputFromActions());
        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::battle::PLAN_MATERIALIZE_ERR, simcore::PSCmp::NE, 0u, LabelRetMaterializeFail));

        ps.ops.push_back(simcore::OpApplyPlanFrameFrom(simcore::keys::battle::TURN_INPUT_INDEX));
        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::core::PLAN_DONE, simcore::PSCmp::EQ, 1u, LabelRunAppliedInputs));
        ps.ops.push_back(simcore::OpGoto(LabelApplyTurn));

        // Run one segment after applying this turn
        ps.ops.push_back(simcore::OpLabel(LabelRunAppliedInputs));
        ps.ops.push_back(simcore::OpRunUntilBp());
        ps.ops.push_back(simcore::OpGotoIf(DW_Outcome, simcore::PSCmp::NE, 0u, LabelRetDWErr));
        ps.ops.push_back(simcore::OpEvalPredicatesAtHitBP());

        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::core::PRED_ABORT_RUN, simcore::PSCmp::EQ, 1u, LabelRetPredFail));
        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::core::RUN_HIT_BP_KEY, simcore::PSCmp::EQ, (uint32_t)BP_Victory, LabelRetVictory));
        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::core::RUN_HIT_BP_KEY, simcore::PSCmp::EQ, (uint32_t)BP_Defeat, LabelRetDefeat));
        ps.ops.push_back(simcore::OpGotoIf(simcore::keys::core::RUN_HIT_BP_KEY, simcore::PSCmp::EQ, (uint32_t)BP_BattleAcceptInput, LabelRetReachedNext));

        // Keep running until one of the terminals above.
        ps.ops.push_back(simcore::OpGoto(LabelRunAppliedInputs));

        // return labels read ending RNG before returning
        ps.ops.push_back(simcore::OpLabel(LabelRetReachedNext));
        ps.ops.push_back(simcore::OpAddU32(simcore::keys::battle::TURN_OUTPUT_INDEX, 1u));
        ps.ops.push_back(simcore::OpGotoIfKeys(simcore::keys::battle::TURN_OUTPUT_INDEX, simcore::PSCmp::GT, simcore::keys::battle::LAST_TURN, LabelRetOutOfTurns));
        ps.ops.push_back(simcore::OpReadU32(addr::Registry::base(addr::core::RNG_SEED), simcore::keys::seed::RNG_SEED));
        ps.ops.push_back(simcore::OpSaveSavestateFrom(simcore::keys::battle::OUTPUT_SAVESTATE_PATH));
        ps.ops.push_back(simcore::OpReturnResult(Battle_Outcome, (uint32_t)Outcome::ReachedNextTurn));

        ps.ops.push_back(simcore::OpLabel(LabelRetOutOfTurns));
        ps.ops.push_back(simcore::OpReadU32(addr::Registry::base(addr::core::RNG_SEED), simcore::keys::seed::RNG_SEED));
        ps.ops.push_back(simcore::OpReturnResult(Battle_Outcome, (uint32_t)Outcome::HitTurnLimit));

        ps.ops.push_back(simcore::OpLabel(LabelRetVictory));
        ps.ops.push_back(simcore::OpReadU32(addr::Registry::base(addr::core::RNG_SEED), simcore::keys::seed::RNG_SEED));
        ps.ops.push_back(simcore::OpSaveSavestateFrom(simcore::keys::battle::OUTPUT_SAVESTATE_PATH));
        ps.ops.push_back(simcore::OpReturnResult(Battle_Outcome, (uint32_t)Outcome::Victory));

        ps.ops.push_back(simcore::OpLabel(LabelRetDefeat));
        ps.ops.push_back(simcore::OpReadU32(addr::Registry::base(addr::core::RNG_SEED), simcore::keys::seed::RNG_SEED));
        ps.ops.push_back(simcore::OpReturnResult(Battle_Outcome, (uint32_t)Outcome::Defeat));

        ps.ops.push_back(simcore::OpLabel(LabelRetPredFail));
        ps.ops.push_back(simcore::OpReadU32(addr::Registry::base(addr::core::RNG_SEED), simcore::keys::seed::RNG_SEED));
        ps.ops.push_back(simcore::OpReturnResult(Battle_Outcome, (uint32_t)Outcome::PredFailure));

        ps.ops.push_back(simcore::OpLabel(LabelRetMaterializeFail));
        ps.ops.push_back(simcore::OpReadU32(addr::Registry::base(addr::core::RNG_SEED), simcore::keys::seed::RNG_SEED));
        ps.ops.push_back(simcore::OpReturnResult(Battle_Outcome, (uint32_t)Outcome::PlanMaterializeFailure));

        ps.ops.push_back(simcore::OpLabel(LabelRetDWErr));
        ps.ops.push_back(simcore::OpReadU32(addr::Registry::base(addr::core::RNG_SEED), simcore::keys::seed::RNG_SEED));
        ps.ops.push_back(simcore::OpReturnResult(Battle_Outcome, (uint32_t)Outcome::DWRunErr));

        return ps;
    }

} // namespace phase::battle::turnrunner
