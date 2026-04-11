#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace soasim::compression::aklz {

enum class AklzError {
    Ok = 0,
    InputTooSmall,
    InvalidMagic,
    TruncatedHeader,
    DecompressedSizeTooLarge,
    TruncatedFlagStream,
    TruncatedLiteral,
    TruncatedBackReference,
    OutputSizeMismatch,
};

constexpr std::size_t kHeaderSize = 0x10;
constexpr std::size_t kMagicSize = 0x0C;
constexpr std::uint32_t kDefaultMaxDecompressedSize = 64u * 1024u * 1024u;

[[nodiscard]] std::string_view errorToString(AklzError error);

struct AklzSizeResult {
    AklzError error{ AklzError::Ok };
    std::uint32_t decompressedSize{ 0 };

    [[nodiscard]] bool ok() const { return error == AklzError::Ok; }
};

struct AklzDecodeResult {
    AklzError error{ AklzError::Ok };
    std::vector<std::uint8_t> bytes{};

    [[nodiscard]] bool ok() const { return error == AklzError::Ok; }
};

[[nodiscard]] bool isAklz(std::span<const std::uint8_t> input);
[[nodiscard]] AklzSizeResult getDecompressedSize(std::span<const std::uint8_t> input,
    std::uint32_t maxAllowedSize = kDefaultMaxDecompressedSize);
[[nodiscard]] AklzDecodeResult decompress(std::span<const std::uint8_t> input,
    std::uint32_t maxAllowedSize = kDefaultMaxDecompressedSize);

} // namespace soasim::compression::aklz
