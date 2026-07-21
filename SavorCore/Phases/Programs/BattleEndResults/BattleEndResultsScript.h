#pragma once

#include "../../../Runner/Breakpoints/BpRegistry.h"
#include "../../../Runner/InputMacro/Providers/BattleResultsScreenInputMacroProvider.h"
#include "../../../Runner/Script/CtxRegistry.h"
#include "../../../Runner/Script/PhaseScriptProgram.h"
#include "BattleEndResultsReport.h"

namespace phase::battle::endresults {

inline savor::PhaseScript MakeBattleResultsScreenProgram()
{
    constexpr const char* LabelLoop = "BATTLE_END_RESULTS_LOOP";
    constexpr const char* LabelSuccess = "BATTLE_END_RESULTS_SUCCESS";
    constexpr const char* LabelFailure = "BATTLE_END_RESULTS_FAILURE";
    constexpr const char* LabelReturnFailure = "BATTLE_END_RESULTS_RETURN_FAILURE";

    namespace key = savor::context::key;
    savor::PhaseScript script{};
    script.canonical_bp_keys = {bp::battle::BattleEndFieldReturnReseedComplete};
    const auto provider_keys =
        savor::inputmacro::BattleResultsScreenInputMacroProvider::required_breakpoint_keys();
    script.gated_bp_keys.assign(provider_keys.begin(), provider_keys.end());

    script.ops.push_back(savor::OpArmPhaseBps());
    script.ops.push_back(savor::OpLoadSnapshot());
    script.ops.push_back(savor::OpClearMemoryWatchpoints());
    script.ops.push_back(savor::OpSetTimeoutFromKey(key::battleend::RUN_TIMEOUT_MS));
    script.ops.push_back(savor::OpSetU32(key::core::RUN_POLL_MS, 0u));
    script.ops.push_back(savor::OpMaterializeBattleResultsScreenMacroSteps());
    script.ops.push_back(savor::OpGotoIf(
        key::battleend::PROVIDER_FAILURE,
        savor::PSCmp::NE,
        0u,
        LabelFailure));

    script.ops.push_back(savor::OpLabel(LabelLoop));
    script.ops.push_back(savor::OpExecuteInputMacroStep());
    script.ops.push_back(savor::OpGotoIf(
        key::battleend::PROVIDER_FAILURE,
        savor::PSCmp::NE,
        0u,
        LabelFailure));
    script.ops.push_back(savor::OpGotoIf(
        key::battleend::RUNTIME_FAILURE,
        savor::PSCmp::NE,
        0u,
        LabelFailure));
    script.ops.push_back(savor::OpGotoIf(
        key::battleend::MACRO_RESULT,
        savor::PSCmp::EQ,
        0u,
        LabelSuccess));
    script.ops.push_back(savor::OpGoto(LabelLoop));

    script.ops.push_back(savor::OpLabel(LabelSuccess));
    script.ops.push_back(savor::OpSaveSavestateFrom(key::battleend::OUTPUT_SAVESTATE_PATH));
    script.ops.push_back(savor::OpReturnResult(
        key::battleend::OUTCOME,
        static_cast<std::uint32_t>(Outcome::Completed)));

    script.ops.push_back(savor::OpLabel(LabelFailure));
    script.ops.push_back(savor::OpGotoIf(
        key::core::DW_RUN_OUTCOME_CODE,
        savor::PSCmp::NE,
        0u,
        LabelReturnFailure));
    script.ops.push_back(savor::OpSetU32(
        key::core::DW_RUN_OUTCOME_CODE,
        static_cast<std::uint32_t>(savor::RunToBpOutcome::InputPlaybackFailed)));
    script.ops.push_back(savor::OpLabel(LabelReturnFailure));
    script.ops.push_back(savor::OpReturnResult(
        key::battleend::OUTCOME,
        static_cast<std::uint32_t>(Outcome::Failed)));
    return script;
}

inline savor::PhaseScript MakeBattleEndResultsProgram()
{
    return MakeBattleResultsScreenProgram();
}

} // namespace phase::battle::endresults
