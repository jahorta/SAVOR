#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <optional>

#include "../../../Runner/Script/PhaseScriptProgram.h"   // PSContext

namespace savor::seedprobe {

    inline constexpr std::uint16_t PayloadVersion = 3;

    enum class SeedProbeTarget : std::uint32_t {
        PreBattle = 0,
        FieldReturn = 1,
    };

    enum class SeedProbeMode : std::uint32_t {
        Observe = 0,
        Materialize = 1,
    };

	// Program-local inputs that remain specific to SeedProbe

	// On-wire layout (little-endian), fixed-size first:
	//
	// [0]      : u8   ProgramKind tag (== PK_SeedProbe)
	// [1..2]   : u16  version = 3
	// [3..(3+sizeof(GCInputFrame)-1)] : raw GCInputFrame bytes

	struct EncodeSpec {
		GCInputFrame frame{};
		SeedProbeTarget target{SeedProbeTarget::PreBattle};
		SeedProbeMode mode{SeedProbeMode::Observe};
		std::optional<std::uint32_t> expected_seed;
		std::string output_savestate_path;
	};

	// Parent-side: build payload bytes (first byte = PK_SeedProbe).
	bool encode_payload(const EncodeSpec& spec, std::vector<uint8_t>& out);

	// Worker-side: parse payload -> populate ctx with:
	//   - K_INPUT  -> GCInputFrame
	bool decode_payload(const std::vector<uint8_t>& in, PSContext& out_ctx);

} // namespace savor::seedprobe
