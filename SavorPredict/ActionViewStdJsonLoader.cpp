#include "ActionViewStdJsonLoader.h"

#include <charconv>
#include <cctype>
#include <fstream>
#include <sstream>

namespace savor::predict {
namespace {

std::string to_string(std::string_view value) {
    return std::string(value.begin(), value.end());
}

void add_error(SpiceStd0JsonLoadResult& result, std::string text) {
    result.errors.push_back(std::move(text));
}

void add_error(SpiceStdActionRowPrefixLoadResult& result, std::string text) {
    result.errors.push_back(std::move(text));
}

void add_error(SpiceStdVisualJsonLoadResult& result, std::string text) {
    result.errors.push_back(std::move(text));
}

void add_error(SpiceStdActionRowsLoadResult& result, std::string text) {
    result.errors.push_back(std::move(text));
}

bool contains_literal(std::string_view text, std::string_view literal) {
    return text.find(literal) != std::string_view::npos;
}

std::optional<std::string_view> find_records_array(std::string_view json_text) {
    const auto entry_table = json_text.find("\"entryTable\"");
    if (entry_table == std::string_view::npos) {
        return std::nullopt;
    }
    const auto records_key = json_text.find("\"records\"", entry_table);
    if (records_key == std::string_view::npos) {
        return std::nullopt;
    }
    const auto array_start = json_text.find('[', records_key);
    if (array_start == std::string_view::npos) {
        return std::nullopt;
    }

    bool in_string = false;
    bool escaped = false;
    int depth = 0;
    for (std::size_t i = array_start; i < json_text.size(); ++i) {
        const char c = json_text[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (in_string) {
            if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
            continue;
        }
        if (c == '[') {
            ++depth;
        } else if (c == ']') {
            --depth;
            if (depth == 0) {
                return json_text.substr(array_start + 1, i - array_start - 1);
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string_view> find_action_rows_array(std::string_view json_text) {
    const auto action_rows = json_text.find("\"actionRows\"");
    if (action_rows == std::string_view::npos) {
        return std::nullopt;
    }
    const auto rows_key = json_text.find("\"rows\"", action_rows);
    if (rows_key == std::string_view::npos) {
        return std::nullopt;
    }
    const auto array_start = json_text.find('[', rows_key);
    if (array_start == std::string_view::npos) {
        return std::nullopt;
    }

    bool in_string = false;
    bool escaped = false;
    int depth = 0;
    for (std::size_t i = array_start; i < json_text.size(); ++i) {
        const char c = json_text[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (in_string) {
            if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
            continue;
        }
        if (c == '[') {
            ++depth;
        } else if (c == ']') {
            --depth;
            if (depth == 0) {
                return json_text.substr(array_start + 1, i - array_start - 1);
            }
        }
    }
    return std::nullopt;
}

std::vector<std::string_view> split_top_level_objects(std::string_view array_text) {
    std::vector<std::string_view> objects;
    bool in_string = false;
    bool escaped = false;
    int depth = 0;
    std::size_t object_start = std::string_view::npos;

    for (std::size_t i = 0; i < array_text.size(); ++i) {
        const char c = array_text[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (in_string) {
            if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
            continue;
        }
        if (c == '{') {
            if (depth == 0) {
                object_start = i;
            }
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0 && object_start != std::string_view::npos) {
                objects.push_back(array_text.substr(object_start, i - object_start + 1));
                object_start = std::string_view::npos;
            }
        }
    }
    return objects;
}

std::optional<std::string_view> find_value_span(std::string_view object, std::string_view key) {
    const auto key_pos = object.find(key);
    if (key_pos == std::string_view::npos) {
        return std::nullopt;
    }
    const auto colon = object.find(':', key_pos + key.size());
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    auto begin = colon + 1;
    while (begin < object.size() && std::isspace(static_cast<unsigned char>(object[begin]))) {
        ++begin;
    }
    if (begin >= object.size()) {
        return std::nullopt;
    }
    if (object[begin] == '"') {
        bool escaped = false;
        for (auto end = begin + 1; end < object.size(); ++end) {
            if (escaped) {
                escaped = false;
                continue;
            }
            if (object[end] == '\\') {
                escaped = true;
                continue;
            }
            if (object[end] == '"') {
                return object.substr(begin, end - begin + 1);
            }
        }
        return std::nullopt;
    }

    auto end = begin;
    while (end < object.size() && object[end] != ',' && object[end] != '}') {
        ++end;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(object[end - 1]))) {
        --end;
    }
    return object.substr(begin, end - begin);
}

std::optional<int> parse_json_int(std::string_view object, std::string_view key) {
    const auto value = find_value_span(object, key);
    if (!value.has_value()) {
        return std::nullopt;
    }
    int parsed = 0;
    const auto begin = value->data();
    const auto end = value->data() + value->size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed, 10);
    if (ec != std::errc() || ptr != end) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<std::uint32_t> parse_json_u32(
    std::string_view object,
    std::string_view key) {
    const auto value = find_value_span(object, key);
    if (!value.has_value()) {
        return std::nullopt;
    }
    std::uint32_t parsed = 0;
    const auto begin = value->data();
    const auto end = value->data() + value->size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed, 10);
    if (ec != std::errc() || ptr != end) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<bool> parse_json_bool(std::string_view object, std::string_view key) {
    const auto value = find_value_span(object, key);
    if (!value.has_value()) {
        return std::nullopt;
    }
    if (*value == "true") {
        return true;
    }
    if (*value == "false") {
        return false;
    }
    return std::nullopt;
}

std::optional<std::string> parse_json_string(std::string_view object, std::string_view key) {
    const auto value = find_value_span(object, key);
    if (!value.has_value() || value->size() < 2 || value->front() != '"' || value->back() != '"') {
        return std::nullopt;
    }
    std::string out;
    out.reserve(value->size() - 2);
    bool escaped = false;
    for (std::size_t i = 1; i + 1 < value->size(); ++i) {
        const char c = (*value)[i];
        if (escaped) {
            out.push_back(c);
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::optional<std::uint8_t> payload_byte(std::string_view hex, std::size_t byte_offset) {
    const auto pos = byte_offset * 2;
    if (pos + 1 >= hex.size()) {
        return std::nullopt;
    }
    const auto high = hex_nibble(hex[pos]);
    const auto low = hex_nibble(hex[pos + 1]);
    if (high < 0 || low < 0) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>((high << 4) | low);
}

std::optional<std::int16_t> payload_s16_be(std::string_view hex, std::size_t byte_offset) {
    const auto high = payload_byte(hex, byte_offset);
    const auto low = payload_byte(hex, byte_offset + 1);
    if (!high.has_value() || !low.has_value()) {
        return std::nullopt;
    }
    const auto combined = static_cast<std::uint16_t>((static_cast<std::uint16_t>(*high) << 8U) | *low);
    return static_cast<std::int16_t>(combined);
}

std::optional<std::uint16_t> payload_u16_be(
    const std::vector<std::uint8_t>& payload,
    std::size_t offset) {
    if (offset + 1 >= payload.size()) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(payload[offset]) << 8U)
        | payload[offset + 1]);
}

std::optional<std::int16_t> payload_s16_be(
    const std::vector<std::uint8_t>& payload,
    std::size_t offset) {
    const auto value = payload_u16_be(payload, offset);
    return value.has_value()
        ? std::optional<std::int16_t>{static_cast<std::int16_t>(*value)}
        : std::nullopt;
}

std::optional<std::uint32_t> payload_u32_be(
    const std::vector<std::uint8_t>& payload,
    std::size_t offset) {
    if (offset + 3 >= payload.size()) {
        return std::nullopt;
    }
    return (static_cast<std::uint32_t>(payload[offset]) << 24U)
        | (static_cast<std::uint32_t>(payload[offset + 1]) << 16U)
        | (static_cast<std::uint32_t>(payload[offset + 2]) << 8U)
        | static_cast<std::uint32_t>(payload[offset + 3]);
}

std::optional<std::vector<std::uint8_t>> decode_payload_hex(std::string_view hex) {
    if ((hex.size() % 2U) != 0U) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(hex.size() / 2U);
    for (std::size_t offset = 0; offset < hex.size(); offset += 2U) {
        const auto high = hex_nibble(hex[offset]);
        const auto low = hex_nibble(hex[offset + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return bytes;
}

CombatantVisualCommandKind visual_command_kind(std::uint32_t combined_type) {
    if (combined_type == 0x00030003U) {
        return CombatantVisualCommandKind::PutModel;
    }
    if (combined_type == 0x00030004U) {
        return CombatantVisualCommandKind::SetCommand;
    }
    if (combined_type == 0x0003000AU) {
        return CombatantVisualCommandKind::MotionPause;
    }
    if (combined_type == 0x0003000BU) {
        return CombatantVisualCommandKind::CollisionBox;
    }
    if (combined_type == 0x0003000CU) {
        return CombatantVisualCommandKind::MoveModel;
    }
    if (combined_type == 0x0003000DU) {
        return CombatantVisualCommandKind::HitWeapon;
    }
    if (combined_type == 0x0003001DU) {
        return CombatantVisualCommandKind::PointLight;
    }
    if (combined_type == 0x0003002AU) {
        return CombatantVisualCommandKind::SystemCamera;
    }
    return CombatantVisualCommandKind::Unknown;
}

std::optional<CombatantVisualCommandRecord> import_visual_record(
    std::string_view object,
    SpiceStdVisualJsonLoadResult& result,
    int fallback_index) {
    const auto index = parse_json_int(object, "\"index\"").value_or(fallback_index);
    const auto location_code = parse_json_int(object, "\"locationCode\"");
    const auto opcode = parse_json_int(object, "\"opcode\"");
    if (!location_code.has_value() || !opcode.has_value()) {
        add_error(result, "record " + std::to_string(index) + " missing locationCode/opcode");
        return std::nullopt;
    }

    CombatantVisualCommandRecord record;
    record.index = index;
    record.location_code = static_cast<std::int16_t>(*location_code);
    record.opcode = static_cast<std::int16_t>(*opcode);
    record.combined_type = std0_combined_entry_id(record.location_code, record.opcode);
    record.kind = visual_command_kind(record.combined_type);
    record.payload_size = parse_json_int(object, "\"payloadSize\"").value_or(0);
    record.payload_in_bounds = parse_json_bool(object, "\"payloadInBounds\"").value_or(false);

    const auto payload_hex = parse_json_string(object, "\"payloadBytesHex\"");
    if (payload_hex.has_value()) {
        const auto decoded = decode_payload_hex(*payload_hex);
        if (!decoded.has_value()) {
            add_error(result, "record " + std::to_string(index) + " has invalid payloadBytesHex");
            return std::nullopt;
        }
        record.payload_bytes = *decoded;
    }

    if (record.location_code < 0) {
        return record;
    }
    if (!record.payload_in_bounds || record.payload_bytes.size() < 6U) {
        return record;
    }

    const auto primary = payload_s16_be(record.payload_bytes, 0);
    const auto generic_secondary = payload_s16_be(record.payload_bytes, 2);
    const auto direct_secondary = payload_s16_be(record.payload_bytes, 4);
    if (!primary.has_value() || !generic_secondary.has_value() || !direct_secondary.has_value()) {
        return record;
    }
    record.gate_fields_known = true;
    record.gate_fields.primary_action_key = *primary;
    record.gate_fields.generic_secondary_key = *generic_secondary;
    record.gate_fields.direct_gate_secondary_key = *direct_secondary;
    record.synchronization_gate = payload_s16_be(record.payload_bytes, 6).value_or(0);

    if (record.kind == CombatantVisualCommandKind::SetCommand) {
        const auto service_flags = payload_u32_be(record.payload_bytes, 0x10);
        const auto delay = payload_s16_be(record.payload_bytes, 0x14);
        const auto forced_mode = payload_s16_be(record.payload_bytes, 0x16);
        if (record.payload_bytes.size() < 0x1cU
            || !service_flags.has_value() || !delay.has_value() || !forced_mode.has_value()) {
            add_error(result, "SET COMMAND record " + std::to_string(index) + " has a short payload");
            return std::nullopt;
        }
        record.set_command = CombatantVisualSetCommandPayload{
            .command_mode = *generic_secondary,
            .command_subtype = *direct_secondary,
            .synchronization_flags = record.synchronization_gate,
            .service_flags = *service_flags,
            .delay = *delay,
            .forced_mode = *forced_mode,
        };
        ++result.visual_records_decoded;
    } else if (record.kind == CombatantVisualCommandKind::CollisionBox) {
        const auto behavior_flags = payload_u32_be(record.payload_bytes, 0x10);
        const auto start_counter = payload_s16_be(record.payload_bytes, 0x14);
        const auto end_counter = payload_s16_be(record.payload_bytes, 0x16);
        const auto object_id = payload_s16_be(record.payload_bytes, 0x18);
        const auto current_x_bits = payload_u32_be(record.payload_bytes, 0x1c);
        const auto current_y_bits = payload_u32_be(record.payload_bytes, 0x20);
        const auto current_z_bits = payload_u32_be(record.payload_bytes, 0x24);
        const auto velocity_x_bits = payload_u32_be(record.payload_bytes, 0x28);
        const auto velocity_y_bits = payload_u32_be(record.payload_bytes, 0x2c);
        const auto velocity_z_bits = payload_u32_be(record.payload_bytes, 0x30);
        const auto trailing_flags = payload_u32_be(record.payload_bytes, 0x34);
        if (record.payload_bytes.size() < 0x38U
            || !behavior_flags.has_value() || !start_counter.has_value()
            || !end_counter.has_value() || !object_id.has_value()
            || !current_x_bits.has_value() || !current_y_bits.has_value()
            || !current_z_bits.has_value() || !velocity_x_bits.has_value()
            || !velocity_y_bits.has_value() || !velocity_z_bits.has_value()
            || !trailing_flags.has_value()) {
            add_error(result, "COLLISION BOX record " + std::to_string(index) + " has a short payload");
            return std::nullopt;
        }
        record.collision_box = CombatantVisualCollisionBoxPayload{
            .behavior_flags = *behavior_flags,
            .start_counter = *start_counter,
            .end_counter = *end_counter,
            .object_id = *object_id,
            .current_x_bits = *current_x_bits,
            .current_y_bits = *current_y_bits,
            .current_z_bits = *current_z_bits,
            .velocity_x_bits = *velocity_x_bits,
            .velocity_y_bits = *velocity_y_bits,
            .velocity_z_bits = *velocity_z_bits,
            .trailing_flags = *trailing_flags,
        };
        ++result.visual_records_decoded;
    } else if (record.kind == CombatantVisualCommandKind::SystemCamera) {
        const auto flags = payload_u32_be(record.payload_bytes, 0x10);
        const auto scalar_bits = payload_u32_be(record.payload_bytes, 0x14);
        const auto start_frame = payload_u32_be(record.payload_bytes, 0x18);
        const auto end_frame = payload_u16_be(record.payload_bytes, 0x1c);
        const auto hold_frames = payload_u16_be(record.payload_bytes, 0x1e);
        const auto step_frames = payload_u16_be(record.payload_bytes, 0x20);
        const auto mode = payload_s16_be(record.payload_bytes, 0x22);
        if (record.payload_bytes.size() < 0x24U
            || !flags.has_value() || !scalar_bits.has_value()
            || !start_frame.has_value() || !end_frame.has_value()
            || !hold_frames.has_value() || !step_frames.has_value()
            || !mode.has_value()) {
            add_error(result, "SYSTEM CAMERA record " + std::to_string(index) + " has a short payload");
            return std::nullopt;
        }
        record.system_camera = CombatantVisualSystemCameraPayload{
            .flags = *flags,
            .scalar_bits = *scalar_bits,
            .start_frame = *start_frame,
            .end_frame = *end_frame,
            .hold_frames = *hold_frames,
            .step_frames = *step_frames,
            .mode = *mode,
        };
        ++result.visual_records_decoded;
    }
    return record;
}

std::optional<Std0EntryRecord> import_record(
    std::string_view object,
    SpiceStd0JsonLoadResult& result,
    int record_index) {
    const auto location_code = parse_json_int(object, "\"locationCode\"");
    if (!location_code.has_value()) {
        add_error(result, "record " + std::to_string(record_index) + " missing locationCode");
        return std::nullopt;
    }

    const auto opcode = parse_json_int(object, "\"opcode\"");
    if (!opcode.has_value()) {
        add_error(result, "record " + std::to_string(record_index) + " missing opcode");
        return std::nullopt;
    }

    Std0EntryRecord record;
    record.location_code = static_cast<std::int16_t>(*location_code);
    record.opcode = static_cast<std::int16_t>(*opcode);

    if (record.location_code < 0) {
        return record;
    }

    const auto payload_in_bounds = parse_json_bool(object, "\"payloadInBounds\"").value_or(false);
    const auto payload_hex = parse_json_string(object, "\"payloadBytesHex\"");
    if (!payload_in_bounds || !payload_hex.has_value() || payload_hex->size() < 12) {
        record.has_payload = false;
        return record;
    }

    const auto primary = payload_s16_be(*payload_hex, 0);
    const auto generic_secondary = payload_s16_be(*payload_hex, 2);
    const auto direct_secondary = payload_s16_be(*payload_hex, 4);
    if (!primary.has_value() || !generic_secondary.has_value() || !direct_secondary.has_value()) {
        add_error(result, "record " + std::to_string(record_index) + " has invalid payloadBytesHex gate fields");
        return std::nullopt;
    }

    record.has_payload = true;
    record.payload.primary_action_key = *primary;
    record.payload.generic_secondary_key = *generic_secondary;
    record.payload.direct_gate_secondary_key = *direct_secondary;
    return record;
}

std::optional<Std0EntryRecord> import_action_row_prefix_record(
    std::string_view object,
    SpiceStdActionRowPrefixLoadResult& result,
    int record_index) {
    const auto index = parse_json_int(object, "\"index\"");
    if (!index.has_value()) {
        add_error(result, "action row " + std::to_string(record_index) + " missing index");
        return std::nullopt;
    }
    const auto action_id = parse_json_int(object, "\"actionId\"");
    if (!action_id.has_value()) {
        add_error(result, "action row " + std::to_string(record_index) + " missing actionId");
        return std::nullopt;
    }
    const auto row_type = parse_json_int(object, "\"rowType\"");
    if (!row_type.has_value()) {
        add_error(result, "action row " + std::to_string(record_index) + " missing rowType");
        return std::nullopt;
    }
    const auto callback_index = parse_json_int(object, "\"callbackIndex\"");
    if (!callback_index.has_value()) {
        add_error(result, "action row " + std::to_string(record_index) + " missing callbackIndex");
        return std::nullopt;
    }

    Std0EntryRecord record;
    record.location_code = static_cast<std::int16_t>(*index);
    record.opcode = static_cast<std::int16_t>(*row_type);
    record.has_payload = true;
    record.payload.primary_action_key = static_cast<std::int16_t>(*action_id);
    record.payload.generic_secondary_key = static_cast<std::int16_t>(*row_type);
    record.payload.direct_gate_secondary_key = static_cast<std::int16_t>(*callback_index);
    return record;
}

std::optional<CombatantStdActionRow> import_action_row(
    std::string_view object,
    SpiceStdActionRowsLoadResult& result,
    int record_index) {
    const auto index = parse_json_int(object, "\"index\"");
    const auto action_id = parse_json_int(object, "\"actionId\"");
    const auto row_type = parse_json_int(object, "\"rowType\"");
    const auto callback_index = parse_json_int(object, "\"callbackIndex\"");
    const auto callback_ordinal = parse_json_int(object, "\"callbackOrdinal\"");
    const auto flags = parse_json_u32(object, "\"flags\"");
    const auto secondary_key = parse_json_int(object, "\"secondaryKey\"");
    const auto callback_aux = parse_json_int(object, "\"callbackAuxParam\"");
    const auto divisor = parse_json_u32(object, "\"transitionGateDivisorBits\"");
    const auto motion = parse_json_u32(object, "\"motionProgressStepBits\"");
    if (!index.has_value() || !action_id.has_value() || !row_type.has_value()
        || !callback_index.has_value() || !callback_ordinal.has_value()
        || !flags.has_value() || !secondary_key.has_value()
        || !callback_aux.has_value() || !divisor.has_value() || !motion.has_value()) {
        add_error(result, "action row " + std::to_string(record_index)
            + " is missing a required structured field");
        return std::nullopt;
    }
    return CombatantStdActionRow{
        .index = *index,
        .action_id = static_cast<std::int16_t>(*action_id),
        .row_type = static_cast<std::int16_t>(*row_type),
        .callback_index = static_cast<std::int16_t>(*callback_index),
        .callback_ordinal = static_cast<std::int16_t>(*callback_ordinal),
        .flags = *flags,
        .secondary_key = static_cast<std::int16_t>(*secondary_key),
        .callback_aux_param = static_cast<std::int16_t>(*callback_aux),
        .transition_gate_divisor_bits = *divisor,
        .motion_progress_step_bits = *motion,
    };
}

} // namespace

SpiceStd0JsonLoadResult load_spice_std0_table_from_json_text(std::string_view json_text) {
    SpiceStd0JsonLoadResult result;

    if (!contains_literal(json_text, "\"schema\": \"spice_std_ir_v1\"")) {
        add_error(result, "not a spice_std_ir_v1 JSON export");
    }
    if (!contains_literal(json_text, "\"layoutKind\": \"entry_table\"")) {
        add_error(result, "STD JSON is not an entry_table layout");
    }
    if (!contains_literal(json_text, "\"parseOk\": true")) {
        add_error(result, "STD JSON parseOk is not true");
    }

    const auto records_array = find_records_array(json_text);
    if (!records_array.has_value()) {
        add_error(result, "missing entryTable.records array");
        return result;
    }

    const auto objects = split_top_level_objects(*records_array);
    result.records_seen = static_cast<int>(objects.size());
    for (std::size_t i = 0; i < objects.size(); ++i) {
        const auto imported = import_record(objects[i], result, static_cast<int>(i));
        if (!imported.has_value()) {
            continue;
        }
        result.table.entries.push_back(*imported);
        ++result.records_imported;
        if (imported->location_code < 0) {
            result.table.includes_sentinel = true;
            break;
        }
    }

    if (result.table.entries.empty()) {
        add_error(result, "entryTable.records did not contain any importable records");
    }

    result.ok = result.errors.empty();
    return result;
}

SpiceStd0JsonLoadResult load_spice_std0_table_from_json_file(
    const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        SpiceStd0JsonLoadResult result;
        add_error(result, "failed to open STD JSON file: " + path.string());
        return result;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return load_spice_std0_table_from_json_text(buffer.str());
}

SpiceStdActionRowPrefixLoadResult load_spice_std_action_row_prefix_from_json_text(
    std::string_view json_text) {
    SpiceStdActionRowPrefixLoadResult result;

    if (!contains_literal(json_text, "\"schema\": \"spice_std_ir_v1\"")) {
        add_error(result, "not a spice_std_ir_v1 JSON export");
    }
    if (!contains_literal(json_text, "\"layoutKind\": \"action_rows\"")) {
        add_error(result, "STD JSON is not an action_rows layout");
    }
    if (!contains_literal(json_text, "\"parseOk\": true")) {
        add_error(result, "STD JSON parseOk is not true");
    }

    const auto rows_array = find_action_rows_array(json_text);
    if (!rows_array.has_value()) {
        add_error(result, "missing actionRows.rows array");
        return result;
    }

    const auto objects = split_top_level_objects(*rows_array);
    result.rows_seen = static_cast<int>(objects.size());
    if (objects.empty()) {
        add_error(result, "actionRows.rows did not contain any rows");
        return result;
    }

    const auto imported = import_action_row_prefix_record(objects.front(), result, 0);
    if (imported.has_value()) {
        result.table.entries.push_back(*imported);
        ++result.rows_imported;
    }

    result.ok = result.errors.empty() && result.rows_imported == 1;
    return result;
}

SpiceStdActionRowPrefixLoadResult load_spice_std_action_row_prefix_from_json_file(
    const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        SpiceStdActionRowPrefixLoadResult result;
        add_error(result, "failed to open STD JSON file: " + path.string());
        return result;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return load_spice_std_action_row_prefix_from_json_text(buffer.str());
}

SpiceStdVisualJsonLoadResult load_spice_std_visual_resource_from_json_text(
    std::string_view json_text) {
    SpiceStdVisualJsonLoadResult result;
    if (!contains_literal(json_text, "\"schema\": \"spice_std_ir_v1\"")) {
        add_error(result, "not a spice_std_ir_v1 JSON export");
    }
    if (!contains_literal(json_text, "\"layoutKind\": \"entry_table\"")) {
        add_error(result, "STD JSON is not an entry_table layout");
    }
    if (!contains_literal(json_text, "\"parseOk\": true")) {
        add_error(result, "STD JSON parseOk is not true");
    }

    const auto records_array = find_records_array(json_text);
    if (!records_array.has_value()) {
        add_error(result, "missing entryTable.records array");
        return result;
    }
    const auto objects = split_top_level_objects(*records_array);
    result.records_seen = static_cast<int>(objects.size());
    for (std::size_t i = 0; i < objects.size(); ++i) {
        const auto record = import_visual_record(objects[i], result, static_cast<int>(i));
        if (!record.has_value()) {
            continue;
        }
        result.resource.records.push_back(*record);
        ++result.records_imported;
        if (record->location_code < 0) {
            result.resource.includes_sentinel = true;
            break;
        }
    }
    result.resource.selector_table = combatant_visual_selector_table(result.resource);
    if (result.resource.records.empty()) {
        add_error(result, "entryTable.records did not contain any importable records");
    }
    result.ok = result.errors.empty();
    return result;
}

SpiceStdVisualJsonLoadResult load_spice_std_visual_resource_from_json_file(
    const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        SpiceStdVisualJsonLoadResult result;
        add_error(result, "failed to open STD JSON file: " + path.string());
        return result;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    auto result = load_spice_std_visual_resource_from_json_text(buffer.str());
    result.resource.provenance = path.string();
    return result;
}

SpiceStdActionRowsLoadResult load_spice_std_action_rows_from_json_text(
    std::string_view json_text) {
    SpiceStdActionRowsLoadResult result;
    if (!contains_literal(json_text, "\"schema\": \"spice_std_ir_v1\"")) {
        add_error(result, "not a spice_std_ir_v1 JSON export");
    }
    if (!contains_literal(json_text, "\"layoutKind\": \"action_rows\"")) {
        add_error(result, "STD JSON is not an action_rows layout");
    }
    if (!contains_literal(json_text, "\"parseOk\": true")) {
        add_error(result, "STD JSON parseOk is not true");
    }
    const auto rows_array = find_action_rows_array(json_text);
    if (!rows_array.has_value()) {
        add_error(result, "missing actionRows.rows array");
        return result;
    }
    const auto objects = split_top_level_objects(*rows_array);
    result.rows_seen = static_cast<int>(objects.size());
    for (std::size_t index = 0; index < objects.size(); ++index) {
        const auto row = import_action_row(objects[index], result, static_cast<int>(index));
        if (row.has_value()) {
            result.rows.push_back(*row);
            ++result.rows_imported;
        }
    }
    if (result.rows.empty()) {
        add_error(result, "actionRows.rows did not contain any complete rows");
    }
    result.ok = result.errors.empty();
    return result;
}

SpiceStdActionRowsLoadResult load_spice_std_action_rows_from_json_file(
    const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        SpiceStdActionRowsLoadResult result;
        add_error(result, "failed to open STD JSON file: " + path.string());
        return result;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return load_spice_std_action_rows_from_json_text(buffer.str());
}

const char* spice_std0_json_loader_rule_detail() {
    return "Loads SPICE spice_std_ir_v1 entry_table JSON into predictor Std0Table records. "
           "Only entryTable.records fields used by STD::CountMatchingStd0Entries are imported: "
           "locationCode, opcode, payloadInBounds, and payloadBytesHex gate fields at "
           "payload offsets 0x00, 0x02, and 0x04. For action_rows JSON, the loader can "
           "also import the first action row as the runtime aux-table prefix observed "
           "before companion _0_STD rows.";
}

} // namespace savor::predict
