#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace Sa3Dport::Structs {

enum class Endianness {
    Little,
    Big,
};

class EndianReader {
public:
    EndianReader(std::span<const std::byte> buffer, Endianness endianness, std::uint32_t imageBase = 0)
        : buffer_(buffer), endianness_(endianness), imageBase_(imageBase) {}

    [[nodiscard]] std::size_t Position() const {
        return position_;
    }

    [[nodiscard]] std::uint32_t ImageBase() const {
        return imageBase_;
    }

    void Seek(std::size_t position) {
        if (position > buffer_.size()) {
            throw std::out_of_range("seek beyond end of buffer");
        }
        position_ = position;
    }

    [[nodiscard]] std::uint8_t ReadU8() {
        return ReadIntegral<std::uint8_t>();
    }

    [[nodiscard]] std::uint16_t ReadU16() {
        return ReadIntegral<std::uint16_t>();
    }

    [[nodiscard]] std::uint32_t ReadU32() {
        return ReadIntegral<std::uint32_t>();
    }

    [[nodiscard]] std::int32_t ReadI32() {
        return ReadIntegral<std::int32_t>();
    }

    [[nodiscard]] float ReadF32() {
        const auto bits = ReadIntegral<std::uint32_t>();
        return std::bit_cast<float>(bits);
    }

    [[nodiscard]] std::uint32_t ReadPointerOffset() {
        const auto absolute = ReadU32();
        return absolute >= imageBase_ ? absolute - imageBase_ : absolute;
    }

private:
    template <typename T>
    [[nodiscard]] T ReadIntegral() {
        static_assert(std::is_integral_v<T>, "T must be an integral type");
        constexpr std::size_t kSize = sizeof(T);

        if (position_ + kSize > buffer_.size()) {
            throw std::out_of_range("read beyond end of buffer");
        }

        std::array<std::byte, kSize> raw {};
        std::memcpy(raw.data(), buffer_.data() + position_, kSize);
        position_ += kSize;

        if ((endianness_ == Endianness::Little && std::endian::native == std::endian::little) ||
            (endianness_ == Endianness::Big && std::endian::native == std::endian::big)) {
            T value {};
            std::memcpy(&value, raw.data(), kSize);
            return value;
        }

        std::array<std::byte, kSize> swapped {};
        for (std::size_t i = 0; i < kSize; ++i) {
            swapped[i] = raw[kSize - 1 - i];
        }

        T value {};
        std::memcpy(&value, swapped.data(), kSize);
        return value;
    }

    std::span<const std::byte> buffer_;
    Endianness endianness_;
    std::uint32_t imageBase_ = 0;
    std::size_t position_ = 0;
};

} // namespace Sa3Dport::Structs
