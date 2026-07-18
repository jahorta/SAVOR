#include <gtest/gtest.h>

#include "CaptureArtifact.h"
#include "CaptureFormat.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace savor::capture_format;
using namespace savor::predict;

class TempCaptureDirectory {
public:
    TempCaptureDirectory()
    {
        static std::atomic<std::uint64_t> next_id{ 0 };
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path()
            / ("savor_capture_format_" + std::to_string(stamp) + "_"
                + std::to_string(next_id.fetch_add(1)));
        std::filesystem::create_directories(path);
    }

    ~TempCaptureDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;
};

FileMetadata metadata()
{
    return FileMetadata{
        .session_id = "capture-format-test",
        .source_identity = "unit-test",
        .executable_sha256 = std::string(64, 'a'),
        .dolphin_source_commit = "9843115ad8414970312c954d83145300d7cdbec3",
        .profile_json = R"({"schema":"savor.capture.profile/1","revision":1,"probes":[{"id":"probe.a","owns_rng_draw":true,"symbol":{"name":"probe_a","function":"UnitTestRng","checkpoint":"unit_draw"}}]})",
        .created_utc_ns = 123456789,
    };
}

Event event(
    std::uint64_t sequence,
    std::string probe_id,
    std::uint64_t scalar,
    std::uint64_t epoch = 0,
    std::uint64_t revision = 1)
{
    Event value;
    value.capture_sequence = sequence;
    value.record_sequence = sequence;
    value.monotonic_ns = sequence * 100;
    value.frame_index = sequence + 10;
    value.guest_state_epoch = epoch;
    value.profile_revision = revision;
    value.snapshot_id = sequence + 1000;
    value.kind = EventKind::Pc;
    value.pc = 0x80001000u + static_cast<std::uint32_t>(sequence * 4);
    value.probe_id = std::move(probe_id);
    value.fields.push_back(Field{
        .name = "seed",
        .type = FieldType::Unsigned,
        .status = FieldStatus::Present,
        .value = scalar,
    });
    value.fields.push_back(Field{
        .name = "stack",
        .type = FieldType::Bytes,
        .status = FieldStatus::Present,
        .bytes = { 0x80, 0x00, static_cast<std::uint8_t>(sequence), 0x44 },
    });
    return value;
}

std::size_t footer_offset(const std::vector<char>& bytes)
{
    const std::array<char, 4> footer{ 'F', 'O', 'O', 'T' };
    std::size_t found = std::string::npos;
    for (auto it = std::search(bytes.begin(), bytes.end(), footer.begin(), footer.end());
         it != bytes.end();
         it = std::search(std::next(it), bytes.end(), footer.begin(), footer.end())) {
        found = static_cast<std::size_t>(std::distance(bytes.begin(), it));
    }
    return found;
}

