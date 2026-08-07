#include "CaptureFormat.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <limits>
#include <ranges>
#include <sstream>
#include <unordered_map>

#include <xxhash.h>
#include <zstd.h>
#include <picojson.h>

namespace savor::capture_format {
namespace {

constexpr std::array<char, 8> kFileMagic{ 'S', 'A', 'V', 'O', 'R', 'S', 'C', '\0' };
constexpr std::array<char, 4> kChunkTag{ 'C', 'H', 'N', 'K' };
constexpr std::array<char, 4> kIndexTag{ 'I', 'N', 'D', 'X' };
constexpr std::array<char, 4> kFooterTag{ 'F', 'O', 'O', 'T' };

struct DeltaState {
    std::vector<Field> fields;
};

using DeltaMap = std::unordered_map<std::string, DeltaState>;

std::string baseline_key(const Event& event)
{
    return event.probe_id + "\x1f" + std::to_string(event.guest_workset_epoch) + "\x1f"
        + std::to_string(event.profile_revision);
}

template <typename T>
void append_pod(std::vector<std::uint8_t>& out, T value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

template <typename T>
bool read_pod(const std::vector<std::uint8_t>& in, std::size_t& offset, T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    if (offset > in.size() || in.size() - offset < sizeof(T))
        return false;
    std::memcpy(&value, in.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

template <typename T>
bool read_stream_pod(std::ifstream& stream, T& value)
{
    return static_cast<bool>(stream.read(reinterpret_cast<char*>(&value), sizeof(T)));
}

template <typename T>
bool write_stream_pod(std::ofstream& stream, T value)
{
    stream.write(reinterpret_cast<const char*>(&value), sizeof(T));
    return static_cast<bool>(stream);
}

void append_varint(std::vector<std::uint8_t>& out, std::uint64_t value)
{
    while (value >= 0x80) {
        out.push_back(static_cast<std::uint8_t>(value) | 0x80);
        value >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

bool read_varint(const std::vector<std::uint8_t>& in, std::size_t& offset, std::uint64_t& value)
{
    value = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        if (offset >= in.size())
            return false;
        const auto byte = in[offset++];
        value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0)
            return true;
    }
    return false;
}

void append_string(std::vector<std::uint8_t>& out, std::string_view value)
{
    append_varint(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

bool read_string(const std::vector<std::uint8_t>& in, std::size_t& offset, std::string& value);

bool field_uses_blob(FieldType type)
{
    return type == FieldType::Bytes || type == FieldType::Text
        || type == FieldType::LinkedList || type == FieldType::StackTrace
        || type == FieldType::AddressTrace;
}

std::vector<std::uint8_t> structured_field_bytes(const Field& field)
{
    if (field.type == FieldType::Bytes || field.type == FieldType::Text)
        return field.bytes;
    std::vector<std::uint8_t> out;
    if (field.type == FieldType::LinkedList && field.linked_list.has_value()) {
        const auto& list = *field.linked_list;
        append_pod(out, list.root_address);
        append_pod(out, list.head);
        out.push_back(list.read_ok ? 1 : 0);
        out.push_back(list.truncated ? 1 : 0);
        out.push_back(list.cycle_detected ? 1 : 0);
        append_varint(out, list.nodes.size());
        for (const auto& node : list.nodes) {
            append_pod(out, node.address);
            append_varint(out, node.fields.size());
            for (const auto& member : node.fields) {
                append_string(out, member.name);
                out.push_back(member.width);
                out.push_back(static_cast<std::uint8_t>(member.status));
                append_pod(out, member.value);
            }
        }
    } else if (field.type == FieldType::StackTrace && field.stack_trace.has_value()) {
        const auto& stack = *field.stack_trace;
        out.push_back(stack.read_ok ? 1 : 0);
        out.push_back(stack.truncated ? 1 : 0);
        out.push_back(stack.cycle_detected ? 1 : 0);
        append_varint(out, stack.frames.size());
        for (const auto& frame : stack.frames) {
            append_pod(out, frame.stack_pointer);
            append_pod(out, frame.next_stack_pointer);
            append_pod(out, frame.return_pc);
            append_pod(out, frame.callsite_pc);
        }
    } else if (field.type == FieldType::AddressTrace && field.address_trace.has_value()) {
        const auto& trace = *field.address_trace;
        out.push_back(trace.success ? 1 : 0);
        append_pod(out, trace.final_address);
        out.push_back(trace.failure_operation.has_value() ? 1 : 0);
        out.push_back(trace.failure_operation.value_or(0));
        append_string(out, trace.failure);
        append_varint(out, trace.operations.size());
        for (const auto& operation : trace.operations) {
            out.push_back(operation.index);
            out.push_back(operation.operation);
            append_pod(out, operation.address_before);
            append_pod(out, operation.address_after);
            out.push_back(operation.dereferenced_value.has_value() ? 1 : 0);
            append_pod(out, operation.dereferenced_value.value_or(0));
            append_string(out, operation.failure);
        }
    }
    return out;
}

bool decode_structured_field(Field& field, const std::vector<std::uint8_t>& bytes)
{
    if (field.type == FieldType::Bytes || field.type == FieldType::Text) {
        field.bytes = bytes;
        return true;
    }
    std::size_t offset = 0;
    if (field.type == FieldType::LinkedList) {
        LinkedListValue list;
        std::uint8_t read_ok = 0;
        std::uint8_t truncated = 0;
        std::uint8_t cycle = 0;
        std::uint64_t node_count = 0;
        if (!read_pod(bytes, offset, list.root_address)
            || !read_pod(bytes, offset, list.head)
            || offset + 3 > bytes.size()) return false;
        read_ok = bytes[offset++]; truncated = bytes[offset++]; cycle = bytes[offset++];
        if (!read_varint(bytes, offset, node_count) || node_count > 256) return false;
        list.read_ok = read_ok != 0;
        list.truncated = truncated != 0;
        list.cycle_detected = cycle != 0;
        for (std::uint64_t i = 0; i < node_count; ++i) {
            LinkedListNode node;
            std::uint64_t field_count = 0;
            if (!read_pod(bytes, offset, node.address)
                || !read_varint(bytes, offset, field_count) || field_count > 4096) return false;
            for (std::uint64_t j = 0; j < field_count; ++j) {
                LinkedListMember member;
                if (!read_string(bytes, offset, member.name) || offset + 2 > bytes.size()) return false;
                member.width = bytes[offset++];
                member.status = static_cast<FieldStatus>(bytes[offset++]);
                if (!read_pod(bytes, offset, member.value)) return false;
                node.fields.push_back(std::move(member));
            }
            list.nodes.push_back(std::move(node));
        }
        if (offset != bytes.size()) return false;
        field.linked_list = std::move(list);
        return true;
    }
    if (field.type == FieldType::StackTrace) {
        StackTraceValue stack;
        if (offset + 3 > bytes.size()) return false;
        stack.read_ok = bytes[offset++] != 0;
        stack.truncated = bytes[offset++] != 0;
        stack.cycle_detected = bytes[offset++] != 0;
        std::uint64_t frame_count = 0;
        if (!read_varint(bytes, offset, frame_count) || frame_count > 64) return false;
        for (std::uint64_t i = 0; i < frame_count; ++i) {
            StackFrame frame;
            if (!read_pod(bytes, offset, frame.stack_pointer)
                || !read_pod(bytes, offset, frame.next_stack_pointer)
                || !read_pod(bytes, offset, frame.return_pc)
                || !read_pod(bytes, offset, frame.callsite_pc)) return false;
            stack.frames.push_back(frame);
        }
        if (offset != bytes.size()) return false;
        field.stack_trace = std::move(stack);
        return true;
    }
    if (field.type == FieldType::AddressTrace) {
        AddressTraceValue trace;
        if (offset >= bytes.size()) return false;
        trace.success = bytes[offset++] != 0;
        if (!read_pod(bytes, offset, trace.final_address) || offset + 2 > bytes.size()) return false;
        const bool has_failure_operation = bytes[offset++] != 0;
        const auto failure_operation = bytes[offset++];
        if (has_failure_operation) trace.failure_operation = failure_operation;
        if (!read_string(bytes, offset, trace.failure)) return false;
        std::uint64_t operation_count = 0;
        if (!read_varint(bytes, offset, operation_count) || operation_count > 64) return false;
        for (std::uint64_t i = 0; i < operation_count; ++i) {
            AddressTraceOperation operation;
            if (offset + 2 > bytes.size()) return false;
            operation.index = bytes[offset++];
            operation.operation = bytes[offset++];
            if (!read_pod(bytes, offset, operation.address_before)
                || !read_pod(bytes, offset, operation.address_after) || offset >= bytes.size()) return false;
            const bool has_dereference = bytes[offset++] != 0;
            std::uint64_t dereferenced = 0;
            if (!read_pod(bytes, offset, dereferenced)
                || !read_string(bytes, offset, operation.failure)) return false;
            if (has_dereference) operation.dereferenced_value = dereferenced;
            trace.operations.push_back(std::move(operation));
        }
        if (offset != bytes.size()) return false;
        field.address_trace = std::move(trace);
        return true;
    }
    return false;
}

bool read_string(const std::vector<std::uint8_t>& in, std::size_t& offset, std::string& value)
{
    std::uint64_t size = 0;
    if (!read_varint(in, offset, size) || size > in.size() - offset)
        return false;
    value.assign(reinterpret_cast<const char*>(in.data() + offset), static_cast<std::size_t>(size));
    offset += static_cast<std::size_t>(size);
    return true;
}

void append_field_value(std::vector<std::uint8_t>& out, const Field& field)
{
    out.push_back(static_cast<std::uint8_t>(field.status));
    append_pod(out, field.value);
    if (field_uses_blob(field.type)) {
        const auto bytes = structured_field_bytes(field);
        append_varint(out, bytes.size());
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
}

bool read_field_value(
    const std::vector<std::uint8_t>& in,
    std::size_t& offset,
    Field& field)
{
    if (offset >= in.size())
        return false;
    field.status = static_cast<FieldStatus>(in[offset++]);
    if (!read_pod(in, offset, field.value))
        return false;
    if (field_uses_blob(field.type)) {
        std::uint64_t size = 0;
        if (!read_varint(in, offset, size) || size > in.size() - offset)
            return false;
        const std::vector<std::uint8_t> bytes(
            in.begin() + offset, in.begin() + offset + static_cast<std::size_t>(size));
        offset += static_cast<std::size_t>(size);
        return decode_structured_field(field, bytes);
    }
    field.bytes.clear();
    field.linked_list.reset();
    field.stack_trace.reset();
    field.address_trace.reset();
    return true;
}

bool same_shape(const std::vector<Field>& lhs, const std::vector<Field>& rhs)
{
    if (lhs.size() != rhs.size())
        return false;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (!lhs[i].same_shape(rhs[i]))
            return false;
    }
    return true;
}

void encode_event(
    std::vector<std::uint8_t>& out,
    const Event& event,
    std::uint64_t& prior_record_sequence,
    DeltaMap& baselines)
{
    append_varint(out, event.record_sequence - prior_record_sequence);
    prior_record_sequence = event.record_sequence;
    append_varint(out, event.capture_sequence);
    append_varint(out, event.monotonic_ns);
    append_varint(out, event.frame_index);
    append_varint(out, event.guest_workset_epoch);
    append_varint(out, event.profile_revision);
    append_varint(out, event.snapshot_id);
    out.push_back(static_cast<std::uint8_t>(event.kind));
    append_pod(out, event.pc);
    append_pod(out, event.address);
    append_pod(out, event.size);
    append_pod(out, event.value);
    append_string(out, event.probe_id);

    const auto key = baseline_key(event);
    const auto found = baselines.find(key);
    const bool keyframe = found == baselines.end() || !same_shape(found->second.fields, event.fields);
    out.push_back(keyframe ? 1u : 0u);
    append_varint(out, event.fields.size());

    if (keyframe) {
        for (const auto& field : event.fields) {
            append_string(out, field.name);
            out.push_back(static_cast<std::uint8_t>(field.type));
            append_field_value(out, field);
        }
    } else {
        const auto bitmap_size = (event.fields.size() + 7) / 8;
        std::vector<std::uint8_t> bitmap(bitmap_size, 0);
        for (std::size_t i = 0; i < event.fields.size(); ++i) {
            const bool blob = field_uses_blob(event.fields[i].type);
            if (blob || !event.fields[i].same_value(found->second.fields[i]))
                bitmap[i / 8] |= static_cast<std::uint8_t>(1u << (i % 8));
        }
        append_varint(out, bitmap.size());
        out.insert(out.end(), bitmap.begin(), bitmap.end());
        for (std::size_t i = 0; i < event.fields.size(); ++i) {
            if ((bitmap[i / 8] & (1u << (i % 8))) != 0)
                append_field_value(out, event.fields[i]);
        }
    }

    baselines[key] = DeltaState{ event.fields };
    if (event.kind == EventKind::Gap)
        baselines.clear();
}

bool decode_event(
    const std::vector<std::uint8_t>& in,
    std::size_t& offset,
    std::uint64_t& prior_record_sequence,
    DeltaMap& baselines,
    Event& event)
{
    std::uint64_t record_sequence_delta = 0;
    std::uint64_t kind = 0;
    if (!read_varint(in, offset, record_sequence_delta)
        || !read_varint(in, offset, event.capture_sequence)
        || !read_varint(in, offset, event.monotonic_ns))
        return false;
    event.record_sequence = prior_record_sequence + record_sequence_delta;
    prior_record_sequence = event.record_sequence;
    if (!read_varint(in, offset, event.frame_index)
        || !read_varint(in, offset, event.guest_workset_epoch)
        || !read_varint(in, offset, event.profile_revision)
        || !read_varint(in, offset, event.snapshot_id)) {
        return false;
    }
    if (offset >= in.size())
        return false;
    kind = in[offset++];
    event.kind = static_cast<EventKind>(kind);
    if (!read_pod(in, offset, event.pc)
        || !read_pod(in, offset, event.address)
        || !read_pod(in, offset, event.size)
        || !read_pod(in, offset, event.value)
        || !read_string(in, offset, event.probe_id)
        || offset >= in.size()) {
        return false;
    }

    const bool keyframe = in[offset++] != 0;
    std::uint64_t field_count = 0;
    if (!read_varint(in, offset, field_count) || field_count > 65536)
        return false;
    const auto key = baseline_key(event);
    if (keyframe) {
        event.fields.clear();
        event.fields.reserve(static_cast<std::size_t>(field_count));
        for (std::uint64_t i = 0; i < field_count; ++i) {
            Field field;
            if (!read_string(in, offset, field.name) || offset >= in.size())
                return false;
            field.type = static_cast<FieldType>(in[offset++]);
            if (!read_field_value(in, offset, field))
                return false;
            event.fields.push_back(std::move(field));
        }
    } else {
        const auto found = baselines.find(key);
        if (found == baselines.end() || found->second.fields.size() != field_count)
            return false;
        event.fields = found->second.fields;
        std::uint64_t bitmap_size = 0;
        if (!read_varint(in, offset, bitmap_size)
            || bitmap_size != (event.fields.size() + 7) / 8
            || bitmap_size > in.size() - offset) {
            return false;
        }
        const auto bitmap_offset = offset;
        offset += static_cast<std::size_t>(bitmap_size);
        for (std::size_t i = 0; i < event.fields.size(); ++i) {
            if ((in[bitmap_offset + i / 8] & (1u << (i % 8))) != 0
                && !read_field_value(in, offset, event.fields[i])) {
                return false;
            }
        }
    }
    baselines[key] = DeltaState{ event.fields };
    if (event.kind == EventKind::Gap)
        baselines.clear();
    return true;
}

std::vector<std::uint8_t> encode_metadata(const FileMetadata& metadata)
{
    std::vector<std::uint8_t> out;
    append_pod(out, metadata.created_utc_ns);
    append_string(out, metadata.session_id);
    append_string(out, metadata.source_identity);
    append_string(out, metadata.executable_sha256);
    append_string(out, metadata.dolphin_source_commit);
    append_string(out, metadata.profile_json);
    return out;
}

bool decode_metadata(const std::vector<std::uint8_t>& bytes, FileMetadata& metadata)
{
    std::size_t offset = 0;
    return read_pod(bytes, offset, metadata.created_utc_ns)
        && read_string(bytes, offset, metadata.session_id)
        && read_string(bytes, offset, metadata.source_identity)
        && read_string(bytes, offset, metadata.executable_sha256)
        && read_string(bytes, offset, metadata.dolphin_source_commit)
        && read_string(bytes, offset, metadata.profile_json)
        && offset == bytes.size();
}

bool read_file_header(std::ifstream& stream, FileMetadata& metadata, std::string& error)
{
    std::array<char, 8> magic{};
    std::uint32_t version = 0;
    std::uint32_t metadata_size = 0;
    if (!stream.read(magic.data(), magic.size())
        || !read_stream_pod(stream, version)
        || !read_stream_pod(stream, metadata_size)) {
        error = "truncated capture header";
        return false;
    }
    if (magic != kFileMagic) {
        error = "invalid capture magic";
        return false;
    }
    if (version != kFormatVersion) {
        error = "unsupported capture version " + std::to_string(version);
        return false;
    }
    if (metadata_size > 64u * 1024u * 1024u) {
        error = "capture metadata is unreasonably large";
        return false;
    }
    std::vector<std::uint8_t> bytes(metadata_size);
    if (metadata_size != 0
        && !stream.read(reinterpret_cast<char*>(bytes.data()), metadata_size)) {
        error = "truncated capture metadata";
        return false;
    }
    if (!decode_metadata(bytes, metadata)) {
        error = "invalid capture metadata";
        return false;
    }
    return true;
}

std::vector<std::filesystem::path> discover_segments(const std::filesystem::path& base)
{
    std::vector<std::filesystem::path> paths;
    std::error_code ec;
    if (std::filesystem::exists(base, ec))
        paths.push_back(base);
    for (std::uint32_t index = 1;; ++index) {
        auto path = base.parent_path()
            / (base.stem().string() + "." + [&] {
                std::ostringstream out;
                out << std::setw(4) << std::setfill('0') << index;
                return out.str();
            }() + base.extension().string());
        if (!std::filesystem::exists(path, ec))
            break;
        paths.push_back(std::move(path));
    }
    return paths;
}

bool read_segment(
    const std::filesystem::path& path,
    SegmentInfo& info,
    std::vector<Event>* events,
    std::vector<std::string>& errors,
    std::vector<std::string>& warnings)
{
    info.path = path;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        errors.push_back("failed to open " + path.string());
        return false;
    }
    std::string header_error;
    if (!read_file_header(stream, info.metadata, header_error)) {
        errors.push_back(path.string() + ": " + header_error);
        return false;
    }

    while (true) {
        const auto record_offset = static_cast<std::uint64_t>(stream.tellg());
        std::array<char, 4> tag{};
        if (!stream.read(tag.data(), tag.size())) {
            if (!stream.eof())
                errors.push_back(path.string() + ": failed reading record tag");
            break;
        }
        if (tag == kChunkTag) {
            ChunkInfo chunk;
            chunk.file_offset = record_offset;
            if (!read_stream_pod(stream, chunk.index)
                || !read_stream_pod(stream, chunk.first_sequence)
                || !read_stream_pod(stream, chunk.last_sequence)
                || !read_stream_pod(stream, chunk.event_count)
                || !read_stream_pod(stream, chunk.uncompressed_bytes)
                || !read_stream_pod(stream, chunk.compressed_bytes)
                || !read_stream_pod(stream, chunk.checksum)) {
                errors.push_back(path.string() + ": truncated chunk header");
                break;
            }
            if (chunk.compressed_bytes > 256u * 1024u * 1024u
                || chunk.uncompressed_bytes > 512u * 1024u * 1024u) {
                errors.push_back(path.string() + ": unreasonable chunk size");
                break;
            }
            std::vector<std::uint8_t> compressed(chunk.compressed_bytes);
            if (chunk.compressed_bytes != 0
                && !stream.read(reinterpret_cast<char*>(compressed.data()), chunk.compressed_bytes)) {
                errors.push_back(path.string() + ": truncated chunk payload");
                break;
            }
            if (XXH64(compressed.data(), compressed.size(), 0) != chunk.checksum) {
                errors.push_back(path.string() + ": chunk checksum mismatch at index "
                    + std::to_string(chunk.index));
                break;
            }
            std::vector<std::uint8_t> uncompressed(chunk.uncompressed_bytes);
            const auto decoded_size = ZSTD_decompress(
                uncompressed.data(), uncompressed.size(), compressed.data(), compressed.size());
            if (ZSTD_isError(decoded_size) || decoded_size != uncompressed.size()) {
                errors.push_back(path.string() + ": chunk decompression failed at index "
                    + std::to_string(chunk.index));
                break;
            }
            DeltaMap baselines;
            std::size_t offset = 0;
            std::uint64_t record_sequence = 0;
            for (std::uint32_t i = 0; i < chunk.event_count; ++i) {
                Event event;
                if (!decode_event(uncompressed, offset, record_sequence, baselines, event)) {
                    errors.push_back(path.string() + ": event decode failed in chunk "
                        + std::to_string(chunk.index));
                    return false;
                }
                if (events)
                    events->push_back(std::move(event));
            }
            if (offset != uncompressed.size())
                warnings.push_back(path.string() + ": chunk has trailing bytes at index "
                    + std::to_string(chunk.index));
            info.event_count += chunk.event_count;
            info.chunks.push_back(chunk);
        } else if (tag == kIndexTag) {
            std::uint32_t count = 0;
            if (!read_stream_pod(stream, count)) {
                errors.push_back(path.string() + ": truncated index header");
                break;
            }
            constexpr std::size_t entry_size = sizeof(std::uint64_t) * 4;
            stream.seekg(static_cast<std::streamoff>(count) * entry_size, std::ios::cur);
            if (!stream) {
                errors.push_back(path.string() + ": truncated index body");
                break;
            }
        } else if (tag == kFooterTag) {
            std::uint64_t chunks = 0;
            std::uint64_t count = 0;
            std::uint64_t last_index = 0;
            std::uint8_t complete = 0;
            std::uint32_t reason_size = 0;
            if (!read_stream_pod(stream, chunks)
                || !read_stream_pod(stream, count)
                || !read_stream_pod(stream, last_index)
                || !read_stream_pod(stream, complete)
                || !read_stream_pod(stream, reason_size)) {
                errors.push_back(path.string() + ": truncated footer");
                break;
            }
            std::string reason(reason_size, '\0');
            if (reason_size && !stream.read(reason.data(), reason.size())) {
                errors.push_back(path.string() + ": truncated footer reason");
                break;
            }
            info.has_footer = true;
            info.complete = complete != 0;
            info.incomplete_reason = reason;
            if (chunks != info.chunks.size() || count != info.event_count)
                warnings.push_back(path.string() + ": footer counts do not match decoded records");
            if (!info.complete && !reason.empty())
                warnings.push_back(path.string() + ": capture marked incomplete: " + reason);
            break;
        } else {
            errors.push_back(path.string() + ": unknown record tag at offset "
                + std::to_string(record_offset));
            break;
        }
    }

    if (!info.has_footer)
        warnings.push_back(path.string() + ": missing footer; valid chunks are recoverable");
    return true;
}

std::string json_escape(std::string_view value)
{
    std::ostringstream out;
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20)
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch) << std::dec;
            else
                out << static_cast<char>(ch);
        }
    }
    return out.str();
}

std::string bytes_hex(const std::vector<std::uint8_t>& bytes)
{
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto byte : bytes)
        out << std::setw(2) << static_cast<unsigned>(byte);
    return out.str();
}

std::string hex_u32(std::uint32_t value)
{
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

std::string_view address_operation_name(std::uint8_t operation)
{
    switch (operation) {
    case 0x00: return "end";
    case 0x01: return "base_key";
    case 0x02: return "load_ptr32";
    case 0x03: return "add_i32";
    case 0x04: return "index_const";
    case 0x05: return "add_u32";
    case 0x06: return "base_gpr";
    case 0x07: return "base_absolute";
    case 0x08: return "index_gpr";
    default: return "unknown";
    }
}

void render_structured_field(std::ostringstream& out, const Field& field, bool raw_bytes)
{
    if (raw_bytes) {
        out << '"' << bytes_hex(structured_field_bytes(field)) << '"';
        return;
    }
    if (field.type == FieldType::LinkedList && field.linked_list.has_value()) {
        const auto& list = *field.linked_list;
        out << "{\"root\":\"" << hex_u32(list.root_address)
            << "\",\"head\":\"" << hex_u32(list.head)
            << "\",\"read_ok\":" << (list.read_ok ? "true" : "false")
            << ",\"truncated\":" << (list.truncated ? "true" : "false")
            << ",\"cycle_detected\":" << (list.cycle_detected ? "true" : "false")
            << ",\"nodes\":[";
        for (std::size_t i = 0; i < list.nodes.size(); ++i) {
            if (i) out << ',';
            const auto& node = list.nodes[i];
            out << "{\"index\":" << i << ",\"address\":\"" << hex_u32(node.address)
                << "\",\"fields\":{";
            for (std::size_t j = 0; j < node.fields.size(); ++j) {
                if (j) out << ',';
                const auto& member = node.fields[j];
                out << '"' << json_escape(member.name) << "\":{";
                out << "\"width\":" << static_cast<unsigned>(member.width)
                    << ",\"read_ok\":" << (member.status == FieldStatus::Present ? "true" : "false")
                    << ",\"value\":";
                if (member.status == FieldStatus::Present) out << member.value;
                else out << "null";
                out << '}';
            }
            out << "}}";
        }
        out << "]}";
        return;
    }
    if (field.type == FieldType::StackTrace && field.stack_trace.has_value()) {
        const auto& stack = *field.stack_trace;
        out << "{\"read_ok\":" << (stack.read_ok ? "true" : "false")
            << ",\"truncated\":" << (stack.truncated ? "true" : "false")
            << ",\"cycle_detected\":" << (stack.cycle_detected ? "true" : "false")
            << ",\"frames\":[";
        for (std::size_t i = 0; i < stack.frames.size(); ++i) {
            if (i) out << ',';
            const auto& frame = stack.frames[i];
            out << "{\"index\":" << i
                << ",\"stack_pointer\":\"" << hex_u32(frame.stack_pointer)
                << "\",\"next_stack_pointer\":\"" << hex_u32(frame.next_stack_pointer)
                << "\",\"return_pc\":\"" << hex_u32(frame.return_pc)
                << "\",\"callsite_pc\":\"" << hex_u32(frame.callsite_pc) << "\"}";
        }
        out << "]}";
        return;
    }
    if (field.type == FieldType::AddressTrace && field.address_trace.has_value()) {
        const auto& trace = *field.address_trace;
        out << "{\"success\":" << (trace.success ? "true" : "false")
            << ",\"final_address\":\"" << hex_u32(trace.final_address) << '"';
        if (trace.failure_operation.has_value())
            out << ",\"failure_operation\":" << static_cast<unsigned>(*trace.failure_operation);
        if (!trace.failure.empty()) out << ",\"failure\":\"" << json_escape(trace.failure) << '"';
        out << ",\"operations\":[";
        for (std::size_t i = 0; i < trace.operations.size(); ++i) {
            if (i) out << ',';
            const auto& operation = trace.operations[i];
            out << "{\"index\":" << static_cast<unsigned>(operation.index)
                << ",\"operation\":\"" << address_operation_name(operation.operation)
                << "\",\"opcode\":" << static_cast<unsigned>(operation.operation)
                << ",\"address_before\":\"" << hex_u32(operation.address_before)
                << "\",\"address_after\":\"" << hex_u32(operation.address_after) << '"';
            if (operation.dereferenced_value.has_value())
                out << ",\"dereferenced_value\":" << *operation.dereferenced_value;
            if (!operation.failure.empty())
                out << ",\"failure\":\"" << json_escape(operation.failure) << '"';
            out << '}';
        }
        out << "]}";
        return;
    }
    out << "null";
}

struct ProbeSymbol {
    std::string name;
    std::string function;
    std::string checkpoint;
    bool owns_rng_draw = false;
};

using ProbeSymbolMap = std::unordered_map<std::string, ProbeSymbol>;
using RevisionSymbolMap = std::unordered_map<std::uint64_t, ProbeSymbolMap>;

const picojson::value* json_member(
    const picojson::value::object& object, std::string_view name)
{
    const auto found = object.find(std::string(name));
    return found == object.end() ? nullptr : &found->second;
}

bool parse_profile_symbols(std::string_view text, RevisionSymbolMap& symbols)
{
    picojson::value root;
    auto position = text.begin();
    const auto error = picojson::parse(root, position, text.end());
    if (!error.empty() || !root.is<picojson::value::object>())
        return false;

    const auto& object = root.get<picojson::value::object>();
    std::uint64_t revision = 1;
    if (const auto* value = json_member(object, "revision"); value && value->is<double>())
        revision = static_cast<std::uint64_t>(value->get<double>());
    const auto* probes = json_member(object, "probes");
    if (!probes || !probes->is<picojson::value::array>())
        return false;

    ProbeSymbolMap parsed;
    for (const auto& probe_value : probes->get<picojson::value::array>()) {
        if (!probe_value.is<picojson::value::object>())
            continue;
        const auto& probe = probe_value.get<picojson::value::object>();
        const auto* id_value = json_member(probe, "id");
        if (!id_value || !id_value->is<std::string>() || id_value->get<std::string>().empty())
            continue;

        ProbeSymbol symbol;
        symbol.name = id_value->get<std::string>();
        symbol.function = "SavorProbe";
        symbol.checkpoint = symbol.name;
        if (const auto* owns = json_member(probe, "owns_rng_draw"); owns && owns->is<bool>())
            symbol.owns_rng_draw = owns->get<bool>();
        if (const auto* symbol_value = json_member(probe, "symbol");
            symbol_value && symbol_value->is<picojson::value::object>()) {
            const auto& symbol_object = symbol_value->get<picojson::value::object>();
            if (const auto* name = json_member(symbol_object, "name"); name && name->is<std::string>())
                symbol.name = name->get<std::string>();
            if (const auto* function = json_member(symbol_object, "function");
                function && function->is<std::string>()) {
                symbol.function = function->get<std::string>();
            }
            if (const auto* checkpoint = json_member(symbol_object, "checkpoint");
                checkpoint && checkpoint->is<std::string>()) {
                symbol.checkpoint = checkpoint->get<std::string>();
            }
        }
        parsed.emplace(id_value->get<std::string>(), std::move(symbol));
    }
    symbols[revision] = std::move(parsed);
    return true;
}

const ProbeSymbol& symbol_for_event(
    const Event& event,
    const RevisionSymbolMap& symbols,
    ProbeSymbol& fallback)
{
    const auto revision = symbols.find(event.profile_revision);
    if (revision != symbols.end()) {
        const auto probe = revision->second.find(event.probe_id);
        if (probe != revision->second.end())
            return probe->second;
    }
    fallback = ProbeSymbol{
        .name = event.probe_id,
        .function = "SavorProbe",
        .checkpoint = event.probe_id,
        .owns_rng_draw = false,
    };
    return fallback;
}

std::optional<std::string_view> text_field(const Event& event, std::string_view name)
{
    for (const auto& field : event.fields) {
        if (field.name == name && field.status == FieldStatus::Present
            && field.type == FieldType::Text) {
            return std::string_view(
                reinterpret_cast<const char*>(field.bytes.data()), field.bytes.size());
        }
    }
    return std::nullopt;
}

} // namespace

bool Field::same_shape(const Field& other) const
{
    return name == other.name && type == other.type;
}

bool Field::same_value(const Field& other) const
{
    return same_shape(other) && status == other.status && value == other.value
        && bytes == other.bytes && linked_list == other.linked_list
        && stack_trace == other.stack_trace && address_trace == other.address_trace;
}

Writer::Writer() = default;

Writer::~Writer()
{
    if (open_)
        close(nullptr);
}

bool Writer::open(
    const std::filesystem::path& path,
    FileMetadata metadata,
    WriterOptions options,
    std::string* error_out)
{
    if (open_) {
        if (error_out) *error_out = "capture writer is already open";
        return false;
    }
    if (options.events_per_chunk == 0 || options.bytes_per_chunk == 0
        || options.index_interval_chunks == 0 || options.rotate_bytes == 0) {
        if (error_out) *error_out = "capture writer limits must be positive";
        return false;
    }
    base_path_ = path;
    metadata_ = std::move(metadata);
    options_ = options;
    complete_ = true;
    incomplete_reason_.clear();
    segment_index_ = 0;
    segment_bytes_ = 0;
    segment_event_count_ = 0;
    segment_chunk_count_ = 0;
    event_count_ = 0;
    chunk_index_ = 0;
    last_index_offset_ = 0;
    pending_.clear();
    pending_index_.clear();
    segment_paths_.clear();
    open_ = true;
    if (!open_segment(error_out)) {
        open_ = false;
        return false;
    }
    return true;
}

bool Writer::append(const Event& event, std::string* error_out)
{
    if (!open_) {
        if (error_out) *error_out = "capture writer is not open";
        return false;
    }
    if (!pending_.empty() && event.record_sequence < pending_.back().record_sequence) {
        if (error_out) *error_out = "capture record sequence moved backwards";
        return false;
    }
    if (pending_.empty())
        pending_started_ns_ = event.monotonic_ns;
    pending_estimated_bytes_ += sizeof(Event) + event.probe_id.size() + event.fields.size() * sizeof(Field);
    for (const auto& field : event.fields)
        pending_estimated_bytes_ += field.name.size() + structured_field_bytes(field).size();
    pending_.push_back(event);
    ++event_count_;
    ++segment_event_count_;
    const bool elapsed = event.monotonic_ns >= pending_started_ns_
        && event.monotonic_ns - pending_started_ns_ >= options_.chunk_interval_ns;
    if (pending_.size() >= options_.events_per_chunk
        || pending_estimated_bytes_ >= options_.bytes_per_chunk
        || elapsed) {
        return flush_chunk(error_out);
    }
    return true;
}

bool Writer::mark_incomplete(std::string reason)
{
    complete_ = false;
    incomplete_reason_ = std::move(reason);
    return true;
}

bool Writer::open_segment(std::string* error_out)
{
    const auto path = segment_path(segment_index_);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        if (error_out) *error_out = "failed creating capture directory: " + ec.message();
        return false;
    }
    stream_.open(path, std::ios::binary | std::ios::trunc);
    if (!stream_) {
        if (error_out) *error_out = "failed opening capture segment " + path.string();
        return false;
    }
    const auto metadata_bytes = encode_metadata(metadata_);
    stream_.write(kFileMagic.data(), kFileMagic.size());
    write_stream_pod(stream_, kFormatVersion);
    write_stream_pod(stream_, static_cast<std::uint32_t>(metadata_bytes.size()));
    if (!metadata_bytes.empty())
        stream_.write(reinterpret_cast<const char*>(metadata_bytes.data()), metadata_bytes.size());
    if (!stream_) {
        if (error_out) *error_out = "failed writing capture header";
        return false;
    }
    segment_paths_.push_back(path);
    segment_bytes_ = static_cast<std::uint64_t>(stream_.tellp());
    segment_event_count_ = 0;
    segment_chunk_count_ = 0;
    last_index_offset_ = 0;
    pending_index_.clear();
    return true;
}

