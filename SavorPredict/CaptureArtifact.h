#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace savor::predict {

struct CaptureSeedOverrideSummary {
    std::optional<std::uint32_t> original_seed;
    std::optional<std::uint32_t> requested_seed;
    std::optional<std::uint32_t> applied_seed;
    std::optional<bool> readback_matches;
};

struct PreparedCaptureArtifact {
    bool verified = false;
    bool complete = false;
    std::string incomplete_reason;
    std::vector<std::filesystem::path> segments;
    std::uint64_t segment_count = 0;
    std::uint64_t chunk_count = 0;
    std::uint64_t event_count = 0;
    std::uint64_t gap_count = 0;
    std::uint64_t revision_count = 0;
    std::uint64_t metrics_event_count = 0;
    std::uint64_t hits = 0;
    std::uint64_t filter_rejections = 0;
    std::uint64_t capture_deliveries = 0;
    std::uint64_t progress_deliveries = 0;
    std::uint64_t control_publications = 0;
    std::uint64_t probe_drops = 0;
    std::uint64_t capture_drops = 0;
    std::uint64_t progress_drops = 0;
    std::uint64_t drops = 0;
    std::uint64_t progress_coalesced = 0;
    std::uint64_t bytes = 0;
    std::uint64_t traces = 0;
    std::uint64_t trace_failures = 0;
    std::uint64_t flight_triggers = 0;
    std::uint64_t flight_completed_windows = 0;
    bool flight_window_active = false;
    CaptureSeedOverrideSummary seed_override;
};

bool copy_capture_segments(
    const std::filesystem::path& source_base,
    const std::filesystem::path& stable_base,
    std::vector<std::filesystem::path>* copied_paths_out,
    std::string* error_out = nullptr);

bool prepare_capture_artifact(
    const std::filesystem::path& capture_path,
    const std::filesystem::path& export_path,
    PreparedCaptureArtifact* result_out,
    std::string* error_out = nullptr);

} // namespace savor::predict
