#pragma once

#include <cstdint>

namespace phase::navigation::ctx {

enum class Outcome : std::uint32_t {
    Completed = 0,
    Failed = 1,
};

enum class FailureCode : std::uint32_t {
    None = 0,
    Timeout = 1,
    ViStalled = 2,
    HostFailure = 3,
    UnexpectedStop = 4,
    CapturePcMismatch = 5,
    Mem1Unavailable = 6,
    InvalidWorksheet = 7,
    MotionStateMismatch = 8,
    InvalidGroundSelector = 9,
    EncodeFailed = 10,
    SavestateFailed = 11,
};

inline constexpr const char* FailureCodeName(FailureCode code) noexcept
{
    switch (code) {
    case FailureCode::None: return "none";
    case FailureCode::Timeout: return "timeout";
    case FailureCode::ViStalled: return "vi_stalled";
    case FailureCode::HostFailure: return "host_failure";
    case FailureCode::UnexpectedStop: return "unexpected_stop";
    case FailureCode::CapturePcMismatch: return "capture_pc_mismatch";
    case FailureCode::Mem1Unavailable: return "mem1_unavailable";
    case FailureCode::InvalidWorksheet: return "invalid_worksheet";
    case FailureCode::MotionStateMismatch: return "motion_state_mismatch";
    case FailureCode::InvalidGroundSelector: return "invalid_ground_selector";
    case FailureCode::EncodeFailed: return "encode_failed";
    case FailureCode::SavestateFailed: return "savestate_failed";
    }
    return "unrecognized";
}

} // namespace phase::navigation::ctx
