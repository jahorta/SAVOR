#pragma once

#include "../../../Core/Memory/Soa/Navigation/NavigationContext.h"
#include "../../../Runner/Breakpoints/BpRegistry.h"
#include "../../../Runner/Script/CtxRegistry.h"
#include "../../../Runner/Script/PhaseScriptProgram.h"
#include "NavigationContextResult.h"

namespace phase::navigation::ctx {

inline savor::PhaseScript MakeNavigationContextProgram()
{
    namespace key = savor::context::key;
    constexpr auto CaptureKey =
        bp::navigation::NavigationContextInitialPlayerInputReady;
    constexpr auto CapturePc = soa::navigation::ctx::CapturePc;
    constexpr const char* ValidateCapture = "NAVIGATION_VALIDATE_CAPTURE";
    constexpr const char* Capture = "NAVIGATION_CAPTURE";
    constexpr const char* FailTimeout = "NAVIGATION_FAIL_TIMEOUT";
    constexpr const char* FailViStalled = "NAVIGATION_FAIL_VI_STALLED";
    constexpr const char* FailHost = "NAVIGATION_FAIL_HOST";
    constexpr const char* FailUnexpected = "NAVIGATION_FAIL_UNEXPECTED";
    constexpr const char* FailCapturePc = "NAVIGATION_FAIL_CAPTURE_PC";
    constexpr const char* ReturnFailure = "NAVIGATION_RETURN_FAILURE";
    constexpr const char* ReturnFailureWithOutcome =
        "NAVIGATION_RETURN_FAILURE_WITH_OUTCOME";

    savor::PhaseScript script{};
    script.canonical_bp_keys = {CaptureKey};
    script.ops.push_back(savor::OpArmPhaseBps());
    script.ops.push_back(savor::OpLoadSnapshot());
    script.ops.push_back(savor::OpApplyInputFrom(key::navigation::NEUTRAL_INPUT));
    script.ops.push_back(savor::OpRecordCurrentPcTo(key::navigation::ENTRY_PC));
    script.ops.push_back(savor::OpRecordCurrentBp());
    script.ops.push_back(savor::OpGotoIf(
        key::core::RUN_HIT_BP_KEY,
        savor::PSCmp::EQ,
        static_cast<std::uint32_t>(CaptureKey),
        ValidateCapture));
    script.ops.push_back(
        savor::OpSetTimeoutFromKey(key::navigation::RUN_TIMEOUT_MS));
    script.ops.push_back(savor::OpRunUntilBp());
    script.ops.push_back(savor::OpGotoIf(
        key::core::DW_RUN_OUTCOME_CODE,
        savor::PSCmp::EQ,
        static_cast<std::uint32_t>(savor::RunToBpOutcome::Timeout),
        FailTimeout));
    script.ops.push_back(savor::OpGotoIf(
        key::core::DW_RUN_OUTCOME_CODE,
        savor::PSCmp::EQ,
        static_cast<std::uint32_t>(savor::RunToBpOutcome::ViStalled),
        FailViStalled));
    script.ops.push_back(savor::OpGotoIf(
        key::core::DW_RUN_OUTCOME_CODE,
        savor::PSCmp::EQ,
        static_cast<std::uint32_t>(savor::RunToBpOutcome::MovieEnded),
        FailUnexpected));
    script.ops.push_back(savor::OpGotoIf(
        key::core::DW_RUN_OUTCOME_CODE,
        savor::PSCmp::NE,
        static_cast<std::uint32_t>(savor::RunToBpOutcome::Hit),
        FailHost));
    script.ops.push_back(savor::OpLabel(ValidateCapture));
    script.ops.push_back(savor::OpGotoIf(
        key::core::RUN_HIT_BP_KEY,
        savor::PSCmp::NE,
        static_cast<std::uint32_t>(CaptureKey),
        FailUnexpected));
    script.ops.push_back(savor::OpGotoIf(
        key::core::RUN_HIT_PC,
        savor::PSCmp::NE,
        CapturePc,
        FailCapturePc));
    script.ops.push_back(savor::OpGoto(Capture));

    script.ops.push_back(savor::OpLabel(Capture));
    script.ops.push_back(savor::OpGetNavigationContext());
    script.ops.push_back(savor::OpSetU32(
        key::navigation::FAILURE,
        static_cast<std::uint32_t>(FailureCode::SavestateFailed)));
    script.ops.push_back(
        savor::OpSaveSavestateFrom(key::navigation::OUTPUT_SAVESTATE_PATH));
    script.ops.push_back(savor::OpSetU32(
        key::navigation::FAILURE,
        static_cast<std::uint32_t>(FailureCode::None)));
    script.ops.push_back(savor::OpEmitResult(key::navigation::CTX_BLOB));
    script.ops.push_back(savor::OpReturnResult(
        key::navigation::OUTCOME,
        static_cast<std::uint32_t>(Outcome::Completed)));

    script.ops.push_back(savor::OpLabel(FailTimeout));
    script.ops.push_back(savor::OpSetU32(
        key::navigation::FAILURE,
        static_cast<std::uint32_t>(FailureCode::Timeout)));
    script.ops.push_back(savor::OpGoto(ReturnFailure));
    script.ops.push_back(savor::OpLabel(FailViStalled));
    script.ops.push_back(savor::OpSetU32(
        key::navigation::FAILURE,
        static_cast<std::uint32_t>(FailureCode::ViStalled)));
    script.ops.push_back(savor::OpGoto(ReturnFailure));
    script.ops.push_back(savor::OpLabel(FailHost));
    script.ops.push_back(savor::OpSetU32(
        key::navigation::FAILURE,
        static_cast<std::uint32_t>(FailureCode::HostFailure)));
    script.ops.push_back(savor::OpGoto(ReturnFailure));
    script.ops.push_back(savor::OpLabel(FailUnexpected));
    script.ops.push_back(savor::OpSetU32(
        key::navigation::FAILURE,
        static_cast<std::uint32_t>(FailureCode::UnexpectedStop)));
    script.ops.push_back(savor::OpGoto(ReturnFailure));
    script.ops.push_back(savor::OpLabel(FailCapturePc));
    script.ops.push_back(savor::OpSetU32(
        key::navigation::FAILURE,
        static_cast<std::uint32_t>(FailureCode::CapturePcMismatch)));
    script.ops.push_back(savor::OpLabel(ReturnFailure));
    script.ops.push_back(savor::OpGotoIf(
        key::core::DW_RUN_OUTCOME_CODE,
        savor::PSCmp::NE,
        static_cast<std::uint32_t>(savor::RunToBpOutcome::Hit),
        ReturnFailureWithOutcome));
    script.ops.push_back(savor::OpSetU32(
        key::core::DW_RUN_OUTCOME_CODE,
        static_cast<std::uint32_t>(
            savor::RunToBpOutcome::InputPlaybackFailed)));
    script.ops.push_back(savor::OpLabel(ReturnFailureWithOutcome));
    script.ops.push_back(savor::OpReturnResult(
        key::navigation::OUTCOME,
        static_cast<std::uint32_t>(Outcome::Failed)));
    return script;
}

} // namespace phase::navigation::ctx
