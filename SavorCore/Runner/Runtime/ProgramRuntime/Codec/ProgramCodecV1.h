#pragma once

#include "Runner/Runtime/ProgramRuntime/Model/ProgramModel.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace savor::runtime::program {

inline constexpr std::uint16_t kProgramCodecVersionV1 = 1;
inline constexpr std::size_t kProgramCodecHeaderSizeV1 = 10;

struct CodecLimits
{
    std::uint64_t maximum_payload_bytes = 64ull * 1024ull * 1024ull;
    std::uint64_t maximum_string_bytes = 4ull * 1024ull * 1024ull;
    std::uint64_t maximum_collection_elements = 1ull * 1024ull * 1024ull;
};

enum class CodecError : std::uint8_t
{
    None,
    InvalidMagic,
    UnsupportedVersion,
    Truncated,
    Oversized,
    InvalidValue,
    NonCanonical,
    TrailingBytes,
    HashMismatch,
    HashFailure,
};

struct CodecStatus
{
    CodecError error = CodecError::None;
    std::string message;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error == CodecError::None;
    }

    auto operator<=>(const CodecStatus&) const = default;
};

struct EncodeResult
{
    CodecStatus status;
    std::vector<Byte> bytes;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return static_cast<bool>(status);
    }
};

template <typename T>
struct DecodeResult
{
    CodecStatus status;
    std::optional<T> value;
    std::size_t consumed = 0;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return static_cast<bool>(status) && value.has_value();
    }
};

enum class CanonicalHashMode : std::uint8_t
{
    IncludeDeclaredHash,
    OmitDeclaredHash,
};

[[nodiscard]] EncodeResult EncodeProgramModuleV1(
    const ProgramModule& module,
    CanonicalHashMode hash_mode = CanonicalHashMode::IncludeDeclaredHash,
    const CodecLimits& limits = {});

[[nodiscard]] DecodeResult<ProgramModule> DecodeProgramModuleV1(
    std::span<const Byte> bytes,
    const CodecLimits& limits = {});

[[nodiscard]] ContentHash256 ComputeProgramModuleHashV1(
    const ProgramModule& module);

[[nodiscard]] CodecStatus ValidateProgramModuleIdentityV1(
    const ProgramModule& module);

[[nodiscard]] EncodeResult EncodeProgramInvocationV1(
    const ProgramInvocation& invocation,
    const CodecLimits& limits = {});

[[nodiscard]] DecodeResult<ProgramInvocation> DecodeProgramInvocationV1(
    std::span<const Byte> bytes,
    const CodecLimits& limits = {});

[[nodiscard]] EncodeResult EncodeProgramResultV1(
    const ProgramResult& result,
    const CodecLimits& limits = {});

[[nodiscard]] DecodeResult<ProgramResult> DecodeProgramResultV1(
    std::span<const Byte> bytes,
    const CodecLimits& limits = {});

} // namespace savor::runtime::program
