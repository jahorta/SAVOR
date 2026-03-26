#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <optional>

#include "../../../Runner/Script/PhaseScriptVM.h"                        // PSContext
#include "TasMovieScript.h"                 // canonical TAS keys (K_*)

namespace simcore::tasmovie {

    // On-wire binary layout (little-endian):
    // [0]      : uint8  ProgramKind tag (== PK_TasMovie)
    // [1..2]   : u16    version = 1
    // [3]      : u8     flags (bit0: save_on_fail)
    // [4..7]   : u32    run_ms (0 means "derive from header")
    // [8..11]  : u32    vi_stall_ms
    // [12]     : u8     headroom_x10
    // [13..19] : 7      reserved (kept for future checksum, currently zeroed)
    // [20..23] : u32    len_dtm
    // [24..]   : bytes  dtm_path (not null-terminated)


    struct EncodeSpec {
        std::string dtm_path;
        uint32_t    run_ms{ 0 };       // 0 => derive from DTM header (VI/input count + headroom)
        uint32_t    vi_stall_ms{ 2000 };
        bool        progress_enable{ false };
        uint8_t     headroom_x10{ 15 };
    };

    // Build payload bytes (first byte PK_TasMovie). Returns true on success.
    bool encode_payload(const EncodeSpec& spec, std::vector<uint8_t>& out);

    // Worker-side: parse payload -> populate job context keys used by the TAS program.
    // Also derives run_ms if the payload had run_ms==0, and extracts id6 by reading the DTM.
    bool decode_payload(const std::vector<uint8_t>& in, PSContext& out_ctx);

    // Utility (exposed for tests): compute a conservative run time from VI/input counts.
    uint32_t compute_run_ms_from_counts(uint64_t vi_count, uint64_t input_count, double headroom = 1.15);

    // Utility (exposed for tests and coordinators): join save_dir + stem(dtm) + ".sav".
    std::string derive_save_path(const std::string& dtm_path, const std::string& save_dir);

} // namespace simcore::tasmovie
