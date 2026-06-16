#pragma once
#include "../../../Runner/Script/PhaseScriptVM.h"
#include "../../../Runner/Script/CtxRegistry.h"
#include "../../../Core/Memory/Soa/SoaAddrRegistry.h"
#include "../../../Runner/Breakpoints/BpRegistry.h"
#include "../BattleRunner/BattleOutcome.h"

namespace savor::seedprobe {

    static constexpr savor::context::key::KeyId DW_Outcome = savor::context::key::core::DW_RUN_OUTCOME_CODE;
    static constexpr savor::context::key::KeyId Battle_Outcome = savor::context::key::battle::BATTLE_OUTCOME;

    static const std::string LabelDWErr = "RET_DW_RUN_ERROR";
    
    // Build a small program for "apply 1 frame input, then run-until-bp, then read RNG"
    inline PhaseScript MakeSeedProbeProgram()
    {
        PhaseScript ps{};
        ps.canonical_bp_keys = { bp::prebattle::AfterRandSeedSet };

        ps.ops.push_back(OpArmPhaseBps());
        ps.ops.push_back(OpLoadSnapshot());

        // Apply input (from numeric key)
        ps.ops.push_back(OpApplyInputFrom(savor::context::key::seed::INPUT));

        // Run until RNG seed set breakpoint
        ps.ops.push_back(OpRunUntilBp());
        ps.ops.push_back(OpGotoIf(DW_Outcome, PSCmp::NE, 0, LabelDWErr));

        // Read RNG and emit
        ps.ops.push_back(OpReadU32(addr::AddrRegistry::base(addr::core::RNG_SEED), savor::context::key::seed::RNG_SEED));

        ps.ops.push_back(OpEmitResult(savor::context::key::seed::RNG_SEED));
        ps.ops.push_back(OpReturnResult(DW_Outcome, (uint32_t)RunToBpOutcome::Hit));

        // ============  Label Dolphin Wrapper Run Error  ===================
        ps.ops.push_back(OpLabel(LabelDWErr));
        ps.ops.push_back(OpReturnResult(Battle_Outcome, (uint32_t)savor::battle::Outcome::DWRunErr));

        return ps;
    }

} // namespace savor
