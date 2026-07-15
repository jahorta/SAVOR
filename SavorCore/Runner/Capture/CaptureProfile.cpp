#include "CaptureProfile.h"

#include "../../Core/Memory/Soa/SoaAddrProgramBuilder.h"
#include "../../Utils/IniDoc.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_set>

namespace savor::capture {
namespace {

std::string trim(std::string value)
{
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    }).base();
    if (first >= last) return {};
    return std::string(first, last);
}

std::string to_lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool parse_u32(std::string value, std::uint32_t& out)
{
    value = trim(std::move(value));
    if (value.empty()) return false;

    int base = 10;
    if (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0) {
        value = value.substr(2);
        base = 16;
    }
    std::uint32_t parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed, base);
    if (ec != std::errc{} || ptr != end) return false;
    out = parsed;
    return true;
}

bool parse_i32(std::string value, std::int32_t& out)
{
    value = trim(std::move(value));
    if (value.empty()) return false;

    int base = 10;
    bool negative = false;
    if (!value.empty() && (value[0] == '-' || value[0] == '+')) {
        negative = value[0] == '-';
        value = value.substr(1);
    }
    if (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0) {
        value = value.substr(2);
        base = 16;
    }
    if (value.empty()) return false;

    std::uint32_t parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed, base);
    if (ec != std::errc{} || ptr != end) return false;
    if (negative) {
        if (parsed > 0x80000000u) return false;
        out = parsed == 0x80000000u
            ? std::numeric_limits<std::int32_t>::min()
            : -static_cast<std::int32_t>(parsed);
    } else {
        if (parsed > 0x7fffffffu) return false;
        out = static_cast<std::int32_t>(parsed);
    }
    return true;
}

bool parse_bool(std::string value, bool& out)
{
    value = to_lower(trim(std::move(value)));
    if (value == "1" || value == "true" || value == "yes") {
        out = true;
        return true;
    }
    if (value == "0" || value == "false" || value == "no") {
        out = false;
        return true;
    }
    return false;
}

std::optional<SampleWidth> parse_width(std::string value)
{
    value = to_lower(trim(std::move(value)));
    if (value == "u8" || value == "1") return SampleWidth::U8;
    if (value == "u16" || value == "2") return SampleWidth::U16;
    if (value == "u32" || value == "4") return SampleWidth::U32;
    if (value == "u64" || value == "8") return SampleWidth::U64;
    if (value == "8bit") return SampleWidth::U8;
    if (value == "16") return SampleWidth::U16;
    if (value == "32") return SampleWidth::U32;
    if (value == "64") return SampleWidth::U64;
    return std::nullopt;
}

std::optional<WatchpointAccess> parse_watchpoint_access(std::string value)
{
    value = to_lower(trim(std::move(value)));
    if (value == "read" || value == "r") return WatchpointAccess::Read;
    if (value == "write" || value == "w") return WatchpointAccess::Write;
    if (value == "access" || value == "rw" || value == "readwrite") return WatchpointAccess::Access;
    return std::nullopt;
}

std::optional<WatchpointScope> parse_watchpoint_scope(std::string value)
{
    value = to_lower(trim(std::move(value)));
    if (value.empty() || value == "normal" || value == "default") return WatchpointScope::Normal;
    if (value == "input_macro" || value == "input-macro" || value == "macro") {
        return WatchpointScope::InputMacro;
    }
    return std::nullopt;
}

bool parse_register_index(std::string value, std::uint8_t& out)
{
    value = trim(std::move(value));
    if (!value.empty() && (value[0] == 'r' || value[0] == 'R')) {
        value = value.substr(1);
    }
    std::uint32_t reg = 0;
    if (!parse_u32(value, reg) || reg > 31) {
        return false;
    }
    out = static_cast<std::uint8_t>(reg);
    return true;
}

std::vector<std::string> split_list(const std::string& value)
{
    std::vector<std::string> out;
    std::string cur;
    std::istringstream in(value);
    while (std::getline(in, cur, ',')) {
        cur = trim(cur);
        if (!cur.empty()) out.push_back(cur);
    }
    return out;
}

std::vector<std::string> split_pipe_list(const std::string& value)
{
    std::vector<std::string> out;
    std::string cur;
    std::istringstream in(value);
    while (std::getline(in, cur, '|')) {
        cur = trim(cur);
        if (!cur.empty()) out.push_back(cur);
    }
    return out;
}

