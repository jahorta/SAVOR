#include "Aklz.h"

#include <array>

namespace soasim::compression::aklz {
namespace {

constexpr std::array<std::uint8_t, kMagicSize> kMagic = {
    'A', 'K', 'L', 'Z', '~', '?', 'Q', 'd', '=', 0xCC, 0xCC, 0xCD
};

[[nodiscard]] std::uint32_t readBeU32(std::span<const std::uint8_t> input, std::size_t offset) {
    return (std::uint32_t(input[offset + 0]) << 24) |
        (std::uint32_t(input[offset + 1]) << 16) |
        (std::uint32_t(input[offset + 2]) << 8) |
        std::uint32_t(input[offset + 3]);
}

class FlagReader {
public:
    explicit FlagReader(std::span<const std::uint8_t> input, std::size_t cursor)
        : m_input(input), m_cursor(cursor) {
    }

    [[nodiscard]] bool readBit(bool& outBit) {
        if (m_bitsRemaining == 0) {
            if (m_cursor >= m_input.size()) {
                return false;
            }
            m_current = m_input[m_cursor++];
            m_bitsRemaining = 8;
        }

        outBit = (m_current & 0x01u) != 0u;
        m_current >>= 1;
        --m_bitsRemaining;
        return true;
    }

    [[nodiscard]] bool readByte(std::uint8_t& outByte) {
        if (m_cursor >= m_input.size()) {
            return false;
        }
        outByte = m_input[m_cursor++];
        return true;
    }

private:
    std::span<const std::uint8_t> m_input{};
    std::size_t m_cursor{ 0 };
    std::uint8_t m_current{ 0 };
    std::uint8_t m_bitsRemaining{ 0 };
};

} // namespace

std::string_view errorToString(AklzError error) {
    switch (error) {
    case AklzError::Ok: return "ok";
    case AklzError::InputTooSmall: return "input too small for AKLZ header";
    case AklzError::InvalidMagic: return "invalid AKLZ magic";
    case AklzError::TruncatedHeader: return "truncated AKLZ header";
    case AklzError::DecompressedSizeTooLarge: return "AKLZ decompressed size exceeds configured cap";
    case AklzError::TruncatedFlagStream: return "AKLZ flag stream truncated";
    case AklzError::TruncatedLiteral: return "AKLZ literal read truncated";
    case AklzError::TruncatedBackReference: return "AKLZ back-reference read truncated";
    case AklzError::OutputSizeMismatch: return "AKLZ output size mismatch";
    default: return "unknown AKLZ error";
    }
}

bool isAklz(std::span<const std::uint8_t> input) {
    if (input.size() < kMagicSize) {
        return false;
    }

    for (std::size_t i = 0; i < kMagicSize; ++i) {
        if (input[i] != kMagic[i]) {
            return false;
        }
    }

    return true;
}

AklzSizeResult getDecompressedSize(std::span<const std::uint8_t> input, std::uint32_t maxAllowedSize) {
    if (input.size() < kMagicSize) {
        return { .error = AklzError::InputTooSmall };
    }

    if (!isAklz(input)) {
        return { .error = AklzError::InvalidMagic };
    }

    if (input.size() < kHeaderSize) {
        return { .error = AklzError::TruncatedHeader };
    }

    const auto decompressedSize = readBeU32(input, kMagicSize);
    if (decompressedSize > maxAllowedSize) {
        return { .error = AklzError::DecompressedSizeTooLarge, .decompressedSize = decompressedSize };
    }

    return { .error = AklzError::Ok, .decompressedSize = decompressedSize };
}

AklzDecodeResult decompress(std::span<const std::uint8_t> input, std::uint32_t maxAllowedSize) {
    const auto sz = getDecompressedSize(input, maxAllowedSize);
    if (!sz.ok()) {
        return { .error = sz.error };
    }

    constexpr std::uint32_t kWindowSize = 1u << 12u;
    constexpr std::uint32_t kWindowMask = kWindowSize - 1u;
    constexpr std::uint32_t kLengthMask = (1u << 4u) - 1u;
    constexpr std::uint32_t kMinLength = 3u;
    // AKLZ uses an LZSS-style 4 KiB ring buffer with a logical start position at 0xFEE.
    // Offsets encoded in back-references are relative to that start.
    constexpr std::uint32_t kWindowStart = 0xFEEu;

    std::vector<std::uint8_t> output;
    output.reserve(sz.decompressedSize);

    std::array<std::uint8_t, kWindowSize> window{0};
    std::uint32_t windowWritePos = 0;

    FlagReader reader(input, kHeaderSize);

    while (output.size() < sz.decompressedSize) {
        bool literal = false;
        if (!reader.readBit(literal)) {
            return { .error = AklzError::TruncatedFlagStream };
        }

        if (literal) {
            std::uint8_t value = 0;
            if (!reader.readByte(value)) {
                return { .error = AklzError::TruncatedLiteral };
            }
            output.push_back(value);
            window[windowWritePos] = value;
            windowWritePos = (windowWritePos + 1u) & kWindowMask;
            continue;
        }

        std::uint8_t b1 = 0;
        std::uint8_t b2 = 0;
        if (!reader.readByte(b1) || !reader.readByte(b2)) {
            return { .error = AklzError::TruncatedBackReference };
        }

        std::uint32_t offset = ((std::uint32_t(b2) >> 4u) << 8u) | std::uint32_t(b1);
        const std::uint32_t length = (std::uint32_t(b2) & kLengthMask) + kMinLength;
        offset = (kWindowSize + offset - kWindowStart) & kWindowMask;

        for (std::uint32_t i = 0; i < length && output.size() < sz.decompressedSize; ++i) {
            const auto sourceIndex = (offset + i) & kWindowMask;

            std::uint8_t value = window[sourceIndex];

            output.push_back(value);
            window[windowWritePos] = value;
            windowWritePos = (windowWritePos + 1u) & kWindowMask;
        }
    }

    if (output.size() != sz.decompressedSize) {
        return { .error = AklzError::OutputSizeMismatch };
    }

    return { .error = AklzError::Ok, .bytes = std::move(output) };
}

} // namespace soasim::compression::aklz
