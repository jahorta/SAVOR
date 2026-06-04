#pragma once

#include "Structs/BaseLUT.h"
#include "Structs/Color.h"
#include "Structs/ColorIOType.h"
#include "Structs/Endian.h"
#include "Structs/EndianStackReader.h"
#include "Structs/EndianStackWriter.h"
#include "Structs/FloatIOType.h"
#include "Structs/Quaternion.h"
#include "Structs/Vector2.h"
#include "Structs/Vector3.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace Sa3Dport::Structs {

using Endianness = Endian;

[[nodiscard]] inline constexpr Endian to_endian(Endianness endianness) {
    return endianness;
}

class EndianReader {
public:
    EndianReader(std::span<const std::byte> buffer, Endianness endianness, std::uint32_t imageBase = 0)
        : reader_(buffer, to_endian(endianness)), imageBase_(imageBase) {}

    [[nodiscard]] std::size_t Position() const {
        return position_;
    }

    [[nodiscard]] std::uint32_t ImageBase() const {
        return imageBase_;
    }

    void Seek(std::size_t position) {
        if (position > reader_.size()) {
            throw std::out_of_range("seek beyond end of buffer");
        }
        position_ = position;
    }

    [[nodiscard]] std::uint8_t ReadU8() { return ReadAndAdvance<std::uint8_t>(&EndianStackReader::read_u8); }
    [[nodiscard]] std::int8_t ReadI8() { return ReadAndAdvance<std::int8_t>(&EndianStackReader::read_i8); }
    [[nodiscard]] std::uint16_t ReadU16() { return ReadAndAdvance<std::uint16_t>(&EndianStackReader::read_u16); }
    [[nodiscard]] std::int16_t ReadI16() { return ReadAndAdvance<std::int16_t>(&EndianStackReader::read_i16); }
    [[nodiscard]] std::uint32_t ReadU32() { return ReadAndAdvance<std::uint32_t>(&EndianStackReader::read_u32); }
    [[nodiscard]] std::int32_t ReadI32() { return ReadAndAdvance<std::int32_t>(&EndianStackReader::read_i32); }

    [[nodiscard]] float ReadF32() {
        return ReadAndAdvance<float>(&EndianStackReader::read_float);
    }

    [[nodiscard]] std::uint32_t ReadPointerOffset() {
        const auto absolute = ReadU32();
        return absolute >= imageBase_ ? absolute - imageBase_ : absolute;
    }

private:
    template <typename T>
    [[nodiscard]] T ReadAndAdvance(T (EndianStackReader::*read)(std::uint32_t) const) {
        const auto address = static_cast<std::uint32_t>(position_);
        T value = (reader_.*read)(address);
        position_ += sizeof(T);
        return value;
    }

    EndianStackReader reader_;
    std::uint32_t imageBase_ = 0;
    std::size_t position_ = 0;
};

