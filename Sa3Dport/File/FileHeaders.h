#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace Sa3Dport::File {

struct FileHeaders {
    static constexpr std::uint16_t NJ = 0x4A4Eu;
    static constexpr std::uint16_t CM = 0x4D43u;
    static constexpr std::uint16_t BM = 0x4D42u;
    static constexpr std::uint16_t TL = 0x4C54u;

    static constexpr std::uint32_t NMDM = 0x4D444D4Eu;
    static constexpr std::uint32_t NSSM = 0x4D53534Eu;
    static constexpr std::uint32_t NCAM = 0x4D41434Eu;

    static constexpr std::array<std::uint32_t, 1> TextureListBlockHeaders {
        (static_cast<std::uint32_t>(TL) << 16) | NJ,
    };

    static constexpr std::array<std::uint32_t, 2> ModelBlockHeaders {
        (static_cast<std::uint32_t>(CM) << 16) | NJ,
        (static_cast<std::uint32_t>(BM) << 16) | NJ,
    };

    static constexpr std::array<std::uint32_t, 3> AnimationBlockHeaders {
        NMDM,
        NSSM,
        NCAM,
    };

    // Model
    static constexpr std::array<char, 4> kNjcmMagic {'N', 'J', 'C', 'M'};
    static constexpr std::array<char, 4> kNjbmMagic{ 'N', 'J', 'B', 'M' };

    // Texture list
    static constexpr std::array<char, 4> kNjtlMagic {'N', 'J', 'T', 'L'};

    // Animations
    static constexpr std::array<char, 4> kNmdmMagic{ 'N', 'M', 'D', 'M' };
    static constexpr std::array<char, 4> kNssmMagic{ 'N', 'S', 'S', 'M' };
    static constexpr std::array<char, 4> kNcamMagic{ 'N', 'C', 'A', 'M' };


    static constexpr bool MatchesMagic(const std::array<char, 4>& candidate,
                                       const std::array<char, 4>& expected) {
        return candidate == expected;
    }

    static constexpr bool IsModel(const std::array<char, 4>& candidate) {
        return candidate == kNjcmMagic || candidate == kNjbmMagic;
    }

    static constexpr bool IsTexList(const std::array<char, 4>& candidate) {
        return candidate == kNjtlMagic;
    }

    static constexpr bool IsAnimation(const std::array<char, 4>& candidate) {
        return candidate == kNmdmMagic || candidate == kNssmMagic || candidate == kNcamMagic;
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