TEST(SavorCaptureFormat, RoundTripsDeltaChunksAndFilteredJsonExport)
{
    TempCaptureDirectory temp;
    const auto capture = temp.path / "capture.scap";

    WriterOptions options;
    options.events_per_chunk = 2;
    options.bytes_per_chunk = 1024 * 1024;
    options.chunk_interval_ns = UINT64_MAX;
    options.index_interval_chunks = 1;

    Writer writer;
    std::string error;
    ASSERT_TRUE(writer.open(capture, metadata(), options, &error)) << error;
    ASSERT_TRUE(writer.append(event(1, "probe.a", 0x11111111), &error)) << error;
    ASSERT_TRUE(writer.append(event(2, "probe.a", 0x22222222), &error)) << error;
    ASSERT_TRUE(writer.append(event(3, "probe.b", 0x33333333, 1, 2), &error)) << error;
    ASSERT_TRUE(writer.close(&error)) << error;

    const auto report = Reader::verify(capture);
    ASSERT_TRUE(report.ok);
    ASSERT_EQ(report.segments.size(), 1u);
    EXPECT_EQ(report.segments.front().chunks.size(), 2u);
    EXPECT_EQ(report.segments.front().event_count, 3u);
    EXPECT_TRUE(report.segments.front().has_footer);
    EXPECT_TRUE(report.segments.front().complete);
    EXPECT_EQ(report.segments.front().metadata.profile_json, metadata().profile_json);

    std::vector<Event> events;
    ASSERT_TRUE(Reader::read_all(capture, events, nullptr, &error)) << error;
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].probe_id, "probe.a");
    ASSERT_EQ(events[1].fields.size(), 2u);
    EXPECT_EQ(events[1].fields[0].value, 0x22222222u);
    EXPECT_EQ(events[1].fields[1].bytes,
        (std::vector<std::uint8_t>{ 0x80, 0x00, 0x02, 0x44 }));
    EXPECT_EQ(events[2].guest_state_epoch, 1u);
    EXPECT_EQ(events[2].profile_revision, 2u);

    const auto exported = temp.path / "probe-a.jsonl";
    ASSERT_TRUE(Reader::export_jsonl(
        capture, exported, std::string_view("probe.a"), 2, UINT64_MAX, false, &error)) << error;
    std::ifstream stream(exported, std::ios::binary);
    const std::string text{
        std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
    EXPECT_EQ(std::count(text.begin(), text.end(), '\n'), 1);
    EXPECT_NE(text.find(R"("capture_sequence":2)"), std::string::npos);
    EXPECT_NE(text.find(R"("checkpoint_id":"probe.a")"), std::string::npos);
    EXPECT_NE(text.find(R"("function":"UnitTestRng")"), std::string::npos);
    EXPECT_NE(text.find(R"("checkpoint":"unit_draw")"), std::string::npos);
    EXPECT_NE(text.find(R"("owns_rng_draw":true)"), std::string::npos);
    EXPECT_NE(text.find(R"("rng_draw_index_before":1)"), std::string::npos);
}

TEST(SavorCaptureFormat, RotatesIntoIndependentlyReadableSegments)
{
    TempCaptureDirectory temp;
    const auto capture = temp.path / "rotated.scap";
    WriterOptions options;
    options.events_per_chunk = 1;
    options.rotate_bytes = 128;
    options.index_interval_chunks = 1;

    Writer writer;
    std::string error;
    ASSERT_TRUE(writer.open(capture, metadata(), options, &error)) << error;
    for (std::uint64_t sequence = 1; sequence <= 3; ++sequence)
        ASSERT_TRUE(writer.append(event(sequence, "rotate", sequence), &error)) << error;
    ASSERT_TRUE(writer.close(&error)) << error;

    const auto report = Reader::verify(capture);
    ASSERT_TRUE(report.ok);
    ASSERT_GE(report.segments.size(), 2u);
    for (const auto& segment : report.segments) {
        EXPECT_TRUE(segment.has_footer);
        EXPECT_TRUE(segment.complete);
    }
    std::vector<Event> events;
    ASSERT_TRUE(Reader::read_all(capture, events, nullptr, &error)) << error;
    EXPECT_EQ(events.size(), 3u);

    const auto copied = temp.path / "stable" / "rotated.scap";
    std::vector<std::filesystem::path> copied_segments;
    ASSERT_TRUE(copy_capture_segments(capture, copied, &copied_segments, &error)) << error;
    EXPECT_EQ(copied_segments.size(), report.segments.size());
    PreparedCaptureArtifact prepared;
    ASSERT_TRUE(prepare_capture_artifact(
        copied, temp.path / "stable" / "rotated.jsonl", &prepared, &error)) << error;
    EXPECT_TRUE(prepared.complete);
    EXPECT_EQ(prepared.segment_count, report.segments.size());
    EXPECT_EQ(prepared.segments, copied_segments);
}

