#pragma once

#include <array>
#include <cstdint>

namespace Sa3Dport::File {

struct FileHeaders {
    static constexpr std::array<char, 4> kNjcmMagic {'N', 'J', 'C', 'M'};
    static constexpr std::array<char, 4> kNjtlMagic {'N', 'J', 'T', 'L'};
    static constexpr std::array<char, 4> kNjmMagic {'N', 'J', 'M', '\0'};
    static constexpr std::array<char, 4> kNjaMagic {'N', 'J', 'A', '\0'};

    static constexpr bool MatchesMagic(const std::array<char, 4>& candidate,
                                       const std::array<char, 4>& expected) {
        return candidate == expected;
    }
};

} // namespace Sa3Dport::File
