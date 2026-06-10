#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../../../Runner/Script/PhaseScriptVM.h"   // PSContext

namespace savor::seedprobe {

	// Program-local inputs that remain specific to SeedProbe

	// On-wire layout (little-endian), fixed-size first:
	//
	// [0]      : u8   ProgramKind tag (== PK_SeedProbe)
	// [1..2]   : u16  version = 1
	// [3..6]   : u32  run_ms (0 => use VM defaults / script-set timeout)
	// [7..10]  : u32  vi_stall_ms (0 => disabled)
	// [11..(11+sizeof(GCInputFrame)-1)] : raw GCInputFrame bytes
	//
	// Goal: let the decoder set common run knobs via VMCoreKeys and provide the input frame.

	struct EncodeSpec {
		GCInputFrame frame{};
		uint32_t     run_ms{ 0 };        // 0 => derive from VM/script defaults
		uint32_t     vi_stall_ms{ 0 };   // 0 => disabled
	};

	// Parent-side: build payload bytes (first byte = PK_SeedProbe).
	bool encode_payload(const EncodeSpec& spec, std::vector<uint8_t>& out);

	// Worker-side: parse payload -> populate ctx with:
	//   - K_INPUT  -> GCInputFrame
	//   - core.input.run_ms / core.input.vi_stall_ms (if nonzero)
	bool decode_payload(const std::vector<uint8_t>& in, PSContext& out_ctx);

} // namespace savor::seedprobe
