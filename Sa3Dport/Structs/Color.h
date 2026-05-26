#pragma once

#include "Structs/MathCompat.h"
#include "Structs/Vector4.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Sa3Dport::Structs {

struct Color {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::uint8_t alpha = 0;

    constexpr Color() = default;
    constexpr Color(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 0xFF)
        : red(r), green(g), blue(b), alpha(a) {}

    constexpr Color(int r, int g, int b, int a = 0xFF)
        : red(static_cast<std::uint8_t>(r)),
          green(static_cast<std::uint8_t>(g)),
          blue(static_cast<std::uint8_t>(b)),
          alpha(static_cast<std::uint8_t>(a)) {}

    Color(float r, float g, float b, float a = 1.0f)
        : red(float_to_byte(r)), green(float_to_byte(g)), blue(float_to_byte(b)), alpha(float_to_byte(a)) {}

    explicit Color(Vector4 rgba) : Color(rgba.x, rgba.y, rgba.z, rgba.w) {}

    [[nodiscard]] static constexpr Color white() { return {std::uint8_t {0xFF}, std::uint8_t {0xFF}, std::uint8_t {0xFF}, std::uint8_t {0xFF}}; }
    [[nodiscard]] static constexpr Color black() { return {std::uint8_t {0x00}, std::uint8_t {0x00}, std::uint8_t {0x00}, std::uint8_t {0xFF}}; }
    [[nodiscard]] static constexpr Color red_color() { return {std::uint8_t {0xFF}, std::uint8_t {0x00}, std::uint8_t {0x00}, std::uint8_t {0xFF}}; }
    [[nodiscard]] static constexpr Color green_color() { return {std::uint8_t {0x00}, std::uint8_t {0xFF}, std::uint8_t {0x00}, std::uint8_t {0xFF}}; }
    [[nodiscard]] static constexpr Color blue_color() { return {std::uint8_t {0x00}, std::uint8_t {0x00}, std::uint8_t {0xFF}, std::uint8_t {0xFF}}; }
    [[nodiscard]] static constexpr Color transparent() { return {std::uint8_t {0x00}, std::uint8_t {0x00}, std::uint8_t {0x00}, std::uint8_t {0x00}}; }

    [[nodiscard]] float red_f() const { return byte_to_float(red); }
    void set_red_f(float value) { red = float_to_byte(value); }

    [[nodiscard]] float green_f() const { return byte_to_float(green); }
    void set_green_f(float value) { green = float_to_byte(value); }

    [[nodiscard]] float blue_f() const { return byte_to_float(blue); }
    void set_blue_f(float value) { blue = float_to_byte(value); }

    [[nodiscard]] float alpha_f() const { return byte_to_float(alpha); }
    void set_alpha_f(float value) { alpha = float_to_byte(value); }

    [[nodiscard]] Vector4 float_vector() const { return {red_f(), green_f(), blue_f(), alpha_f()}; }

    void set_float_vector(Vector4 value) {
        set_red_f(value.x);
        set_blue_f(value.y);
        set_green_f(value.z);
        set_alpha_f(value.w);
    }

    [[nodiscard]] std::uint32_t rgba() const {
        return (static_cast<std::uint32_t>(red) << 24) |
               (static_cast<std::uint32_t>(green) << 16) |
               (static_cast<std::uint32_t>(blue) << 8) |
               static_cast<std::uint32_t>(alpha);
    }

    void set_rgba(std::uint32_t value) {
        red = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
        green = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
        blue = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
        alpha = static_cast<std::uint8_t>(value & 0xFFu);
    }

    [[nodiscard]] std::uint32_t argb() const {
        return (static_cast<std::uint32_t>(alpha) << 24) |
               (static_cast<std::uint32_t>(red) << 16) |
               (static_cast<std::uint32_t>(green) << 8) |
               static_cast<std::uint32_t>(blue);
    }

    void set_argb(std::uint32_t value) {
        alpha = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
        red = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
        green = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
        blue = static_cast<std::uint8_t>(value & 0xFFu);
    }

    [[nodiscard]] std::uint16_t argb4() const {
        return static_cast<std::uint16_t>((blue >> 4) |
                                          (green & 0xFu) |
                                          ((red << 4) & 0xFu) |
                                          ((alpha << 8) & 0xFu));
    }

    void set_argb4(std::uint16_t value) {
        alpha = expand4(static_cast<std::uint8_t>((value >> 12) & 0x0Fu));
        red = expand4(static_cast<std::uint8_t>((value >> 8) & 0x0Fu));
        green = expand4(static_cast<std::uint8_t>((value >> 4) & 0x0Fu));
        blue = expand4(static_cast<std::uint8_t>(value & 0x0Fu));
    }

    [[nodiscard]] std::uint16_t rgb565() const {
        return static_cast<std::uint16_t>((blue >> 3) |
                                          ((green << 3) & 0x3Fu) |
                                          ((red << 8) & 0x1Fu));
    }

    void set_rgb565(std::uint16_t value) {
        red = expand5(static_cast<std::uint8_t>((value >> 11) & 0x1Fu));
        green = expand6(static_cast<std::uint8_t>((value >> 5) & 0x3Fu));
        blue = expand5(static_cast<std::uint8_t>(value & 0x1Fu));
        alpha = 0xFF;
    }

