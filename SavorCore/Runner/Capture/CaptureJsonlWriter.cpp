#include "CaptureJsonlWriter.h"

#include <iomanip>
#include <sstream>

namespace savor::capture {
namespace {

void append_json_string_field(std::ostream& out, const char* key, const std::string& value, bool comma = true)
{
    out << "\"" << key << "\":\"" << JsonEscape(value) << "\"";
    if (comma) out << ",";
}

void append_json_u64_field(std::ostream& out, const char* key, std::uint64_t value, bool comma = true)
{
    out << "\"" << key << "\":" << value;
    if (comma) out << ",";
}

void append_json_bool_field(std::ostream& out, const char* key, bool value, bool comma = true)
{
    out << "\"" << key << "\":" << (value ? "true" : "false");
    if (comma) out << ",";
}

} // namespace

std::string JsonEscape(const std::string& value)
{
    std::ostringstream out;
    for (const char c : value) {
        switch (c) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default: out << c; break;
        }
    }
    return out.str();
}

std::string HexU32(std::uint32_t value)
{
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

std::string HexU64(std::uint64_t value)
{
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

std::string SerializeJsonlRecord(const CheckpointCaptureRecord& record)
{
    std::ostringstream out;
    out << "{";
    append_json_u64_field(out, "capture_sequence", record.capture_sequence);
    append_json_u64_field(out, "checkpoint_hit_count", record.checkpoint_hit_count);
    append_json_string_field(out, "pc", HexU32(record.pc));
    append_json_string_field(out, "checkpoint_id", record.checkpoint_id);
    append_json_string_field(out, "checkpoint_name", record.checkpoint_name);
    append_json_string_field(out, "function", record.function);
    append_json_string_field(out, "checkpoint", record.checkpoint);
    append_json_u64_field(out, "movie_input_count", record.movie_input_count);
    append_json_u64_field(out, "vi_field_count", record.vi_field_count);
    append_json_u64_field(out, "frame_count", record.frame_count);
    append_json_u64_field(out, "tbr_high", record.tbr_high);
    append_json_u64_field(out, "tbr_low", record.tbr_low);
    append_json_string_field(out, "tbr_u64_hex", HexU64(record.tbr_u64));
    append_json_u64_field(out, "rng_draw_index_before", record.rng_draw_index_before);
    append_json_bool_field(out, "owns_rng_draw", record.owns_rng_draw, false);

    for (std::size_t i = 0; i < record.fields.size(); ++i) {
        const auto& field = record.fields[i];
        out << ",";
        if (field.quote) {
            append_json_string_field(out, field.name.c_str(), field.value, false);
        } else {
            out << "\"" << field.name << "\":" << field.value;
        }
    }
    out << "}";
    return out.str();
}

bool CaptureJsonlWriter::open(const std::filesystem::path& path, std::string* error_out)
{
    path_ = path;
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            if (error_out) *error_out = "failed to create capture output directory: " + ec.message();
            return false;
        }
    }
    out_.open(path, std::ios::binary | std::ios::trunc);
    if (!out_.is_open()) {
        if (error_out) *error_out = "failed to open capture output: " + path.string();
        return false;
    }
    return true;
}

bool CaptureJsonlWriter::write(const CheckpointCaptureRecord& record, std::string* error_out)
{
    if (!out_.is_open()) {
        if (error_out) *error_out = "capture output is not open";
        return false;
    }
    const auto line = SerializeJsonlRecord(record);
    out_ << line << '\n';
    out_.flush();
    if (!out_.good()) {
        if (error_out) *error_out = "failed to write capture output: " + path_.string();
        return false;
    }
    return true;
}

} // namespace savor::capture
