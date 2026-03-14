#pragma once
#include <cstdint>
#include <span>

namespace simcore {
	class DolphinWrapper;
	class IDerivedBuffer;
}

namespace addrprog {

	static constexpr size_t PROG_VERSION = 1;

	// Compact stackless sequence of ops operating on a single working VA.
	enum Op : uint8_t {
		END = 0x00,   // stop; final VA is current
		BASE_KEY = 0x01,   // arg:u16 key  -> VA = Registry::base(key)
		LOAD_PTR32 = 0x02,   // VA = *(u32*)VA
		ADD_I32 = 0x03,   // arg:i32 imm  -> VA += imm
		INDEX = 0x04,   // arg:u16 count, u16 stride -> VA += count * stride
		FIELD_OFF = 0x05,   // arg:u32 off  -> VA += off
		// reserved: PTR_CHAIN_N, MUL, MASK, ALIGN, etc.
	};

	struct ExecResult {
		uint32_t va{ 0 };
		bool ok{ false };
	};

	// Executes a program starting at blob[offset], computes final VA.
	// Region must be inferred by the caller (e.g., from a predicate's addr_key).

	ExecResult exec(const uint8_t* blob, size_t blob_size, uint32_t offset,
		simcore::DolphinWrapper& host,
		const simcore::IDerivedBuffer* derived);

} // namespace addrprog
