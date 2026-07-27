#include "CaptureProfileJson.h"

#include "Core/Memory/Soa/SoaAddrProgramBuilder.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <picojson.h>

namespace savor::predict {
namespace {

using JsonArray = picojson::value::array;
using JsonObject = picojson::value::object;

struct BuilderSection {
    std::string name;
    std::map<std::string, std::string> values;
};

std::string trim(std::string value)
{
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    return first < last ? std::string(first, last) : std::string{};
}

std::vector<std::string> split(std::string_view text, char delimiter)
{
    std::vector<std::string> values;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const auto end = text.find(delimiter, begin);
        auto value = trim(std::string(text.substr(
            begin, end == std::string_view::npos ? text.size() - begin : end - begin)));
        if (!value.empty())
            values.push_back(std::move(value));
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }
    return values;
}

std::vector<BuilderSection> parse_sections(std::string_view text)
{
    std::vector<BuilderSection> sections;
    std::istringstream input{ std::string(text) };
    std::string line;
    BuilderSection* current = nullptr;
    while (std::getline(input, line)) {
        line = trim(std::move(line));
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        if (line.front() == '[' && line.back() == ']') {
            sections.push_back({ line.substr(1, line.size() - 2), {} });
            current = &sections.back();
            continue;
        }
        if (!current)
            throw std::runtime_error("capture profile builder text has data before a section");
        const auto equals = line.find('=');
        if (equals == std::string::npos)
            throw std::runtime_error("capture profile builder line is missing '=': " + line);
        current->values[trim(line.substr(0, equals))] = trim(line.substr(equals + 1));
    }
    return sections;
}

const std::string& required(const BuilderSection& section, std::string_view key)
{
    const auto found = section.values.find(std::string(key));
    if (found == section.values.end() || found->second.empty())
        throw std::runtime_error(section.name + " requires " + std::string(key));
    return found->second;
}

std::string optional(const BuilderSection& section, std::string_view key)
{
    const auto found = section.values.find(std::string(key));
    return found == section.values.end() ? std::string{} : found->second;
}

std::uint64_t parse_unsigned(std::string text)
{
    text = trim(std::move(text));
    int base = 10;
    std::string_view digits = text;
    if (digits.starts_with("0x") || digits.starts_with("0X")) {
        digits.remove_prefix(2);
        base = 16;
    }
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(
        digits.data(), digits.data() + digits.size(), value, base);
    if (error != std::errc{} || end != digits.data() + digits.size())
        throw std::runtime_error("invalid unsigned capture profile value: " + text);
    return value;
}

std::int64_t parse_signed(std::string text)
{
    text = trim(std::move(text));
    bool negative = false;
    if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
        negative = text.front() == '-';
        text.erase(text.begin());
    }
    const auto value = parse_unsigned(text);
    return negative ? -static_cast<std::int64_t>(value) : static_cast<std::int64_t>(value);
}

