#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "../../MemView.h"
#include "NavigationContext.h"

namespace soa::navigation::ctx::codec {

inline constexpr const char* ext = ".nctx";
inline constexpr std::uint16_t Version = 1;
inline constexpr std::uint16_t FlagGroundPresent = 1u << 0;
inline constexpr std::size_t EncodedSize = 86;

enum class ExtractFailure : std::uint32_t {
    None = 0,
    InvalidMem1 = 1,
    CapturePcMismatch = 2,
    InvalidWorksheet = 3,
    MotionStateMismatch = 4,
    InvalidGroundSelector = 5,
};

bool extract_from_mem1(
    const savor::MemView& view,
    std::uint32_t capture_pc,
    NavigationContext& out,
    ExtractFailure* failure_out = nullptr);

bool encode(const NavigationContext& in, std::string& out);
bool decode(std::string_view in, NavigationContext& out);

} // namespace soa::navigation::ctx::codec
