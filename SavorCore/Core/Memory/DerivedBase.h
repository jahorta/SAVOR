#pragma once
#include <cstdint>
#include <string>
#include <cstring>
#include <span>
#include "Soa/SoaAddrRegistry.h"
#include "../../Runner/Script/PSContext.h" // PSContext fwd-dep for update_on_bp

namespace savor {

	class IDerivedBuffer {
	public:

		virtual ~IDerivedBuffer() = default;

		virtual bool can_serve(addr::AddrKey k) const = 0;

		virtual bool read_key(addr::AddrKey k, uint8_t width, uint64_t& out_bits) const = 0;

		// NEW: raw byte-offset read inside the derived buffer
		virtual bool read_raw(uint32_t offset, uint8_t width, uint64_t& out_bits) const = 0;

		virtual void on_init(const PSContext& ctx) { (void)ctx; }

		virtual void update_on_bp(uint32_t hit_bp, const PSContext& ctx, savor::DolphinWrapper& host) = 0;
	};

} // namespace savor
