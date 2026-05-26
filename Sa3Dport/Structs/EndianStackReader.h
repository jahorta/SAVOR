#pragma once

#include "Structs/Endian.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>

namespace Sa3Dport::Structs {

class EndianStackReader {
public:
    EndianStackReader(std::span<const std::byte> data, Endian endian)
        : data_(data), endian_(endian) {}

    [[nodiscard]] std::uint8_t read_u8(std::uint32_t address) const {
        ensure(address, sizeof(std::uint8_t));
        return static_cast<std::uint8_t>(data_[address]);
    }

    [[nodiscard]] std::int8_t read_i8(std::uint32_t address) const {
        return static_cast<std::int8_t>(read_u8(address));
    }

    [[nodiscard]] std::uint16_t read_u16(std::uint32_t address) const {
        auto value = read_raw<std::uint16_t>(address);
        return needs_swap(endian_) ? byteswap(value) : value;
    }

    [[nodiscard]] std::int16_t read_i16(std::uint32_t address) const {
        return static_cast<std::int16_t>(read_u16(address));
    }

    [[nodiscard]] std::uint32_t read_u32(std::uint32_t address) const {
        auto value = read_raw<std::uint32_t>(address);
        return needs_swap(endian_) ? byteswap(value) : value;
    }

    [[nodiscard]] std::int32_t read_i32(std::uint32_t address) const {
        return static_cast<std::int32_t>(read_u32(address));
    }

    [[nodiscard]] float read_float(std::uint32_t address) const {
        return std::bit_cast<float>(read_u32(address));
    }

    [[nodiscard]] Endian endian() const { return endian_; }
    [[nodiscard]] std::size_t size() const { return data_.size(); }

    [[nodiscard]] std::uint32_t ReadUInt(std::uint32_t address) const { return read_u32(address); }
    [[nodiscard]] float ReadFloat(std::uint32_t address) const { return read_float(address); }

private:
    void ensure(std::uint32_t address, std::size_t size) const {
        if (static_cast<std::size_t>(address) + size > data_.size()) {
            throw std::out_of_range("read beyond end of buffer");
        }
    }

    template <class T>
    [[nodiscard]] T read_raw(std::uint32_t address) const {
        ensure(address, sizeof(T));
        T value {};
        std::memcpy(&value, data_.data() + address, sizeof(T));
        return value;
    }

    std::span<const std::byte> data_;
    Endian endian_;
};

} // namespace Sa3Dport::Structs