std::vector<std::string> split_semicolon_list(const std::string& value)
{
    std::vector<std::string> out;
    std::string cur;
    std::istringstream in(value);
    while (std::getline(in, cur, ';')) {
        cur = trim(cur);
        if (!cur.empty()) out.push_back(cur);
    }
    return out;
}

std::optional<addr::AddrKey> parse_addr_key(std::string value)
{
    value = trim(std::move(value));
    if (value.rfind("key:", 0) == 0 || value.rfind("KEY:", 0) == 0) {
        value = value.substr(4);
    }
    std::uint32_t numeric = 0;
    if (parse_u32(value, numeric) && numeric <= 0xffffu) {
        const auto key = static_cast<addr::AddrKey>(numeric);
        if (addr::AddrRegistry::exists(key)) return key;
    }

    const auto lower = to_lower(value);
    for (const auto& rec : addr::AddrRegistry::all()) {
        if (to_lower(rec.name) == lower) return rec.key;
    }
    return std::nullopt;
}

bool append_addrprog_base(
    addrprog::Builder& builder,
    const std::string& base,
    std::vector<std::string>& errors,
    const std::string& section,
    const std::string& token)
{
    const auto base_trimmed = trim(base);
    std::uint8_t reg = 0;
    if (parse_register_index(base_trimmed, reg)) {
        builder.op_base_gpr(reg);
        return true;
    }
    if (base_trimmed.rfind("key:", 0) == 0 || base_trimmed.rfind("KEY:", 0) == 0) {
        const auto key = parse_addr_key(base_trimmed);
        if (!key.has_value()) {
            errors.push_back(section + ": invalid addrprog key base in '" + token + "'");
            return false;
        }
        builder.op_base_key(*key);
        return true;
    }
    std::uint32_t address = 0;
    if (parse_u32(base_trimmed, address)) {
        builder.op_base_abs(address);
        return true;
    }

    errors.push_back(section + ": invalid addrprog base in '" + token + "'");
    return false;
}

bool append_addrprog_step(
    addrprog::Builder& builder,
    const std::string& step,
    std::vector<std::string>& errors,
    const std::string& section,
    const std::string& token)
{
    const auto step_trimmed = trim(step);
    const auto step_lower = to_lower(step_trimmed);
    if (step_lower == "load_ptr32" || step_lower == "deref32") {
        builder.op_load_ptr32();
        return true;
    }

    if (!step_trimmed.empty() && (step_trimmed[0] == '+' || step_trimmed[0] == '-')) {
        std::int32_t offset = 0;
        if (!parse_i32(step_trimmed, offset)) {
            errors.push_back(section + ": invalid addrprog offset step in '" + token + "'");
            return false;
        }
        builder.op_add_i32(offset);
        return true;
    }

    if (step_lower.rfind("field:", 0) == 0) {
        std::uint32_t offset = 0;
        if (!parse_u32(step_trimmed.substr(6), offset)) {
            errors.push_back(section + ": invalid addrprog field step in '" + token + "'");
            return false;
        }
        builder.op_field(offset);
        return true;
    }

    if (step_lower.rfind("index:", 0) == 0) {
        const auto first = step_trimmed.find(':');
        const auto second = first == std::string::npos ? std::string::npos : step_trimmed.find(':', first + 1);
        if (first == std::string::npos || second == std::string::npos) {
            errors.push_back(section + ": addrprog index step must be index:count:stride in '" + token + "'");
            return false;
        }
        std::uint32_t count = 0;
        std::uint32_t stride = 0;
        if (!parse_u32(step_trimmed.substr(first + 1, second - first - 1), count)
            || !parse_u32(step_trimmed.substr(second + 1), stride)
            || count > 0xffffu
            || stride > 0xffffu) {
            errors.push_back(section + ": invalid addrprog index step in '" + token + "'");
            return false;
        }
        builder.op_index(static_cast<std::uint16_t>(count), static_cast<std::uint16_t>(stride));
        return true;
    }

    errors.push_back(section + ": unknown addrprog step '" + step_trimmed + "' in '" + token + "'");
    return false;
}

