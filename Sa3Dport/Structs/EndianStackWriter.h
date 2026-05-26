#pragma once

#include "Structs/Endian.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Sa3Dport::Structs {

class EndianStackWriter {
public:
    explicit EndianStackWriter(Endian endian) : endian_(endian) {}

    [[nodiscard]] std::uint32_t pointer_position() const {
        return static_cast<std::uint32_t>(data_.size());
    }

    void write_u8(std::uint8_t value) { data_.push_back(static_cast<std::byte>(value)); }
    void write_i8(std::int8_t value) { write_u8(static_cast<std::uint8_t>(value)); }
    void write_u16(std::uint16_t value) { write_integral(needs_swap(endian_) ? byteswap(value) : value); }
    void write_i16(std::int16_t value) { write_u16(static_cast<std::uint16_t>(value)); }
    void write_u32(std::uint32_t value) { write_integral(needs_swap(endian_) ? byteswap(value) : value); }
    void write_i32(std::int32_t value) { write_u32(static_cast<std::uint32_t>(value)); }
    void write_float(float value) { write_u32(std::bit_cast<std::uint32_t>(value)); }

    [[nodiscard]] Endian endian() const { return endian_; }
    [[nodiscard]] const std::vector<std::byte>& data() const { return data_; }
    [[nodiscard]] std::vector<std::byte> take_data() { return std::move(data_); }

private:
    template <class T>
    void write_integral(T value) {
        const auto* bytes = reinterpret_cast<const std::byte*>(&value);
        data_.insert(data_.end(), bytes, bytes + sizeof(T));
    }

    Endian endian_;
    std::vector<std::byte> data_;
};

} // namespace Sa3Dport::Structs
