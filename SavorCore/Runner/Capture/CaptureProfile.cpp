#include "CaptureProfile.h"

#include "../../Utils/IniDoc.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>

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
    if (profile.name.empty()) {
        result.errors.push_back("profile.name is required");
    }
    if (profile.schema_version != 1) {
        result.errors.push_back("unsupported profile.schema_version=" + std::to_string(profile.schema_version));
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

        bool owns_rng_draw = false;
        const auto owns_text = ini.get(section, "owns_rng_draw", "false");
        if (!parse_bool(owns_text, owns_rng_draw)) {
            result.errors.push_back(section + ": owns_rng_draw must be true/false");
        }
        checkpoint.owns_rng_draw = owns_rng_draw;

        checkpoint.memory_samples = profile.default_memory_samples;
        checkpoint.gpr_samples = profile.default_gpr_samples;
        checkpoint.register_memory_samples = profile.default_register_memory_samples;
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

        profile.memory_watchpoints.push_back(std::move(watchpoint));
    }

    if (profile.checkpoints.empty() && profile.memory_watchpoints.empty()) {
        result.errors.push_back("profile must define at least one [checkpoint.*] or [watchpoint.*] section");
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