TEST(SavorCaptureFormat, PreservesCaptureOrderForDelayedStructuredRecords)
{
    TempCaptureDirectory temp;
    const auto capture = temp.path / "structured.scap";
    WriterOptions options;
    options.events_per_chunk = 1;
    Writer writer;
    std::string error;
    ASSERT_TRUE(writer.open(capture, metadata(), options, &error)) << error;

    Event later = event(10, "structured", 1);
    later.record_sequence = 1;
    later.fields.clear();
    Field list_field;
    list_field.name = "threads";
    list_field.type = FieldType::LinkedList;
    list_field.linked_list = LinkedListValue{
        .root_address = 0x80311A84u,
        .head = 0x81230000u,
        .read_ok = true,
        .truncated = false,
        .cycle_detected = false,
        .nodes = { LinkedListNode{
            .address = 0x81230000u,
            .fields = { LinkedListMember{ "callback", 4, FieldStatus::Present, 0x80012345u } },
        } },
    };
    later.fields.push_back(std::move(list_field));
    Field stack_field;
    stack_field.name = "stack";
    stack_field.type = FieldType::StackTrace;
    stack_field.stack_trace = StackTraceValue{
        .read_ok = true,
        .frames = { StackFrame{ 0x81700000u, 0x81700020u, 0x80001004u, 0x80001000u } },
    };
    later.fields.push_back(std::move(stack_field));
    Field trace_field;
    trace_field.name = "address_trace";
    trace_field.type = FieldType::AddressTrace;
    trace_field.address_trace = AddressTraceValue{
        .success = false,
        .final_address = 0x80311A84u,
        .failure_operation = 1,
        .failure = "guest_read_failed",
        .operations = {
            AddressTraceOperation{ 0, 7, 0, 0x80311A84u, std::nullopt, "" },
            AddressTraceOperation{ 1, 2, 0x80311A84u, 0x80311A84u,
                std::nullopt, "guest_read_failed" },
        },
    };
    later.fields.push_back(std::move(trace_field));
    ASSERT_TRUE(writer.append(later, &error)) << error;

    Event earlier = event(5, "delayed", 2);
    earlier.record_sequence = 2;
    ASSERT_TRUE(writer.append(earlier, &error)) << error;
    ASSERT_TRUE(writer.close(&error)) << error;

    std::vector<Event> events;
    ASSERT_TRUE(Reader::read_all(capture, events, nullptr, &error)) << error;
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].capture_sequence, 10u);
    EXPECT_EQ(events[0].record_sequence, 1u);
    EXPECT_EQ(events[1].capture_sequence, 5u);
    EXPECT_EQ(events[1].record_sequence, 2u);
    ASSERT_EQ(events[0].fields.size(), 3u);
    EXPECT_TRUE(events[0].fields[0].linked_list.has_value());
    EXPECT_TRUE(events[0].fields[1].stack_trace.has_value());
    EXPECT_TRUE(events[0].fields[2].address_trace.has_value());

    const auto expanded = event_to_json(events[0]);
    EXPECT_NE(expanded.find(R"("root":"0x80311a84")"), std::string::npos);
    EXPECT_NE(expanded.find(R"("callsite_pc":"0x80001000")"), std::string::npos);
    EXPECT_NE(expanded.find(R"("failure_operation":1)"), std::string::npos);
    EXPECT_NE(expanded.find(R"("operation":"load_ptr32")"), std::string::npos);
    const auto raw = event_to_json(events[0], true);
    EXPECT_EQ(raw.find(R"("nodes")"), std::string::npos);
}

TEST(SavorCaptureArtifact, ReportsGapsMetricsAndIncompleteStatus)
{
    TempCaptureDirectory temp;
    const auto capture = temp.path / "incomplete.scap";
    Writer writer;
    std::string error;
    ASSERT_TRUE(writer.open(capture, metadata(), {}, &error)) << error;
    Event gap = event(1, "probe.capture.gap", 0);
    gap.kind = EventKind::Gap;
    ASSERT_TRUE(writer.append(gap, &error)) << error;
    Event metrics = event(2, "probe.session.metrics", 0);
    metrics.kind = EventKind::Metrics;
    metrics.fields = {
        Field{ .name = "complete", .value = 0 },
        Field{ .name = "capture_drops", .value = 3 },
        Field{ .name = "progress_drops", .value = 2 },
        Field{ .name = "profile_revision", .value = 1 },
    };
    ASSERT_TRUE(writer.append(metrics, &error)) << error;
    writer.mark_incomplete("unit-test loss");
    ASSERT_TRUE(writer.close(&error)) << error;

    PreparedCaptureArtifact result;
    ASSERT_TRUE(prepare_capture_artifact(
        capture, temp.path / "incomplete.jsonl", &result, &error)) << error;
    EXPECT_FALSE(result.verified);
    EXPECT_FALSE(result.complete);
    EXPECT_EQ(result.gap_count, 1u);
    EXPECT_EQ(result.metrics_event_count, 1u);
    EXPECT_EQ(result.capture_drops, 3u);
    EXPECT_EQ(result.progress_drops, 2u);
    EXPECT_EQ(result.drops, 5u);
    EXPECT_FALSE(result.incomplete_reason.empty());
}