namespace EndianIOExtensions {

inline void write_color(EndianStackWriter& writer, Color color, ColorIOType type) {
    switch (type) {
    case ColorIOType::RGBA8:
        writer.write_u32(color.rgba());
        break;
    case ColorIOType::ARGB8_32:
        writer.write_u32(color.argb());
        break;
    case ColorIOType::ARGB8_16: {
        const auto value = color.argb();
        writer.write_u16(static_cast<std::uint16_t>(value & 0xFFFFu));
        writer.write_u16(static_cast<std::uint16_t>((value >> 16) & 0xFFFFu));
        break;
    }
    case ColorIOType::ARGB4:
        writer.write_u16(color.argb4());
        break;
    case ColorIOType::RGB565:
        writer.write_u16(color.rgb565());
        break;
    }
}

[[nodiscard]] inline Color read_color(const EndianStackReader& reader,
                                      std::size_t address,
                                      ColorIOType type) {
    Color color;
    switch (type) {
    case ColorIOType::RGBA8:
        color.set_rgba(reader.read_u32(static_cast<std::uint32_t>(address)));
        break;
    case ColorIOType::ARGB8_32:
        color.set_argb(reader.read_u32(static_cast<std::uint32_t>(address)));
        break;
    case ColorIOType::ARGB8_16: {
        const auto low = reader.read_u16(static_cast<std::uint32_t>(address));
        const auto high = reader.read_u16(static_cast<std::uint32_t>(address + 2));
        color.set_argb((static_cast<std::uint32_t>(high) << 16) | low);
        break;
    }
    case ColorIOType::ARGB4:
        color.set_argb4(reader.read_u16(static_cast<std::uint32_t>(address)));
        break;
    case ColorIOType::RGB565:
        color.set_rgb565(reader.read_u16(static_cast<std::uint32_t>(address)));
        break;
    }
    return color;
}

[[nodiscard]] inline Color read_color(const EndianStackReader& reader,
                                      std::uint32_t& address,
                                      ColorIOType type) {
    const Color color = read_color(reader, static_cast<std::size_t>(address), type);
    address += static_cast<std::uint32_t>(byte_size(type));
    return color;
}

inline void write_vector2(EndianStackWriter& writer, Vector2 value, FloatIOType type = FloatIOType::Float) {
    write_float_as(writer, value.x, type);
    write_float_as(writer, value.y, type);
}

inline void write_vector3(EndianStackWriter& writer, Vector3 value, FloatIOType type = FloatIOType::Float) {
    write_float_as(writer, value.x, type);
    write_float_as(writer, value.y, type);
    write_float_as(writer, value.z, type);
}

[[nodiscard]] inline Vector2 read_vector2(const EndianStackReader& reader,
                                          std::size_t address,
                                          FloatIOType type = FloatIOType::Float) {
    const auto step = static_cast<std::uint32_t>(byte_size(type));
    return {
        read_float_as(reader, static_cast<std::uint32_t>(address), type),
        read_float_as(reader, static_cast<std::uint32_t>(address + step), type)};
}

[[nodiscard]] inline Vector2 read_vector2(const EndianStackReader& reader,
                                          std::uint32_t& address,
                                          FloatIOType type = FloatIOType::Float) {
    const Vector2 value = read_vector2(reader, static_cast<std::size_t>(address), type);
    address += static_cast<std::uint32_t>(byte_size(type) * 2);
    return value;
}

[[nodiscard]] inline Vector3 read_vector3(const EndianStackReader& reader,
                                          std::size_t address,
                                          FloatIOType type = FloatIOType::Float) {
    const auto step = static_cast<std::uint32_t>(byte_size(type));
    return {
        read_float_as(reader, static_cast<std::uint32_t>(address), type),
        read_float_as(reader, static_cast<std::uint32_t>(address + step), type),
        read_float_as(reader, static_cast<std::uint32_t>(address + step * 2), type)};
}

[[nodiscard]] inline Vector3 read_vector3(const EndianStackReader& reader,
                                          std::uint32_t& address,
                                          FloatIOType type = FloatIOType::Float) {
    const Vector3 value = read_vector3(reader, static_cast<std::size_t>(address), type);
    address += static_cast<std::uint32_t>(byte_size(type) * 3);
    return value;
}

inline void write_quaternion(EndianStackWriter& writer, Quaternion value) {
    writer.write_float(value.w);
    writer.write_float(value.x);
    writer.write_float(value.y);
    writer.write_float(value.z);
}

[[nodiscard]] inline Quaternion read_quaternion(const EndianStackReader& reader, std::size_t address) {
    return {
        reader.read_float(static_cast<std::uint32_t>(address + 4)),
        reader.read_float(static_cast<std::uint32_t>(address + 8)),
        reader.read_float(static_cast<std::uint32_t>(address + 12)),
        reader.read_float(static_cast<std::uint32_t>(address))};
}

[[nodiscard]] inline Quaternion read_quaternion(const EndianStackReader& reader, std::uint32_t& address) {
    const Quaternion value = read_quaternion(reader, static_cast<std::size_t>(address));
    address += 16;
    return value;
}

template <class Range, class WriteFn>
std::uint32_t write_collection(EndianStackWriter& writer, const Range& values, WriteFn write) {
    const std::uint32_t address = writer.pointer_position();
    for (const auto& value : values) {
        write(writer, value);
    }
    return address;
}

template <class Range, class WriteFn, class PreWriteFn>
std::uint32_t write_collection(EndianStackWriter& writer,
                               const Range& values,
                               WriteFn write,
                               PreWriteFn pre_write) {
    for (const auto& value : values) {
        pre_write(writer, value);
    }
    return write_collection(writer, values, write);
}

template <class Range, class WriteFn, class PreWriteFn>
std::uint32_t write_collection_with_lut(EndianStackWriter& writer,
                                        const Range* values,
                                        WriteFn write,
                                        PreWriteFn pre_write,
                                        BaseLUT& lut) {
    return lut.get_add_address(values, [&]() {
        if (values == nullptr) {
            return 0u;
        }
        return write_collection(writer, *values, write, pre_write);
    });
}

template <class Range, class WriteFn>
std::uint32_t write_collection_with_lut(EndianStackWriter& writer,
                                        const Range* values,
                                        WriteFn write,
                                        BaseLUT& lut) {
    return lut.get_add_address(values, [&]() {
        if (values == nullptr) {
            return 0u;
        }
        return write_collection(writer, *values, write);
    });
}

template <class Range, class WriteFn, class Hash, class Equal>
std::uint32_t write_collection_with_value_lut(EndianStackWriter& writer,
                                              const Range& values,
                                              WriteFn write,
                                              Hash hash,
                                              Equal equal,
                                              BaseLUT& lut) {
    return lut.get_add_address_by_value(values, hash, equal, [&]() {
        return write_collection(writer, values, write);
    });
}

template <class T, class ReadAdvanceFn>
std::vector<T> read_array(const EndianStackReader& reader,
                          std::uint32_t address,
                          std::uint32_t count,
                          ReadAdvanceFn read) {
    std::vector<T> result;
    result.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        result.push_back(read(reader, address));
    }
    return result;
}

