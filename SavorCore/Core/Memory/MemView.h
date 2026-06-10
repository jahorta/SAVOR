#pragma once
#include <cstdint>
#include <cstddef>
#include <string_view>
#include <cstring>

namespace savor {

    class MemView {
    public:
        MemView() = default;
        MemView(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}

        bool valid() const { return m_data && m_size >= kMem1Size; }

        static constexpr uint32_t kMem1Base = 0x80000000u;
        static constexpr uint32_t kMem1Size = 0x01800000u; // 24 MiB

        bool in_mem1(uint32_t va) const {
            return va >= kMem1Base && va < (kMem1Base + kMem1Size);
        }

        bool read_u8(uint32_t va, uint8_t& out) const { return read_raw(va, &out, 1); }
        bool read_u16(uint32_t va, uint16_t& out) const {
            uint8_t b[2]; if (!read_raw(va, b, 2)) return false;
            out = (uint16_t(b[0]) << 8) | uint16_t(b[1]);
            return true;
        }
        bool read_u32(uint32_t va, uint32_t& out) const {
            uint8_t b[4]; if (!read_raw(va, b, 4)) return false;
            out = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | uint32_t(b[3]);
            return true;
        }
        bool read_u64(uint32_t va, uint64_t& out) const {
            uint8_t b[8]; if (!read_raw(va, b, 8)) return false;
            out = (uint64_t(b[0]) << 56) | (uint64_t(b[1]) << 48) | (uint64_t(b[2]) << 40) | (uint64_t(b[3]) << 32)
                | (uint64_t(b[4]) << 24) | (uint64_t(b[5]) << 16) | (uint64_t(b[6]) << 8) | uint64_t(b[7]);
            return true;
        }
        bool read_block(uint32_t va, void* dst, size_t n) const { return read_raw(va, dst, n); }

    private:
        bool read_raw(uint32_t va, void* dst, size_t n) const {
            if (!m_data || !in_mem1(va)) return false;
            const uint64_t off = uint64_t(va) - kMem1Base;
            if (off + n > m_size) return false;
            std::memcpy(dst, m_data + off, n);
            return true;
        }

        const uint8_t* m_data{ nullptr };
        size_t m_size{ 0 };
    };

} // namespace savor