TEST(SavorCaptureFormat, RecoversCompleteChunksAfterFooterLoss)
{
    TempCaptureDirectory temp;
    const auto original = temp.path / "original.scap";
    const auto damaged = temp.path / "damaged.scap";
    const auto recovered = temp.path / "recovered.scap";

    WriterOptions options;
    options.events_per_chunk = 1;
    Writer writer;
    std::string error;
    ASSERT_TRUE(writer.open(original, metadata(), options, &error)) << error;
    ASSERT_TRUE(writer.append(event(1, "recover", 9), &error)) << error;
    ASSERT_TRUE(writer.close(&error)) << error;

    std::filesystem::copy_file(original, damaged);
    std::ifstream input(damaged, std::ios::binary);
    std::vector<char> bytes{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
    const auto footer = footer_offset(bytes);
    ASSERT_NE(footer, std::string::npos);
    std::filesystem::resize_file(damaged, footer);

    const auto damaged_report = Reader::verify(damaged);
    EXPECT_FALSE(damaged_report.ok);
    EXPECT_TRUE(damaged_report.recoverable);
    ASSERT_TRUE(Reader::recover(damaged, recovered, &error)) << error;

    const auto recovered_report = Reader::verify(recovered);
    EXPECT_FALSE(recovered_report.ok);
    EXPECT_TRUE(recovered_report.recoverable);
    ASSERT_EQ(recovered_report.segments.size(), 1u);
    EXPECT_TRUE(recovered_report.segments.front().has_footer);
    EXPECT_FALSE(recovered_report.segments.front().complete);

    std::vector<Event> recovered_events;
    ASSERT_TRUE(Reader::read_all(recovered, recovered_events, nullptr, &error)) << error;
    ASSERT_EQ(recovered_events.size(), 1u);
    EXPECT_EQ(recovered_events.front().fields.front().value, 9u);
}

TEST(SavorCaptureArtifact, VerifiesExportsAndReducesSeedOverrideMarkers)
{
    TempCaptureDirectory temp;
    const auto capture = temp.path / "markers.scap";
    const auto exported = temp.path / "exports" / "markers.jsonl";

    Writer writer;
    std::string error;
    ASSERT_TRUE(writer.open(capture, metadata(), {}, &error)) << error;
    std::uint64_t sequence = 1;
    for (const auto& [id, value] : std::vector<std::pair<std::string, std::uint64_t>>{
             { "rng.seed_override.original", 0x11111111u },
             { "rng.seed_override.requested", 0x22222222u },
             { "rng.seed_override.applied", 0x22222222u },
         }) {
        Event marker;
        marker.capture_sequence = sequence;
        marker.record_sequence = sequence++;
        marker.kind = EventKind::Marker;
        marker.probe_id = id;
        marker.value = value;
        ASSERT_TRUE(writer.append(marker, &error)) << error;
    }
    ASSERT_TRUE(writer.close(&error)) << error;

    PreparedCaptureArtifact result;
    ASSERT_TRUE(prepare_capture_artifact(capture, exported, &result, &error)) << error;
    EXPECT_TRUE(result.verified);
    EXPECT_TRUE(result.complete);
    EXPECT_EQ(result.segment_count, 1u);
    EXPECT_EQ(result.chunk_count, 1u);
    EXPECT_EQ(result.event_count, 3u);
    EXPECT_EQ(result.seed_override.original_seed, 0x11111111u);
    EXPECT_EQ(result.seed_override.requested_seed, 0x22222222u);
    EXPECT_EQ(result.seed_override.applied_seed, 0x22222222u);
    EXPECT_EQ(result.seed_override.readback_matches, true);
    EXPECT_TRUE(std::filesystem::exists(exported));
}

} // namespace
