#pragma once

#include <array>
#include <cstdint>

namespace Sa3Dport::File {

struct FileHeaders {
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
};

} // namespace Sa3Dport::File
