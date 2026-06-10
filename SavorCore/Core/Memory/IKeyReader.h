#pragma once
#include <cstdint>
#include "Soa/SoaAddrRegistry.h"

struct IKeyReader {
	virtual ~IKeyReader() = default;
	virtual bool read_key(addr::AddrKey k, uint8_t width, uint64_t& out_bits) = 0;
};