bool Writer::flush_chunk(std::string* error_out)
{
    if (pending_.empty())
        return true;
    std::vector<std::uint8_t> uncompressed;
    uncompressed.reserve(pending_estimated_bytes_);
    DeltaMap baselines;
    std::uint64_t prior_record_sequence = 0;
    for (const auto& event : pending_)
        encode_event(uncompressed, event, prior_record_sequence, baselines);

    std::vector<std::uint8_t> compressed(ZSTD_compressBound(uncompressed.size()));
    const auto compressed_size = ZSTD_compress(
        compressed.data(), compressed.size(), uncompressed.data(), uncompressed.size(),
        options_.compression_level);
    if (ZSTD_isError(compressed_size)) {
        if (error_out) *error_out = std::string("zstd compression failed: ") + ZSTD_getErrorName(compressed_size);
        mark_incomplete("compression failure");
        return false;
    }
    compressed.resize(compressed_size);

    ChunkInfo chunk;
    chunk.index = chunk_index_++;
    chunk.file_offset = static_cast<std::uint64_t>(stream_.tellp());
    chunk.first_sequence = pending_.front().record_sequence;
    chunk.last_sequence = pending_.back().record_sequence;
    chunk.event_count = static_cast<std::uint32_t>(pending_.size());
    chunk.uncompressed_bytes = static_cast<std::uint32_t>(uncompressed.size());
    chunk.compressed_bytes = static_cast<std::uint32_t>(compressed.size());
    chunk.checksum = XXH64(compressed.data(), compressed.size(), 0);

    stream_.write(kChunkTag.data(), kChunkTag.size());
    write_stream_pod(stream_, chunk.index);
    write_stream_pod(stream_, chunk.first_sequence);
    write_stream_pod(stream_, chunk.last_sequence);
    write_stream_pod(stream_, chunk.event_count);
    write_stream_pod(stream_, chunk.uncompressed_bytes);
    write_stream_pod(stream_, chunk.compressed_bytes);
    write_stream_pod(stream_, chunk.checksum);
    if (!compressed.empty())
        stream_.write(reinterpret_cast<const char*>(compressed.data()), compressed.size());
    if (!stream_) {
        if (error_out) *error_out = "failed writing capture chunk";
        mark_incomplete("capture file write failure");
        return false;
    }
    ++segment_chunk_count_;
    pending_index_.push_back(chunk);
    pending_.clear();
    pending_estimated_bytes_ = 0;
    pending_started_ns_ = 0;
    segment_bytes_ = static_cast<std::uint64_t>(stream_.tellp());
    if (segment_chunk_count_ % options_.index_interval_chunks == 0
        && !write_index_checkpoint(error_out)) {
        return false;
    }
    return rotate_if_needed(error_out);
}