std::optional<AddressProgramSampleSpec> parse_address_program_sample(
    const std::string& token,
    std::vector<std::string>& errors,
    const std::string& section)
{
    const auto first = token.find(':');
    const auto last = token.rfind(':');
    if (first == std::string::npos || last == std::string::npos || first == last) {
        errors.push_back(section + ": addrprog sample must be name:base:steps:width or name:base:width, got '" + token + "'");
        return std::nullopt;
    }

    AddressProgramSampleSpec spec{};
    spec.name = trim(token.substr(0, first));
    if (spec.name.empty()) {
        errors.push_back(section + ": addrprog sample name is empty");
        return std::nullopt;
    }

    const auto width = parse_width(token.substr(last + 1));
    if (!width.has_value()) {
        errors.push_back(section + ": invalid addrprog sample width in '" + token + "'");
        return std::nullopt;
    }
    spec.width = *width;

    const auto body = token.substr(first + 1, last - first - 1);
    std::string base;
    std::string steps;
    if (body.rfind("key:", 0) == 0 || body.rfind("KEY:", 0) == 0) {
        const auto separator = body.find(':', 4);
        if (separator == std::string::npos) {
            base = body;
        } else {
            base = body.substr(0, separator);
            steps = body.substr(separator + 1);
        }
    } else {
        const auto separator = body.find(':');
        if (separator == std::string::npos) {
            base = body;
        } else {
            base = body.substr(0, separator);
            steps = body.substr(separator + 1);
        }
    }

    addrprog::Builder builder;
    if (!append_addrprog_base(builder, base, errors, section, token)) {
        return std::nullopt;
    }
    for (const auto& step : split_pipe_list(steps)) {
        if (!append_addrprog_step(builder, step, errors, section, token)) {
            return std::nullopt;
        }
    }
    builder.op_end();
    spec.program = builder.blob();
    return spec;
}

std::optional<std::vector<std::uint8_t>> parse_address_program_expression(
    const std::string& token,
    std::vector<std::string>& errors,
    const std::string& section)
{
    const auto separator = token.find(':');
    if (separator == std::string::npos) {
        errors.push_back(section + ": addrprog must be base:steps, got '" + token + "'");
        return std::nullopt;
    }

    std::string base;
    std::string steps;
    if (token.rfind("key:", 0) == 0 || token.rfind("KEY:", 0) == 0) {
        const auto key_separator = token.find(':', 4);
        if (key_separator == std::string::npos) {
            base = token;
        } else {
            base = token.substr(0, key_separator);
            steps = token.substr(key_separator + 1);
        }
    } else {
        base = token.substr(0, separator);
        steps = token.substr(separator + 1);
    }

    addrprog::Builder builder;
    if (!append_addrprog_base(builder, base, errors, section, token)) {
        return std::nullopt;
    }
    for (const auto& step : split_pipe_list(steps)) {
        if (!append_addrprog_step(builder, step, errors, section, token)) {
            return std::nullopt;
        }
    }
    builder.op_end();
    return builder.blob();
}

std::optional<MemorySampleSpec> parse_memory_sample(
    const std::string& token,
    std::vector<std::string>& errors,
    const std::string& section)
{
    const auto first = token.find(':');
    const auto second = first == std::string::npos ? std::string::npos : token.find(':', first + 1);
    if (first == std::string::npos || second == std::string::npos) {
        errors.push_back(section + ": memory sample must be name:address:width, got '" + token + "'");
        return std::nullopt;
    }

    MemorySampleSpec spec{};
    spec.name = trim(token.substr(0, first));
    std::uint32_t address = 0;
    if (spec.name.empty()) {
        errors.push_back(section + ": memory sample name is empty");
        return std::nullopt;
    }
    if (!parse_u32(token.substr(first + 1, second - first - 1), address)) {
        errors.push_back(section + ": invalid memory sample address in '" + token + "'");
        return std::nullopt;
    }
    const auto width = parse_width(token.substr(second + 1));
    if (!width.has_value()) {
        errors.push_back(section + ": invalid memory sample width in '" + token + "'");
        return std::nullopt;
    }
    spec.address = address;
    spec.width = *width;
    return spec;
}

