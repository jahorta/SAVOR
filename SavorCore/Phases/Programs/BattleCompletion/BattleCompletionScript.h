#pragma once

#include "../../../Runner/Breakpoints/BpRegistry.h"
#include "../../../Runner/InputMacro/Providers/BattleCompletionInputMacroProvider.h"
#include "../../../Runner/Script/CtxRegistry.h"
#include "../../../Runner/Script/PhaseScriptProgram.h"
#include "../BattleEndResults/BattleEndResultsReport.h"

namespace phase::battle::completion {

inline savor::PhaseScript MakeBattleCompletionProgram()
{
    constexpr const char* Loop = "BATTLE_COMPLETION_LOOP";
    constexpr const char* Success = "BATTLE_COMPLETION_SUCCESS";
    constexpr const char* Failure = "BATTLE_COMPLETION_FAILURE";
    constexpr const char* ReturnFailure = "BATTLE_COMPLETION_RETURN_FAILURE";
    namespace key = savor::context::key;

    savor::PhaseScript script{};
    script.canonical_bp_keys = {bp::battle::EndBattleVictory};
    const auto provider_keys =
        savor::inputmacro::BattleCompletionInputMacroProvider::required_breakpoint_keys();
    script.gated_bp_keys.assign(provider_keys.begin(), provider_keys.end());
    script.ops.push_back(savor::OpArmPhaseBps());
    script.ops.push_back(savor::OpLoadSnapshot());
    script.ops.push_back(savor::OpClearMemoryWatchpoints());
    script.ops.push_back(savor::OpSetTimeoutFromKey(key::battlecompletion::RUN_TIMEOUT_MS));
    script.ops.push_back(savor::OpSetU32(key::core::RUN_POLL_MS, 0u));
    script.ops.push_back(savor::OpMaterializeBattleCompletionMacroSteps());
    script.ops.push_back(savor::OpGotoIf(
        key::battlecompletion::PROVIDER_FAILURE, savor::PSCmp::NE, 0u, Failure));
    script.ops.push_back(savor::OpLabel(Loop));
    script.ops.push_back(savor::OpExecuteInputMacroStep());
    script.ops.push_back(savor::OpGotoIf(
        key::battlecompletion::PROVIDER_FAILURE, savor::PSCmp::NE, 0u, Failure));
    script.ops.push_back(savor::OpGotoIf(
        key::battlecompletion::RUNTIME_FAILURE, savor::PSCmp::NE, 0u, Failure));
    script.ops.push_back(savor::OpGotoIf(
        key::battlecompletion::MACRO_RESULT, savor::PSCmp::EQ, 0u, Success));
    script.ops.push_back(savor::OpGoto(Loop));
    script.ops.push_back(savor::OpLabel(Success));
    script.ops.push_back(savor::OpSaveSavestateFrom(
        key::battlecompletion::OUTPUT_SAVESTATE_PATH));
    script.ops.push_back(savor::OpReturnResult(
        key::battlecompletion::OUTCOME,
        static_cast<std::uint32_t>(phase::battle::endresults::Outcome::Completed)));
    script.ops.push_back(savor::OpLabel(Failure));
    script.ops.push_back(savor::OpGotoIf(
        key::core::DW_RUN_OUTCOME_CODE, savor::PSCmp::NE, 0u, ReturnFailure));
    script.ops.push_back(savor::OpSetU32(
        key::core::DW_RUN_OUTCOME_CODE,
        static_cast<std::uint32_t>(savor::RunToBpOutcome::InputPlaybackFailed)));
    script.ops.push_back(savor::OpLabel(ReturnFailure));
    script.ops.push_back(savor::OpReturnResult(
        key::battlecompletion::OUTCOME,
        static_cast<std::uint32_t>(phase::battle::endresults::Outcome::Failed)));
    return script;
}

} // namespace phase::battle::completion
