#include "MldBinaryReader.h"

namespace soasim::mld::parsing {

std::optional<std::uint8_t> MldBinaryReader::readU8() {
    if (!canRead(1)) {
        return std::nullopt;
    }

    return bytes_[position_++];
}

std::optional<std::uint16_t> MldBinaryReader::readU16LE() {
    if (!canRead(2)) {
        return std::nullopt;
    }

    const std::uint16_t value = static_cast<std::uint16_t>(bytes_[position_]) |
        (static_cast<std::uint16_t>(bytes_[position_ + 1]) << 8);
    position_ += 2;
    return value;
}

std::optional<std::uint16_t> MldBinaryReader::readU16BE() {
    if (!canRead(2)) {
        return std::nullopt;
    }

    const std::uint16_t value = (static_cast<std::uint16_t>(bytes_[position_]) << 8) |
        static_cast<std::uint16_t>(bytes_[position_ + 1]);
    position_ += 2;
    return value;
}

std::optional<std::uint32_t> MldBinaryReader::readU32LE() {
    if (!canRead(4)) {
        return std::nullopt;
    }

    const std::uint32_t value = static_cast<std::uint32_t>(bytes_[position_]) |
        (static_cast<std::uint32_t>(bytes_[position_ + 1]) << 8) |
        (static_cast<std::uint32_t>(bytes_[position_ + 2]) << 16) |
        (static_cast<std::uint32_t>(bytes_[position_ + 3]) << 24);
    position_ += 4;
    return value;
}

std::optional<std::uint32_t> MldBinaryReader::readU32BE() {
    if (!canRead(4)) {
        return std::nullopt;
    }

    const std::uint32_t value = (static_cast<std::uint32_t>(bytes_[position_]) << 24) |
        (static_cast<std::uint32_t>(bytes_[position_ + 1]) << 16) |
        (static_cast<std::uint32_t>(bytes_[position_ + 2]) << 8) |
        static_cast<std::uint32_t>(bytes_[position_ + 3]);
    position_ += 4;
    return value;
}

std::optional<std::span<const std::uint8_t>> MldBinaryReader::readBytes(std::size_t count) {
    if (!canRead(count)) {
        return std::nullopt;
    }

    const std::span<const std::uint8_t> range = bytes_.subspan(position_, count);
    position_ += count;
    return range;
}

} // namespace soasim::mld::parsing