bool Writer::write_index_checkpoint(std::string* error_out)
{
    last_index_offset_ = static_cast<std::uint64_t>(stream_.tellp());
    stream_.write(kIndexTag.data(), kIndexTag.size());
    write_stream_pod(stream_, static_cast<std::uint32_t>(pending_index_.size()));
    for (const auto& chunk : pending_index_) {
        write_stream_pod(stream_, chunk.index);
        write_stream_pod(stream_, chunk.file_offset);
        write_stream_pod(stream_, chunk.first_sequence);
        write_stream_pod(stream_, chunk.last_sequence);
    }
    if (!stream_) {
        if (error_out) *error_out = "failed writing capture index";
        mark_incomplete("capture index write failure");
        return false;
    }
    pending_index_.clear();
    segment_bytes_ = static_cast<std::uint64_t>(stream_.tellp());
    return true;
}

bool Writer::write_footer(std::string* error_out)
{
    if (!pending_index_.empty() && !write_index_checkpoint(error_out))
        return false;
    stream_.write(kFooterTag.data(), kFooterTag.size());
    write_stream_pod(stream_, segment_chunk_count_);
    write_stream_pod(stream_, segment_event_count_);
    write_stream_pod(stream_, last_index_offset_);
    write_stream_pod(stream_, static_cast<std::uint8_t>(complete_ ? 1 : 0));
    write_stream_pod(stream_, static_cast<std::uint32_t>(incomplete_reason_.size()));
    if (!incomplete_reason_.empty())
        stream_.write(incomplete_reason_.data(), incomplete_reason_.size());
    stream_.flush();
    if (!stream_) {
        if (error_out) *error_out = "failed writing capture footer";
        return false;
    }
    return true;
}

