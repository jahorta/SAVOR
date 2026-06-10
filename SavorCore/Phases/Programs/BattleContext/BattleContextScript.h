#pragma once
#include "../../../Runner/Script/PhaseScriptVM.h"
#include "../../../Runner/Breakpoints/BPRegistry.h" // for battle::FirstTurnInputs

using namespace savor;

namespace phase::battle::ctx {

    inline PhaseScript MakeBattleContextProbeProgram()
    {
        PhaseScript ps{};
        ps.canonical_bp_keys = { bp::battle::TurnInputs };

        static const std::string LabelCaptureContext = "CAPTURE_CONTEXT";

        ps.ops.push_back(OpArmPhaseBps());  // arms canonical list above
        ps.ops.push_back(OpLoadSnapshot()); // caller must have placed savestate into VM
        ps.ops.push_back(OpRecordCurrentBp());
        ps.ops.push_back(OpGotoIf(
            savor::keys::core::RUN_HIT_BP_KEY,
            PSCmp::EQ,
            static_cast<uint32_t>(bp::battle::TurnInputs),
            LabelCaptureContext));
        ps.ops.push_back(OpRunUntilBp());   // run neutral/turn transition -> accept input
        ps.ops.push_back(OpLabel(LabelCaptureContext));
        ps.ops.push_back(OpGetBattleContext());
        ps.ops.push_back(OpEmitResult(savor::keys::battle::CTX_BLOB));
        return ps;
    }

} // namespace savor::battlectx
