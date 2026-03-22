#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include "../Soa/SoaAddrRegistry.h"
#include "SoaAddrProgram.h"

namespace addrprog {

	class Builder {
	public:
		uint32_t begin() { return 0; }
		void op_base_key(addr::AddrKey k) { emit_byte((uint8_t)Op::BASE_KEY); emit_u16(static_cast<uint16_t>(k)); }
		void op_add_i32(int32_t imm) { emit_byte((uint8_t)Op::ADD_I32);  emit_i32(imm); }
		void op_index(uint16_t count, uint16_t stride) { emit_byte((uint8_t)Op::INDEX); emit_u16(count); emit_u16(stride); }
		void op_field(uint32_t off) { emit_byte((uint8_t)Op::FIELD_OFF); emit_u32(off); }
		void op_end() { emit_byte((uint8_t)Op::END); }

		void op_load_ptr32() { emit_byte((uint8_t)Op::LOAD_PTR32); }

		// NEW: field offset from member pointer (nice IntelliSense)
		template<class T, class M>
		void op_field_of(M T::* member) {
			// Runtime-safe offset computation (no UB): take address diff within a real object
			T tmp{};
			const auto* base = reinterpret_cast<const std::byte*>(&tmp);
			const auto* memp = reinterpret_cast<const std::byte*>(&(tmp.*member));
			const uint32_t off = static_cast<uint32_t>(memp - base);
			op_field(off);
		}

		// NEW: index by element count with stride = sizeof(Elem)
		template<class Elem>
		void op_index_elems(uint32_t index) {
			op_index(static_cast<uint16_t>(index), static_cast<uint16_t>(sizeof(Elem)));
		}

		const std::vector<uint8_t>& blob() const { return blob_; }
		uint32_t size() const { return static_cast<uint32_t>(blob_.size()); }

		// Intern: append a new program and return its starting offset.
		uint32_t finalize_program() { emit_byte((uint8_t)Op::END); return last_prog_offset_; }

		// Manual control: capture current write offset to annotate into PredicateRecord
		uint32_t current_offset() const { return static_cast<uint32_t>(blob_.size()); }

	private:
		void emit_byte(uint8_t b) { if (blob_.empty()) last_prog_offset_ = 0; blob_.push_back(b); }
		void emit_u16(uint16_t v) { blob_.push_back(uint8_t(v)); blob_.push_back(uint8_t(v >> 8)); }
		void emit_i32(int32_t v) { emit_u32(static_cast<uint32_t>(v)); }
		void emit_u32(uint32_t v) { blob_.push_back(uint8_t(v)); blob_.push_back(uint8_t(v >> 8)); blob_.push_back(uint8_t(v >> 16)); blob_.push_back(uint8_t(v >> 24)); }

		uint32_t last_prog_offset_{ 0 };
		std::vector<uint8_t> blob_;
	};

} // namespace addrprog
