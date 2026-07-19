// Runner/Script/PSContextCodec.h
#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <variant>
#include <unordered_map>
#include "PSContext.h"
#include "CtxRegistry.h"

namespace savor::psctx {

	// magic 'CTX2' (numeric-keys codec v1)
	inline constexpr uint32_t CTX_MAGIC = 0x32585443u;
	inline constexpr uint16_t CTX_VERSION = 1;

	enum class TypeCode : uint8_t {
		U8 = 0x01,
		U16 = 0x02,
		U32 = 0x03,
		F32 = 0x04,
		F64 = 0x05,
		STR = 0x10,
		GC_INPUT_FRAME = 0x20,
		BATTLE_PATH = 0x21,
	};

	struct Header {
		uint32_t magic;
		uint16_t version;
		uint16_t reserved;
		uint32_t count;
	};

	struct EntryPrefix {
		uint16_t key_id;      // savor::context::key::KeyId
		uint8_t  type;        // TypeCode
		uint8_t  reserved;    // 0
		uint32_t vlen;        // payload length
	};

	// Returns true on success. Unknown keys are skipped.
	bool encode_numeric(const PSContext& ctx, std::vector<uint8_t>& out);

	// Returns true on success; unknown key_ids are skipped.
	// On any structural error, returns false and leaves `out` partially filled or untouched.
	bool decode_numeric(const uint8_t* data, size_t size, PSContext& out);

} // namespace savor::psctx