bool Writer::rotate_if_needed(std::string* error_out)
{
    if (segment_bytes_ < options_.rotate_bytes)
        return true;
    if (!write_footer(error_out))
        return false;
    stream_.close();
    ++segment_index_;
    return open_segment(error_out);
}

bool Writer::close(std::string* error_out)
{
    if (!open_)
        return true;
    bool ok = flush_chunk(error_out);
    if (ok)
        ok = write_footer(error_out);
    stream_.close();
    open_ = false;
    return ok;
}

std::filesystem::path Writer::segment_path(std::uint32_t index) const
{
    if (index == 0)
        return base_path_;
    std::ostringstream suffix;
    suffix << '.' << std::setw(4) << std::setfill('0') << index;
    return base_path_.parent_path()
        / (base_path_.stem().string() + suffix.str() + base_path_.extension().string());
}

VerificationReport Reader::verify(const std::filesystem::path& path)
{
    VerificationReport report;
    const auto segments = discover_segments(path);
    if (segments.empty()) {
        report.errors.push_back("capture file does not exist: " + path.string());
        return report;
    }
    for (const auto& segment_path : segments) {
        SegmentInfo info;
        read_segment(segment_path, info, nullptr, report.errors, report.warnings);
        report.segments.push_back(std::move(info));
    }
    report.recoverable = std::ranges::any_of(report.segments, [](const auto& segment) {
        return !segment.chunks.empty();
    });
    report.ok = report.errors.empty()
        && std::ranges::all_of(report.segments, [](const auto& segment) {
            return segment.has_footer && segment.complete;
        });
    return report;
}

