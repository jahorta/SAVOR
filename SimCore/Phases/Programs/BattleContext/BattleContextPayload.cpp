#include "BattleContextPayload.h"

#include "../../../Runner/IPC/Wire.h"

namespace phase::battle::ctx {

	static constexpr int VERSION = 1;

	static inline void put_u32(std::vector<uint8_t>& b, uint32_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v >> 16)); b.push_back(uint8_t(v >> 24)); }
	static inline bool get_u32(const uint8_t*& p, const uint8_t* e, uint32_t& v) { if (p + 4 > e) return false; v = (uint32_t)p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); p += 4; return true; }

	// parent helper
	bool encode_payload(const EncodeSpec& spec, std::vector<uint8_t>& out) {
		out.clear();
		out.push_back(simcore::PK_BattleContextProbe);
		put_u32(out, VERSION);
		put_u32(out, spec.run_ms);
		put_u32(out, spec.vi_stall_ms);
		return true;
	}

	// ProgramRegistry.decode -> fill ctx
	bool decode_payload(const std::vector<uint8_t>& in, simcore::PSContext& out_ctx) {

		if (in.size() < 1 + 4 + 4 + 4) return false;

		const uint8_t* p = in.data();
		const uint8_t* e = p + in.size();

		const uint8_t tag = *p++; if (tag != simcore::PK_BattleContextProbe) return false;

		uint32_t version = 0, run_ms = 0, vi_stall_ms = 0;
		if (!get_u32(p, e, version)) return false;
		if (version != VERSION) return false; // only accept current version
		if (!get_u32(p, e, run_ms)) return false;
		if (!get_u32(p, e, vi_stall_ms)) return false;

		out_ctx[simcore::keys::core::RUN_MS] = run_ms;
		out_ctx[simcore::keys::core::VI_STALL_MS] = vi_stall_ms;
		out_ctx[simcore::keys::core::PROGRESS_CORE_FLAGS] = (uint32_t)0;

		return true;
	}
}