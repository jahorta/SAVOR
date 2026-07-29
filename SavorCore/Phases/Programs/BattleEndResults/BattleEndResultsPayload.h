#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "BattleEndResultsReport.h"
#include "../../../Runner/Script/PhaseScriptProgram.h"

namespace phase::battle::endresults {

inline constexpr std::uint32_t PayloadVersion = 3;

struct EncodeSpec {
    AccelerationPolicy acceleration_policy{AccelerationPolicy::FullAdaptive};
    std::string completion_manifest_blob;
    std::string output_savestate_path;
};

bool encode_payload(const EncodeSpec& spec, std::vector<std::uint8_t>& out);
bool decode_payload(const std::vector<std::uint8_t>& in, savor::PSContext& out_ctx);

} // namespace phase::battle::endresults
