#pragma once
#include <string>
#include "../../MemView.h"
#include "BattleContext.h"
#include "../SoaAddrRegistry.h"

namespace soa::battle::ctx::codec {

    static constexpr const char* ext = ".bctx";
    static constexpr const int ver = 1;

	// Extracts the full context from a MEM1 snapshot (fills materialized structs)
	bool extract_from_mem1(const savor::MemView& view, BattleContext& out);

	// Encodes/decodes the rich context (no version bump; v1 is now materialized)
	bool encode(const BattleContext& in, std::string& out);
	bool decode(std::string_view in, BattleContext& out);

    // Resolve to final VA. For PtrChain with spec.base==0, this fails - use resolve_from_base.
    static bool resolve(const savor::MemView& view, const addr::DolphinAddr& a, uint32_t& out_va);

    // Typed reads (use resolve() or resolve_from_base() before calling these if needed).
    static bool readU8(const savor::MemView& v, addr::AddrKey k, uint8_t& out);
    static bool readU16(const savor::MemView& v, addr::AddrKey k, uint16_t& out);
    static bool readU32(const savor::MemView& v, addr::AddrKey k, uint32_t& out);
    static bool readU64(const savor::MemView& v, addr::AddrKey k, uint64_t& out);

} // namespace soa::battle::ctx::codec
