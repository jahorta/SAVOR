#include "ProbeProfile.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <ranges>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <picojson.h>

namespace savor::probe {
namespace {

using Object = picojson::value::object;
using Array = picojson::value::array;

const picojson::value* member(const Object& object, std::string_view key)
{
    const auto found = object.find(std::string(key));
    return found == object.end() ? nullptr : &found->second;
}

void reject_unknown_keys(
    const Object& object,
    std::initializer_list<std::string_view> allowed,
    std::string_view context,
    std::vector<std::string>& errors)
{
    for (const auto& [key, value] : object) {
        (void)value;
        if (std::ranges::find(allowed, key) == allowed.end())
            errors.push_back(std::string(context) + " contains unknown field '" + key + "'");
    }
}

std::optional<std::string> string_value(const Object& object, std::string_view key)
{
    const auto* value = member(object, key);
    if (!value || !value->is<std::string>())
        return std::nullopt;
    return value->get<std::string>();
}

std::optional<std::uint64_t> unsigned_value(const picojson::value& value)
{
    if (value.is<double>()) {
        const auto number = value.get<double>();
        if (number < 0 || number > static_cast<double>(UINT64_MAX)
            || number != static_cast<double>(static_cast<std::uint64_t>(number))) {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(number);
    }
    if (!value.is<std::string>())
        return std::nullopt;
    const auto text = value.get<std::string>();
    std::uint64_t result = 0;
    int base = 10;
    std::string_view digits = text;
    if (digits.starts_with("0x") || digits.starts_with("0X")) {
        digits.remove_prefix(2);
        base = 16;
    }
    if (digits.empty())
        return std::nullopt;
    const auto [end, error] = std::from_chars(
        digits.data(), digits.data() + digits.size(), result, base);
    if (error != std::errc{} || end != digits.data() + digits.size())
        return std::nullopt;
    return result;
}

std::optional<std::uint64_t> unsigned_value(const Object& object, std::string_view key)
{
    const auto* value = member(object, key);
    return value ? unsigned_value(*value) : std::nullopt;
}

std::optional<std::int64_t> signed_value(const Object& object, std::string_view key)
{
    const auto* value = member(object, key);
    if (!value)
        return std::nullopt;
    if (value->is<double>()) {
        const auto number = value->get<double>();
        const auto converted = static_cast<std::int64_t>(number);
        return number == static_cast<double>(converted)
            ? std::optional<std::int64_t>(converted) : std::nullopt;
    }
    if (!value->is<std::string>())
        return std::nullopt;
    const auto text = value->get<std::string>();
    std::int64_t result = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size())
        return std::nullopt;
    return result;
}

bool bool_value(const Object& object, std::string_view key, bool fallback = false)
{
    const auto* value = member(object, key);
    return value && value->is<bool>() ? value->get<bool>() : fallback;
}

bool require_type(
    const Object& object,
    std::string_view key,
    bool valid,
    std::string_view expected,
    std::string_view context,
    std::vector<std::string>& errors)
{
    if (!member(object, key) || valid)
        return true;
    errors.push_back(std::string(context) + " field '" + std::string(key)
        + "' must be " + std::string(expected));
    return false;
}

SampleWidth parse_width(const picojson::value* value, bool& ok)
{
    ok = true;
    if (!value)
        return SampleWidth::U32;
    const auto parsed = unsigned_value(*value);
    if (!parsed.has_value()) {
        ok = false;
        return SampleWidth::U32;
    }
    switch (*parsed) {
    case 1: return SampleWidth::U8;
    case 2: return SampleWidth::U16;
    case 4: return SampleWidth::U32;
    case 8: return SampleWidth::U64;
    default: ok = false; return SampleWidth::U32;
    }
}

std::optional<SampleKind> parse_sample_kind(std::string_view value)
{
    if (value == "gpr") return SampleKind::Gpr;
    if (value == "memory") return SampleKind::Memory;
    if (value == "register_memory") return SampleKind::RegisterMemory;
    if (value == "address_program") return SampleKind::AddressProgram;
    if (value == "linked_list") return SampleKind::LinkedList;
    if (value == "constant") return SampleKind::Constant;
    if (value == "stack_trace") return SampleKind::StackTrace;
    if (value == "routed_sample") return SampleKind::RoutedSample;
    return std::nullopt;
}

std::string_view sample_kind_name(SampleKind value)
{
    switch (value) {
    case SampleKind::Gpr: return "gpr";
    case SampleKind::Memory: return "memory";
    case SampleKind::RegisterMemory: return "register_memory";
    case SampleKind::AddressProgram: return "address_program";
    case SampleKind::LinkedList: return "linked_list";
    case SampleKind::Constant: return "constant";
    case SampleKind::StackTrace: return "stack_trace";
    case SampleKind::RoutedSample: return "routed_sample";
    }
    return "unknown";
}

std::optional<AddressTracePolicy> parse_trace_policy(std::string_view value)
{
    if (value == "off") return AddressTracePolicy::Off;
    if (value == "on_failure") return AddressTracePolicy::OnFailure;
    if (value == "always") return AddressTracePolicy::Always;
    return std::nullopt;
}

std::string_view trace_policy_name(AddressTracePolicy value)
{
    switch (value) {
    case AddressTracePolicy::Off: return "off";
    case AddressTracePolicy::OnFailure: return "on_failure";
    case AddressTracePolicy::Always: return "always";
    }
    return "off";
}

std::optional<PredicateOp> parse_predicate_op(std::string_view value)
{
    if (value == "constant") return PredicateOp::PushConstant;
    if (value == "pc") return PredicateOp::PushPc;
    if (value == "lr") return PredicateOp::PushLr;
    if (value == "gpr") return PredicateOp::PushGpr;
    if (value == "hit_count") return PredicateOp::PushHitCount;
    if (value == "sample") return PredicateOp::PushSample;
    if (value == "caller") return PredicateOp::PushCaller;
    if (value == "eq") return PredicateOp::Equal;
    if (value == "ne") return PredicateOp::NotEqual;
    if (value == "lt") return PredicateOp::Less;
    if (value == "le") return PredicateOp::LessEqual;
    if (value == "gt") return PredicateOp::Greater;
    if (value == "ge") return PredicateOp::GreaterEqual;
    if (value == "mask_eq") return PredicateOp::MaskEqual;
    if (value == "and") return PredicateOp::LogicalAnd;
    if (value == "or") return PredicateOp::LogicalOr;
    if (value == "not") return PredicateOp::LogicalNot;
    return std::nullopt;
}

std::string_view predicate_op_name(PredicateOp value)
{
    switch (value) {
    case PredicateOp::PushConstant: return "constant";
    case PredicateOp::PushPc: return "pc";
    case PredicateOp::PushLr: return "lr";
    case PredicateOp::PushGpr: return "gpr";
    case PredicateOp::PushHitCount: return "hit_count";
    case PredicateOp::PushSample: return "sample";
    case PredicateOp::PushCaller: return "caller";
    case PredicateOp::Equal: return "eq";
    case PredicateOp::NotEqual: return "ne";
    case PredicateOp::Less: return "lt";
    case PredicateOp::LessEqual: return "le";
    case PredicateOp::Greater: return "gt";
    case PredicateOp::GreaterEqual: return "ge";
    case PredicateOp::MaskEqual: return "mask_eq";
    case PredicateOp::LogicalAnd: return "and";
    case PredicateOp::LogicalOr: return "or";
    case PredicateOp::LogicalNot: return "not";
    }
    return "unknown";
}

std::optional<SamplingMode> parse_sampling_mode(std::string_view value)
{
    if (value == "every_hit") return SamplingMode::EveryHit;
    if (value == "every_n") return SamplingMode::EveryN;
    if (value == "first_n") return SamplingMode::FirstN;
    if (value == "changed_only") return SamplingMode::ChangedOnly;
    if (value == "hit_ranges") return SamplingMode::HitRanges;
    if (value == "last_n") return SamplingMode::LastN;
    if (value == "burst") return SamplingMode::Burst;
    return std::nullopt;
}

std::string_view sampling_mode_name(SamplingMode value)
{
    switch (value) {
    case SamplingMode::EveryHit: return "every_hit";
    case SamplingMode::EveryN: return "every_n";
    case SamplingMode::FirstN: return "first_n";
    case SamplingMode::ChangedOnly: return "changed_only";
    case SamplingMode::HitRanges: return "hit_ranges";
    case SamplingMode::LastN: return "last_n";
    case SamplingMode::Burst: return "burst";
    }
    return "every_hit";
}

bool parse_byte_program(
    const Object& object,
    std::string_view key,
    std::string_view context,
    std::vector<std::uint8_t>& program,
    std::vector<std::string>& errors)
{
    const auto* value = member(object, key);
    if (!value)
        return true;
    if (!value->is<Array>()) {
        errors.push_back(std::string(context) + " field '" + std::string(key) + "' must be an array");
        return false;
    }
    for (const auto& raw_byte : value->get<Array>()) {
        const auto byte = unsigned_value(raw_byte);
        if (!byte.has_value() || *byte > 0xff) {
            errors.push_back(std::string(context) + " has an invalid address-program byte");
            return false;
        }
        program.push_back(static_cast<std::uint8_t>(*byte));
    }
    const auto validation = validate_address_program(program);
    if (!validation.valid) {
        errors.push_back(std::string(context) + " address program is invalid at byte "
            + std::to_string(validation.failure_offset) + ": " + validation.message);
        return false;
    }
    return true;
}

void parse_sample(
    const picojson::value& value,
    ProbeDefinition& probe,
    std::vector<std::string>& errors)
{
    if (!value.is<Object>()) {
        errors.push_back("probe '" + probe.id + "' contains a non-object sample");
        return;
    }
    const auto& object = value.get<Object>();
    const auto name = string_value(object, "name");
    const auto type = string_value(object, "type");
    if (!name.has_value() || name->empty() || !type.has_value()) {
        errors.push_back("probe '" + probe.id + "' sample requires string name and type");
        return;
    }
    const auto kind = parse_sample_kind(*type);
    if (!kind.has_value()) {
        errors.push_back("probe '" + probe.id + "' has unknown sample type '" + *type + "'");
        return;
    }
    const auto context = "probe '" + probe.id + "' sample '" + *name + "'";
    switch (*kind) {
    case SampleKind::Gpr:
        reject_unknown_keys(object, { "name", "type", "width", "register" }, context, errors);
        break;
    case SampleKind::Memory:
        reject_unknown_keys(object, { "name", "type", "width", "address" }, context, errors);
        break;
    case SampleKind::RegisterMemory:
        reject_unknown_keys(object, { "name", "type", "width", "register", "offset" }, context, errors);
        break;
    case SampleKind::AddressProgram:
        reject_unknown_keys(object, { "name", "type", "width", "program", "trace" }, context, errors);
        break;
    case SampleKind::LinkedList:
        reject_unknown_keys(object,
            { "name", "type", "width", "address", "program", "trace", "next_offset", "max_nodes", "fields" },
            context, errors);
        break;
    case SampleKind::Constant:
        reject_unknown_keys(object, { "name", "type", "value" }, context, errors);
        break;
    case SampleKind::StackTrace:
        reject_unknown_keys(object, { "name", "type", "max_frames" }, context, errors);
        break;
    case SampleKind::RoutedSample:
        reject_unknown_keys(object, { "name", "type", "width", "descriptor_id" }, context, errors);
        break;
    }

    SampleDefinition sample;
    sample.name = *name;
    sample.kind = *kind;
    bool width_ok = false;
    sample.width = parse_width(member(object, "width"), width_ok);
    if (!width_ok)
        errors.push_back(context + " has invalid width");
    sample.address_provided = member(object, "address") != nullptr;
    if (sample.address_provided) {
        const auto parsed = unsigned_value(object, "address");
        if (!parsed.has_value() || *parsed > UINT32_MAX)
            errors.push_back(context + " has invalid address");
        else
            sample.address = static_cast<std::uint32_t>(*parsed);
    }
    if (member(object, "register")) {
        const auto parsed = unsigned_value(object, "register");
        if (!parsed.has_value() || *parsed >= 32)
            errors.push_back(context + " has invalid register");
        else
            sample.base_register = static_cast<std::uint8_t>(*parsed);
    }
    if (member(object, "offset")) {
        const auto parsed = signed_value(object, "offset");
        if (!parsed.has_value() || *parsed < INT32_MIN || *parsed > INT32_MAX)
            errors.push_back(context + " has invalid offset");
        else
            sample.offset = static_cast<std::int32_t>(*parsed);
    }
    if (member(object, "value")) {
        const auto parsed = unsigned_value(object, "value");
        if (!parsed.has_value())
            errors.push_back(context + " has invalid value");
        else
            sample.constant = *parsed;
    }
    if (member(object, "descriptor_id")) {
        const auto parsed = unsigned_value(object, "descriptor_id");
        if (!parsed.has_value() || *parsed == 0 || *parsed > UINT32_MAX)
            errors.push_back(context + " has invalid routed sample descriptor_id");
        else
            sample.routed_sample_descriptor_id = static_cast<std::uint32_t>(*parsed);
    }
    if (const auto trace = string_value(object, "trace"); trace.has_value()) {
        const auto parsed = parse_trace_policy(*trace);
        if (!parsed.has_value())
            errors.push_back(context + " has invalid trace policy '" + *trace + "'");
        else
            sample.trace = *parsed;
    } else if (member(object, "trace")) {
        errors.push_back(context + " trace must be a string");
    }
    parse_byte_program(object, "program", context, sample.address_program, errors);

    if (*kind == SampleKind::Gpr && !member(object, "register"))
        errors.push_back(context + " requires register");
    if (*kind == SampleKind::Memory && !sample.address_provided)
        errors.push_back(context + " requires address");
    if (*kind == SampleKind::RegisterMemory && !member(object, "register"))
        errors.push_back(context + " requires register");
    if (*kind == SampleKind::AddressProgram && sample.address_program.empty())
        errors.push_back(context + " requires program");
    if (*kind == SampleKind::Constant && !member(object, "value"))
        errors.push_back(context + " requires value");
    if (*kind == SampleKind::RoutedSample && sample.routed_sample_descriptor_id == 0)
        errors.push_back(context + " requires descriptor_id");

    if (*kind == SampleKind::LinkedList) {
        const bool static_root = sample.address_provided;
        const bool dynamic_root = !sample.address_program.empty();
        if (static_root == dynamic_root)
            errors.push_back(context + " requires exactly one of address or program");
        if (static_root && sample.trace != AddressTracePolicy::Off)
            errors.push_back(context + " trace requires an address-program root");
        const auto max_nodes = unsigned_value(object, "max_nodes");
        if (!max_nodes.has_value() || *max_nodes == 0 || *max_nodes > 256)
            errors.push_back(context + " max_nodes must be between 1 and 256");
        else
            sample.max_nodes = static_cast<std::uint32_t>(*max_nodes);
        const auto next_offset = signed_value(object, "next_offset");
        if (!next_offset.has_value() || *next_offset < INT32_MIN || *next_offset > INT32_MAX)
            errors.push_back(context + " requires a valid next_offset");
        else
            sample.next_offset = static_cast<std::int32_t>(*next_offset);
        const auto* fields = member(object, "fields");
        if (!fields || !fields->is<Array>() || fields->get<Array>().empty()) {
            errors.push_back(context + " fields must be a nonempty array");
        } else {
            std::unordered_set<std::string> names;
            for (const auto& raw_field : fields->get<Array>()) {
                if (!raw_field.is<Object>()) {
                    errors.push_back(context + " linked-list field must be an object");
                    continue;
                }
                const auto& field_object = raw_field.get<Object>();
                reject_unknown_keys(field_object, { "name", "offset", "width" }, context + " field", errors);
                const auto field_name = string_value(field_object, "name");
                bool field_width_ok = false;
                const auto field_width = parse_width(member(field_object, "width"), field_width_ok);
                const auto field_offset = signed_value(field_object, "offset");
                if (!field_name.has_value() || field_name->empty() || !names.insert(*field_name).second
                    || !field_width_ok || !field_offset.has_value()
                    || *field_offset < INT32_MIN || *field_offset > INT32_MAX) {
                    errors.push_back(context + " contains an invalid linked-list field");
                    continue;
                }
                sample.linked_list_fields.push_back(LinkedListField{
                    *field_name, static_cast<std::int32_t>(*field_offset), field_width });
            }
        }
    }

    if (*kind == SampleKind::StackTrace) {
        const auto max_frames = unsigned_value(object, "max_frames");
        if (!max_frames.has_value() || *max_frames == 0 || *max_frames > 64)
            errors.push_back(context + " max_frames must be between 1 and 64");
        else
            sample.max_frames = static_cast<std::uint32_t>(*max_frames);
    }
    probe.samples.push_back(std::move(sample));
}

Array string_array(const std::vector<std::string>& values)
{
    Array result;
    for (const auto& value : values)
        result.emplace_back(value);
    return result;
}

Array byte_array(const std::vector<std::uint8_t>& values)
{
    Array result;
    for (const auto value : values)
        result.emplace_back(static_cast<double>(value));
    return result;
}

picojson::value json_u64(std::uint64_t value)
{
    return picojson::value(static_cast<double>(value));
}

picojson::value json_i64(std::int64_t value)
{
    return picojson::value(static_cast<double>(value));
}

} // namespace

std::string_view address_program_operation_name(std::uint8_t operation)
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

std::string_view address_program_failure_name(AddressProgramFailure failure)
{
    switch (failure) {
    case AddressProgramFailure::None: return "none";
    case AddressProgramFailure::MissingTerminator: return "missing_terminator";
    case AddressProgramFailure::TrailingBytes: return "trailing_bytes";
    case AddressProgramFailure::UnknownOperation: return "unknown_operation";
    case AddressProgramFailure::TruncatedOperand: return "truncated_operand";
    case AddressProgramFailure::InvalidRegister: return "invalid_register";
    case AddressProgramFailure::MissingBaseResolver: return "missing_base_resolver";
    case AddressProgramFailure::BaseResolutionFailed: return "base_resolution_failed";
    case AddressProgramFailure::GuestReadFailed: return "guest_read_failed";
    case AddressProgramFailure::OperationLimit: return "operation_limit";
    }
    return "unknown";
}

AddressProgramValidation validate_address_program(std::span<const std::uint8_t> program)
{
    AddressProgramValidation result;
    std::size_t offset = 0;
    while (offset < program.size()) {
        if (result.operation_count == 64) {
            result.failure = AddressProgramFailure::OperationLimit;
            result.failure_offset = offset;
            result.message = "address program exceeds 64 operations";
            return result;
        }
        const auto operation_offset = offset;
        const auto operation = program[offset++];
        ++result.operation_count;
        std::size_t operand_bytes = 0;
        switch (operation) {
        case 0x00:
            if (offset != program.size()) {
                result.failure = AddressProgramFailure::TrailingBytes;
                result.failure_offset = offset;
                result.message = "bytes follow the terminating operation";
                return result;
            }
            result.valid = true;
            return result;
        case 0x01: operand_bytes = 2; break;
        case 0x02: operand_bytes = 0; break;
        case 0x03: operand_bytes = 4; break;
        case 0x04: operand_bytes = 4; break;
        case 0x05: operand_bytes = 4; break;
        case 0x06: operand_bytes = 1; break;
        case 0x07: operand_bytes = 4; break;
        case 0x08: operand_bytes = 5; break;
        default:
            result.failure = AddressProgramFailure::UnknownOperation;
            result.failure_offset = operation_offset;
            result.message = "unknown operation 0x" + [&] {
                std::ostringstream out; out << std::hex << static_cast<unsigned>(operation); return out.str(); }();
            return result;
        }
        if (program.size() - offset < operand_bytes) {
            result.failure = AddressProgramFailure::TruncatedOperand;
            result.failure_offset = operation_offset;
            result.message = "operation '" + std::string(address_program_operation_name(operation))
                + "' has a truncated operand";
            return result;
        }
        if ((operation == 0x06 || operation == 0x08) && program[offset] >= 32) {
            result.failure = AddressProgramFailure::InvalidRegister;
            result.failure_offset = operation_offset;
            result.message = "operation uses an invalid GPR";
            return result;
        }
        offset += operand_bytes;
    }
    result.failure = AddressProgramFailure::MissingTerminator;
    result.failure_offset = program.size();
    result.message = "address program has no terminating operation";
    return result;
}

ProfileParseResult parse_profile_json(const std::string& text)
{
    ProfileParseResult result;
    picojson::value root;
    auto position = text.begin();
    const auto parse_error = picojson::parse(root, position, text.end());
    if (!parse_error.empty()) {
        result.errors.push_back("invalid capture profile JSON: " + parse_error);
        return result;
    }
    position = std::find_if_not(position, text.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    if (position != text.end()) {
        result.errors.push_back("invalid capture profile JSON: trailing content after root object");
        return result;
    }
    if (!root.is<Object>()) {
        result.errors.push_back("capture profile root must be an object");
        return result;
    }
    const auto& object = root.get<Object>();
    reject_unknown_keys(object,
        { "schema", "name", "revision", "expected_module_sha256", "battle_progress_enabled", "limits", "probes", "windows", "flight_recorders" },
        "capture profile", result.errors);

    Profile profile;
    profile.schema = string_value(object, "schema").value_or("");
    profile.name = string_value(object, "name").value_or("");
    profile.revision = unsigned_value(object, "revision").value_or(1);
    profile.expected_module_sha256 = string_value(object, "expected_module_sha256").value_or("");
    profile.battle_progress_enabled = bool_value(object, "battle_progress_enabled", true);
    require_type(object, "schema", string_value(object, "schema").has_value(), "a string", "capture profile", result.errors);
    require_type(object, "name", string_value(object, "name").has_value(), "a string", "capture profile", result.errors);
    require_type(object, "revision", unsigned_value(object, "revision").has_value(), "an unsigned integer", "capture profile", result.errors);
    require_type(object, "battle_progress_enabled",
        !member(object, "battle_progress_enabled") || member(object, "battle_progress_enabled")->is<bool>(),
        "a boolean", "capture profile", result.errors);
    if (profile.schema != "savor.capture.profile/1")
        result.errors.push_back("unsupported capture profile schema '" + profile.schema + "'");
    if (profile.name.empty())
        result.errors.push_back("capture profile name must not be empty");
    if (profile.revision == 0)
        result.errors.push_back("capture profile revision must be positive");
    if (!profile.expected_module_sha256.empty()
        && (profile.expected_module_sha256.size() != 64
            || !std::ranges::all_of(profile.expected_module_sha256, [](unsigned char ch) {
                return std::isxdigit(ch) != 0;
            }))) {
        result.errors.push_back("expected_module_sha256 must contain exactly 64 hex characters");
    }

    if (const auto* limits = member(object, "limits")) {
        if (!limits->is<Object>()) {
            result.errors.push_back("capture profile limits must be an object");
        } else {
            const auto& limit_object = limits->get<Object>();
            reject_unknown_keys(limit_object,
                { "queue_bytes", "max_events", "progress_events", "chunk_events" },
                "capture profile limits", result.errors);
            if (const auto value = unsigned_value(limit_object, "queue_bytes")) profile.limits.queue_bytes = *value;
            else if (member(limit_object, "queue_bytes")) result.errors.push_back("limits.queue_bytes must be an unsigned integer");
            if (const auto value = unsigned_value(limit_object, "max_events")) profile.limits.max_events = static_cast<std::uint32_t>(*value);
            else if (member(limit_object, "max_events")) result.errors.push_back("limits.max_events must be an unsigned integer");
            if (const auto value = unsigned_value(limit_object, "progress_events")) profile.limits.progress_events = static_cast<std::uint32_t>(*value);
            else if (member(limit_object, "progress_events")) result.errors.push_back("limits.progress_events must be an unsigned integer");
            if (const auto value = unsigned_value(limit_object, "chunk_events")) profile.limits.chunk_events = static_cast<std::uint32_t>(*value);
            else if (member(limit_object, "chunk_events")) result.errors.push_back("limits.chunk_events must be an unsigned integer");
        }
    }
    if (profile.limits.queue_bytes < sizeof(std::uint64_t)
        || profile.limits.max_events < 16 || profile.limits.max_events > 4096
        || profile.limits.progress_events < 16 || profile.limits.progress_events > 4096
        || profile.limits.chunk_events < 16 || profile.limits.chunk_events > 4096) {
        result.errors.push_back("capture profile limits are outside their bounded ranges");
    }

    std::unordered_set<std::string> probe_ids;
    const auto* probes = member(object, "probes");
    if (!probes || !probes->is<Array>()) {
        result.errors.push_back("capture profile probes must be an array");
    } else {
        for (const auto& raw_probe : probes->get<Array>()) {
            if (!raw_probe.is<Object>()) {
                result.errors.push_back("capture profile contains a non-object probe");
                continue;
            }
            const auto& probe_object = raw_probe.get<Object>();
            const auto id = string_value(probe_object, "id").value_or("");
            const auto context = "probe '" + id + "'";
            reject_unknown_keys(probe_object,
                { "id", "group", "kind", "address", "size", "access", "activate_on_pc", "max_hits", "one_shot", "owns_rng_draw", "trace", "record_progress", "frame_clock", "progress_formatter", "window", "sampling", "predicate", "samples", "address_program", "subscriptions", "symbol" },
                context, result.errors);
            ProbeDefinition probe;
            probe.id = id;
            probe.group = string_value(probe_object, "group").value_or("");
            const auto kind = string_value(probe_object, "kind").value_or("");
            if (probe.id.empty()) result.errors.push_back("probe id must not be empty");
            else if (!probe_ids.insert(probe.id).second) result.errors.push_back("duplicate probe id '" + probe.id + "'");
            if (kind == "pc") probe.kind = ProbeKind::Pc;
            else if (kind == "memory") probe.kind = ProbeKind::Memory;
            else if (kind == "marker") probe.kind = ProbeKind::Marker;
            else result.errors.push_back(context + " has unknown kind '" + kind + "'");

            const bool has_address = member(probe_object, "address") != nullptr;
            if (has_address) {
                const auto value = unsigned_value(probe_object, "address");
                if (!value.has_value() || *value > UINT32_MAX) result.errors.push_back(context + " has invalid address");
                else probe.address = static_cast<std::uint32_t>(*value);
            }
            if (const auto value = unsigned_value(probe_object, "size")) probe.size = static_cast<std::uint32_t>(*value);
            else if (member(probe_object, "size")) result.errors.push_back(context + " has invalid size");
            if (const auto value = unsigned_value(probe_object, "activate_on_pc")) probe.activate_on_pc = static_cast<std::uint32_t>(*value);
            else if (member(probe_object, "activate_on_pc")) result.errors.push_back(context + " has invalid activate_on_pc");
            probe.max_hits = unsigned_value(probe_object, "max_hits");
            probe.one_shot = bool_value(probe_object, "one_shot");
            probe.owns_rng_draw = bool_value(probe_object, "owns_rng_draw");
            probe.progress_record = bool_value(probe_object, "record_progress", true);
            probe.frame_clock = bool_value(probe_object, "frame_clock");
            probe.progress_formatter = string_value(probe_object, "progress_formatter").value_or("");
            probe.window_id = string_value(probe_object, "window").value_or("");
            for (const auto key : { "one_shot", "owns_rng_draw", "record_progress", "frame_clock" })
                require_type(probe_object, key, !member(probe_object, key) || member(probe_object, key)->is<bool>(), "a boolean", context, result.errors);
            if (const auto trace = string_value(probe_object, "trace"); trace.has_value()) {
                const auto parsed = parse_trace_policy(*trace);
                if (!parsed.has_value()) result.errors.push_back(context + " has invalid trace policy '" + *trace + "'");
                else probe.address_trace = *parsed;
            } else if (member(probe_object, "trace")) result.errors.push_back(context + " trace must be a string");
            parse_byte_program(probe_object, "address_program", context, probe.address_program, result.errors);

            const auto access = string_value(probe_object, "access").value_or("write");
            if (access == "read") probe.memory_access = MemoryAccess::Read;
            else if (access == "write") probe.memory_access = MemoryAccess::Write;
            else if (access == "access") probe.memory_access = MemoryAccess::Access;
            else result.errors.push_back(context + " has unknown memory access '" + access + "'");

            probe.subscriptions = Subscription::None;
            const auto* subscriptions = member(probe_object, "subscriptions");
            if (!subscriptions || !subscriptions->is<Array>()) {
                result.errors.push_back(context + " subscriptions must be an array");
            } else {
                std::unordered_set<std::string> seen;
                for (const auto& value : subscriptions->get<Array>()) {
                    if (!value.is<std::string>()) {
                        result.errors.push_back(context + " contains a non-string subscription");
                        continue;
                    }
                    const auto subscription = value.get<std::string>();
                    if (!seen.insert(subscription).second) result.errors.push_back(context + " duplicates subscription '" + subscription + "'");
                    else if (subscription == "capture") probe.subscriptions = probe.subscriptions | Subscription::Capture;
                    else if (subscription == "progress") probe.subscriptions = probe.subscriptions | Subscription::Progress;
                    else if (subscription == "control") probe.subscriptions = probe.subscriptions | Subscription::Control;
                    else result.errors.push_back(context + " has unknown subscription '" + subscription + "'");
                }
            }
            if (probe.subscriptions == Subscription::None)
                result.errors.push_back(context + " must have at least one subscription");

            if (const auto* symbol = member(probe_object, "symbol")) {
                if (!symbol->is<Object>()) {
                    result.errors.push_back(context + " symbol must be an object");
                } else {
                    const auto& symbol_object = symbol->get<Object>();
                    reject_unknown_keys(symbol_object, { "name", "function", "checkpoint" }, context + " symbol", result.errors);
                    probe.symbol.name = string_value(symbol_object, "name").value_or("");
                    probe.symbol.function = string_value(symbol_object, "function").value_or("");
                    probe.symbol.checkpoint = string_value(symbol_object, "checkpoint").value_or("");
                }
            }

            if (const auto* samples = member(probe_object, "samples")) {
                if (!samples->is<Array>()) result.errors.push_back(context + " samples must be an array");
                else for (const auto& sample : samples->get<Array>()) parse_sample(sample, probe, result.errors);
            }

            if (const auto* predicate = member(probe_object, "predicate")) {
                if (!predicate->is<Array>()) {
                    result.errors.push_back(context + " predicate must be an array");
                } else if (predicate->get<Array>().size() > 64) {
                    result.errors.push_back(context + " predicate exceeds 64 operations");
                } else {
                    std::int32_t stack_depth = 0;
                    for (const auto& instruction : predicate->get<Array>()) {
                        if (!instruction.is<Object>()) {
                            result.errors.push_back(context + " predicate instruction must be an object");
                            continue;
                        }
                        const auto& instruction_object = instruction.get<Object>();
                        const auto name = string_value(instruction_object, "op").value_or("");
                        const auto op = parse_predicate_op(name);
                        if (!op.has_value()) {
                            result.errors.push_back(context + " has unknown predicate op '" + name + "'");
                            continue;
                        }
                        switch (*op) {
                        case PredicateOp::PushConstant: reject_unknown_keys(instruction_object, { "op", "value" }, context + " predicate", result.errors); break;
                        case PredicateOp::PushGpr: reject_unknown_keys(instruction_object, { "op", "register" }, context + " predicate", result.errors); break;
                        case PredicateOp::PushSample: reject_unknown_keys(instruction_object, { "op", "index" }, context + " predicate", result.errors); break;
                        case PredicateOp::MaskEqual: reject_unknown_keys(instruction_object, { "op", "value", "mask" }, context + " predicate", result.errors); break;
                        default: reject_unknown_keys(instruction_object, { "op" }, context + " predicate", result.errors); break;
                        }
                        const auto operand = unsigned_value(instruction_object, "value").value_or(
                            unsigned_value(instruction_object, "index").value_or(
                                unsigned_value(instruction_object, "register").value_or(0)));
                        const auto operand2 = unsigned_value(instruction_object, "mask").value_or(0);
                        if (*op == PredicateOp::PushGpr && operand >= 32)
                            result.errors.push_back(context + " predicate uses an invalid GPR");
                        if (*op == PredicateOp::PushSample && operand >= probe.samples.size())
                            result.errors.push_back(context + " predicate uses an invalid sample index");
                        const bool unary = *op == PredicateOp::LogicalNot || *op == PredicateOp::MaskEqual;
                        const bool push = static_cast<std::uint8_t>(*op) < 16;
                        if (push) ++stack_depth;
                        else if (unary) {
                            if (stack_depth < 1) result.errors.push_back(context + " predicate stack underflow");
                        } else {
                            if (stack_depth < 2) result.errors.push_back(context + " predicate stack underflow");
                            else --stack_depth;
                        }
                        probe.predicate.push_back(PredicateInstruction{ *op, operand, operand2 });
                    }
                    if (!probe.predicate.empty() && stack_depth != 1)
                        result.errors.push_back(context + " predicate must leave exactly one stack value");
                }
            }

            if (const auto* sampling = member(probe_object, "sampling")) {
                if (!sampling->is<Object>()) {
                    result.errors.push_back(context + " sampling must be an object");
                } else {
                    const auto& sampling_object = sampling->get<Object>();
                    const auto mode_name = string_value(sampling_object, "mode").value_or("");
                    const auto mode = parse_sampling_mode(mode_name);
                    if (!mode.has_value()) {
                        result.errors.push_back(context + " has unknown sampling mode '" + mode_name + "'");
                    } else {
                        probe.sampling.mode = *mode;
                        switch (*mode) {
                        case SamplingMode::EveryHit:
                        case SamplingMode::ChangedOnly:
                            reject_unknown_keys(sampling_object, { "mode" }, context + " sampling", result.errors);
                            break;
                        case SamplingMode::EveryN:
                        case SamplingMode::FirstN:
                        case SamplingMode::LastN:
                            reject_unknown_keys(sampling_object, { "mode", "n" }, context + " sampling", result.errors);
                            probe.sampling.n = unsigned_value(sampling_object, "n").value_or(0);
                            if (probe.sampling.n == 0) result.errors.push_back(context + " sampling n must be positive");
                            break;
                        case SamplingMode::HitRanges: {
                            reject_unknown_keys(sampling_object, { "mode", "ranges" }, context + " sampling", result.errors);
                            const auto* ranges = member(sampling_object, "ranges");
                            if (!ranges || !ranges->is<Array>() || ranges->get<Array>().empty()) {
                                result.errors.push_back(context + " hit_ranges requires ranges");
                            } else {
                                for (const auto& range : ranges->get<Array>()) {
                                    if (!range.is<Array>() || range.get<Array>().size() != 2) {
                                        result.errors.push_back(context + " contains an invalid hit range");
                                        continue;
                                    }
                                    const auto first = unsigned_value(range.get<Array>()[0]);
                                    const auto last = unsigned_value(range.get<Array>()[1]);
                                    if (!first.has_value() || !last.has_value() || *first > *last)
                                        result.errors.push_back(context + " contains an invalid hit range");
                                    else probe.sampling.hit_ranges.emplace_back(*first, *last);
                                }
                            }
                            break;
                        }
                        case SamplingMode::Burst:
                            reject_unknown_keys(sampling_object, { "mode", "period", "length" }, context + " sampling", result.errors);
                            probe.sampling.burst_period = unsigned_value(sampling_object, "period").value_or(0);
                            probe.sampling.burst_length = unsigned_value(sampling_object, "length").value_or(0);
                            if (probe.sampling.burst_period == 0 || probe.sampling.burst_length == 0
                                || probe.sampling.burst_length > probe.sampling.burst_period) {
                                result.errors.push_back(context + " burst sampling requires 0 < length <= period");
                            }
                            break;
                        }
                    }
                }
            }

            const std::size_t trace_fields = std::ranges::count_if(probe.samples, [](const auto& sample) {
                return sample.trace != AddressTracePolicy::Off;
            }) + (probe.address_trace != AddressTracePolicy::Off ? 1u : 0u);
            if (probe.samples.size() + trace_fields > kMaxProbeSamples)
                result.errors.push_back(context + " exceeds the fixed raw-field capacity");
            if (probe.frame_clock && probe.kind != ProbeKind::Pc)
                result.errors.push_back(context + " uses frame_clock but is not a PC probe");
            if (probe.kind == ProbeKind::Pc && !has_address)
                result.errors.push_back(context + " PC probe requires address");
            if (probe.kind == ProbeKind::Memory) {
                if (probe.size != 1 && probe.size != 2 && probe.size != 4 && probe.size != 8)
                    result.errors.push_back(context + " memory size must be 1, 2, 4, or 8");
                if (has_address == !probe.address_program.empty())
                    result.errors.push_back(context + " memory probe requires exactly one of address or address_program");
                if (has_address && probe.address_trace != AddressTracePolicy::Off)
                    result.errors.push_back(context + " trace requires an address_program");
                if (!probe.address_program.empty() && !probe.activate_on_pc.has_value())
                    result.errors.push_back(context + " dynamic memory root requires activate_on_pc");
            } else if (!probe.address_program.empty() || probe.address_trace != AddressTracePolicy::Off) {
                result.errors.push_back(context + " address_program and root trace are valid only for memory probes");
            }
            profile.probes.push_back(std::move(probe));
        }
    }

    std::unordered_set<std::string> window_ids;
    if (const auto* windows = member(object, "windows")) {
        if (!windows->is<Array>()) {
            result.errors.push_back("capture profile windows must be an array");
        } else {
            for (const auto& raw_window : windows->get<Array>()) {
                if (!raw_window.is<Object>()) {
                    result.errors.push_back("capture window must be an object");
                    continue;
                }
                const auto& window_object = raw_window.get<Object>();
                reject_unknown_keys(window_object,
                    { "id", "open_probe", "close_probe", "open_marker", "close_marker", "open_on_control", "close_on_control", "open_frame", "close_frame", "event_limit", "frame_limit", "initially_open" },
                    "capture window", result.errors);
                WindowDefinition window;
                window.id = string_value(window_object, "id").value_or("");
                window.open_probe = string_value(window_object, "open_probe").value_or("");
                window.close_probe = string_value(window_object, "close_probe").value_or("");
                window.open_marker = string_value(window_object, "open_marker").value_or("");
                window.close_marker = string_value(window_object, "close_marker").value_or("");
                window.open_on_control = bool_value(window_object, "open_on_control");
                window.close_on_control = bool_value(window_object, "close_on_control");
                window.open_frame = unsigned_value(window_object, "open_frame");
                window.close_frame = unsigned_value(window_object, "close_frame");
                window.event_limit = unsigned_value(window_object, "event_limit").value_or(0);
                window.frame_limit = unsigned_value(window_object, "frame_limit").value_or(0);
                window.initially_open = bool_value(window_object, "initially_open");
                if (window.id.empty() || !window_ids.insert(window.id).second)
                    result.errors.push_back("capture window id is empty or duplicated");
                if (!window.open_probe.empty() && !probe_ids.contains(window.open_probe))
                    result.errors.push_back("capture window '" + window.id + "' references unknown open_probe");
                if (!window.close_probe.empty() && !probe_ids.contains(window.close_probe))
                    result.errors.push_back("capture window '" + window.id + "' references unknown close_probe");
                if (!window.initially_open && window.open_probe.empty() && window.open_marker.empty()
                    && !window.open_on_control && !window.open_frame.has_value()) {
                    result.errors.push_back("capture window '" + window.id + "' has no opener");
                }
                profile.windows.push_back(std::move(window));
            }
        }
    }
    for (const auto& probe : profile.probes) {
        if (!probe.window_id.empty() && !window_ids.contains(probe.window_id))
            result.errors.push_back("probe '" + probe.id + "' references unknown window '" + probe.window_id + "'");
    }

    if (const auto* recorders = member(object, "flight_recorders")) {
        if (!recorders->is<Array>()) {
            result.errors.push_back("flight_recorders must be an array");
        } else {
            std::unordered_set<std::string> recorder_ids;
            std::unordered_map<std::string, std::string> memberships;
            for (const auto& raw_recorder : recorders->get<Array>()) {
                if (!raw_recorder.is<Object>()) {
                    result.errors.push_back("flight recorder must be an object");
                    continue;
                }
                const auto& recorder_object = raw_recorder.get<Object>();
                reject_unknown_keys(recorder_object,
                    { "id", "member_probes", "pre_events", "post_events", "trigger_probes", "trigger_markers", "trigger_on_control" },
                    "flight recorder", result.errors);
                FlightRecorderDefinition recorder;
                recorder.id = string_value(recorder_object, "id").value_or("");
                recorder.pre_events = static_cast<std::uint32_t>(unsigned_value(recorder_object, "pre_events").value_or(0));
                recorder.post_events = static_cast<std::uint32_t>(unsigned_value(recorder_object, "post_events").value_or(0));
                recorder.trigger_on_control = bool_value(recorder_object, "trigger_on_control");
                if (recorder.id.empty() || !recorder_ids.insert(recorder.id).second)
                    result.errors.push_back("flight recorder id is empty or duplicated");
                const auto parse_names = [&](std::string_view key, std::vector<std::string>& output) {
                    const auto* values = member(recorder_object, key);
                    if (!values || !values->is<Array>()) {
                        result.errors.push_back("flight recorder '" + recorder.id + "' field '" + std::string(key) + "' must be an array");
                        return;
                    }
                    std::unordered_set<std::string> unique;
                    for (const auto& value : values->get<Array>()) {
                        if (!value.is<std::string>() || value.get<std::string>().empty()
                            || !unique.insert(value.get<std::string>()).second) {
                            result.errors.push_back("flight recorder '" + recorder.id + "' contains an invalid '" + std::string(key) + "' entry");
                        } else output.push_back(value.get<std::string>());
                    }
                };
                parse_names("member_probes", recorder.member_probes);
                if (member(recorder_object, "trigger_probes")) parse_names("trigger_probes", recorder.trigger_probes);
                if (member(recorder_object, "trigger_markers")) parse_names("trigger_markers", recorder.trigger_markers);
                if (recorder.member_probes.empty())
                    result.errors.push_back("flight recorder '" + recorder.id + "' requires member_probes");
                if (recorder.trigger_probes.empty() && recorder.trigger_markers.empty() && !recorder.trigger_on_control)
                    result.errors.push_back("flight recorder '" + recorder.id + "' requires a trigger");
                for (const auto& id : recorder.member_probes) {
                    if (!probe_ids.contains(id)) result.errors.push_back("flight recorder '" + recorder.id + "' references unknown member probe '" + id + "'");
                    if (memberships.contains(id)) result.errors.push_back("probe '" + id + "' belongs to more than one flight recorder");
                    else memberships[id] = recorder.id;
                    const auto probe = std::ranges::find(profile.probes, id, &ProbeDefinition::id);
                    if (probe != profile.probes.end() && probe->sampling.mode == SamplingMode::LastN)
                        result.errors.push_back("probe '" + id + "' cannot combine LastN with flight recording");
                }
                for (const auto& id : recorder.trigger_probes)
                    if (!probe_ids.contains(id)) result.errors.push_back("flight recorder '" + recorder.id + "' references unknown trigger probe '" + id + "'");
                profile.flight_recorders.push_back(std::move(recorder));
            }
        }
    }

    if (result.errors.empty())
        result.profile = std::move(profile);
    return result;
}

std::string serialize_profile_json(const Profile& profile)
{
    Object root{
        { "schema", picojson::value(profile.schema) },
        { "name", picojson::value(profile.name) },
        { "revision", json_u64(profile.revision) },
        { "limits", picojson::value(Object{
            { "queue_bytes", json_u64(profile.limits.queue_bytes) },
            { "max_events", json_u64(profile.limits.max_events) },
            { "progress_events", json_u64(profile.limits.progress_events) },
            { "chunk_events", json_u64(profile.limits.chunk_events) },
        }) },
    };
    if (!profile.expected_module_sha256.empty())
        root["expected_module_sha256"] = picojson::value(profile.expected_module_sha256);
    if (!profile.battle_progress_enabled)
        root["battle_progress_enabled"] = picojson::value(false);
    Array probes;
    for (const auto& probe : profile.probes) {
        Object object{
            { "id", picojson::value(probe.id) },
            { "group", picojson::value(probe.group) },
            { "kind", picojson::value(probe.kind == ProbeKind::Pc ? "pc" : probe.kind == ProbeKind::Memory ? "memory" : "marker") },
        };
        if (probe.kind == ProbeKind::Pc || (probe.kind == ProbeKind::Memory && probe.address_program.empty()))
            object["address"] = json_u64(probe.address);
        if (probe.kind == ProbeKind::Memory) {
            object["size"] = json_u64(probe.size);
            object["access"] = picojson::value(probe.memory_access == MemoryAccess::Read ? "read" : probe.memory_access == MemoryAccess::Write ? "write" : "access");
            if (!probe.address_program.empty()) object["address_program"] = picojson::value(byte_array(probe.address_program));
            if (probe.address_trace != AddressTracePolicy::Off) object["trace"] = picojson::value(std::string(trace_policy_name(probe.address_trace)));
        }
        if (probe.activate_on_pc) object["activate_on_pc"] = json_u64(*probe.activate_on_pc);
        if (probe.max_hits) object["max_hits"] = json_u64(*probe.max_hits);
        if (probe.one_shot) object["one_shot"] = picojson::value(true);
        if (probe.owns_rng_draw) object["owns_rng_draw"] = picojson::value(true);
        if (!probe.progress_record) object["record_progress"] = picojson::value(false);
        if (probe.frame_clock) object["frame_clock"] = picojson::value(true);
        if (!probe.progress_formatter.empty()) object["progress_formatter"] = picojson::value(probe.progress_formatter);
        if (!probe.window_id.empty()) object["window"] = picojson::value(probe.window_id);
        Array subscriptions;
        if (has_subscription(probe.subscriptions, Subscription::Capture)) subscriptions.emplace_back("capture");
        if (has_subscription(probe.subscriptions, Subscription::Progress)) subscriptions.emplace_back("progress");
        if (has_subscription(probe.subscriptions, Subscription::Control)) subscriptions.emplace_back("control");
        object["subscriptions"] = picojson::value(std::move(subscriptions));
        if (!probe.symbol.name.empty() || !probe.symbol.function.empty() || !probe.symbol.checkpoint.empty()) {
            Object symbol;
            if (!probe.symbol.name.empty()) symbol["name"] = picojson::value(probe.symbol.name);
            if (!probe.symbol.function.empty()) symbol["function"] = picojson::value(probe.symbol.function);
            if (!probe.symbol.checkpoint.empty()) symbol["checkpoint"] = picojson::value(probe.symbol.checkpoint);
            object["symbol"] = picojson::value(std::move(symbol));
        }
        if (!probe.predicate.empty()) {
            Array predicate;
            for (const auto& instruction : probe.predicate) {
                Object item{ { "op", picojson::value(std::string(predicate_op_name(instruction.op))) } };
                if (instruction.op == PredicateOp::PushConstant) item["value"] = json_u64(instruction.operand);
                else if (instruction.op == PredicateOp::PushGpr) item["register"] = json_u64(instruction.operand);
                else if (instruction.op == PredicateOp::PushSample) item["index"] = json_u64(instruction.operand);
                else if (instruction.op == PredicateOp::MaskEqual) {
                    item["value"] = json_u64(instruction.operand);
                    item["mask"] = json_u64(instruction.operand2);
                }
                predicate.emplace_back(std::move(item));
            }
            object["predicate"] = picojson::value(std::move(predicate));
        }
        if (probe.sampling.mode != SamplingMode::EveryHit) {
            Object sampling{ { "mode", picojson::value(std::string(sampling_mode_name(probe.sampling.mode))) } };
            if (probe.sampling.mode == SamplingMode::EveryN || probe.sampling.mode == SamplingMode::FirstN || probe.sampling.mode == SamplingMode::LastN)
                sampling["n"] = json_u64(probe.sampling.n);
            else if (probe.sampling.mode == SamplingMode::HitRanges) {
                Array ranges;
                for (const auto& [first, last] : probe.sampling.hit_ranges)
                    ranges.emplace_back(Array{ json_u64(first), json_u64(last) });
                sampling["ranges"] = picojson::value(std::move(ranges));
            } else if (probe.sampling.mode == SamplingMode::Burst) {
                sampling["period"] = json_u64(probe.sampling.burst_period);
                sampling["length"] = json_u64(probe.sampling.burst_length);
            }
            object["sampling"] = picojson::value(std::move(sampling));
        }
        if (!probe.samples.empty()) {
            Array samples;
            for (const auto& sample : probe.samples) {
                Object item{
                    { "name", picojson::value(sample.name) },
                    { "type", picojson::value(std::string(sample_kind_name(sample.kind))) },
                };
                if (sample.kind == SampleKind::Gpr || sample.kind == SampleKind::Memory
                    || sample.kind == SampleKind::RegisterMemory || sample.kind == SampleKind::AddressProgram
                    || sample.kind == SampleKind::LinkedList || sample.kind == SampleKind::RoutedSample) {
                    item["width"] = json_u64(static_cast<std::uint8_t>(sample.width));
                }
                if (sample.kind == SampleKind::Gpr || sample.kind == SampleKind::RegisterMemory)
                    item["register"] = json_u64(sample.base_register);
                if (sample.kind == SampleKind::RegisterMemory) item["offset"] = json_i64(sample.offset);
                if (sample.kind == SampleKind::Memory || (sample.kind == SampleKind::LinkedList && sample.address_provided))
                    item["address"] = json_u64(sample.address);
                if (sample.kind == SampleKind::Constant) item["value"] = json_u64(sample.constant);
                if (sample.kind == SampleKind::RoutedSample)
                    item["descriptor_id"] = json_u64(sample.routed_sample_descriptor_id);
                if (!sample.address_program.empty()) item["program"] = picojson::value(byte_array(sample.address_program));
                if (sample.trace != AddressTracePolicy::Off) item["trace"] = picojson::value(std::string(trace_policy_name(sample.trace)));
                if (sample.kind == SampleKind::LinkedList) {
                    item["max_nodes"] = json_u64(sample.max_nodes);
                    item["next_offset"] = json_i64(sample.next_offset);
                    Array fields;
                    for (const auto& field : sample.linked_list_fields) {
                        fields.emplace_back(Object{
                            { "name", picojson::value(field.name) },
                            { "offset", json_i64(field.offset) },
                            { "width", json_u64(static_cast<std::uint8_t>(field.width)) },
                        });
                    }
                    item["fields"] = picojson::value(std::move(fields));
                }
                if (sample.kind == SampleKind::StackTrace) item["max_frames"] = json_u64(sample.max_frames);
                samples.emplace_back(std::move(item));
            }
            object["samples"] = picojson::value(std::move(samples));
        }
        probes.emplace_back(std::move(object));
    }
    root["probes"] = picojson::value(std::move(probes));
    if (!profile.windows.empty()) {
        Array windows;
        for (const auto& window : profile.windows) {
            Object item{ { "id", picojson::value(window.id) } };
            if (!window.open_probe.empty()) item["open_probe"] = picojson::value(window.open_probe);
            if (!window.close_probe.empty()) item["close_probe"] = picojson::value(window.close_probe);
            if (!window.open_marker.empty()) item["open_marker"] = picojson::value(window.open_marker);
            if (!window.close_marker.empty()) item["close_marker"] = picojson::value(window.close_marker);
            if (window.open_on_control) item["open_on_control"] = picojson::value(true);
            if (window.close_on_control) item["close_on_control"] = picojson::value(true);
            if (window.open_frame) item["open_frame"] = json_u64(*window.open_frame);
            if (window.close_frame) item["close_frame"] = json_u64(*window.close_frame);
            if (window.event_limit) item["event_limit"] = json_u64(window.event_limit);
            if (window.frame_limit) item["frame_limit"] = json_u64(window.frame_limit);
            if (window.initially_open) item["initially_open"] = picojson::value(true);
            windows.emplace_back(std::move(item));
        }
        root["windows"] = picojson::value(std::move(windows));
    }
    if (!profile.flight_recorders.empty()) {
        Array recorders;
        for (const auto& recorder : profile.flight_recorders) {
            Object item{
                { "id", picojson::value(recorder.id) },
                { "member_probes", picojson::value(string_array(recorder.member_probes)) },
                { "pre_events", json_u64(recorder.pre_events) },
                { "post_events", json_u64(recorder.post_events) },
            };
            if (!recorder.trigger_probes.empty()) item["trigger_probes"] = picojson::value(string_array(recorder.trigger_probes));
            if (!recorder.trigger_markers.empty()) item["trigger_markers"] = picojson::value(string_array(recorder.trigger_markers));
            if (recorder.trigger_on_control) item["trigger_on_control"] = picojson::value(true);
            recorders.emplace_back(std::move(item));
        }
        root["flight_recorders"] = picojson::value(std::move(recorders));
    }
    return picojson::value(std::move(root)).serialize(true);
}

ProfileParseResult load_profile_json(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        ProfileParseResult result;
        result.errors.push_back("failed to open capture profile: " + path.string());
        return result;
    }
    std::ostringstream text;
    text << stream.rdbuf();
    return parse_profile_json(text.str());
}

std::string format_profile_errors(const ProfileParseResult& result)
{
    std::ostringstream out;
    for (const auto& error : result.errors) out << error << '\n';
    for (const auto& warning : result.warnings) out << "warning: " << warning << '\n';
    return out.str();
}

} // namespace savor::probe
