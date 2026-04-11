#include "MldBinaryReader.h"

#include "../common/ByteUtils.h"

#include <cstring>

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

    const auto value = common::readU32AtLE(bytes_, position_);
    if (!value.has_value()) {
        return std::nullopt;
    }

    position_ += 4;
    return *value;
}

std::optional<std::uint32_t> MldBinaryReader::readU32BE() {
    if (!canRead(4)) {
        return std::nullopt;
    }

    const auto value = common::readU32AtBE(bytes_, position_);
    if (!value.has_value()) {
        return std::nullopt;
    }

    position_ += 4;
    return *value;
}

std::optional<float> MldBinaryReader::readF32LE() {
    const auto bits = readU32LE();
    if (!bits.has_value()) {
        return std::nullopt;
    }

    float value = 0.0f;
    std::memcpy(&value, &(*bits), sizeof(float));
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

bool MldBinaryReader::seek(std::size_t offset) {
    if (offset > bytes_.size()) {
        return false;
    }

    position_ = offset;
    return true;
}

std::size_t MldBinaryReader::remaining() const {
    return bytes_.size() - position_;
}

} // namespace soasim::mld::parsing
