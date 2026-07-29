#pragma once
#include <vector>
#include <cstdint>
#include "../../Runner/Script/PhaseScriptProgram.h"

namespace savor::programs {

    // Returns a fixed main program for the given ProgramKind.
    // The program must use only *_FROM(key) ops and read all dynamic values from context keys.
    PhaseScript build_main_program(uint8_t program_kind);

    // Decodes payload bytes into a PSContext for the active ProgramKind.
    // Returns true on success and fills out_ctx.
    // The function internally validates that the payload matches the active kind (e.g., first byte tag).
    bool decode_payload_for(uint8_t active_program_kind,
        const std::vector<uint8_t>& payload,
        PSContext& out_ctx);

} // namespace savor::programs
