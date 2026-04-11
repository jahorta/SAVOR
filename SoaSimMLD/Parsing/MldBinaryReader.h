#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace soasim::mld::parsing {

class MldBinaryReader {
public:
    explicit MldBinaryReader(std::span<const std::uint8_t> bytes)
        : bytes_(bytes) {
    }

    [[nodiscard]] std::size_t position() const {
        return position_;
    }

    [[nodiscard]] std::size_t size() const {
        return bytes_.size();
    }

    [[nodiscard]] bool canRead(std::size_t count) const {
        return position_ + count <= bytes_.size();
    }

    [[nodiscard]] std::optional<std::uint8_t> readU8();
    [[nodiscard]] std::optional<std::uint16_t> readU16LE();
    [[nodiscard]] std::optional<std::uint16_t> readU16BE();
    [[nodiscard]] std::optional<std::uint32_t> readU32LE();
    [[nodiscard]] std::optional<std::uint32_t> readU32BE();
    [[nodiscard]] std::optional<float> readF32LE();
    [[nodiscard]] std::optional<std::span<const std::uint8_t>> readBytes(std::size_t count);
    [[nodiscard]] bool seek(std::size_t offset);
    [[nodiscard]] std::size_t remaining() const;

private:
    std::span<const std::uint8_t> bytes_{};
    std::size_t position_ = 0;
};

} // namespace soasim::mld::parsing
