#pragma once
#include "../../../Runner/Script/PSContext.h"

namespace phase::battle::ctx {

    // encode side (parent)
    struct EncodeSpec {};

    // parent helper
    bool encode_payload(const EncodeSpec& spec, std::vector<uint8_t>& out);

    // ProgramRegistry.decode -> fill ctx
    bool decode_payload(const std::vector<uint8_t>& in, savor::PSContext& out_ctx);

}
