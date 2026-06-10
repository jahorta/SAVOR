#pragma once
#include <bit>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <tuple>
#include "SoaStructs.reflect.h"  // generated file

namespace savor::endian {

    // byte swaps (constexpr-friendly)
    constexpr inline uint16_t bswap16(uint16_t v) { return uint16_t((v >> 8) | (v << 8)); }

    constexpr inline uint32_t bswap32(uint32_t v) {
        return (v >> 24) |
            ((v >> 8) & 0x0000FF00u) |
            ((v << 8) & 0x00FF0000u) |
            (v << 24);
    }

    constexpr inline uint64_t bswap64(uint64_t v) {
        return (v >> 56) |
            ((v >> 40) & 0x000000000000FF00ull) |
            ((v >> 24) & 0x0000000000FF0000ull) |
            ((v >> 8) & 0x00000000FF000000ull) |
            ((v << 8) & 0x000000FF00000000ull) |
            ((v << 24) & 0x0000FF0000000000ull) |
            ((v << 40) & 0x00FF000000000000ull) |
            (v << 56);
    }

    // swap helpers for floats without UB
    inline float bswapf(float f) {
        uint32_t u; std::memcpy(&u, &f, 4); u = bswap32(u); std::memcpy(&f, &u, 4); return f;
    }
    inline double bswapd(double d) {
        uint64_t u; std::memcpy(&u, &d, 8); u = bswap64(u); std::memcpy(&d, &u, 8); return d;
    }

    template <class T> inline void fix_endianness_in_place(T& v);

    template<class T>
    inline void fix_arith(T& v) {
        if constexpr (std::is_same_v<T, uint16_t> || std::is_same_v<T, int16_t>) {
            auto u = static_cast<uint16_t>(std::bit_cast<uint16_t>(v));
            u = bswap16(u);
            v = std::bit_cast<T>(u);
        }
        else if constexpr (std::is_same_v<T, uint32_t> || std::is_same_v<T, int32_t>) {
            auto u = static_cast<uint32_t>(std::bit_cast<uint32_t>(v));
            u = bswap32(u);
            v = std::bit_cast<T>(u);
        }
        else if constexpr (std::is_same_v<T, uint64_t> || std::is_same_v<T, int64_t>) {
            auto u = static_cast<uint64_t>(std::bit_cast<uint64_t>(v));
            u = bswap64(u);
            v = std::bit_cast<T>(u);
        }
        else if constexpr (std::is_same_v<T, float>) {
            v = bswapf(v);
        }
        else if constexpr (std::is_same_v<T, double>) {
            v = bswapd(v);
        }
    }

    // Detection: does reflect<T> exist and expose ::members?
    template <class T, class = void>
    struct has_reflect_members : std::false_type {};
    template <class T>
    struct has_reflect_members<T, std::void_t<decltype(reflect<T>::members)>> : std::true_type {};

    template <class T>
    inline void fix_endianness_in_place(T& v) {
        if constexpr (std::is_enum_v<T>) {
            using U = std::underlying_type_t<T>;
            auto u = static_cast<U>(v);
            fix_endianness_in_place(u);
            v = static_cast<T>(u);
        }
        else if constexpr (std::is_arithmetic_v<T>) {
            if constexpr (sizeof(T) == 1) { /* no-op */ }
            else {
                if constexpr (std::is_same_v<T, float>) { v = bswapf(v); }
                else if constexpr (std::is_same_v<T, double>) { v = bswapd(v); }
                else if constexpr (sizeof(T) == 2) { auto u = std::bit_cast<uint16_t>(v); u = bswap16(u); v = std::bit_cast<T>(u); }
                else if constexpr (sizeof(T) == 4) { auto u = std::bit_cast<uint32_t>(v); u = bswap32(u); v = std::bit_cast<T>(u); }
                else if constexpr (sizeof(T) == 8) { auto u = std::bit_cast<uint64_t>(v); u = bswap64(u); v = std::bit_cast<T>(u); }
            }
        }
        else if constexpr (std::is_array_v<T>) {
            for (auto& e : v) fix_endianness_in_place(e);
        }
        else if constexpr (requires (T & x) { x.size(); x.data(); }) {
            for (auto& e : v) fix_endianness_in_place(e);
        }
        else if constexpr (has_reflect_members<T>::value) {
            // Visit every declared data member automatically
            std::apply([&](auto... mem) {
                (fix_endianness_in_place(v.*mem), ...);
                }, reflect<T>::members);
        }
        else {
            // Unknown class/struct with no reflection: treat as blob (no-op)
        }
    }

} // namespace savor::endian
