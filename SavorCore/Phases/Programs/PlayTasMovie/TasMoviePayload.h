#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <optional>

#include "../../../Runner/Script/PhaseScriptProgram.h"                   // PSContext
#include "TasMovieScript.h"                 // canonical TAS keys (K_*)

namespace savor::tasmovie {

    // On-wire binary layout (little-endian):
    // [0]      : uint8  ProgramKind tag (== PK_TasMovie)
    // [1..2]   : u16    version = 3
    // [3]      : u8     flags (bit0: save_on_fail)
    // [4..11]  : 8      reserved
    // [12..15] : u32    len_dtm
    // [16..]   : bytes  dtm_path (not null-terminated)


    struct EncodeSpec {
        std::string dtm_path;
    };

    // Build payload bytes (first byte PK_TasMovie). Returns true on success.
    bool encode_payload(const EncodeSpec& spec, std::vector<uint8_t>& out);

    // Worker-side: parse payload -> populate job context keys used by the TAS program.
    // Also extracts id6 by reading the DTM.
    bool decode_payload(const std::vector<uint8_t>& in, PSContext& out_ctx);

    // Utility: derive <dtm_dir>/<stem(dtm)>.sav.
    std::string derive_save_path(const std::string& dtm_path);

} // namespace savor::tasmovie
