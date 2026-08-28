#include "TasMovieCheckpointModule.h"

#include <charconv>
#include <system_error>
#include <utility>

namespace savor::runtime::tasmovie {
namespace {

std::optional<std::uint32_t> AreaFromSct(std::string_view filename) noexcept
{
    if (filename.size() != 10 || filename.substr(0, 2) != "me" ||
        filename.substr(6) != ".sct")
        return std::nullopt;
    std::uint32_t area = 0;
    const auto result = std::from_chars(
        filename.data() + 2, filename.data() + 5, area);
    if (result.ec != std::errc{} || result.ptr != filename.data() + 5 ||
        area > 999)
        return std::nullopt;
    return area;
}

} // namespace

std::string_view TasMovieBreakpointIdForPcV1(std::uint32_t pc) noexcept
{
    switch (pc)
    {
    case BeforeRandSeedSetPc:
        return "soa.field.point.prebattle.BeforeRandSeedSet";
    case FieldFastPreseedPc:
        return "soa.field.point.transition.FastPreseed";
    case FieldDeferredPreseedPc:
        return "soa.field.point.transition.DeferredPreseed";
    default:
        return {};
    }
}

TasMovieNextPhaseV1 ClassifyTasMovieNextPhaseV1(
    std::uint32_t pc,
    std::optional<std::string_view> sct_filename,
    std::optional<std::uint32_t> area) noexcept
{
    if (pc == BeforeRandSeedSetPc)
        return TasMovieNextPhaseV1::Battle;
    if (!area && sct_filename)
        area = AreaFromSct(*sct_filename);
    if (!area || *area > 999)
        return TasMovieNextPhaseV1::Unknown;
    if (*area == 99)
        return TasMovieNextPhaseV1::OverworldNavigation;
    if (*area < 200)
        return TasMovieNextPhaseV1::FieldNavigation;
    if (*area < 500)
        return TasMovieNextPhaseV1::Cutscene;
    return TasMovieNextPhaseV1::ShipBattle;
}

std::string_view TasMovieNextPhaseNameV1(TasMovieNextPhaseV1 phase) noexcept
{
    switch (phase)
    {
    case TasMovieNextPhaseV1::Battle: return "BATTLE";
    case TasMovieNextPhaseV1::Cutscene: return "CUTSCENE";
    case TasMovieNextPhaseV1::FieldNavigation: return "FIELD_NAVIGATION";
    case TasMovieNextPhaseV1::OverworldNavigation: return "OVERWORLD_NAVIGATION";
    case TasMovieNextPhaseV1::ShipBattle: return "SHIP_BATTLE";
    case TasMovieNextPhaseV1::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

TasMovieCheckpointV1 MakeTasMovieCheckpointV1(
    std::uint32_t pc,
    std::uint64_t movie_input_cursor,
    std::uint64_t vi_count,
    std::optional<std::string> sct_filename,
    std::optional<std::uint32_t> area,
    std::optional<std::uint8_t> subfield)
{
    const auto sct_view = sct_filename
        ? std::optional<std::string_view>(*sct_filename) : std::nullopt;
    const auto next_phase = ClassifyTasMovieNextPhaseV1(pc, sct_view, area);
    return {
        .pc = pc,
        .input_count = DtmInputCount{movie_input_cursor},
        .breakpoint_id = std::string(TasMovieBreakpointIdForPcV1(pc)),
        .vi_count = vi_count,
        .sct_filename = std::move(sct_filename),
        .area = area,
        .subfield = subfield,
        .expected_next_phase = next_phase,
    };
}

} // namespace savor::runtime::tasmovie
