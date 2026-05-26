#pragma once

#include <stdexcept>

namespace Sa3Dport::Structs {

enum class ColorIOType {
    ARGB8_32,
    ARGB8_16,
    ARGB4,
    RGB565,
    RGBA8,
};

[[nodiscard]] constexpr int byte_size(ColorIOType type) {
    switch (type) {
    case ColorIOType::ARGB8_32:
    case ColorIOType::ARGB8_16:
    case ColorIOType::RGBA8:
        return 4;
    case ColorIOType::ARGB4:
    case ColorIOType::RGB565:
        return 2;
    }
    return 0;
}

} // namespace Sa3Dport::Structs
