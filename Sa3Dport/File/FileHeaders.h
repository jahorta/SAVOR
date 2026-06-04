#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace Sa3Dport::File {

struct FileHeaders {
    static constexpr std::uint32_t NJCM = 0x4D434A4Eu;
    static constexpr std::uint32_t NJBM = 0x4D424A4Eu;
    static constexpr std::uint32_t NJTL = 0x4C544A4Eu;
    static constexpr std::uint32_t NMDM = 0x4D444D4Eu;
    static constexpr std::uint32_t NSSM = 0x4D53534Eu;
    static constexpr std::uint32_t NCAM = 0x4D41434Eu;

    static constexpr std::array<std::uint32_t, 1> TextureListBlockHeaders {
        NJTL,
    };

    static constexpr std::array<std::uint32_t, 2> ModelBlockHeaders {
        NJCM,
        NJBM,
    };

    static constexpr std::array<std::uint32_t, 3> AnimationBlockHeaders {
        NMDM,
        NSSM,
        NCAM,
    };

    [[nodiscard]] static constexpr std::array<char, 4> ToMagic(std::uint32_t header) {
        return {
            static_cast<char>(header & 0xFFu),
            static_cast<char>((header >> 8u) & 0xFFu),
            static_cast<char>((header >> 16u) & 0xFFu),
            static_cast<char>((header >> 24u) & 0xFFu),
        };
    }

    [[nodiscard]] static constexpr std::uint32_t FromMagic(const std::array<char, 4>& magic) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(magic[0])) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(magic[1])) << 8u) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(magic[2])) << 16u) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(magic[3])) << 24u);
    }

    static constexpr bool MatchesMagic(const std::array<char, 4>& candidate, std::uint32_t expected) {
        return FromMagic(candidate) == expected;
    }

    static constexpr bool IsModel(const std::array<char, 4>& candidate) {
        return IsModelBlockHeader(FromMagic(candidate));
    }

    static constexpr bool IsTexList(const std::array<char, 4>& candidate) {
        return IsTextureListBlockHeader(FromMagic(candidate));
    }

    static constexpr bool IsAnimation(const std::array<char, 4>& candidate) {
        return IsAnimationBlockHeader(FromMagic(candidate));
    }

    template <std::size_t N>
    static constexpr bool Contains(const std::array<std::uint32_t, N>& headers, std::uint32_t header) {
        for (const std::uint32_t candidate : headers) {
            if (candidate == header) {
                return true;
            }
        }

        return false;
    }

    static constexpr bool Contains(std::span<const std::uint32_t> headers, std::uint32_t header) {
        for (const std::uint32_t candidate : headers) {
            if (candidate == header) {
                return true;
            }
        }

        return false;
    }

    static constexpr bool IsModelBlockHeader(std::uint32_t header) {
        return Contains(ModelBlockHeaders, header);
    }

    static constexpr bool IsTextureListBlockHeader(std::uint32_t header) {
        return Contains(TextureListBlockHeaders, header);
    }

    static constexpr bool IsAnimationBlockHeader(std::uint32_t header) {
        return Contains(AnimationBlockHeaders, header);
    }
};

} // namespace Sa3Dport::File
