#include "CaptureArtifact.h"

#include "../SavorCaptureFormat/CaptureFormat.h"

#include <sstream>
#include <set>
#include <iomanip>
#include <ranges>
#include <string_view>
#include <vector>

namespace savor::predict {
namespace {

std::optional<std::uint32_t> marker_value(
    const std::vector<capture_format::Event>& events,
    const char* marker)
{
    for (const auto& event : events) {
        if (event.kind == capture_format::EventKind::Marker && event.probe_id == marker)
            return static_cast<std::uint32_t>(event.value);
    }
    return std::nullopt;
}

std::string verification_error(const capture_format::VerificationReport& report)
{
    std::ostringstream out;
    out << "capture verification failed";
    for (const auto& error : report.errors)
        out << "; " << error;
    return out.str();
}

std::filesystem::path segment_path(const std::filesystem::path& base, std::uint32_t index)
{
    if (index == 0)
        return base;
    std::ostringstream suffix;
    suffix << '.' << std::setw(4) << std::setfill('0') << index;
    return base.parent_path() / (base.stem().string() + suffix.str() + base.extension().string());
}

std::optional<std::uint64_t> field_value(
    const capture_format::Event& event,
    std::string_view name)
{
    const auto field = std::ranges::find(event.fields, name, &capture_format::Field::name);
    if (field == event.fields.end() || field->status != capture_format::FieldStatus::Present)
        return std::nullopt;
    return field->value;
}

} // namespace

bool copy_capture_segments(
    const std::filesystem::path& source_base,
    const std::filesystem::path& stable_base,
    std::vector<std::filesystem::path>* copied_paths_out,
    std::string* error_out)
{
    if (copied_paths_out == nullptr) {
        if (error_out) *error_out = "capture segment output is null";
        return false;
    }
    copied_paths_out->clear();
    std::error_code ec;
    std::filesystem::create_directories(stable_base.parent_path(), ec);
    if (ec) {
        if (error_out) *error_out = "failed creating stable capture directory: " + ec.message();
        return false;
    }
    for (std::uint32_t index = 0;; ++index) {
        const auto source = segment_path(source_base, index);
        if (!std::filesystem::exists(source, ec)) {
            if (index == 0) {
                if (error_out) *error_out = "capture base segment does not exist: " + source.string();
                return false;
            }
            break;
        }
        const auto destination = segment_path(stable_base, index);
        ec.clear();
        std::filesystem::copy_file(
            source, destination, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            if (error_out) *error_out = "failed copying capture segment: " + ec.message();
            return false;
        }
        copied_paths_out->push_back(destination);
    }
    return true;
}

bool prepare_capture_artifact(
    const std::filesystem::path& capture_path,
    const std::filesystem::path& export_path,
    PreparedCaptureArtifact* result_out,
    std::string* error_out)
{
    if (result_out == nullptr) {
        if (error_out) *error_out = "capture result output is null";
        return false;
    }
    *result_out = {};

    auto report = capture_format::Reader::verify(capture_path);
    if (!report.errors.empty()) {
        if (error_out) *error_out = verification_error(report);
        return false;
    }
    result_out->verified = report.ok;
    result_out->complete = report.ok;
    result_out->segment_count = report.segments.size();
    for (const auto& segment : report.segments) {
        result_out->segments.push_back(segment.path);
        result_out->event_count += segment.event_count;
        result_out->chunk_count += segment.chunks.size();
        if (!segment.complete && result_out->incomplete_reason.empty())
            result_out->incomplete_reason = segment.incomplete_reason.empty()
                ? "capture segment is incomplete"
                : segment.incomplete_reason;
    }

    std::error_code ec;
    std::filesystem::create_directories(export_path.parent_path(), ec);
    if (ec) {
        if (error_out) *error_out = "failed creating capture export directory: " + ec.message();
        return false;
    }

    std::string export_error;
    if (!capture_format::Reader::export_jsonl(
            capture_path, export_path, std::nullopt, 0, UINT64_MAX, false, &export_error)) {
        if (error_out) *error_out = "failed exporting capture JSONL: " + export_error;
        return false;
    }

    std::vector<capture_format::Event> events;
    std::string read_error;
    if (!capture_format::Reader::read_all(capture_path, events, nullptr, &read_error)) {
        if (error_out) *error_out = "failed reading verified capture: " + read_error;
        return false;
    }

    result_out->seed_override.original_seed = marker_value(events, "rng.seed_override.original");
    result_out->seed_override.requested_seed = marker_value(events, "rng.seed_override.requested");
    result_out->seed_override.applied_seed = marker_value(events, "rng.seed_override.applied");
    if (result_out->seed_override.requested_seed.has_value()
        && result_out->seed_override.applied_seed.has_value()) {
        result_out->seed_override.readback_matches =
            *result_out->seed_override.requested_seed == *result_out->seed_override.applied_seed;
    }

    std::set<std::uint64_t> revisions;
    for (const auto& event : events) {
        revisions.insert(event.profile_revision);
        if (event.kind == capture_format::EventKind::Gap)
            ++result_out->gap_count;
        if (event.kind != capture_format::EventKind::Metrics)
            continue;
        ++result_out->metrics_event_count;
        if (event.probe_id.starts_with("probe.metrics.")) {
            result_out->hits += field_value(event, "hits").value_or(0);
            result_out->filter_rejections += field_value(event, "predicate_rejections").value_or(0)
                + field_value(event, "window_rejections").value_or(0)
                + field_value(event, "sampling_rejections").value_or(0);
            result_out->capture_deliveries += field_value(event, "capture_deliveries").value_or(0);
            result_out->progress_deliveries += field_value(event, "progress_deliveries").value_or(0);
            result_out->control_publications += field_value(event, "control_publications").value_or(0);
            result_out->probe_drops += field_value(event, "drops").value_or(0);
            result_out->progress_coalesced += field_value(event, "progress_coalesced").value_or(0);
            result_out->bytes += field_value(event, "bytes").value_or(0);
            result_out->traces += field_value(event, "traces").value_or(0);
            result_out->trace_failures += field_value(event, "trace_failures").value_or(0);
        } else if (event.probe_id.starts_with("flight.metrics.")) {
            result_out->flight_triggers += field_value(event, "triggers").value_or(0);
            result_out->flight_completed_windows += field_value(event, "completed_windows").value_or(0);
            result_out->flight_window_active = result_out->flight_window_active
                || field_value(event, "active").value_or(0) != 0;
        } else if (event.probe_id == "probe.session.metrics") {
            result_out->capture_drops = field_value(event, "capture_drops").value_or(0);
            result_out->progress_drops = field_value(event, "progress_drops").value_or(0);
            if (field_value(event, "complete").value_or(1) == 0) {
                result_out->complete = false;
                if (result_out->incomplete_reason.empty())
                    result_out->incomplete_reason = "capture session metrics report incomplete";
            }
        }
    }
    result_out->drops = result_out->capture_drops + result_out->progress_drops;
    if (result_out->drops == 0)
        result_out->drops = result_out->probe_drops;
    result_out->revision_count = revisions.size();
    return true;
}

} // namespace savor::predict
