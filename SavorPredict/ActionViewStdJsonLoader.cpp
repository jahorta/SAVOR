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

const char* spice_std0_json_loader_rule_detail() {
    return "Loads SPICE spice_std_ir_v1 entry_table JSON into predictor Std0Table records. "
           "Only entryTable.records fields used by STD::CountMatchingStd0Entries are imported: "
           "locationCode, opcode, payloadInBounds, and payloadBytesHex gate fields at "
           "payload offsets 0x00, 0x02, and 0x04. For action_rows JSON, the loader can "
           "also import the first action row as the runtime aux-table prefix observed "
           "before companion _0_STD rows.";
}

} // namespace savor::predict
