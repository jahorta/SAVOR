#pragma once
#include "../../../Runner/Script/PSContext.h"

namespace phase::battle::ctx {

    // encode side (parent)
    struct EncodeSpec {
        uint32_t run_ms{ 0 };
        uint32_t vi_stall_ms{ 0 };
    };

    // parent helper
    bool encode_payload(const EncodeSpec& spec, std::vector<uint8_t>& out);

    // ProgramRegistry.decode -> fill ctx
    bool decode_payload(const std::vector<uint8_t>& in, savor::PSContext& out_ctx);

}