    [[nodiscard]] std::string hex() const {
        static constexpr char digits[] = "0123456789ABCDEF";
        std::string result(9, '#');
        const std::uint32_t value = rgba();
        for (int i = 0; i < 8; ++i) {
            result[1 + i] = digits[(value >> ((7 - i) * 4)) & 0xFu];
        }
        return result;
    }

    void set_hex(std::string_view value) {
        std::string normalized;
        normalized.reserve(value.size());
        for (char ch : value) {
            if (ch == '#' || ch == ' ') {
                continue;
            }
            normalized.push_back(ch);
        }

        if (normalized.size() == 3 || normalized.size() == 4) {
            std::string expanded;
            expanded.reserve(normalized.size() * 2);
            for (char ch : normalized) {
                expanded.push_back(ch);
                expanded.push_back(ch);
            }
            normalized = expanded;
        }

        if (normalized.size() == 6) {
            normalized += "FF";
        }

        if (normalized.size() != 8) {
            throw std::invalid_argument("hex color must be RGB, RGBA, RRGGBB, or RRGGBBAA");
        }

        std::uint32_t parsed = 0;
        const auto [ptr, ec] = std::from_chars(normalized.data(), normalized.data() + normalized.size(), parsed, 16);
        if (ec != std::errc{} || ptr != normalized.data() + normalized.size()) {
            throw std::invalid_argument("invalid hex color");
        }
        set_rgba(parsed);
    }

    [[nodiscard]] float luminance() const {
        return 0.2126f * red_f() + 0.7152f * green_f() + 0.0722f * blue_f();
    }

    [[nodiscard]] static Color lerp(Color from, Color to, float t) {
        return {
            from.red_f() + (to.red_f() - from.red_f()) * t,
            from.green_f() + (to.green_f() - from.green_f()) * t,
            from.blue_f() + (to.blue_f() - from.blue_f()) * t,
            from.alpha_f() + (to.alpha_f() - from.alpha_f()) * t};
    }

    [[nodiscard]] static float distance(Color from, Color to) {
        return Sa3Dport::Structs::distance(from.float_vector(), to.float_vector());
    }

    [[nodiscard]] friend constexpr bool operator==(Color lhs, Color rhs) = default;

private:
    [[nodiscard]] static float byte_to_float(std::uint8_t value) {
        return static_cast<float>(value) / 255.0f;
    }

    [[nodiscard]] static std::uint8_t float_to_byte(float value) {
        return static_cast<std::uint8_t>(MathCompat::clamp01(value) * 255.0f);
    }

    [[nodiscard]] static constexpr std::uint8_t expand4(std::uint8_t value) {
        return static_cast<std::uint8_t>((value << 4) | value);
    }

    [[nodiscard]] static constexpr std::uint8_t expand5(std::uint8_t value) {
        return static_cast<std::uint8_t>((value << 3) | (value >> 2));
    }

    [[nodiscard]] static constexpr std::uint8_t expand6(std::uint8_t value) {
        return static_cast<std::uint8_t>((value << 2) | (value >> 4));
    }
};

inline const Color ColorWhite = Color::white();
inline const Color ColorBlack = Color::black();
inline const Color ColorRed = Color::red_color();
inline const Color ColorGreen = Color::green_color();
inline const Color ColorBlue = Color::blue_color();
inline const Color ColorTransparent = Color::transparent();

[[nodiscard]] inline Color operator+(Color lhs, Color rhs) {
    return {
        static_cast<std::uint8_t>(std::min(255, static_cast<int>(lhs.red) + rhs.red)),
        static_cast<std::uint8_t>(std::min(255, static_cast<int>(lhs.green) + rhs.green)),
        static_cast<std::uint8_t>(std::min(255, static_cast<int>(lhs.blue) + rhs.blue)),
        static_cast<std::uint8_t>(std::min(255, static_cast<int>(lhs.alpha) + rhs.alpha))};
}

[[nodiscard]] inline Color operator-(Color lhs, Color rhs) {
    return {
        static_cast<std::uint8_t>(std::max(0, static_cast<int>(lhs.red) - rhs.red)),
        static_cast<std::uint8_t>(std::max(0, static_cast<int>(lhs.green) - rhs.green)),
        static_cast<std::uint8_t>(std::max(0, static_cast<int>(lhs.blue) - rhs.blue)),
        static_cast<std::uint8_t>(std::max(0, static_cast<int>(lhs.alpha) - rhs.alpha))};
}

[[nodiscard]] inline Color operator*(Color lhs, float rhs) {
    return {lhs.red_f() * rhs, lhs.green_f() * rhs, lhs.blue_f() * rhs, lhs.alpha_f() * rhs};
}

[[nodiscard]] inline Color operator*(float lhs, Color rhs) {
    return rhs * lhs;
}

[[nodiscard]] inline Color operator/(Color lhs, float rhs) {
    return {lhs.red_f() / rhs, lhs.green_f() / rhs, lhs.blue_f() / rhs, lhs.alpha_f() / rhs};
}

[[nodiscard]] inline std::string to_string(Color value) {
    return value.hex();
}

} // namespace Sa3Dport::Structs
