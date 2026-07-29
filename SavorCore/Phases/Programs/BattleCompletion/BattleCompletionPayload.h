#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../../../Runner/Script/PhaseScriptProgram.h"

namespace phase::battle::completion {

inline constexpr std::uint32_t PayloadVersion = 2;

struct EncodeSpec {
    std::string output_savestate_path;
};

bool encode_payload(const EncodeSpec& spec, std::vector<std::uint8_t>& out);
bool decode_payload(const std::vector<std::uint8_t>& in, savor::PSContext& out_ctx);

} // namespace phase::battle::completion
