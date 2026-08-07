#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace savor::capture_format {

inline constexpr std::uint32_t kFormatVersion = 1;
inline constexpr std::string_view kProfileSchema = "savor.capture.profile/1";

enum class EventKind : std::uint8_t {
    Pc = 1,
    Memory = 2,
    Progress = 3,
    Marker = 4,
    Control = 5,
    Gap = 6,
    Metrics = 7,
};

enum class FieldType : std::uint8_t {
    Unsigned = 1,
    Signed = 2,
    FloatBits = 3,
    Bytes = 4,
    Text = 5,
    LinkedList = 6,
    StackTrace = 7,
    AddressTrace = 8,
};

enum class FieldStatus : std::uint8_t {
    Present = 0,
    Missing = 1,
    Truncated = 2,
    Invalid = 3,
};

struct LinkedListMember {
    std::string name;
    std::uint8_t width = 4;
    FieldStatus status = FieldStatus::Present;
    std::uint64_t value = 0;
    bool operator==(const LinkedListMember&) const = default;
};

struct LinkedListNode {
    std::uint32_t address = 0;
    std::vector<LinkedListMember> fields;
    bool operator==(const LinkedListNode&) const = default;
};

struct LinkedListValue {
    std::uint32_t root_address = 0;
    std::uint32_t head = 0;
    bool read_ok = false;
    bool truncated = false;
    bool cycle_detected = false;
    std::vector<LinkedListNode> nodes;
    bool operator==(const LinkedListValue&) const = default;
};

struct StackFrame {
    std::uint32_t stack_pointer = 0;
    std::uint32_t next_stack_pointer = 0;
    std::uint32_t return_pc = 0;
    std::uint32_t callsite_pc = 0;
    bool operator==(const StackFrame&) const = default;
};

struct StackTraceValue {
    bool read_ok = false;
    bool truncated = false;
    bool cycle_detected = false;
    std::vector<StackFrame> frames;
    bool operator==(const StackTraceValue&) const = default;
};

struct AddressTraceOperation {
    std::uint8_t index = 0;
    std::uint8_t operation = 0;
    std::uint32_t address_before = 0;
    std::uint32_t address_after = 0;
    std::optional<std::uint64_t> dereferenced_value;
    std::string failure;
    bool operator==(const AddressTraceOperation&) const = default;
};

struct AddressTraceValue {
    bool success = false;
    std::uint32_t final_address = 0;
    std::optional<std::uint8_t> failure_operation;
    std::string failure;
    std::vector<AddressTraceOperation> operations;
    bool operator==(const AddressTraceValue&) const = default;
};

struct Field {
    std::string name;
    FieldType type = FieldType::Unsigned;
    FieldStatus status = FieldStatus::Present;
    std::uint64_t value = 0;
    std::vector<std::uint8_t> bytes;
    std::optional<LinkedListValue> linked_list;
    std::optional<StackTraceValue> stack_trace;
    std::optional<AddressTraceValue> address_trace;

    bool same_shape(const Field& other) const;
    bool same_value(const Field& other) const;
};

struct Event {
    std::uint64_t capture_sequence = 0;
    std::uint64_t record_sequence = 0;
    std::uint64_t monotonic_ns = 0;
    std::uint64_t frame_index = 0;
    std::uint64_t guest_workset_epoch = 0;
    std::uint64_t profile_revision = 0;
    std::uint64_t snapshot_id = 0;
    EventKind kind = EventKind::Pc;
    std::uint32_t pc = 0;
    std::uint32_t address = 0;
    std::uint32_t size = 0;
    std::uint64_t value = 0;
    std::string probe_id;
    std::vector<Field> fields;
};

struct FileMetadata {
    std::string session_id;
    std::string source_identity;
    std::string executable_sha256;
    std::string dolphin_source_commit;
    std::string profile_json;
    std::uint64_t created_utc_ns = 0;
};

struct WriterOptions {
    std::size_t events_per_chunk = 1024;
    std::size_t bytes_per_chunk = 4u * 1024u * 1024u;
    std::uint64_t chunk_interval_ns = 1'000'000'000ull;
    std::uint64_t rotate_bytes = 1ull * 1024ull * 1024ull * 1024ull;
    std::size_t index_interval_chunks = 64;
    int compression_level = 3;
};

struct ChunkInfo {
    std::uint64_t index = 0;
    std::uint64_t file_offset = 0;
    std::uint64_t first_sequence = 0;
    std::uint64_t last_sequence = 0;
    std::uint32_t event_count = 0;
    std::uint32_t compressed_bytes = 0;
    std::uint32_t uncompressed_bytes = 0;
    std::uint64_t checksum = 0;
};

struct SegmentInfo {
    std::filesystem::path path;
    FileMetadata metadata;
    std::vector<ChunkInfo> chunks;
    std::uint64_t event_count = 0;
    bool has_footer = false;
    bool complete = false;
    std::string incomplete_reason;
};

struct VerificationReport {
    bool ok = false;
    bool recoverable = false;
    std::vector<SegmentInfo> segments;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

class Writer {
public:
    Writer();
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    bool open(
        const std::filesystem::path& path,
        FileMetadata metadata,
        WriterOptions options = {},
        std::string* error_out = nullptr);
    bool append(const Event& event, std::string* error_out = nullptr);
    bool mark_incomplete(std::string reason);
    bool close(std::string* error_out = nullptr);

    bool is_open() const { return open_; }
    bool complete() const { return complete_; }
    std::uint64_t event_count() const { return event_count_; }
    std::uint64_t chunk_count() const { return chunk_index_; }
    const std::vector<std::filesystem::path>& segment_paths() const { return segment_paths_; }

private:
    bool open_segment(std::string* error_out);
    bool flush_chunk(std::string* error_out);
    bool write_index_checkpoint(std::string* error_out);
    bool write_footer(std::string* error_out);
    bool rotate_if_needed(std::string* error_out);
    std::filesystem::path segment_path(std::uint32_t index) const;

    bool open_ = false;
    bool complete_ = true;
    std::string incomplete_reason_;
    std::filesystem::path base_path_;
    FileMetadata metadata_;
    WriterOptions options_;
    std::ofstream stream_;
    std::uint32_t segment_index_ = 0;
    std::uint64_t segment_bytes_ = 0;
    std::uint64_t segment_event_count_ = 0;
    std::uint64_t segment_chunk_count_ = 0;
    std::uint64_t event_count_ = 0;
    std::uint64_t chunk_index_ = 0;
    std::uint64_t last_index_offset_ = 0;
    std::vector<Event> pending_;
    std::size_t pending_estimated_bytes_ = 0;
    std::uint64_t pending_started_ns_ = 0;
    std::vector<ChunkInfo> pending_index_;
    std::vector<std::filesystem::path> segment_paths_;
};

class Reader {
public:
    static VerificationReport verify(const std::filesystem::path& path);
    static bool read_all(
        const std::filesystem::path& path,
        std::vector<Event>& events,
        VerificationReport* report = nullptr,
        std::string* error_out = nullptr);
    static bool export_jsonl(
        const std::filesystem::path& input,
        const std::filesystem::path& output,
        std::optional<std::string_view> probe_filter = std::nullopt,
        std::uint64_t first_sequence = 0,
        std::uint64_t last_sequence = UINT64_MAX,
        bool raw_bytes = false,
        std::string* error_out = nullptr);
    static bool recover(
        const std::filesystem::path& input,
        const std::filesystem::path& output,
        std::string* error_out = nullptr);
};

std::string event_to_json(const Event& event, bool raw_bytes = false);
std::string event_kind_name(EventKind kind);

} // namespace savor::capture_format