std::optional<RegisterMemorySampleSpec> parse_register_memory_sample(
    const std::string& token,
    std::vector<std::string>& errors,
    const std::string& section)
{
    const auto first = token.find(':');
    const auto second = first == std::string::npos ? std::string::npos : token.find(':', first + 1);
    const auto third = second == std::string::npos ? std::string::npos : token.find(':', second + 1);
    if (first == std::string::npos || second == std::string::npos || third == std::string::npos) {
        errors.push_back(section + ": register memory sample must be name:reg:offset:width, got '" + token + "'");
        return std::nullopt;
    }

    RegisterMemorySampleSpec spec{};
    spec.name = trim(token.substr(0, first));
    if (spec.name.empty()) {
        errors.push_back(section + ": register memory sample name is empty");
        return std::nullopt;
    }
    if (!parse_register_index(token.substr(first + 1, second - first - 1), spec.base_reg)) {
        errors.push_back(section + ": invalid register in '" + token + "'");
        return std::nullopt;
    }
    if (!parse_i32(token.substr(second + 1, third - second - 1), spec.offset)) {
        errors.push_back(section + ": invalid register memory offset in '" + token + "'");
        return std::nullopt;
    }
    const auto width = parse_width(token.substr(third + 1));
    if (!width.has_value()) {
        errors.push_back(section + ": invalid register memory width in '" + token + "'");
        return std::nullopt;
    }
    spec.width = *width;
    return spec;
}

std::optional<GprSampleSpec> parse_gpr_sample(
    const std::string& token,
    std::vector<std::string>& errors,
    const std::string& section)
{
    const auto colon = token.find(':');
    if (colon == std::string::npos) {
        errors.push_back(section + ": gpr sample must be name:reg, got '" + token + "'");
        return std::nullopt;
    }

    GprSampleSpec spec{};
    spec.name = trim(token.substr(0, colon));
    std::uint32_t reg = 0;
    if (spec.name.empty()) {
        errors.push_back(section + ": gpr sample name is empty");
        return std::nullopt;
    }
    if (!parse_u32(token.substr(colon + 1), reg) || reg > 31) {
        errors.push_back(section + ": invalid gpr index in '" + token + "'");
        return std::nullopt;
    }
    spec.reg = static_cast<std::uint8_t>(reg);
    return spec;
}

std::optional<LinkedListFieldSpec> parse_linked_list_field(
    const std::string& token,
    std::vector<std::string>& errors,
    const std::string& section,
    const std::string& list_name)
{
    const auto at = token.find('@');
    const auto colon = token.rfind(':');
    if (at == std::string::npos || colon == std::string::npos || at >= colon) {
        errors.push_back(section + ": linked_list field must be name@offset:width in '" + token + "'");
        return std::nullopt;
    }

    LinkedListFieldSpec spec{};
    spec.name = trim(token.substr(0, at));
    if (spec.name.empty()) {
        errors.push_back(section + ": linked_list '" + list_name + "' has an empty field name");
        return std::nullopt;
    }
    if (!parse_i32(token.substr(at + 1, colon - at - 1), spec.offset)) {
        errors.push_back(section + ": linked_list '" + list_name + "' has invalid field offset in '" + token + "'");
        return std::nullopt;
    }
    const auto width = parse_width(token.substr(colon + 1));
    if (!width.has_value()) {
        errors.push_back(section + ": linked_list '" + list_name + "' has invalid field width in '" + token + "'");
        return std::nullopt;
    }
    spec.width = *width;
    return spec;
}

