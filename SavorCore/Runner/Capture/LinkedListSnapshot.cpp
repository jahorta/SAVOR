#include "LinkedListSnapshot.h"

#include <sstream>
#include <string>
#include <unordered_set>

namespace savor::capture {
namespace {

std::string sample_value_hex(std::uint64_t value, SampleWidth width)
{
    if (width == SampleWidth::U64) {
        return HexU64(value);
    }
    return HexU32(static_cast<std::uint32_t>(value));
}

std::uint32_t offset_address(std::uint32_t base, std::int32_t offset)
{
    return static_cast<std::uint32_t>(
        static_cast<std::int64_t>(base) + static_cast<std::int64_t>(offset));
}

} // namespace

std::vector<CaptureField> BuildLinkedListSnapshotFields(
    const LinkedListSnapshotSpec& spec,
    const LinkedListMemoryReader& read_memory)
{
    std::vector<CaptureField> fields;
    bool read_ok = true;
    bool truncated = false;
    bool cycle_detected = false;
    std::string error;
    std::uint64_t first_node_value = 0;

    fields.push_back(CaptureField{
        spec.name + "_head_ptr",
        "\"" + HexU32(spec.head_ptr_address) + "\"",
        false });
    if (!read_memory(spec.head_ptr_address, SampleWidth::U32, first_node_value)) {
        fields.push_back(CaptureField{ spec.name + "_read_ok", "false", false });
        fields.push_back(CaptureField{ spec.name + "_first_node", "\"" + HexU32(0) + "\"", false });
        fields.push_back(CaptureField{ spec.name + "_node_count", "0", false });
        fields.push_back(CaptureField{ spec.name + "_truncated", "false", false });
        fields.push_back(CaptureField{ spec.name + "_cycle_detected", "false", false });
        fields.push_back(CaptureField{ spec.name + "_error", "failed to read head_ptr", true });
        fields.push_back(CaptureField{ spec.name + "_nodes", "[]", false });
        return fields;
    }

    std::uint32_t node = static_cast<std::uint32_t>(first_node_value);
    std::uint32_t node_count = 0;
    std::unordered_set<std::uint32_t> seen;
    std::ostringstream nodes_json;
    nodes_json << "[";

    while (node != 0 && node_count < spec.max_nodes) {
        if (!seen.insert(node).second) {
            cycle_detected = true;
            error = "cycle detected at " + HexU32(node);
            break;
        }

        if (node_count != 0) {
            nodes_json << ",";
        }
        nodes_json << "{";
        nodes_json << "\"index\":" << node_count;
        nodes_json << ",\"node\":\"" << HexU32(node) << "\"";

        for (const auto& field : spec.fields) {
            const auto field_address = offset_address(node, field.offset);
            std::uint64_t field_value = 0;
            nodes_json << ",\"" << JsonEscape(field.name) << "_address\":\"" << HexU32(field_address) << "\"";
            if (!read_memory(field_address, field.width, field_value)) {
                read_ok = false;
                if (error.empty()) {
                    error = "failed to read field '" + field.name + "' at " + HexU32(field_address);
                }
                nodes_json << ",\"" << JsonEscape(field.name) << "_read_ok\":false";
            } else {
                nodes_json << ",\"" << JsonEscape(field.name) << "_read_ok\":true";
                nodes_json << ",\"" << JsonEscape(field.name) << "\":\"" << sample_value_hex(field_value, field.width) << "\"";
            }
        }

        const auto next_address = offset_address(node, spec.next_offset);
        std::uint64_t next_value = 0;
        nodes_json << ",\"_next_address\":\"" << HexU32(next_address) << "\"";
        if (!read_memory(next_address, SampleWidth::U32, next_value)) {
            read_ok = false;
            if (error.empty()) {
                error = "failed to read next at " + HexU32(next_address);
            }
            nodes_json << ",\"_next_read_ok\":false";
            nodes_json << "}";
            ++node_count;
            node = 0;
            break;
        }

        const auto next_node = static_cast<std::uint32_t>(next_value);
        nodes_json << ",\"_next_read_ok\":true";
        nodes_json << ",\"_next\":\"" << HexU32(next_node) << "\"";
        nodes_json << "}";
        ++node_count;
        node = next_node;
    }

    if (node != 0 && node_count >= spec.max_nodes) {
        truncated = true;
        if (error.empty()) {
            error = "max nodes reached before null";
        }
    }

    nodes_json << "]";

    fields.push_back(CaptureField{ spec.name + "_read_ok", read_ok ? "true" : "false", false });
    fields.push_back(CaptureField{
        spec.name + "_first_node",
        "\"" + HexU32(static_cast<std::uint32_t>(first_node_value)) + "\"",
        false });
    fields.push_back(CaptureField{ spec.name + "_node_count", std::to_string(node_count), false });
    fields.push_back(CaptureField{ spec.name + "_truncated", truncated ? "true" : "false", false });
    fields.push_back(CaptureField{ spec.name + "_cycle_detected", cycle_detected ? "true" : "false", false });
    fields.push_back(CaptureField{ spec.name + "_error", error, true });
    fields.push_back(CaptureField{ spec.name + "_nodes", nodes_json.str(), false });
    return fields;
}

} // namespace savor::capture
