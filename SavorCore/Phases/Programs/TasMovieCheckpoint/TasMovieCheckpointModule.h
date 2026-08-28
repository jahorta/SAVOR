#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace savor::runtime::tasmovie {

inline constexpr std::size_t MaximumItineraryEntries = 4096;
inline constexpr std::uint32_t BeforeRandSeedSetPc = 0x80101E48u;
inline constexpr std::uint32_t FieldFastPreseedPc = 0x80101894u;
inline constexpr std::uint32_t FieldDeferredPreseedPc = 0x801018ACu;

struct DtmInputCount
{
    std::uint64_t value = 0;

    [[nodiscard]] std::optional<std::uint64_t>
    PayloadByteOffset() const noexcept;

    auto operator<=>(const DtmInputCount&) const = default;
};

enum class TasMovieNextPhaseV1 : std::uint8_t
{
    Battle = 0,
    Cutscene = 1,
    FieldNavigation = 2,
    OverworldNavigation = 3,
    ShipBattle = 4,
    Unknown = 5,
};

struct TasMovieCheckpointV1
{
    // Keep the executable boundary first: validation consumes these two
    // fields directly, while the remaining fields describe the handoff.
    std::uint32_t pc = 0;
    DtmInputCount input_count;
    std::string breakpoint_id;
    std::uint64_t vi_count = 0;
    std::optional<std::string> sct_filename;
    std::optional<std::uint32_t> area;
    std::optional<std::uint8_t> subfield;
    TasMovieNextPhaseV1 expected_next_phase = TasMovieNextPhaseV1::Unknown;

    auto operator<=>(const TasMovieCheckpointV1&) const = default;
};

struct TasMovieItineraryV1
{
    std::vector<TasMovieCheckpointV1> checkpoints;

    auto operator<=>(const TasMovieItineraryV1&) const = default;
};

[[nodiscard]] std::string_view TasMovieBreakpointIdForPcV1(
    std::uint32_t pc) noexcept;
[[nodiscard]] TasMovieNextPhaseV1 ClassifyTasMovieNextPhaseV1(
    std::uint32_t pc,
    std::optional<std::string_view> sct_filename = std::nullopt,
    std::optional<std::uint32_t> area = std::nullopt) noexcept;
[[nodiscard]] std::string_view TasMovieNextPhaseNameV1(
    TasMovieNextPhaseV1 phase) noexcept;
[[nodiscard]] TasMovieCheckpointV1 MakeTasMovieCheckpointV1(
    std::uint32_t pc,
    std::uint64_t movie_input_cursor,
    std::uint64_t vi_count,
    std::optional<std::string> sct_filename = std::nullopt,
    std::optional<std::uint32_t> area = std::nullopt,
    std::optional<std::uint8_t> subfield = std::nullopt);

[[nodiscard]] std::vector<std::uint8_t> EncodeTasMovieItineraryArtifactV1(
    const TasMovieItineraryV1& itinerary,
    std::string* diagnostic = nullptr);
[[nodiscard]] bool DecodeTasMovieItineraryArtifactV1(
    std::span<const std::uint8_t> payload,
    TasMovieItineraryV1& itinerary,
    std::string* diagnostic = nullptr);
[[nodiscard]] bool ValidateTasMovieItineraryArtifactV1(
    const TasMovieItineraryV1& itinerary,
    std::uint64_t total_dtm_input_count,
    std::uint32_t required_final_pc,
    std::string* diagnostic = nullptr);

} // namespace savor::runtime::tasmovie