std::optional<LinkedListSnapshotSpec> parse_linked_list_sample(
    const std::string& token,
    std::vector<std::string>& errors,
    const std::string& section)
{
    const auto colon = token.find(':');
    if (colon == std::string::npos) {
        errors.push_back(section + ": linked_list sample must be name:head_ptr=...,next=...,max=...,fields=..., got '" + token + "'");
        return std::nullopt;
    }

    LinkedListSnapshotSpec spec{};
    spec.name = trim(token.substr(0, colon));
    if (spec.name.empty()) {
        errors.push_back(section + ": linked_list sample name is empty");
        return std::nullopt;
    }

    bool has_head_ptr = false;
    bool has_next = false;
    bool has_max = false;
    bool has_fields = false;
    std::unordered_set<std::string> field_names;

    for (const auto& option : split_list(token.substr(colon + 1))) {
        const auto eq = option.find('=');
        if (eq == std::string::npos) {
            errors.push_back(section + ": linked_list '" + spec.name + "' option must be key=value, got '" + option + "'");
            return std::nullopt;
        }
        const auto key = to_lower(trim(option.substr(0, eq)));
        const auto value = trim(option.substr(eq + 1));
        if (key == "head_ptr" || key == "head") {
            std::uint32_t address = 0;
            if (!parse_u32(value, address) || address == 0) {
                errors.push_back(section + ": linked_list '" + spec.name + "' has invalid head_ptr");
                return std::nullopt;
            }
            spec.head_ptr_address = address;
            has_head_ptr = true;
        } else if (key == "next") {
            std::int32_t offset = 0;
            if (!parse_i32(value, offset)) {
                errors.push_back(section + ": linked_list '" + spec.name + "' has invalid next offset");
                return std::nullopt;
            }
            spec.next_offset = offset;
            has_next = true;
        } else if (key == "max" || key == "max_nodes") {
            std::uint32_t max_nodes = 0;
            if (!parse_u32(value, max_nodes) || max_nodes == 0 || max_nodes > 256) {
                errors.push_back(section + ": linked_list '" + spec.name + "' max must be 1..256");
                return std::nullopt;
            }
            spec.max_nodes = max_nodes;
            has_max = true;
        } else if (key == "fields") {
            has_fields = true;
            for (const auto& field_token : split_pipe_list(value)) {
                auto field = parse_linked_list_field(field_token, errors, section, spec.name);
                if (!field.has_value()) {
                    return std::nullopt;
                }
                if (!field_names.insert(field->name).second) {
                    errors.push_back(section + ": linked_list '" + spec.name + "' duplicate field '" + field->name + "'");
                    return std::nullopt;
                }
                spec.fields.push_back(*field);
            }
        } else {
            errors.push_back(section + ": linked_list '" + spec.name + "' has unknown option '" + key + "'");
            return std::nullopt;
        }
    }

    if (!has_head_ptr) errors.push_back(section + ": linked_list '" + spec.name + "' missing head_ptr");
    if (!has_next) errors.push_back(section + ": linked_list '" + spec.name + "' missing next");
    if (!has_max) errors.push_back(section + ": linked_list '" + spec.name + "' missing max");
    if (!has_fields || spec.fields.empty()) {
        errors.push_back(section + ": linked_list '" + spec.name + "' missing fields");
    }
    if (!has_head_ptr || !has_next || !has_max || !has_fields || spec.fields.empty()) {
        return std::nullopt;
    }
    return spec;
}

void append_linked_list_samples(
    std::vector<LinkedListSnapshotSpec>& out,
    const std::string& value,
    std::vector<std::string>& errors,
    const std::string& section)
{
    for (const auto& token : split_semicolon_list(value)) {
        if (auto parsed = parse_linked_list_sample(token, errors, section)) {
            out.push_back(*parsed);
        }
    }
}

void append_register_memory_samples(
    std::vector<RegisterMemorySampleSpec>& out,
    const std::string& value,
    std::vector<std::string>& errors,
    const std::string& section)
{
    for (const auto& token : split_list(value)) {
        if (auto parsed = parse_register_memory_sample(token, errors, section)) {
            out.push_back(*parsed);
        }
    }
}

void append_address_program_samples(
    std::vector<AddressProgramSampleSpec>& out,
    const std::string& value,
    std::vector<std::string>& errors,
    const std::string& section)
{
    for (const auto& token : split_list(value)) {
        if (auto parsed = parse_address_program_sample(token, errors, section)) {
            out.push_back(*parsed);
        }
    }
}

void append_memory_samples(
    std::vector<MemorySampleSpec>& out,
    const std::string& value,
    std::vector<std::string>& errors,
    const std::string& section)
{
    for (const auto& token : split_list(value)) {
        if (auto parsed = parse_memory_sample(token, errors, section)) {
            out.push_back(*parsed);
        }
    }
}

void append_gpr_samples(
    std::vector<GprSampleSpec>& out,
    const std::string& value,
    std::vector<std::string>& errors,
    const std::string& section)
{
    for (const auto& token : split_list(value)) {
        if (auto parsed = parse_gpr_sample(token, errors, section)) {
            out.push_back(*parsed);
        }
    }
}

} // namespace

