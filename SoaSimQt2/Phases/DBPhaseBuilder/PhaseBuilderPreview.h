#pragma once

#include <cstdint>
#include <optional>

namespace simcore::db::phasebuilder {

struct TasMoviePreview {
    std::int64_t jobs = 0;
    std::int64_t rtc_low = 0;
    std::int64_t rtc_high = 0;
    bool artifact_exists = false;
    std::optional<std::uint64_t> artifact_size;
};

struct SeedProbePreview {
    std::int64_t neutral_jobs = 1;
    std::int64_t grid_jobs = 0;
    std::optional<std::int64_t> unique_jobs;
    bool unique_deferred = false;
};

struct ExplorerRunPreview {
    std::int64_t jobs = 1;
    std::int32_t predicate_count = 0;
};

struct PhasePreview {
    int program_kind = 0;
    std::optional<TasMoviePreview> tasmovie;
    std::optional<SeedProbePreview> seedprobe;
    std::optional<ExplorerRunPreview> explorer;
};

} // namespace simcore::db::phasebuilder