bool Reader::read_all(
    const std::filesystem::path& path,
    std::vector<Event>& events,
    VerificationReport* report_out,
    std::string* error_out)
{
    VerificationReport report;
    const auto segments = discover_segments(path);
    if (segments.empty()) {
        if (error_out) *error_out = "capture file does not exist: " + path.string();
        return false;
    }
    for (const auto& segment_path : segments) {
        SegmentInfo info;
        read_segment(segment_path, info, &events, report.errors, report.warnings);
        report.segments.push_back(std::move(info));
    }
    report.recoverable = !events.empty();
    report.ok = report.errors.empty();
    if (report_out)
        *report_out = report;
    if (!report.errors.empty()) {
        if (error_out) *error_out = report.errors.front();
        return false;
    }
    return true;
}

std::string render_event_json(
    const Event& event,
    const ProbeSymbol* symbol,
    std::optional<std::uint64_t> rng_draw_index_before,
    bool raw_bytes)
{
    std::ostringstream out;
    out << '{'
        << "\"capture_sequence\":" << event.capture_sequence
        << ",\"record_sequence\":" << event.record_sequence
        << ",\"monotonic_ns\":" << event.monotonic_ns
        << ",\"frame_index\":" << event.frame_index
        << ",\"guest_workset_epoch\":" << event.guest_workset_epoch
        << ",\"profile_revision\":" << event.profile_revision
        << ",\"snapshot_id\":" << event.snapshot_id
        << ",\"event_kind\":\"" << event_kind_name(event.kind) << '"'
        << ",\"checkpoint_id\":\"" << json_escape(event.probe_id) << '"';
    if (symbol) {
        out << ",\"symbol\":\"" << json_escape(symbol->name) << '"'
            << ",\"function\":\"" << json_escape(symbol->function) << '"'
            << ",\"checkpoint\":\"" << json_escape(symbol->checkpoint) << '"'
            << ",\"owns_rng_draw\":" << (symbol->owns_rng_draw ? "true" : "false");
    }
    if (rng_draw_index_before.has_value())
        out << ",\"rng_draw_index_before\":" << *rng_draw_index_before;
    out << ",\"pc\":\"0x" << std::hex << std::setw(8) << std::setfill('0') << event.pc << std::dec << '"'
        << ",\"address\":\"0x" << std::hex << std::setw(8) << std::setfill('0') << event.address << std::dec << '"'
        << ",\"size\":" << event.size
        << ",\"value\":" << event.value;
    for (const auto& field : event.fields) {
        out << ",\"" << json_escape(field.name) << "\":";
        if (field.type == FieldType::LinkedList
            || field.type == FieldType::StackTrace
            || field.type == FieldType::AddressTrace) {
            render_structured_field(out, field, raw_bytes);
        } else if (field.status != FieldStatus::Present) {
            out << "null";
        } else if (field.type == FieldType::Text) {
            out << '"' << json_escape(std::string_view(
                reinterpret_cast<const char*>(field.bytes.data()), field.bytes.size())) << '"';
        } else if (field.type == FieldType::Bytes) {
            out << '"' << bytes_hex(field.bytes) << '"';
        } else {
            out << field.value;
        }
    }
    out << '}';
    return out.str();
}

