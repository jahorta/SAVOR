#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../../../Runner/Script/PhaseScriptProgram.h"

namespace savor::tasframedetector {

struct EncodeSpec {
    std::string dtm_path;
    uint32_t vi_stall_ms{ 0 };
};

bool encode_payload(const EncodeSpec& spec, std::vector<uint8_t>& out);
bool decode_payload(const std::vector<uint8_t>& in, PSContext& out_ctx);

} // namespace savor::tasframedetector
