#pragma once
#include "../../../Runner/Script/PhaseScriptVM.h"
#include "../../../Runner/Script/KeyRegistry.h"
#include "../../../Core/Memory/Soa/SoaAddrRegistry.h"
#include "../../../Runner/Breakpoints/BPRegistry.h"
#include "../BattleRunner/BattleOutcome.h"

namespace simcore::seedprobe {

    static constexpr keys::KeyId DW_Outcome = keys::core::DW_RUN_OUTCOME_CODE;
    static constexpr keys::KeyId Battle_Outcome = keys::battle::BATTLE_OUTCOME;

    static const std::string LabelDWErr = "RET_DW_RUN_ERROR";
    
    // Build a small program for "apply 1 frame input, then run-until-bp, then read RNG"
    inline PhaseScript MakeSeedProbeProgram()
    {
        PhaseScript ps{};
        ps.canonical_bp_keys = { bp::prebattle::AfterRandSeedSet };

        ps.ops.push_back(OpArmPhaseBps());
        ps.ops.push_back(OpLoadSnapshot());

        // Apply input (from numeric key)
        ps.ops.push_back(OpApplyInputFrom(simcore::keys::seed::INPUT));

        // Run until RNG seed set breakpoint
        ps.ops.push_back(OpRunUntilBp());
        ps.ops.push_back(OpGotoIf(DW_Outcome, PSCmp::NE, 0, LabelDWErr));

        // Read RNG and emit
        ps.ops.push_back(OpReadU32(addr::Registry::base(addr::core::RNG_SEED), simcore::keys::seed::RNG_SEED));

        ps.ops.push_back(OpEmitResult(simcore::keys::seed::RNG_SEED));
        ps.ops.push_back(OpReturnResult(DW_Outcome, (uint32_t)RunToBpOutcome::Hit));

        // ============  Label Dolphin Wrapper Run Error  ===================
        ps.ops.push_back(OpLabel(LabelDWErr));
        ps.ops.push_back(OpReturnResult(Battle_Outcome, (uint32_t)simcore::battle::Outcome::DWRunErr));

        return ps;
    }

} // namespace simcore
