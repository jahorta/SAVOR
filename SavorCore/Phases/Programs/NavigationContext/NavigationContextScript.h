#pragma once

#include "../../../Runner/Script/PhaseScriptProgram.h"

namespace phase::navigation::ctx {

inline savor::PhaseScript MakeNavigationContextProgram(BPKey capture_bp)
{
    savor::PhaseScript script{};
    script.canonical_bp_keys = { capture_bp };

    static const std::string LabelCaptureContext = "CAPTURE_CONTEXT";

    script.ops.push_back(savor::OpArmPhaseBps());
    script.ops.push_back(savor::OpLoadSnapshot());
    script.ops.push_back(savor::OpRecordCurrentBp());
    script.ops.push_back(savor::OpGotoIf(
        savor::context::key::core::RUN_HIT_BP_KEY,
        savor::PSCmp::EQ,
        static_cast<std::uint32_t>(capture_bp),
        LabelCaptureContext));
    script.ops.push_back(savor::OpRunUntilBp());
    script.ops.push_back(savor::OpLabel(LabelCaptureContext));
    script.ops.push_back(savor::OpGetNavigationContext());
    script.ops.push_back(savor::OpEmitResult(
        savor::context::key::navigation::CTX_BLOB));
    return script;
}

} // namespace phase::navigation::ctx
