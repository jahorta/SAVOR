#include "BattleContextPayload.h"

#include "../../../Runner/IPC/Wire.h"
#include "../../../Runner/Script/ScriptProgress.h"

namespace phase::battle::ctx {

	static constexpr int VERSION = 2;

	static inline void put_u32(std::vector<uint8_t>& b, uint32_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v >> 16)); b.push_back(uint8_t(v >> 24)); }
	static inline bool get_u32(const uint8_t*& p, const uint8_t* e, uint32_t& v) { if (p + 4 > e) return false; v = (uint32_t)p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); p += 4; return true; }

	// parent helper
	bool encode_payload(const EncodeSpec& spec, std::vector<uint8_t>& out) {
		out.clear();
		out.push_back(savor::PK_BattleContextProbe);
		put_u32(out, VERSION);
		return true;
	}

	// ProgramRegistry.decode -> fill ctx
	bool decode_payload(const std::vector<uint8_t>& in, savor::PSContext& out_ctx) {

		if (in.size() != 1 + 4) return false;

		const uint8_t* p = in.data();
		const uint8_t* e = p + in.size();

		const uint8_t tag = *p++; if (tag != savor::PK_BattleContextProbe) return false;

		uint32_t version = 0;
		if (!get_u32(p, e, version)) return false;
		if (version != VERSION) return false; // only accept current version
		if (p != e) return false;
		savor::progress::ProgressDeets progress{};
		progress.set_flag(CoreProgressFlags::DontRecordHeartbeat);
		out_ctx[savor::context::key::core::PROGRESS_CORE_FLAGS] = progress.flags;

		return true;
	}
}