bool Reader::export_jsonl(
    const std::filesystem::path& input,
    const std::filesystem::path& output,
    std::optional<std::string_view> probe_filter,
    std::uint64_t first_sequence,
    std::uint64_t last_sequence,
    bool raw_bytes,
    std::string* error_out)
{
    std::vector<Event> events;
    VerificationReport report;
    if (!read_all(input, events, &report, error_out))
        return false;
    std::stable_sort(events.begin(), events.end(), [](const Event& lhs, const Event& rhs) {
        return lhs.capture_sequence < rhs.capture_sequence;
    });

    RevisionSymbolMap symbols;
    for (const auto& segment : report.segments)
        parse_profile_symbols(segment.metadata.profile_json, symbols);

    std::ofstream stream(output, std::ios::binary | std::ios::trunc);
    if (!stream) {
        if (error_out) *error_out = "failed opening export output " + output.string();
        return false;
    }
    std::uint64_t rng_draw_index = 0;
    for (const auto& event : events) {
        if (event.kind == EventKind::Marker && event.probe_id == "profile.reload") {
            if (const auto profile_json = text_field(event, "profile_json"))
                parse_profile_symbols(*profile_json, symbols);
        }
        ProbeSymbol fallback;
        const auto& symbol = symbol_for_event(event, symbols, fallback);
        const auto event_draw_index = rng_draw_index;
        if (symbol.owns_rng_draw)
            ++rng_draw_index;
        if (event.capture_sequence < first_sequence || event.capture_sequence > last_sequence)
            continue;
        if (probe_filter.has_value() && event.probe_id != *probe_filter)
            continue;
        stream << render_event_json(event, &symbol, event_draw_index, raw_bytes) << '\n';
    }
    if (!stream) {
        if (error_out) *error_out = "failed writing JSONL export";
        return false;
    }
    return true;
}