bool parse_bool(std::string value)
{
    std::ranges::transform(value, value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value == "1" || value == "true" || value == "yes";
}

std::uint32_t parse_width(std::string value)
{
    std::ranges::transform(value, value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (value == "u8" || value == "1" || value == "8bit") return 1;
    if (value == "u16" || value == "2" || value == "16") return 2;
    if (value == "u32" || value == "4" || value == "32") return 4;
    if (value == "u64" || value == "8" || value == "64") return 8;
    throw std::runtime_error("invalid capture sample width: " + value);
}

std::uint32_t parse_register(std::string value)
{
    value = trim(std::move(value));
    if (!value.empty() && (value.front() == 'r' || value.front() == 'R'))
        value.erase(value.begin());
    const auto reg = parse_unsigned(value);
    if (reg > 31)
        throw std::runtime_error("invalid capture register: " + value);
    return static_cast<std::uint32_t>(reg);
}

picojson::value json_u64(std::uint64_t value)
{
    return picojson::value(static_cast<double>(value));
}

picojson::value json_i64(std::int64_t value)
{
    return picojson::value(static_cast<double>(value));
}

JsonArray byte_array(const std::vector<std::uint8_t>& bytes)
{
    JsonArray result;
    result.reserve(bytes.size());
    for (const auto byte : bytes)
        result.emplace_back(static_cast<double>(byte));
    return result;
}

std::optional<addr::AddrKey> parse_addr_key(std::string value)
{
    if (value.starts_with("key:") || value.starts_with("KEY:"))
        value = value.substr(4);
    try {
        const auto raw = parse_unsigned(value);
        if (raw <= UINT16_MAX) {
            const auto key = static_cast<addr::AddrKey>(raw);
            if (addr::AddrRegistry::exists(key))
                return key;
        }
    } catch (const std::runtime_error&) {
    }
    std::ranges::transform(value, value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    for (const auto& record : addr::AddrRegistry::all()) {
        std::string name = record.name;
        std::ranges::transform(name, name.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (name == value)
            return record.key;
    }
    return std::nullopt;
}

std::vector<std::uint8_t> parse_address_program(std::string expression)
{
    expression = trim(std::move(expression));
    if (expression.empty())
        throw std::runtime_error("empty capture address program");

    addrprog::Builder builder;
    std::string base;
    std::string steps;
    if (expression.starts_with("key:") || expression.starts_with("KEY:")) {
        const auto separator = expression.find(':', 4);
        base = separator == std::string::npos ? expression : expression.substr(0, separator);
        if (separator != std::string::npos)
            steps = expression.substr(separator + 1);
    } else {
        const auto separator = expression.find(':');
        base = separator == std::string::npos ? expression : expression.substr(0, separator);
        if (separator != std::string::npos)
            steps = expression.substr(separator + 1);
    }
    if (!base.empty() && (base.front() == 'r' || base.front() == 'R')) {
        builder.op_base_gpr(static_cast<std::uint8_t>(parse_register(base)));
    } else if (base.starts_with("key:") || base.starts_with("KEY:")) {
        const auto key = parse_addr_key(base);
        if (!key)
            throw std::runtime_error("unknown capture address key: " + base);
        builder.op_base_key(*key);
    } else {
        builder.op_base_abs(static_cast<std::uint32_t>(parse_unsigned(base)));
    }

    for (const auto& step : split(steps, '|')) {
        if (step == "load_ptr32" || step == "deref32") {
            builder.op_load_ptr32();
        } else if (!step.empty() && (step.front() == '+' || step.front() == '-')) {
            builder.op_add_i32(static_cast<std::int32_t>(parse_signed(step)));
        } else if (step.starts_with("field:")) {
            builder.op_field(static_cast<std::uint32_t>(parse_unsigned(step.substr(6))));
        } else if (step.starts_with("index:")) {
            const auto values = split(step, ':');
            if (values.size() != 3)
                throw std::runtime_error("address index must be index:count:stride");
            builder.op_index(
                static_cast<std::uint16_t>(parse_unsigned(values[1])),
                static_cast<std::uint16_t>(parse_unsigned(values[2])));
        } else {
            throw std::runtime_error("unknown capture address-program step: " + step);
        }
    }
    builder.op_end();
    return builder.blob();
}

JsonObject memory_sample(const std::string& token)
{
    const auto values = split(token, ':');
    if (values.size() != 3)
        throw std::runtime_error("memory sample must be name:address:width: " + token);
    return {
        { "name", picojson::value(values[0]) },
        { "type", picojson::value("memory") },
        { "address", json_u64(parse_unsigned(values[1])) },
        { "width", json_u64(parse_width(values[2])) },
    };
}

JsonObject gpr_sample(const std::string& token)
{
    const auto values = split(token, ':');
    if (values.size() != 2)
        throw std::runtime_error("GPR sample must be name:register: " + token);
    return {
        { "name", picojson::value(values[0]) },
        { "type", picojson::value("gpr") },
        { "register", json_u64(parse_register(values[1])) },
        { "width", json_u64(4) },
    };
}

JsonObject register_memory_sample(const std::string& token)
{
    const auto values = split(token, ':');
    if (values.size() != 4)
        throw std::runtime_error("register-memory sample must be name:register:offset:width: " + token);
    return {
        { "name", picojson::value(values[0]) },
        { "type", picojson::value("register_memory") },
        { "register", json_u64(parse_register(values[1])) },
        { "offset", json_i64(parse_signed(values[2])) },
        { "width", json_u64(parse_width(values[3])) },
    };
}

JsonObject address_program_sample(const std::string& token)
{
    const auto first = token.find(':');
    const auto last = token.rfind(':');
    if (first == std::string::npos || first == last)
        throw std::runtime_error("address-program sample is malformed: " + token);
    const auto name = token.substr(0, first);
    const auto expression = token.substr(first + 1, last - first - 1);
    return {
        { "name", picojson::value(name) },
        { "type", picojson::value("address_program") },
        { "program", picojson::value(byte_array(parse_address_program(expression))) },
        { "width", json_u64(parse_width(token.substr(last + 1))) },
    };
}

JsonObject linked_list_sample(const std::string& token)
{
    const auto colon = token.find(':');
    if (colon == std::string::npos)
        throw std::runtime_error("linked-list sample is malformed: " + token);
    JsonObject sample{
        { "name", picojson::value(token.substr(0, colon)) },
        { "type", picojson::value("linked_list") },
        { "width", json_u64(4) },
    };
    JsonArray fields;
    for (const auto& option : split(token.substr(colon + 1), ',')) {
        const auto equals = option.find('=');
        if (equals == std::string::npos)
            throw std::runtime_error("linked-list option is malformed: " + option);
        const auto key = option.substr(0, equals);
        const auto value = option.substr(equals + 1);
        if (key == "head_ptr") sample["address"] = json_u64(parse_unsigned(value));
        else if (key == "next") sample["next_offset"] = json_i64(parse_signed(value));
        else if (key == "max") sample["max_nodes"] = json_u64(parse_unsigned(value));
        else if (key == "fields") {
            for (const auto& field : split(value, '|')) {
                const auto at = field.find('@');
                const auto width_colon = field.rfind(':');
                if (at == std::string::npos || width_colon == std::string::npos || at > width_colon)
                    throw std::runtime_error("linked-list field is malformed: " + field);
                fields.emplace_back(JsonObject{
                    { "name", picojson::value(field.substr(0, at)) },
                    { "offset", json_i64(parse_signed(field.substr(at + 1, width_colon - at - 1))) },
                    { "width", json_u64(parse_width(field.substr(width_colon + 1))) },
                });
            }
        }
    }
    sample["fields"] = picojson::value(std::move(fields));
    return sample;
}

void append_samples(JsonArray& output, const BuilderSection& section)
{
    if (const auto text = optional(section, "memory"); !text.empty())
        for (const auto& token : split(text, ',')) output.emplace_back(memory_sample(token));
    if (const auto text = optional(section, "gprs"); !text.empty())
        for (const auto& token : split(text, ',')) output.emplace_back(gpr_sample(token));
    if (const auto text = optional(section, "reg_memory"); !text.empty())
        for (const auto& token : split(text, ',')) output.emplace_back(register_memory_sample(token));
    if (const auto text = optional(section, "addrprog"); !text.empty())
        for (const auto& token : split(text, ',')) output.emplace_back(address_program_sample(token));
    if (const auto text = optional(section, "linked_list"); !text.empty())
        for (const auto& token : split(text, ';')) output.emplace_back(linked_list_sample(token));
}

JsonArray subscriptions()
{
    return JsonArray{ picojson::value("capture") };
}

JsonObject stack_sample()
{
    return {
        { "name", picojson::value("call_stack") },
        { "type", picojson::value("stack_trace") },
        { "max_frames", json_u64(8) },
    };
}

void add_stack_for_rng(JsonArray& samples, bool owns_rng_draw)
{
    if (owns_rng_draw)
        samples.emplace_back(stack_sample());
}

JsonObject checkpoint_probe(const BuilderSection& section, const BuilderSection& defaults)
{
    if (section.values.contains("address_program_trace"))
        throw std::runtime_error("address_program_trace was removed; use per-sample trace policy");
    const auto owns_rng = parse_bool(optional(section, "owns_rng_draw"));
    const auto pc = parse_unsigned(required(section, "pc"));
    JsonArray samples;
    append_samples(samples, defaults);
    append_samples(samples, section);
    add_stack_for_rng(samples, owns_rng);
    JsonObject probe{
        { "id", picojson::value(section.name.substr(std::string("checkpoint.").size())) },
        { "group", picojson::value("capture_profile") },
        { "kind", picojson::value("pc") },
        { "address", json_u64(pc) },
        { "subscriptions", picojson::value(subscriptions()) },
        { "owns_rng_draw", picojson::value(owns_rng) },
        { "samples", picojson::value(std::move(samples)) },
    };
    if (const auto value = optional(section, "activate_on_pc"); !value.empty())
        probe["activate_on_pc"] = json_u64(parse_unsigned(value));
    if (const auto value = optional(section, "max_hits"); !value.empty())
        probe["max_hits"] = json_u64(parse_unsigned(value));
    if (pc == 0x8000A388u)
        probe["frame_clock"] = picojson::value(true);
    JsonObject symbol;
    for (const auto key : { "name", "function", "checkpoint" })
        if (const auto value = optional(section, key); !value.empty()) symbol[key] = picojson::value(value);
    probe["symbol"] = picojson::value(std::move(symbol));
    return probe;
}

JsonObject memory_probe(
    const BuilderSection& section,
    const BuilderSection& defaults,
    bool dynamic,
    bool& uses_macro_window)
{
    if (section.values.contains("address_program_trace"))
        throw std::runtime_error("address_program_trace was removed; use per-root trace policy");
    const auto prefix = dynamic ? std::string("dynamic_watchpoint.") : std::string("watchpoint.");
    const auto owns_rng = parse_bool(optional(section, "owns_rng_draw"));
    JsonArray samples;
    append_samples(samples, defaults);
    add_stack_for_rng(samples, owns_rng);
    JsonObject probe{
        { "id", picojson::value(section.name.substr(prefix.size())) },
        { "group", picojson::value("capture_profile") },
        { "kind", picojson::value("memory") },
        { "size", json_u64(parse_width(required(section, "size"))) },
        { "access", picojson::value(required(section, "access")) },
        { "subscriptions", picojson::value(subscriptions()) },
        { "owns_rng_draw", picojson::value(owns_rng) },
        { "samples", picojson::value(std::move(samples)) },
    };
    if (!dynamic) {
        probe["address"] = json_u64(parse_unsigned(required(section, "address")));
    } else {
        probe["activate_on_pc"] = json_u64(parse_unsigned(required(section, "pc")));
        if (const auto address = optional(section, "address"); !address.empty()) {
            probe["address"] = json_u64(parse_unsigned(address));
        } else if (const auto expression = optional(section, "addrprog"); !expression.empty()) {
            probe["address_program"] = picojson::value(byte_array(parse_address_program(expression)));
        } else {
            const auto reg = parse_register(required(section, "base_gpr"));
            const auto offset = parse_signed(required(section, "offset"));
            addrprog::Builder builder;
            builder.op_base_gpr(static_cast<std::uint8_t>(reg));
            builder.op_add_i32(static_cast<std::int32_t>(offset));
            builder.op_end();
            probe["address_program"] = picojson::value(byte_array(builder.blob()));
        }
        if (parse_bool(optional(section, "one_shot")))
            probe["one_shot"] = picojson::value(true);
    }
    if (const auto scope = optional(section, "scope"); scope == "input_macro") {
        probe["window"] = picojson::value("input_macro");
        uses_macro_window = true;
    }
    return probe;
}

} // namespace

std::string build_capture_profile_json(std::string_view builder_text)
{
    const auto sections = parse_sections(builder_text);
    const auto profile_it = std::ranges::find_if(sections, [](const auto& section) {
        return section.name == "profile";
    });
    if (profile_it == sections.end())
        throw std::runtime_error("capture profile builder text has no profile section");

    BuilderSection defaults{ "defaults", {} };
    for (const auto key : { "memory", "gprs", "reg_memory", "addrprog", "linked_list" }) {
        if (const auto value = optional(*profile_it, key); !value.empty())
            defaults.values[key] = value;
    }

    JsonArray probes;
    bool uses_macro_window = false;
    for (const auto& section : sections) {
        if (section.name.starts_with("checkpoint."))
            probes.emplace_back(checkpoint_probe(section, defaults));
        else if (section.name.starts_with("watchpoint."))
            probes.emplace_back(memory_probe(section, defaults, false, uses_macro_window));
        else if (section.name.starts_with("dynamic_watchpoint."))
            probes.emplace_back(memory_probe(section, defaults, true, uses_macro_window));
    }

    JsonObject root{
        { "schema", picojson::value("savor.capture.profile/1") },
        { "name", picojson::value(required(*profile_it, "name")) },
        { "revision", json_u64(1) },
        { "limits", picojson::value(JsonObject{
            { "queue_bytes", json_u64(64ull * 1024ull * 1024ull) },
            { "max_events", json_u64(2048) },
            { "progress_events", json_u64(256) },
        }) },
        { "probes", picojson::value(std::move(probes)) },
    };
    if (uses_macro_window) {
        root["windows"] = picojson::value(JsonArray{ picojson::value(JsonObject{
            { "id", picojson::value("input_macro") },
            { "open_marker", picojson::value("macro.begin") },
            { "close_marker", picojson::value("macro.end") },
            { "initially_open", picojson::value(false) },
        }) });
    }
    return picojson::value(std::move(root)).serialize(true);
}

std::string pin_capture_profile_module_hash(
    std::string_view profile_json,
    std::string_view module_sha256)
{
    if (module_sha256.size() != 64)
        throw std::runtime_error("capture profile module SHA-256 must contain 64 hex characters");

    picojson::value root;
    auto begin = profile_json.begin();
    const auto parse_error = picojson::parse(root, begin, profile_json.end());
    if (!parse_error.empty() || !root.is<picojson::object>())
        throw std::runtime_error("capture profile is not a valid JSON object: " + parse_error);

    auto& object = root.get<picojson::object>();
    const auto schema = object.find("schema");
    if (schema == object.end() || !schema->second.is<std::string>()
        || schema->second.get<std::string>() != "savor.capture.profile/1") {
        throw std::runtime_error("capture profile does not use savor.capture.profile/1");
    }
    object["expected_module_sha256"] = picojson::value(std::string(module_sha256));
    return root.serialize(true);
}

} // namespace savor::predict
