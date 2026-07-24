#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../../../Runner/Script/PhaseScriptProgram.h"

namespace phase::navigation::ctx {

inline constexpr std::uint32_t PayloadVersion = 1;

struct EncodeSpec {
    std::uint32_t run_timeout_ms{0};
    std::string output_savestate_path;
};

bool encode_payload(
    const EncodeSpec& spec,
    std::vector<std::uint8_t>& out);
bool decode_payload(
    const std::vector<std::uint8_t>& in,
    savor::PSContext& out_ctx);

} // namespace phase::navigation::ctx
