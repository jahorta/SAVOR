#pragma once
#include <cstdint>
#include <cstring>
#include "../MemView.h"
#include "../Endian.h"

namespace soa::readers {

	// Copy a struct image verbatim from MEM1 into host memory.
	// (No field-wise swapping yet; safe & deterministic snapshot.)
	template <class T>
	inline bool read_raw(const savor::MemView& view, uint32_t va, T& out) {
		if (!view.valid() || !view.in_mem1(va)) return false;
		return view.read_block(va, &out, sizeof(T));
	}

	template <class T>
	inline bool read(const savor::MemView& view, uint32_t va, T& out) {
		if (!read_raw(view, va, out)) return false;
		savor::endian::fix_endianness_in_place(out);
		return true;
	}

	template <class T>
	inline bool read(const std::string& view, T& out) {
		static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
		if (view.size() != sizeof(T)) return false;            // or < if you allow partials
		std::memcpy(&out, view.data(), view.size());
		savor::endian::fix_endianness_in_place(out);
		return true;
	}

} // namespace soa::readers