template <class T, class ReadFn>
std::vector<T> read_array(const EndianStackReader& reader,
                          std::uint32_t address,
                          std::uint32_t count,
                          std::uint32_t element_byte_size,
                          ReadFn read) {
    std::vector<T> result;
    result.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        result.push_back(read(reader, address));
        address += element_byte_size;
    }
    return result;
}

template <class T, class ReadAdvanceFn>
std::vector<T> read_array_with_lut(const EndianStackReader& reader,
                                   std::uint32_t address,
                                   std::uint32_t count,
                                   ReadAdvanceFn read,
                                   BaseLUT& lut) {
    return lut.get_add_value<T>(address, [&]() {
        return read_array<T>(reader, address, count, read);
    });
}

template <class T, class ReadFn>
std::vector<T> read_array_with_lut(const EndianStackReader& reader,
                                   std::uint32_t address,
                                   std::uint32_t count,
                                   std::uint32_t element_byte_size,
                                   ReadFn read,
                                   BaseLUT& lut) {
    return lut.get_add_value<T>(address, [&]() {
        return read_array<T>(reader, address, count, element_byte_size, read);
    });
}

template <class T, class ReadAdvanceFn>
LabeledArray<T> read_labeled_array(const EndianStackReader& reader,
                                   std::uint32_t address,
                                   std::uint32_t count,
                                   ReadAdvanceFn read,
                                   std::string_view generated_prefix,
                                   BaseLUT& lut) {
    return lut.get_add_labeled_value<T>(address, generated_prefix, [&]() {
        return read_array<T>(reader, address, count, read);
    });
}

template <class T, class ReadFn>
LabeledArray<T> read_labeled_array(const EndianStackReader& reader,
                                   std::uint32_t address,
                                   std::uint32_t count,
                                   std::uint32_t element_byte_size,
                                   ReadFn read,
                                   std::string_view generated_prefix,
                                   BaseLUT& lut) {
    return lut.get_add_labeled_value<T>(address, generated_prefix, [&]() {
        return read_array<T>(reader, address, count, element_byte_size, read);
    });
}

template <class T, class ReadAdvanceFn>
LabeledReadOnlyArray<T> read_labeled_read_only_array(const EndianStackReader& reader,
                                                     std::uint32_t address,
                                                     std::uint32_t count,
                                                     ReadAdvanceFn read,
                                                     std::string_view generated_prefix,
                                                     BaseLUT& lut) {
    return lut.get_add_labeled_read_only_value<T>(address, generated_prefix, [&]() {
        return read_array<T>(reader, address, count, read);
    });
}

template <class T, class ReadFn>
LabeledReadOnlyArray<T> read_labeled_read_only_array(const EndianStackReader& reader,
                                                     std::uint32_t address,
                                                     std::uint32_t count,
                                                     std::uint32_t element_byte_size,
                                                     ReadFn read,
                                                     std::string_view generated_prefix,
                                                     BaseLUT& lut) {
    return lut.get_add_labeled_read_only_value<T>(address, generated_prefix, [&]() {
        return read_array<T>(reader, address, count, element_byte_size, read);
    });
}

} // namespace EndianIOExtensions

} // namespace Sa3Dport::Structs
