#pragma once

#include "../../../Runner/Script/PhaseScriptProgram.h"
#include "../../../Runner/Script/CtxRegistry.h"
#include "../../../Core/Memory/Soa/SoaAddrRegistry.h"
#include "../../../Runner/Breakpoints/BpRegistry.h"
#include "../BattleRunner/BattleOutcome.h"
#include "SeedProbePayload.h"

namespace savor::seedprobe {

inline PhaseScript MakeSeedProbeProgram()
{
    namespace key = savor::context::key;
    constexpr const char* PreBattle = "SEED_PROBE_PRE_BATTLE";
    constexpr const char* ReadSeed = "SEED_PROBE_READ_SEED";
    constexpr const char* Observe = "SEED_PROBE_OBSERVE";
    constexpr const char* Mismatch = "SEED_PROBE_MISMATCH";
    constexpr const char* RunError = "SEED_PROBE_RUN_ERROR";

    PhaseScript script{};
    // The internal field-return checkpoint is the only canonical stop. The
    // public pre-battle checkpoint remains gated and is selected explicitly
    // by RUN_UNTIL_BP_KEY for legacy/v1 probe jobs.
    script.canonical_bp_keys = {bp::battle::BattleEndFieldReturnReseedComplete};
    script.gated_bp_keys = {bp::prebattle::AfterRandSeedSet};
    script.ops.push_back(OpArmPhaseBps());
    script.ops.push_back(OpLoadSnapshot());
    script.ops.push_back(OpApplyInputFrom(key::seed::INPUT));
    script.ops.push_back(OpGotoIf(
        key::seed::TARGET, PSCmp::EQ,
        static_cast<std::uint32_t>(SeedProbeTarget::PreBattle), PreBattle));

    // FieldReturn starts at BattleEndRewardCommitComplete and reaches the
    // instruction immediately after the field script's RNG::srand call.
    script.ops.push_back(OpRunUntilBp());
    script.ops.push_back(OpGotoIf(
        key::core::DW_RUN_OUTCOME_CODE, PSCmp::NE, 0u, RunError));
    script.ops.push_back(OpGoto(ReadSeed));

    script.ops.push_back(OpLabel(PreBattle));
    script.ops.push_back(OpRunUntilBpKey(bp::prebattle::AfterRandSeedSet));
    script.ops.push_back(OpGotoIf(
        key::core::DW_RUN_OUTCOME_CODE, PSCmp::NE, 0u, RunError));

    script.ops.push_back(OpLabel(ReadSeed));
    script.ops.push_back(OpReadU32(
        addr::AddrRegistry::base(addr::core::RNG_SEED), key::seed::RNG_SEED));
    script.ops.push_back(OpGotoIf(
        key::seed::MODE, PSCmp::EQ,
        static_cast<std::uint32_t>(SeedProbeMode::Observe), Observe));
    script.ops.push_back(OpGotoIfKeys(
        key::seed::RNG_SEED, PSCmp::NE, key::seed::EXPECTED_SEED, Mismatch));
    script.ops.push_back(OpSaveSavestateFrom(key::seed::OUTPUT_SAVESTATE_PATH));

    script.ops.push_back(OpLabel(Observe));
    script.ops.push_back(OpEmitResult(key::seed::RNG_SEED));
    script.ops.push_back(OpReturnResult(
        key::core::DW_RUN_OUTCOME_CODE,
        static_cast<std::uint32_t>(RunToBpOutcome::Hit)));

    script.ops.push_back(OpLabel(Mismatch));
    script.ops.push_back(OpSetU32(
        key::core::DW_RUN_OUTCOME_CODE,
        static_cast<std::uint32_t>(RunToBpOutcome::InputPlaybackFailed)));
    script.ops.push_back(OpReturnResult(
        key::core::DW_RUN_OUTCOME_CODE,
        static_cast<std::uint32_t>(RunToBpOutcome::InputPlaybackFailed)));

    script.ops.push_back(OpLabel(RunError));
    script.ops.push_back(OpReturnResult(
        key::battle::BATTLE_OUTCOME,
        static_cast<std::uint32_t>(savor::battle::Outcome::DWRunErr)));
    return script;
}

} // namespace savor::seedprobe