const CheckpointSpec* CaptureProfile::find_checkpoint(std::uint32_t pc) const
{
    for (const auto& checkpoint : checkpoints) {
        if (checkpoint.pc == pc) return &checkpoint;
    }
    return nullptr;
}

std::vector<const CheckpointSpec*> CaptureProfile::find_checkpoints(std::uint32_t pc) const
{
    std::vector<const CheckpointSpec*> out;
    for (const auto& checkpoint : checkpoints) {
        if (checkpoint.pc == pc) {
            out.push_back(&checkpoint);
        }
    }
    return out;
}

std::vector<std::uint32_t> CaptureProfile::pcs() const
{
    std::vector<std::uint32_t> out;
    out.reserve(checkpoints.size());
    for (const auto& checkpoint : checkpoints) {
        if (std::find(out.begin(), out.end(), checkpoint.pc) == out.end()) {
            out.push_back(checkpoint.pc);
        }
    }
    return out;
}

CaptureProfileParseResult ParseCaptureProfileText(const std::string& text)
{
    CaptureProfileParseResult result{};
    const auto ini = IniDoc::parse(text);

    CaptureProfile profile{};
    profile.name = ini.get("profile", "name", "");
    profile.schema_version = ini.get_u32("profile", "schema_version", 1);
    profile.capture_only_hit_limit =
        ini.get_u32("profile", "capture_only_hit_limit", 4096);
    if (profile.name.empty()) {
        result.errors.push_back("profile.name is required");
    }
    if (profile.schema_version != 1) {
        result.errors.push_back("unsupported profile.schema_version=" + std::to_string(profile.schema_version));
    }
    if (profile.capture_only_hit_limit == 0) {
        result.errors.push_back("profile.capture_only_hit_limit must be a positive integer");
    }

    append_memory_samples(
        profile.default_memory_samples,
        ini.get("profile", "memory", ""),
        result.errors,
        "profile");
    append_gpr_samples(
        profile.default_gpr_samples,
        ini.get("profile", "gprs", ""),
        result.errors,
        "profile");
    append_register_memory_samples(
        profile.default_register_memory_samples,
        ini.get("profile", "reg_memory", ""),
        result.errors,
        "profile");
    append_address_program_samples(
        profile.default_address_program_samples,
        ini.get("profile", "addrprog", ""),
        result.errors,
        "profile");
    append_linked_list_samples(
        profile.default_linked_list_samples,
        ini.get("profile", "linked_list", ""),
        result.errors,
        "profile");
    const auto default_addrprog_trace_text = ini.get("profile", "addrprog_trace", "false");
    if (!parse_bool(default_addrprog_trace_text, profile.default_address_program_trace)) {
        result.errors.push_back("profile: addrprog_trace must be true/false");
    }

    std::set<std::string> ids;
    for (const auto& section : ini.list_sections(false)) {
        if (section.rfind("checkpoint.", 0) != 0) continue;

        CheckpointSpec checkpoint{};
        checkpoint.id = section.substr(std::string("checkpoint.").size());
        checkpoint.name = ini.get(section, "name", checkpoint.id);
        checkpoint.function = ini.get(section, "function", "unknown");
        checkpoint.checkpoint = ini.get(section, "checkpoint", checkpoint.name);

        std::uint32_t pc = 0;
        if (!parse_u32(ini.get(section, "pc", ""), pc) || pc == 0) {
            result.errors.push_back(section + ": pc is required");
        }
        checkpoint.pc = pc;

        const auto activate_on_pc_text = ini.get(section, "activate_on_pc", ini.get(section, "activation_pc", ""));
        if (!activate_on_pc_text.empty()) {
            std::uint32_t activation_pc = 0;
            if (!parse_u32(activate_on_pc_text, activation_pc) || activation_pc == 0) {
                result.errors.push_back(section + ": activate_on_pc must be a nonzero u32");
            } else {
                checkpoint.activate_on_pc = activation_pc;
            }
        }

        bool owns_rng_draw = false;
        const auto owns_text = ini.get(section, "owns_rng_draw", "false");
        if (!parse_bool(owns_text, owns_rng_draw)) {
            result.errors.push_back(section + ": owns_rng_draw must be true/false");
        }
        checkpoint.owns_rng_draw = owns_rng_draw;
        const auto max_hits_text = ini.get(section, "max_hits", "");
        if (!max_hits_text.empty()) {
            std::uint32_t max_hits = 0;
            if (!parse_u32(max_hits_text, max_hits) || max_hits == 0) {
                result.errors.push_back(section + ": max_hits must be a positive integer");
            } else {
                checkpoint.max_hits = max_hits;
            }
        }
        checkpoint.address_program_trace = profile.default_address_program_trace;
        const auto addrprog_trace_text = ini.get(section, "addrprog_trace", "");
        if (!addrprog_trace_text.empty()
            && !parse_bool(addrprog_trace_text, checkpoint.address_program_trace)) {
            result.errors.push_back(section + ": addrprog_trace must be true/false");
        }

        checkpoint.memory_samples = profile.default_memory_samples;
        checkpoint.gpr_samples = profile.default_gpr_samples;
        checkpoint.register_memory_samples = profile.default_register_memory_samples;
        checkpoint.address_program_samples = profile.default_address_program_samples;
        checkpoint.linked_list_samples = profile.default_linked_list_samples;
        append_memory_samples(
            checkpoint.memory_samples,
            ini.get(section, "memory", ""),
            result.errors,
            section);
        append_gpr_samples(
            checkpoint.gpr_samples,
            ini.get(section, "gprs", ""),
            result.errors,
            section);
        append_register_memory_samples(
            checkpoint.register_memory_samples,
            ini.get(section, "reg_memory", ""),
            result.errors,
            section);
        append_address_program_samples(
            checkpoint.address_program_samples,
            ini.get(section, "addrprog", ""),
            result.errors,
            section);
        append_linked_list_samples(
            checkpoint.linked_list_samples,
            ini.get(section, "linked_list", ""),
            result.errors,
            section);

        if (checkpoint.id.empty()) {
            result.errors.push_back(section + ": checkpoint id is empty");
        } else if (!ids.insert(checkpoint.id).second) {
            result.errors.push_back(section + ": duplicate checkpoint id");
        }
        profile.checkpoints.push_back(std::move(checkpoint));
    }

    std::set<std::string> watchpoint_ids;
    std::set<std::uint32_t> watchpoint_addresses;
    for (const auto& section : ini.list_sections(false)) {
        if (section.rfind("watchpoint.", 0) != 0) continue;

        MemoryWatchpointSpec watchpoint{};
        watchpoint.id = section.substr(std::string("watchpoint.").size());
        if (watchpoint.id.empty()) {
            result.errors.push_back(section + ": watchpoint id is empty");
        } else if (!watchpoint_ids.insert(watchpoint.id).second) {
            result.errors.push_back(section + ": duplicate watchpoint id");
        }

        std::uint32_t address = 0;
        if (!parse_u32(ini.get(section, "address", ""), address) || address == 0) {
            result.errors.push_back(section + ": address is required");
        }
        watchpoint.address = address;
        if (address != 0 && !watchpoint_addresses.insert(address).second) {
            result.errors.push_back(section + ": duplicate watchpoint address");
        }

        const auto size = parse_width(ini.get(section, "size", ""));
        if (!size.has_value()) {
            result.errors.push_back(section + ": size must be u8/u16/u32/u64");
        } else {
            watchpoint.size = *size;
        }

        const auto access = parse_watchpoint_access(ini.get(section, "access", ""));
        if (!access.has_value()) {
            result.errors.push_back(section + ": access must be read/write/access");
        } else {
            watchpoint.access = *access;
        }

        const auto scope = parse_watchpoint_scope(ini.get(section, "scope", ""));
        if (!scope.has_value()) {
            result.errors.push_back(section + ": scope must be normal/input_macro");
        } else {
            watchpoint.scope = *scope;
        }

        const auto owns_text = ini.get(section, "owns_rng_draw", "");
        if (!owns_text.empty()
            && !parse_bool(owns_text, watchpoint.owns_rng_draw)) {
            result.errors.push_back(section + ": owns_rng_draw must be true/false");
        }

        profile.memory_watchpoints.push_back(std::move(watchpoint));
    }

    for (const auto& section : ini.list_sections(false)) {
        if (section.rfind("dynamic_watchpoint.", 0) != 0) continue;

        DynamicMemoryWatchpointSpec watchpoint{};
        watchpoint.id = section.substr(std::string("dynamic_watchpoint.").size());
        if (watchpoint.id.empty()) {
            result.errors.push_back(section + ": dynamic watchpoint id is empty");
        } else if (!watchpoint_ids.insert(watchpoint.id).second) {
            result.errors.push_back(section + ": duplicate watchpoint id");
        }

        std::uint32_t pc = 0;
        if (!parse_u32(ini.get(section, "pc", ""), pc) || pc == 0) {
            result.errors.push_back(section + ": pc is required");
        }
        watchpoint.pc = pc;

        const auto address_text = ini.get(section, "address", "");
        const auto addrprog_text = ini.get(section, "addrprog", ini.get(section, "address_program", ""));
        if (!address_text.empty()) {
            if (!addrprog_text.empty()) {
                result.errors.push_back(section + ": specify only one of address or addrprog");
            }
            std::uint32_t address = 0;
            if (!parse_u32(address_text, address) || address == 0) {
                result.errors.push_back(section + ": address must be a nonzero u32");
            } else {
                watchpoint.use_absolute_address = true;
                watchpoint.address = address;
            }
        } else if (!addrprog_text.empty()) {
            if (auto program = parse_address_program_expression(addrprog_text, result.errors, section)) {
                watchpoint.use_address_program = true;
                watchpoint.address_program = std::move(*program);
            }
        } else {
            const auto base_text = ini.get(section, "base_gpr", ini.get(section, "base_reg", ""));
            if (!parse_register_index(base_text, watchpoint.base_reg)) {
                result.errors.push_back(section + ": base_gpr must be r0-r31 when address or addrprog is not provided");
            }

            std::int32_t offset = 0;
            if (!parse_i32(ini.get(section, "offset", ""), offset)) {
                result.errors.push_back(section + ": offset is required when address or addrprog is not provided");
            }
            watchpoint.offset = offset;
        }

        const auto size = parse_width(ini.get(section, "size", ""));
        if (!size.has_value()) {
            result.errors.push_back(section + ": size must be u8/u16/u32/u64");
        } else {
            watchpoint.size = *size;
        }

        const auto access = parse_watchpoint_access(ini.get(section, "access", ""));
        if (!access.has_value()) {
            result.errors.push_back(section + ": access must be read/write/access");
        } else {
            watchpoint.access = *access;
        }

        const auto scope = parse_watchpoint_scope(ini.get(section, "scope", ""));
        if (!scope.has_value()) {
            result.errors.push_back(section + ": scope must be normal/input_macro");
        } else {
            watchpoint.scope = *scope;
        }

        const auto one_shot_text = ini.get(section, "one_shot", "");
        if (!one_shot_text.empty()
            && !parse_bool(one_shot_text, watchpoint.one_shot)) {
            result.errors.push_back(section + ": one_shot must be true/false");
        }

        const auto owns_text = ini.get(section, "owns_rng_draw", "");
        if (!owns_text.empty()
            && !parse_bool(owns_text, watchpoint.owns_rng_draw)) {
            result.errors.push_back(section + ": owns_rng_draw must be true/false");
        }

        profile.dynamic_memory_watchpoints.push_back(std::move(watchpoint));
    }

    if (profile.checkpoints.empty()
        && profile.memory_watchpoints.empty()
        && profile.dynamic_memory_watchpoints.empty()) {
        result.errors.push_back("profile must define at least one [checkpoint.*], [watchpoint.*], or [dynamic_watchpoint.*] section");
    }

    if (result.errors.empty()) {
        result.profile = std::move(profile);
    }
    return result;
}

CaptureProfileParseResult LoadCaptureProfileFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        CaptureProfileParseResult result;
        result.errors.push_back("failed to open capture profile: " + path);
        return result;
    }

    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (!in.good() && !in.eof()) {
        CaptureProfileParseResult result;
        result.errors.push_back("failed to read capture profile: " + path);
        return result;
    }
    return ParseCaptureProfileText(buffer.str());
}

std::string FormatCaptureProfileError(const CaptureProfileParseResult& result)
{
    std::ostringstream out;
    for (const auto& error : result.errors) {
        if (out.tellp() > 0) out << "; ";
        out << error;
    }
    return out.str();
}

} // namespace savor::capture