bool Reader::recover(
    const std::filesystem::path& input,
    const std::filesystem::path& output,
    std::string* error_out)
{
    std::vector<Event> events;
    VerificationReport report;
    read_all(input, events, &report, nullptr);
    if (events.empty()) {
        if (error_out) *error_out = report.errors.empty() ? "no recoverable events" : report.errors.front();
        return false;
    }
    FileMetadata metadata = report.segments.front().metadata;
    metadata.source_identity += ";recovered";
    Writer writer;
    if (!writer.open(output, std::move(metadata), {}, error_out))
        return false;
    std::string recovery_reason = "recovered capture";
    for (const auto& segment : report.segments) {
        if (!segment.incomplete_reason.empty()) {
            recovery_reason += "; " + segment.incomplete_reason;
            break;
        }
    }
    writer.mark_incomplete(std::move(recovery_reason));
    for (const auto& event : events) {
        if (!writer.append(event, error_out))
            return false;
    }
    return writer.close(error_out);
}

std::string event_kind_name(EventKind kind)
{
    switch (kind) {
    case EventKind::Pc: return "pc";
    case EventKind::Memory: return "memory";
    case EventKind::Progress: return "progress";
    case EventKind::Marker: return "marker";
    case EventKind::Control: return "control";
    case EventKind::Gap: return "gap";
    case EventKind::Metrics: return "metrics";
    }
    return "unknown";
}

std::string event_to_json(const Event& event, bool raw_bytes)
{
    return render_event_json(event, nullptr, std::nullopt, raw_bytes);
}

} // namespace savor::capture_format
