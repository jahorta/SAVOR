#include "PhaseScriptVM.h"

#include "../../Core/Memory/MemView.h"
#include "../../Core/Memory/Soa/Navigation/NavigationContextCodec.h"
#include "../../Phases/Programs/NavigationContext/NavigationContextResult.h"
#include "../IPC/Wire.h"

#include <utility>

namespace savor {
namespace {

bool FailNavigationContext(
    PSResult& result,
    PSContext& ctx,
    phase::navigation::ctx::FailureCode failure,
    const char* diagnostic)
{
    namespace key = savor::context::key;
    ctx.erase(key::navigation::CTX_BLOB);
    result.ctx.erase(key::navigation::CTX_BLOB);
    ctx[key::navigation::OUTCOME] =
        static_cast<std::uint32_t>(phase::navigation::ctx::Outcome::Failed);
    ctx[key::navigation::FAILURE] =
        static_cast<std::uint32_t>(failure);
    ctx[key::navigation::DIAGNOSTIC] = std::string{diagnostic};
    ctx[key::core::WORKER_ERROR] =
        static_cast<std::uint32_t>(WERR_UnknownError);
    result.ctx = ctx;
    result.ok = false;
    return false;
}

phase::navigation::ctx::FailureCode MapExtractFailure(
    soa::navigation::ctx::codec::ExtractFailure failure)
{
    using ExtractFailure =
        soa::navigation::ctx::codec::ExtractFailure;
    using FailureCode = phase::navigation::ctx::FailureCode;
    switch (failure) {
    case ExtractFailure::None: return FailureCode::None;
    case ExtractFailure::InvalidMem1: return FailureCode::Mem1Unavailable;
    case ExtractFailure::CapturePcMismatch:
        return FailureCode::CapturePcMismatch;
    case ExtractFailure::InvalidWorksheet:
        return FailureCode::InvalidWorksheet;
    case ExtractFailure::MotionStateMismatch:
        return FailureCode::MotionStateMismatch;
    case ExtractFailure::InvalidGroundSelector:
        return FailureCode::InvalidGroundSelector;
    }
    return FailureCode::Mem1Unavailable;
}

const char* ExtractFailureDiagnostic(
    soa::navigation::ctx::codec::ExtractFailure failure)
{
    using ExtractFailure =
        soa::navigation::ctx::codec::ExtractFailure;
    switch (failure) {
    case ExtractFailure::None: return "";
    case ExtractFailure::InvalidMem1:
        return "navigation context MEM1 snapshot is invalid";
    case ExtractFailure::CapturePcMismatch:
        return "navigation context capture PC does not match";
    case ExtractFailure::InvalidWorksheet:
        return "navigation player worksheet is null or unreadable";
    case ExtractFailure::MotionStateMismatch:
        return "navigation player motion state is not ordinary state 1";
    case ExtractFailure::InvalidGroundSelector:
        return "navigation ground selector is unreadable";
    }
    return "navigation context extraction failed";
}

} // namespace

bool PhaseScriptVM::op_get_navigation_context(
    PSResult& result,
    PSContext& ctx) const
{
    namespace key = savor::context::key;
    using FailureCode = phase::navigation::ctx::FailureCode;
    constexpr auto CaptureKey =
        bp::navigation::NavigationContextInitialPlayerInputReady;
    constexpr auto CapturePc = soa::navigation::ctx::CapturePc;

    ctx.erase(key::navigation::CTX_BLOB);
    result.ctx.erase(key::navigation::CTX_BLOB);

    std::uint32_t run_outcome = 0;
    std::uint32_t hit_key = 0;
    std::uint32_t hit_pc = 0;
    if (!ctx.get(key::core::DW_RUN_OUTCOME_CODE, run_outcome)
        || !ctx.get(key::core::RUN_HIT_BP_KEY, hit_key)
        || !ctx.get(key::core::RUN_HIT_PC, hit_pc)
        || run_outcome
            != static_cast<std::uint32_t>(RunToBpOutcome::Hit)
        || hit_key != static_cast<std::uint32_t>(CaptureKey)) {
        return FailNavigationContext(
            result,
            ctx,
            FailureCode::UnexpectedStop,
            "navigation context opcode did not follow the expected breakpoint hit");
    }

    const auto current_pc = host_.getPC();
    if (current_pc != CapturePc || hit_pc != CapturePc) {
        return FailNavigationContext(
            result,
            ctx,
            FailureCode::CapturePcMismatch,
            "navigation context opcode is not paused at the capture PC");
    }

    std::string mem1;
    if (!host_.getMem1(mem1)) {
        return FailNavigationContext(
            result,
            ctx,
            FailureCode::Mem1Unavailable,
            "navigation context MEM1 snapshot is unavailable");
    }

    const savor::MemView view(
        reinterpret_cast<const std::uint8_t*>(mem1.data()),
        mem1.size());
    soa::navigation::ctx::NavigationContext navigation_context{};
    soa::navigation::ctx::codec::ExtractFailure extract_failure =
        soa::navigation::ctx::codec::ExtractFailure::None;
    if (!soa::navigation::ctx::codec::extract_from_mem1(
            view,
            current_pc,
            navigation_context,
            &extract_failure)) {
        return FailNavigationContext(
            result,
            ctx,
            MapExtractFailure(extract_failure),
            ExtractFailureDiagnostic(extract_failure));
    }

    std::string blob;
    if (!soa::navigation::ctx::codec::encode(
            navigation_context,
            blob)) {
        return FailNavigationContext(
            result,
            ctx,
            FailureCode::EncodeFailed,
            "navigation context encoding failed");
    }

    ctx[key::navigation::CTX_BLOB] = std::move(blob);
    ctx[key::navigation::OUTCOME] =
        static_cast<std::uint32_t>(phase::navigation::ctx::Outcome::Failed);
    ctx[key::navigation::FAILURE] =
        static_cast<std::uint32_t>(FailureCode::None);
    ctx[key::navigation::DIAGNOSTIC] = std::string{};
    return true;
}

} // namespace savor
