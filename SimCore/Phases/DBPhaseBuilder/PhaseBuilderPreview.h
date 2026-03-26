#pragma once
#include <string>
#include <optional>
#include <cstdint>

namespace simcore::db::phasebuilder {

    struct TasMoviePreview {
        int64_t jobs{ 0 };
        int64_t rtc_low{ 0 };
        int64_t rtc_high{ 0 };
        bool artifact_exists{ false };
        std::optional<uint64_t> artifact_size;
    };

    struct SeedProbePreview {
        int64_t neutral_jobs{ 1 };
        int64_t grid_jobs{ 0 };
        std::optional<int64_t> unique_jobs;     // missing => deferred (no grid yet)
        bool unique_deferred{ false };
    };

    struct ExplorerRunPreview {
        int64_t jobs{ 1 };
        int32_t predicate_count{ 0 };
    };

    struct PhasePreview {
        int program_kind{};
        std::optional<TasMoviePreview> tasmovie;
        std::optional<SeedProbePreview> seedprobe;
        std::optional<ExplorerRunPreview> explorer;
    };

} // namespace simcore::db::phasebuilder
