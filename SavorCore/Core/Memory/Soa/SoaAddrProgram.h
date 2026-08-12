#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace savor {
	class DolphinWrapper;
}

namespace addrprog {

	static constexpr size_t PROG_VERSION = 1;
	static constexpr size_t PROG_VERSION_V2 = 2;

	// Compact stackless sequence of ops operating on a single working VA.
	enum Op : uint8_t {
		END = 0x00,   // stop; final VA is current
		BASE_KEY = 0x01,   // arg:u16 key  -> VA = Registry::base(key)
		LOAD_PTR32 = 0x02,   // VA = *(u32*)VA
		ADD_I32 = 0x03,   // arg:i32 imm  -> VA += imm
		INDEX = 0x04,   // arg:u16 count, u16 stride -> VA += count * stride
		FIELD_OFF = 0x05,   // arg:u32 off  -> VA += off
		BASE_GPR = 0x06,   // arg:u8 reg   -> VA = GPR[reg]
		BASE_ABS = 0x07,   // arg:u32 va   -> VA = va
		// reserved: PTR_CHAIN_N, MUL, MASK, ALIGN, etc.
	};

	struct ExecResult {
		uint32_t va{ 0 };
		bool ok{ false };
	};

	struct EvalTraceStep {
		uint8_t op{ 0 };
		std::string op_name;
		uint32_t address_before{ 0 };
		uint32_t address_after{ 0 };
		uint64_t value{ 0 };
		bool has_value{ false };
	};

	struct EvalResult {
		uint32_t va{ 0 };
		bool ok{ false };
		std::string error;
		std::vector<EvalTraceStep> trace;
	};

	using RegisterReadFn = std::function<bool(uint8_t, uint32_t&)>;

	// Executes a program starting at blob[offset], computes final VA.
	// Region must be inferred by the caller (e.g., from a predicate's addr_key).

	ExecResult exec(const uint8_t* blob, size_t blob_size, uint32_t offset,
		savor::DolphinWrapper& host);

	EvalResult evaluate(const uint8_t* blob, size_t blob_size, uint32_t offset,
		savor::DolphinWrapper& host,
		const RegisterReadFn& read_register = {},
		bool include_trace = false);

	bool read_value(const uint8_t* blob, size_t blob_size, uint32_t offset,
		savor::DolphinWrapper& host,
		uint8_t width,
		uint64_t& out_bits,
		EvalResult* eval_out = nullptr,
		const RegisterReadFn& read_register = {},
		bool include_trace = false);

} // namespace addrprog
