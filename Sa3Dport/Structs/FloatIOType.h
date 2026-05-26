#pragma once

#include "Structs/BAMSFHelper.h"
#include "Structs/EndianStackReader.h"
#include "Structs/EndianStackWriter.h"
#include "Structs/MathCompat.h"
#include "Structs/MathHelper.h"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace Sa3Dport::Structs {

enum class FloatIOType {
    Float,
    Short,
    Integer,
    BAMS16,
    BAMS32,
    BAMS16F,
    BAMS32F,
};

[[nodiscard]] constexpr int byte_size(FloatIOType type) {
    switch (type) {
    case FloatIOType::Float:
    case FloatIOType::Integer:
    case FloatIOType::BAMS32:
    case FloatIOType::BAMS32F:
        return 4;
    case FloatIOType::Short:
    case FloatIOType::BAMS16:
    case FloatIOType::BAMS16F:
        return 2;
    }
    return 0;
}

[[nodiscard]] inline std::string print_float_as(float value, FloatIOType type) {
    switch (type) {
    case FloatIOType::Float:
        return MathCompat::fixed5_float(value);
    case FloatIOType::Short:
    case FloatIOType::Integer:
        return std::to_string(MathCompat::round_to_even_i32(value));
    case FloatIOType::BAMS16:
        return MathHelper::to_c_hex(static_cast<std::uint16_t>(MathHelper::rad_to_bams(value)));
    case FloatIOType::BAMS32:
        return MathHelper::to_c_hex(static_cast<std::uint32_t>(MathHelper::rad_to_bams(value)));
    case FloatIOType::BAMS16F:
        return MathHelper::to_c_hex(static_cast<std::uint16_t>(BAMSFHelper::RadToBAMSF(value)));
    case FloatIOType::BAMS32F:
        return MathHelper::to_c_hex(static_cast<std::uint32_t>(BAMSFHelper::RadToBAMSF(value)));
    }
    throw std::invalid_argument("unknown FloatIOType");
}

inline void write_float_as(EndianStackWriter& writer, float value, FloatIOType type) {
    switch (type) {
    case FloatIOType::Float:
        writer.write_float(value);
        break;
    case FloatIOType::Short:
        writer.write_i16(MathCompat::round_to_even_i16(value));
        break;
    case FloatIOType::Integer:
        writer.write_i32(MathCompat::round_to_even_i32(value));
        break;
    case FloatIOType::BAMS16:
        writer.write_i16(static_cast<std::int16_t>(MathHelper::rad_to_bams(value)));
        break;
    case FloatIOType::BAMS32:
        writer.write_i32(MathHelper::rad_to_bams(value));
        break;
    case FloatIOType::BAMS16F:
        writer.write_i16(static_cast<std::int16_t>(BAMSFHelper::RadToBAMSF(value)));
        break;
    case FloatIOType::BAMS32F:
        writer.write_i32(BAMSFHelper::RadToBAMSF(value));
        break;
    }
}

[[nodiscard]] inline float read_float_as(const EndianStackReader& reader, std::uint32_t address, FloatIOType type) {
    switch (type) {
    case FloatIOType::Float:
        return reader.read_float(address);
    case FloatIOType::Short:
        return static_cast<float>(reader.read_i16(address));
    case FloatIOType::Integer:
        return static_cast<float>(reader.read_i32(address));
    case FloatIOType::BAMS16:
        return MathHelper::bams_to_rad(reader.read_i16(address));
    case FloatIOType::BAMS32:
        return MathHelper::bams_to_rad(reader.read_i32(address));
    case FloatIOType::BAMS16F:
        return BAMSFHelper::BAMSFToRad(reader.read_i16(address));
    case FloatIOType::BAMS32F:
        return BAMSFHelper::BAMSFToRad(reader.read_i32(address));
    }
    throw std::invalid_argument("unknown FloatIOType");
}

} // namespace Sa3Dport::Structs
